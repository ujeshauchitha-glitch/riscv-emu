//! The F and D extensions: single- and double-precision floating point.
//!
//! Thirty-two registers, separate from the integer file, each 64 bits wide. Doubles
//! live in them directly; singles are **NaN-boxed** - stored in the low 32 bits with
//! all of the upper 32 set to one.
//!
//! NaN boxing is not an optimisation but a correctness rule. The two precisions share
//! one register file, so something has to happen when a program stores a float and
//! reads it as a double. The spec's answer: a 32-bit value in a 64-bit register is
//! only valid if the upper half is all ones, which happens to be the encoding of a
//! quiet NaN. Read that register as a double and you get a NaN - a value that says
//! "this is not a double" - instead of a plausible-looking wrong answer.
//!
//! So every instruction that writes a single must box it, and every instruction that
//! reads one must check the box and substitute a canonical NaN if it is not there.
//!
//! All arithmetic goes through `softfloat`, which implements all five RISC-V rounding
//! modes and the five exception flags exactly.

use super::Cpu;
use crate::csr::csr;
use crate::decoder::{opcodes, DecodedInst};
use crate::device::AccessType;
use crate::softfloat::{self as sf, Rm, F32, F64};
use crate::trap::{Exception, Res, Trap};

pub const CANONICAL_NAN_F32: u32 = 0x7fc0_0000;

/// Wrap a 32-bit single for storage in a 64-bit register.
#[inline(always)]
pub const fn nan_box(single: u32) -> u64 {
    0xffff_ffff_0000_0000 | single as u64
}

/// Unwrap a register as a single. A value that is not properly boxed is not a float
/// at all, and the spec says it must read as the canonical NaN.
#[inline(always)]
pub const fn nan_unbox(reg: u64) -> u32 {
    if reg >> 32 == 0xffff_ffff {
        reg as u32
    } else {
        CANONICAL_NAN_F32
    }
}

// The classes fclass reports, one bit each. Software uses this instead of comparisons
// because it distinguishes -0 from +0 and signalling from quiet NaNs.
pub const FCLASS_NEG_INF: u64 = 1 << 0;
pub const FCLASS_NEG_NORMAL: u64 = 1 << 1;
pub const FCLASS_NEG_SUBNORM: u64 = 1 << 2;
pub const FCLASS_NEG_ZERO: u64 = 1 << 3;
pub const FCLASS_POS_ZERO: u64 = 1 << 4;
pub const FCLASS_POS_SUBNORM: u64 = 1 << 5;
pub const FCLASS_POS_NORMAL: u64 = 1 << 6;
pub const FCLASS_POS_INF: u64 = 1 << 7;
pub const FCLASS_SIGNALLING: u64 = 1 << 8;
pub const FCLASS_QUIET_NAN: u64 = 1 << 9;

/// Classification works on the bit pattern rather than on host predicates, because
/// the distinctions that matter here - negative zero from positive zero, signalling
/// NaN from quiet - are precisely the ones comparisons erase.
pub fn fclass(bits: u64, f: sf::Fmt) -> u64 {
    let negative = bits >> (f.exp_bits + f.frac_bits) != 0;
    let exp_max = (1u64 << f.exp_bits) - 1;
    let exponent = (bits >> f.frac_bits) & exp_max;
    let mantissa = bits & ((1u64 << f.frac_bits) - 1);

    if exponent == exp_max {
        if mantissa == 0 {
            return if negative { FCLASS_NEG_INF } else { FCLASS_POS_INF };
        }
        // The top mantissa bit distinguishes quiet from signalling: set means quiet.
        let quiet_bit = 1u64 << (f.frac_bits - 1);
        return if mantissa & quiet_bit != 0 { FCLASS_QUIET_NAN } else { FCLASS_SIGNALLING };
    }
    if exponent == 0 {
        if mantissa == 0 {
            return if negative { FCLASS_NEG_ZERO } else { FCLASS_POS_ZERO };
        }
        return if negative { FCLASS_NEG_SUBNORM } else { FCLASS_POS_SUBNORM };
    }
    if negative {
        FCLASS_NEG_NORMAL
    } else {
        FCLASS_POS_NORMAL
    }
}

fn illegal(inst: &DecodedInst) -> Trap {
    Trap::new(Exception::IllegalInstruction, inst.encoded as u64)
}

impl Cpu {
    /// mstatus.FS == Off means the floating-point unit is disabled, and every
    /// instruction that touches it is illegal. This is not optional: a kernel that
    /// does not save the f registers across a context switch turns FS off precisely so
    /// user code cannot use them.
    fn require_fpu(&self, inst: &DecodedInst) -> Res<()> {
        if !self.csrs.fpu_enabled() {
            return Err(illegal(inst));
        }
        Ok(())
    }

    fn write_freg(&mut self, index: usize, value: u64) {
        self.fregs[index] = value;
        // Writing any f register makes the unit Dirty, which is what tells a context
        // switch it has 32 registers to save.
        self.csrs.mark_fpu_dirty();
    }

    /// The rounding mode: from the instruction's rm field, or from fcsr when that
    /// field says DYN. A reserved mode makes the instruction illegal rather than
    /// defaulting to something plausible.
    fn rounding(&self, inst: &DecodedInst) -> Res<Rm> {
        let bits = if inst.funct3 == csr::FRM_DYN { self.csrs.rounding_mode() } else { inst.funct3 };
        Rm::from_bits(bits).ok_or_else(|| illegal(inst))
    }

    pub(crate) fn execute_load_fp(&mut self, inst: &DecodedInst) -> Res<()> {
        self.require_fpu(inst)?;
        let rs1 = self.read_reg(inst.rs1);
        let addr = rs1.wrapping_add(inst.imm as u64);
        match inst.funct3 {
            0x2 => {
                // FLW: boxed on the way in, so reading this register as a double yields
                // a NaN rather than a plausible-looking wrong number.
                let v = self.mem_load(addr, 4, AccessType::Load)?;
                self.write_freg(inst.rd, nan_box(v as u32));
                Ok(())
            }
            0x3 => {
                let v = self.mem_load(addr, 8, AccessType::Load)?;
                self.write_freg(inst.rd, v);
                Ok(())
            }
            _ => Err(illegal(inst)),
        }
    }

    pub(crate) fn execute_store_fp(&mut self, inst: &DecodedInst) -> Res<()> {
        self.require_fpu(inst)?;
        let rs1 = self.read_reg(inst.rs1);
        let addr = rs1.wrapping_add(inst.imm as u64);
        match inst.funct3 {
            // FSW stores the low half only; the box is not stored.
            0x2 => self.mem_store(addr, 4, self.fregs[inst.rs2] & 0xffff_ffff),
            0x3 => self.mem_store(addr, 8, self.fregs[inst.rs2]),
            _ => Err(illegal(inst)),
        }
    }

    pub(crate) fn execute_op_fp(&mut self, inst: &DecodedInst) -> Res<()> {
        self.require_fpu(inst)?;

        // funct7's low bits select the precision: 0 single, 1 double. The rest names
        // the operation. fmt 2 and 3 are half and quad, which we do not implement.
        let op = inst.funct7 >> 2;
        let is_double = (inst.funct7 & 0x3) == 0x1;
        if (inst.funct7 & 0x3) > 0x1 {
            return Err(illegal(inst));
        }
        let fmt = if is_double { F64 } else { F32 };

        let a_bits = self.fregs[inst.rs1];
        let b_bits = self.fregs[inst.rs2];
        // The operand as the operation sees it: the whole register for a double, the
        // unboxed low half (or the canonical NaN, if unboxed) for a single.
        let a = if is_double { a_bits } else { nan_unbox(a_bits) as u64 };
        let b = if is_double { b_bits } else { nan_unbox(b_bits) as u64 };

        // Write a result of `fmt` back, boxing a single.
        macro_rules! put {
            ($bits:expr, $flags:expr) => {{
                let (bits, flags): (u64, u64) = ($bits, $flags);
                self.csrs.raise_fflags(flags);
                let v = if is_double { bits } else { nan_box(bits as u32) };
                self.write_freg(inst.rd, v);
                return Ok(());
            }};
        }

        match op {
            0x00 => {
                let rm = self.rounding(inst)?;
                let (r, fl) = sf::add(a, b, fmt, rm);
                put!(r, fl)
            }
            0x01 => {
                let rm = self.rounding(inst)?;
                let (r, fl) = sf::sub(a, b, fmt, rm);
                put!(r, fl)
            }
            0x02 => {
                let rm = self.rounding(inst)?;
                let (r, fl) = sf::mul(a, b, fmt, rm);
                put!(r, fl)
            }
            0x03 => {
                let rm = self.rounding(inst)?;
                let (r, fl) = sf::div(a, b, fmt, rm);
                put!(r, fl)
            }
            0x0b => {
                // FSQRT
                if inst.rs2 != 0 {
                    return Err(illegal(inst));
                }
                let rm = self.rounding(inst)?;
                let (r, fl) = sf::sqrt(a, fmt, rm);
                put!(r, fl)
            }

            0x04 => {
                // FSGNJ / FSGNJN / FSGNJX - sign injection. These are bit manipulation,
                // not arithmetic: they never round, never raise a flag, and never treat
                // a NaN specially. That is what makes them the canonical way to write
                // fabs (FSGNJX rd, rs, rs), fneg (FSGNJN) and a register move (FSGNJ rd,
                // rs, rs).
                let sign_bit = if is_double { 1u64 << 63 } else { 1u64 << 31 };
                let sign = match inst.funct3 {
                    0x0 => b & sign_bit,
                    0x1 => !b & sign_bit,
                    0x2 => (a ^ b) & sign_bit,
                    _ => return Err(illegal(inst)),
                };
                let result = (a & !sign_bit) | sign;
                self.write_freg(inst.rd, if is_double { result } else { nan_box(result as u32) });
                Ok(())
            }

            0x05 => {
                // FMIN / FMAX have semantics that are neither C's fmin/fmax nor a
                // comparison: a signalling NaN in either operand raises Invalid; if one
                // operand is NaN the result is the *other* operand; if both are NaN the
                // result is the canonical NaN; and -0.0 is defined to be less than +0.0.
                if inst.funct3 > 0x1 {
                    return Err(illegal(inst));
                }
                let want_max = inst.funct3 == 0x1;
                let mut flags = 0;
                if sf::is_snan(a, fmt) || sf::is_snan(b, fmt) {
                    flags |= csr::FFLAG_NV;
                }
                let (an, bn) = (sf::is_nan(a, fmt), sf::is_nan(b, fmt));
                let result = if an && bn {
                    fmt.canonical_nan()
                } else if an {
                    b
                } else if bn {
                    a
                } else {
                    let ord = sf::compare(a, b, fmt);
                    if ord == 0 {
                        // Equal as numbers: only the zeros are distinguishable, and the
                        // sign decides. max picks +0, min picks -0.
                        let neg_zero = fmt.zero(true);
                        if a == neg_zero && b != neg_zero {
                            if want_max { b } else { a }
                        } else if b == neg_zero && a != neg_zero {
                            if want_max { a } else { b }
                        } else {
                            a
                        }
                    } else if (ord > 0) == want_max {
                        a
                    } else {
                        b
                    }
                };
                put!(result, flags)
            }

            0x08 => {
                // FCVT.S.D / FCVT.D.S - between the two precisions. The destination is
                // `fmt`; rs2 names the source (0 = single, 1 = double).
                let want_rs2 = if is_double { 0 } else { 1 };
                if inst.rs2 != want_rs2 {
                    return Err(illegal(inst));
                }
                let rm = self.rounding(inst)?;
                let (src_fmt, src) = if is_double { (F32, nan_unbox(a_bits) as u64) } else { (F64, a_bits) };
                let (r, fl) = sf::convert(src, src_fmt, fmt, rm);
                put!(r, fl)
            }

            0x14 => {
                // FEQ / FLT / FLE - comparisons, result to an x register. The quiet
                // comparison (FEQ) raises Invalid only for a signalling NaN; the ordered
                // ones (FLT, FLE) raise it for any NaN.
                let (an, bn) = (sf::is_nan(a, fmt), sf::is_nan(b, fmt));
                let any_snan = sf::is_snan(a, fmt) || sf::is_snan(b, fmt);
                let mut flags = 0;
                let result = match inst.funct3 {
                    0x2 => {
                        if any_snan {
                            flags |= csr::FFLAG_NV;
                        }
                        !an && !bn && sf::compare(a, b, fmt) == 0
                    }
                    0x1 => {
                        if an || bn {
                            flags |= csr::FFLAG_NV;
                        }
                        !an && !bn && sf::compare(a, b, fmt) < 0
                    }
                    0x0 => {
                        if an || bn {
                            flags |= csr::FFLAG_NV;
                        }
                        !an && !bn && sf::compare(a, b, fmt) <= 0
                    }
                    _ => return Err(illegal(inst)),
                };
                self.csrs.raise_fflags(flags);
                self.write_reg(inst.rd, result as u64);
                Ok(())
            }

            0x18 => {
                // FCVT.W/WU/L/LU.S/D - float to integer. rs2: 0 W, 1 WU, 2 L, 3 LU.
                let rm = self.rounding(inst)?;
                let (bits, signed) = match inst.rs2 {
                    0 => (32, true),
                    1 => (32, false),
                    2 => (64, true),
                    3 => (64, false),
                    _ => return Err(illegal(inst)),
                };
                let (v, fl) = sf::to_int(a, fmt, rm, bits, signed);
                // A 32-bit result is sign-extended into the 64-bit register, even the
                // unsigned one. That surprises people, and it is what the spec says.
                let result = if bits == 32 { v as u32 as i32 as i64 as u64 } else { v };
                self.csrs.raise_fflags(fl);
                self.write_reg(inst.rd, result);
                Ok(())
            }

            0x1a => {
                // FCVT.S/D.W/WU/L/LU - integer to float. rs2 names the source width and
                // signedness; 4..31 are reserved. Accepting them and writing zero would
                // give a guest probing for an unimplemented conversion a wrong answer
                // instead of the trap it is waiting for.
                if inst.rs2 > 3 {
                    return Err(illegal(inst));
                }
                let rm = self.rounding(inst)?;
                let x = self.read_reg(inst.rs1);
                let (neg, mag) = match inst.rs2 {
                    0 => ((x as i32) < 0, (x as i32).unsigned_abs() as u64),
                    1 => (false, x as u32 as u64),
                    2 => ((x as i64) < 0, (x as i64).unsigned_abs()),
                    _ => (false, x),
                };
                // Straight to the target format - never via double. Going through a
                // double rounds twice, and the two roundings do not compose: a 64-bit
                // integer that lands exactly on a float tie after the first rounding is
                // then broken the wrong way, giving a result one ULP off.
                let (r, fl) = sf::from_int(neg, mag, fmt, rm);
                put!(r, fl)
            }

            0x1c => {
                if inst.funct3 == 0x0 {
                    // FMV.X.W / FMV.X.D: a raw bit move out of the float file. No
                    // conversion, no rounding, no flags. Raw means raw: FMV.X.W takes
                    // bits 31:0 as they are and sign-extends them. It does *not* apply
                    // the NaN-boxing rule - unboxing here would replace the bits software
                    // asked to see with a canonical NaN, defeating the instruction.
                    if inst.rs2 != 0 {
                        return Err(illegal(inst));
                    }
                    let v = if is_double { a_bits } else { a_bits as u32 as i32 as i64 as u64 };
                    self.write_reg(inst.rd, v);
                    return Ok(());
                }
                if inst.funct3 == 0x1 {
                    // FCLASS
                    self.write_reg(inst.rd, fclass(a, fmt));
                    return Ok(());
                }
                Err(illegal(inst))
            }

            0x1e => {
                // FMV.W.X / FMV.D.X - raw bit move into the float file
                if inst.funct3 != 0x0 || inst.rs2 != 0 {
                    return Err(illegal(inst));
                }
                let x = self.read_reg(inst.rs1);
                self.write_freg(inst.rd, if is_double { x } else { nan_box(x as u32) });
                Ok(())
            }

            _ => Err(illegal(inst)),
        }
    }

    pub(crate) fn execute_fused_madd(&mut self, inst: &DecodedInst) -> Res<()> {
        self.require_fpu(inst)?;

        // R4-type: a fourth register in the top five bits of what would be funct7.
        let rs3 = (inst.raw >> 27) as usize;
        let fmt_bits = (inst.raw >> 25) & 0x3;
        if fmt_bits > 0x1 {
            return Err(illegal(inst));
        }
        let is_double = fmt_bits == 0x1;
        let fmt = if is_double { F64 } else { F32 };
        let rm = self.rounding(inst)?;

        // The point of a fused multiply-add is that the product is *not* rounded before
        // the addition: a*b+c is computed once and rounded once.
        let negate_product = inst.opcode == opcodes::NMSUB || inst.opcode == opcodes::NMADD;
        let subtract_addend = inst.opcode == opcodes::MSUB || inst.opcode == opcodes::NMADD;
        let sign = if is_double { 1u64 << 63 } else { 1u64 << 31 };

        let get = |r: u64| if is_double { r } else { nan_unbox(r) as u64 };
        let mut a = get(self.fregs[inst.rs1]);
        let b = get(self.fregs[inst.rs2]);
        let mut c = get(self.fregs[rs3]);
        if negate_product {
            a ^= sign;
        }
        if subtract_addend {
            c ^= sign;
        }
        let (r, fl) = sf::fma(a, b, c, fmt, rm);
        self.csrs.raise_fflags(fl);
        self.write_freg(inst.rd, if is_double { r } else { nan_box(r as u32) });
        Ok(())
    }
}
