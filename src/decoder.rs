//! Instruction decoding.
//!
//! RISC-V has six instruction formats. Every 32-bit instruction is one of them,
//! and the format determines which fields are present and - critically - how the
//! immediate is assembled:
//!
//! ```text
//!   R  funct7 | rs2 | rs1 | funct3 | rd     | opcode    no immediate
//!   I  imm[11:0]    | rs1 | funct3 | rd     | opcode    12-bit signed
//!   S  imm[11:5]|rs2| rs1 | funct3 |imm[4:0]| opcode    12-bit signed, split
//!   B  imm[12|10:5] | ... |imm[4:1|11]      | opcode    13-bit signed, scrambled
//!   U  imm[31:12]                  | rd     | opcode    upper 20 bits
//!   J  imm[20|10:1|11|19:12]       | rd     | opcode    21-bit signed, scrambled
//! ```
//!
//! The decoded result is a struct carrying *all* the fields, so nothing downstream
//! has to go back to the raw word. The decoder is a pure function: raw word in,
//! resolved instruction out, no CPU state.

use crate::types::sign_extend;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum Format {
    R,
    I,
    S,
    B,
    U,
    J,
    /// An opcode we do not recognise; the CPU raises IllegalInstruction.
    #[default]
    Unknown,
}

/// RISC-V major opcodes (bits [6:0]), named after the spec's own opcode map.
pub mod opcodes {
    pub const LOAD: u32 = 0x03;
    pub const LOAD_FP: u32 = 0x07;
    pub const MISC_MEM: u32 = 0x0f;
    pub const OP_IMM: u32 = 0x13;
    pub const AUIPC: u32 = 0x17;
    pub const OP_IMM_32: u32 = 0x1b;
    pub const STORE: u32 = 0x23;
    pub const STORE_FP: u32 = 0x27;
    pub const AMO: u32 = 0x2f;
    pub const OP: u32 = 0x33;
    pub const LUI: u32 = 0x37;
    pub const OP_32: u32 = 0x3b;
    pub const MADD: u32 = 0x43;
    pub const MSUB: u32 = 0x47;
    pub const NMSUB: u32 = 0x4b;
    pub const NMADD: u32 = 0x4f;
    pub const OP_FP: u32 = 0x53;
    pub const BRANCH: u32 = 0x63;
    pub const JALR: u32 = 0x67;
    pub const JAL: u32 = 0x6f;
    pub const SYSTEM: u32 = 0x73;
}

#[derive(Clone, Copy, Debug, Default)]
pub struct DecodedInst {
    /// The 32-bit instruction that is executed. For a compressed instruction this
    /// is its *expansion*, so every execute path works on one uniform encoding.
    pub raw: u32,
    /// What was actually in memory: the same as `raw` for a normal instruction, or
    /// the 16-bit halfword for a compressed one. This is what belongs in mtval on
    /// an illegal-instruction trap.
    pub encoded: u32,
    /// 2 for a compressed instruction, 4 otherwise. The PC advances by this.
    pub length: u64,
    pub opcode: u32,
    pub rd: usize,
    pub rs1: usize,
    pub rs2: usize,
    pub funct3: u32,
    pub funct7: u32,
    pub imm: i64,
    pub fmt: Format,
}

impl DecodedInst {
    /// RV64 shifts use a 6-bit shift amount, which steals a bit from what would be
    /// funct7 on RV32; the remaining 6 bits (inst[31:26]) select logical vs
    /// arithmetic, so shifts dispatch on funct6. Getting this wrong makes SRAI
    /// decode as SRLI.
    #[inline(always)]
    pub fn funct6(&self) -> u32 {
        (self.raw >> 26) & 0x3f
    }
    #[inline(always)]
    pub fn shamt6(&self) -> u32 {
        (self.raw >> 20) & 0x3f
    }
    #[inline(always)]
    pub fn shamt5(&self) -> u32 {
        (self.raw >> 20) & 0x1f
    }
}

// The B and J immediates look absurdly scrambled. The encoding is chosen so that
// every immediate bit sits in the same physical instruction bit across as many
// formats as possible, which lets a hardware decoder wire the bits straight
// through. It costs a few lines of shifting here. B and J immediates encode a
// multiple of two - bit 0 is always zero and is not stored.

fn imm_i(raw: u32) -> i64 {
    sign_extend(((raw >> 20) & 0xfff) as u64, 12)
}

fn imm_s(raw: u32) -> i64 {
    let bits = (((raw >> 25) & 0x7f) as u64) << 5 | ((raw >> 7) & 0x1f) as u64;
    sign_extend(bits, 12)
}

fn imm_b(raw: u32) -> i64 {
    let bits = (((raw >> 31) & 0x1) as u64) << 12
        | (((raw >> 7) & 0x1) as u64) << 11
        | (((raw >> 25) & 0x3f) as u64) << 5
        | (((raw >> 8) & 0xf) as u64) << 1;
    sign_extend(bits, 13)
}

fn imm_u(raw: u32) -> i64 {
    // The upper 20 bits, already in place, sign-extended from bit 31 - so LUI with
    // a large immediate produces a negative 64-bit value, as the spec requires.
    (raw & 0xffff_f000) as i32 as i64
}

fn imm_j(raw: u32) -> i64 {
    let bits = (((raw >> 31) & 0x1) as u64) << 20
        | (((raw >> 12) & 0xff) as u64) << 12
        | (((raw >> 20) & 0x1) as u64) << 11
        | (((raw >> 21) & 0x3ff) as u64) << 1;
    sign_extend(bits, 21)
}

pub fn format_for_opcode(opcode: u32) -> Format {
    use opcodes::*;
    match opcode {
        LOAD | LOAD_FP | MISC_MEM | OP_IMM | OP_IMM_32 | JALR | SYSTEM => Format::I,
        STORE | STORE_FP => Format::S,
        BRANCH => Format::B,
        AUIPC | LUI => Format::U,
        JAL => Format::J,
        // The floating-point operations reuse the R-type field layout. The fused
        // multiply-adds add a fourth register in what would be funct7's top five
        // bits (R4-type), which the CPU reads out of `raw` directly.
        OP | OP_32 | AMO | OP_FP | MADD | MSUB | NMSUB | NMADD => Format::R,
        _ => Format::Unknown,
    }
}

/// Decode a 32-bit instruction word. Never fails: an unrecognised opcode yields
/// `Format::Unknown` and the CPU turns that into an IllegalInstruction trap.
pub fn decode(raw: u32) -> DecodedInst {
    let opcode = raw & 0x7f;
    let fmt = format_for_opcode(opcode);
    let imm = match fmt {
        Format::I => imm_i(raw),
        Format::S => imm_s(raw),
        Format::B => imm_b(raw),
        Format::U => imm_u(raw),
        Format::J => imm_j(raw),
        Format::R | Format::Unknown => 0,
    };
    DecodedInst {
        raw,
        encoded: raw,
        length: 4,
        opcode,
        rd: ((raw >> 7) & 0x1f) as usize,
        rs1: ((raw >> 15) & 0x1f) as usize,
        rs2: ((raw >> 20) & 0x1f) as usize,
        funct3: (raw >> 12) & 0x07,
        funct7: (raw >> 25) & 0x7f,
        imm,
        fmt,
    }
}

// ---------------------------------------------------------------------------
// The C extension: expanding 16 bits into 32.
//
// Every compressed instruction is *defined by the spec* as equivalent to exactly
// one 32-bit base instruction, so the whole extension is a translation from 16
// bits to 32 with the existing decoder and every execute path untouched. A
// parallel 16-bit execute path would be a second implementation of instructions
// that already work.
//
// Two things make it fiddly: the registers are three bits wide (x8..x15) and the
// immediates are scattered across whatever corners of the word were free. Every
// encoding below is written out with the spec's own bit-range notation.
// ---------------------------------------------------------------------------

fn creg(three_bits: u32) -> u32 {
    three_bits + 8
}

fn enc_r(opcode: u32, rd: u32, funct3: u32, rs1: u32, rs2: u32, funct7: u32) -> u32 {
    (funct7 << 25) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) | (rd << 7) | opcode
}
fn enc_i(opcode: u32, rd: u32, funct3: u32, rs1: u32, imm: u32) -> u32 {
    ((imm & 0xfff) << 20) | (rs1 << 15) | (funct3 << 12) | (rd << 7) | opcode
}
fn enc_s(opcode: u32, funct3: u32, rs1: u32, rs2: u32, imm: u32) -> u32 {
    ((imm & 0xfe0) << 20) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) | ((imm & 0x1f) << 7) | opcode
}
fn enc_b(opcode: u32, funct3: u32, rs1: u32, rs2: u32, imm: u32) -> u32 {
    ((imm & 0x1000) << 19)
        | ((imm & 0x7e0) << 20)
        | (rs2 << 20)
        | (rs1 << 15)
        | (funct3 << 12)
        | ((imm & 0x1e) << 7)
        | ((imm & 0x800) >> 4)
        | opcode
}
fn enc_u(opcode: u32, rd: u32, imm: u32) -> u32 {
    (imm & 0xffff_f000) | (rd << 7) | opcode
}
fn enc_j(opcode: u32, rd: u32, imm: u32) -> u32 {
    ((imm & 0x10_0000) << 11) | ((imm & 0x7fe) << 20) | ((imm & 0x800) << 9) | (imm & 0xff000) | (rd << 7) | opcode
}

/// Sign-extend the low `bits` of `value` (32-bit result).
fn sx(value: u32, bits: u32) -> u32 {
    let sign = 1u32 << (bits - 1);
    (value ^ sign).wrapping_sub(sign)
}

fn bits(h: u16, hi: u32, lo: u32) -> u32 {
    ((h as u32) >> lo) & ((1u32 << (hi - lo + 1)) - 1)
}

/// Expand a compressed instruction to its 32-bit equivalent, or 0 for an encoding
/// that is not a valid compressed instruction. Zero is permanently illegal in
/// RISC-V - deliberately, so that a jump into zeroed memory faults immediately -
/// so the caller needs no separate error channel.
pub fn decompress(h: u16) -> u32 {
    use opcodes::*;
    if h == 0 {
        return 0;
    }

    let op = (h & 0x3) as u32;
    let funct3 = bits(h, 15, 13);

    let rd_rs1 = bits(h, 11, 7); // full 5-bit field
    let rs2 = bits(h, 6, 2); // full 5-bit field
    let rd_p = creg(bits(h, 4, 2)); // rd' / rs2'
    let rs1_p = creg(bits(h, 9, 7)); // rs1' / rd'

    match op {
        // --- Quadrant 0 ----------------------------------------------------------
        0 => match funct3 {
            0 => {
                // C.ADDI4SPN: addi rd', x2, nzuimm. A zero immediate is reserved.
                let imm = (bits(h, 10, 7) << 6) | (bits(h, 12, 11) << 4) | (bits(h, 5, 5) << 3) | (bits(h, 6, 6) << 2);
                if imm == 0 {
                    return 0;
                }
                enc_i(OP_IMM, rd_p, 0x0, 2, imm)
            }
            2 => {
                // C.LW: offset[5:3]=inst[12:10], [2]=inst[6], [6]=inst[5]
                let imm = (bits(h, 5, 5) << 6) | (bits(h, 12, 10) << 3) | (bits(h, 6, 6) << 2);
                enc_i(LOAD, rd_p, 0x2, rs1_p, imm)
            }
            1 => {
                // C.FLD: same immediate layout as C.LD.
                let imm = (bits(h, 6, 5) << 6) | (bits(h, 12, 10) << 3);
                enc_i(LOAD_FP, rd_p, 0x3, rs1_p, imm)
            }
            3 => {
                // C.LD: offset[5:3]=inst[12:10], [7:6]=inst[6:5]
                let imm = (bits(h, 6, 5) << 6) | (bits(h, 12, 10) << 3);
                enc_i(LOAD, rd_p, 0x3, rs1_p, imm)
            }
            5 => {
                let imm = (bits(h, 6, 5) << 6) | (bits(h, 12, 10) << 3);
                enc_s(STORE_FP, 0x3, rs1_p, rd_p, imm) // C.FSD
            }
            6 => {
                let imm = (bits(h, 5, 5) << 6) | (bits(h, 12, 10) << 3) | (bits(h, 6, 6) << 2);
                enc_s(STORE, 0x2, rs1_p, rd_p, imm) // C.SW
            }
            7 => {
                let imm = (bits(h, 6, 5) << 6) | (bits(h, 12, 10) << 3);
                enc_s(STORE, 0x3, rs1_p, rd_p, imm) // C.SD
            }
            _ => 0, // funct3 4 is reserved
        },

        // --- Quadrant 1 ----------------------------------------------------------
        1 => match funct3 {
            0 => {
                // C.ADDI: rd == 0 is C.NOP, which is a genuine no-op either way.
                let imm = sx((bits(h, 12, 12) << 5) | rs2, 6);
                enc_i(OP_IMM, rd_rs1, 0x0, rd_rs1, imm)
            }
            1 => {
                // C.ADDIW: rd == 0 is reserved.
                if rd_rs1 == 0 {
                    return 0;
                }
                let imm = sx((bits(h, 12, 12) << 5) | rs2, 6);
                enc_i(OP_IMM_32, rd_rs1, 0x0, rd_rs1, imm)
            }
            2 => {
                let imm = sx((bits(h, 12, 12) << 5) | rs2, 6);
                enc_i(OP_IMM, rd_rs1, 0x0, 0, imm) // C.LI
            }
            3 => {
                if rd_rs1 == 2 {
                    // C.ADDI16SP: nzimm[9]=inst[12], [4]=inst[6], [6]=inst[5],
                    // [8:7]=inst[4:3], [5]=inst[2]
                    let imm = sx(
                        (bits(h, 12, 12) << 9)
                            | (bits(h, 4, 3) << 7)
                            | (bits(h, 5, 5) << 6)
                            | (bits(h, 2, 2) << 5)
                            | (bits(h, 6, 6) << 4),
                        10,
                    );
                    if imm == 0 {
                        return 0;
                    }
                    return enc_i(OP_IMM, 2, 0x0, 2, imm);
                }
                // C.LUI: rd must not be x0 or x2, and the immediate must not be zero.
                if rd_rs1 == 0 {
                    return 0;
                }
                let imm = sx((bits(h, 12, 12) << 5) | rs2, 6);
                if imm == 0 {
                    return 0;
                }
                enc_u(LUI, rd_rs1, imm << 12)
            }
            4 => {
                // MISC-ALU. inst[11:10] selects the group.
                let group = bits(h, 11, 10);
                if group == 0 || group == 1 {
                    // C.SRLI / C.SRAI: RV64 allows the full 6-bit shamt.
                    let shamt = (bits(h, 12, 12) << 5) | rs2;
                    let funct6 = if group == 0 { 0x00 } else { 0x10 };
                    return (funct6 << 26) | (shamt << 20) | (rs1_p << 15) | (0x5 << 12) | (rs1_p << 7) | OP_IMM;
                }
                if group == 2 {
                    let imm = sx((bits(h, 12, 12) << 5) | rs2, 6);
                    return enc_i(OP_IMM, rs1_p, 0x7, rs1_p, imm); // C.ANDI
                }
                // group 3: register-register ALU. inst[12] picks the *W forms.
                let sel = bits(h, 6, 5);
                if bits(h, 12, 12) == 0 {
                    match sel {
                        0 => enc_r(OP, rs1_p, 0x0, rs1_p, rd_p, 0x20), // C.SUB
                        1 => enc_r(OP, rs1_p, 0x4, rs1_p, rd_p, 0x00), // C.XOR
                        2 => enc_r(OP, rs1_p, 0x6, rs1_p, rd_p, 0x00), // C.OR
                        _ => enc_r(OP, rs1_p, 0x7, rs1_p, rd_p, 0x00), // C.AND
                    }
                } else {
                    match sel {
                        0 => enc_r(OP_32, rs1_p, 0x0, rs1_p, rd_p, 0x20), // C.SUBW
                        1 => enc_r(OP_32, rs1_p, 0x0, rs1_p, rd_p, 0x00), // C.ADDW
                        _ => 0,
                    }
                }
            }
            5 => {
                // C.J: offset[11]=inst[12], [4]=inst[11], [9:8]=inst[10:9], [10]=inst[8],
                // [6]=inst[7], [7]=inst[6], [3:1]=inst[5:3], [5]=inst[2]
                let imm = sx(
                    (bits(h, 12, 12) << 11)
                        | (bits(h, 8, 8) << 10)
                        | (bits(h, 10, 9) << 8)
                        | (bits(h, 6, 6) << 7)
                        | (bits(h, 7, 7) << 6)
                        | (bits(h, 2, 2) << 5)
                        | (bits(h, 11, 11) << 4)
                        | (bits(h, 5, 3) << 1),
                    12,
                );
                enc_j(JAL, 0, imm)
            }
            6 | 7 => {
                // C.BEQZ / C.BNEZ: offset[8]=inst[12], [4:3]=inst[11:10], [7:6]=inst[6:5],
                // [2:1]=inst[4:3], [5]=inst[2]
                let imm = sx(
                    (bits(h, 12, 12) << 8)
                        | (bits(h, 6, 5) << 6)
                        | (bits(h, 2, 2) << 5)
                        | (bits(h, 11, 10) << 3)
                        | (bits(h, 4, 3) << 1),
                    9,
                );
                enc_b(BRANCH, if funct3 == 6 { 0x0 } else { 0x1 }, rs1_p, 0, imm)
            }
            _ => 0,
        },

        // --- Quadrant 2 ----------------------------------------------------------
        2 => match funct3 {
            0 => {
                // C.SLLI
                if rd_rs1 == 0 {
                    return 0;
                }
                let shamt = (bits(h, 12, 12) << 5) | rs2;
                (shamt << 20) | (rd_rs1 << 15) | (0x1 << 12) | (rd_rs1 << 7) | OP_IMM
            }
            2 => {
                // C.LWSP: offset[5]=inst[12], [4:2]=inst[6:4], [7:6]=inst[3:2]
                if rd_rs1 == 0 {
                    return 0;
                }
                let imm = (bits(h, 3, 2) << 6) | (bits(h, 12, 12) << 5) | (bits(h, 6, 4) << 2);
                enc_i(LOAD, rd_rs1, 0x2, 2, imm)
            }
            1 => {
                // C.FLDSP: no reserved encoding - f0 is an ordinary register.
                let imm = (bits(h, 4, 2) << 6) | (bits(h, 12, 12) << 5) | (bits(h, 6, 5) << 3);
                enc_i(LOAD_FP, rd_rs1, 0x3, 2, imm)
            }
            3 => {
                // C.LDSP: offset[5]=inst[12], [4:3]=inst[6:5], [8:6]=inst[4:2]
                if rd_rs1 == 0 {
                    return 0;
                }
                let imm = (bits(h, 4, 2) << 6) | (bits(h, 12, 12) << 5) | (bits(h, 6, 5) << 3);
                enc_i(LOAD, rd_rs1, 0x3, 2, imm)
            }
            5 => {
                let imm = (bits(h, 9, 7) << 6) | (bits(h, 12, 10) << 3);
                enc_s(STORE_FP, 0x3, 2, rs2, imm) // C.FSDSP
            }
            4 => {
                if bits(h, 12, 12) == 0 {
                    // C.JR: jalr x0, 0(rs1)   -   C.MV: add rd, x0, rs2
                    if rs2 == 0 {
                        if rd_rs1 == 0 {
                            return 0; // reserved
                        }
                        return enc_i(JALR, 0, 0x0, rd_rs1, 0);
                    }
                    return enc_r(OP, rd_rs1, 0x0, 0, rs2, 0x00);
                }
                // C.EBREAK / C.JALR / C.ADD
                if rs2 == 0 {
                    if rd_rs1 == 0 {
                        return 0x0010_0073; // C.EBREAK
                    }
                    return enc_i(JALR, 1, 0x0, rd_rs1, 0); // C.JALR
                }
                enc_r(OP, rd_rs1, 0x0, rd_rs1, rs2, 0x00) // C.ADD
            }
            6 => {
                // C.SWSP: offset[5:2]=inst[12:9], [7:6]=inst[8:7]
                let imm = (bits(h, 8, 7) << 6) | (bits(h, 12, 9) << 2);
                enc_s(STORE, 0x2, 2, rs2, imm)
            }
            7 => {
                // C.SDSP: offset[5:3]=inst[12:10], [8:6]=inst[9:7]
                let imm = (bits(h, 9, 7) << 6) | (bits(h, 12, 10) << 3);
                enc_s(STORE, 0x3, 2, rs2, imm)
            }
            _ => 0,
        },

        // op == 3 means this was not a compressed instruction at all; the caller
        // is responsible for checking that before calling here.
        _ => 0,
    }
}

/// RISC-V marks instruction length in the low bits: bits [1:0] == 11 means 32 bits
/// or longer, anything else is a 16-bit compressed instruction.
#[inline(always)]
pub fn is_32bit_instruction(half: u16) -> bool {
    (half & 0x3) == 0x3
}

/// Decode an instruction that may be compressed, given the halfword at the PC and
/// (for a 32-bit instruction) the one after it.
pub fn decode16(low: u16, high: u16) -> DecodedInst {
    if is_32bit_instruction(low) {
        let raw = ((high as u32) << 16) | low as u32;
        let mut inst = decode(raw);
        inst.encoded = raw;
        inst.length = 4;
        return inst;
    }
    // A compressed instruction decodes as its expansion, so everything downstream
    // is unchanged - but `encoded` and `length` remember the program contained 16
    // bits, not 32.
    let mut inst = decode(decompress(low));
    inst.encoded = low as u32;
    inst.length = 2;
    inst
}

/// A short human-readable name for an instruction, for the tracer.
pub fn mnemonic(inst: &DecodedInst) -> &'static str {
    use opcodes::*;
    match inst.opcode {
        LUI => "lui",
        AUIPC => "auipc",
        JAL => "jal",
        JALR => "jalr",
        BRANCH => match inst.funct3 {
            0x0 => "beq",
            0x1 => "bne",
            0x4 => "blt",
            0x5 => "bge",
            0x6 => "bltu",
            0x7 => "bgeu",
            _ => "unimp",
        },
        LOAD => match inst.funct3 {
            0x0 => "lb",
            0x1 => "lh",
            0x2 => "lw",
            0x3 => "ld",
            0x4 => "lbu",
            0x5 => "lhu",
            0x6 => "lwu",
            _ => "unimp",
        },
        STORE => match inst.funct3 {
            0x0 => "sb",
            0x1 => "sh",
            0x2 => "sw",
            0x3 => "sd",
            _ => "unimp",
        },
        OP_IMM => match inst.funct3 {
            0x0 => "addi",
            0x1 => "slli",
            0x2 => "slti",
            0x3 => "sltiu",
            0x4 => "xori",
            0x5 => {
                if inst.funct6() == 0x10 {
                    "srai"
                } else {
                    "srli"
                }
            }
            0x6 => "ori",
            0x7 => "andi",
            _ => "unimp",
        },
        OP_IMM_32 => match inst.funct3 {
            0x0 => "addiw",
            0x1 => "slliw",
            0x5 => {
                if inst.funct7 == 0x20 {
                    "sraiw"
                } else {
                    "srliw"
                }
            }
            _ => "unimp",
        },
        OP => {
            if inst.funct7 == 0x01 {
                return match inst.funct3 {
                    0x0 => "mul",
                    0x1 => "mulh",
                    0x2 => "mulhsu",
                    0x3 => "mulhu",
                    0x4 => "div",
                    0x5 => "divu",
                    0x6 => "rem",
                    0x7 => "remu",
                    _ => "unimp",
                };
            }
            match inst.funct3 {
                0x0 => {
                    if inst.funct7 == 0x20 {
                        "sub"
                    } else {
                        "add"
                    }
                }
                0x1 => "sll",
                0x2 => "slt",
                0x3 => "sltu",
                0x4 => "xor",
                0x5 => {
                    if inst.funct7 == 0x20 {
                        "sra"
                    } else {
                        "srl"
                    }
                }
                0x6 => "or",
                0x7 => "and",
                _ => "unimp",
            }
        }
        OP_32 => {
            if inst.funct7 == 0x01 {
                return match inst.funct3 {
                    0x0 => "mulw",
                    0x4 => "divw",
                    0x5 => "divuw",
                    0x6 => "remw",
                    0x7 => "remuw",
                    _ => "unimp",
                };
            }
            match inst.funct3 {
                0x0 => {
                    if inst.funct7 == 0x20 {
                        "subw"
                    } else {
                        "addw"
                    }
                }
                0x1 => "sllw",
                0x5 => {
                    if inst.funct7 == 0x20 {
                        "sraw"
                    } else {
                        "srlw"
                    }
                }
                _ => "unimp",
            }
        }
        MISC_MEM => {
            if inst.funct3 == 0x1 {
                "fence.i"
            } else {
                "fence"
            }
        }
        AMO => {
            let w = inst.funct3 == 0x2;
            let pick = |a: &'static str, b: &'static str| if w { a } else { b };
            match (inst.funct7 >> 2) & 0x1f {
                0x00 => pick("amoadd.w", "amoadd.d"),
                0x01 => pick("amoswap.w", "amoswap.d"),
                0x02 => pick("lr.w", "lr.d"),
                0x03 => pick("sc.w", "sc.d"),
                0x04 => pick("amoxor.w", "amoxor.d"),
                0x08 => pick("amoor.w", "amoor.d"),
                0x0c => pick("amoand.w", "amoand.d"),
                0x10 => pick("amomin.w", "amomin.d"),
                0x14 => pick("amomax.w", "amomax.d"),
                0x18 => pick("amominu.w", "amominu.d"),
                0x1c => pick("amomaxu.w", "amomaxu.d"),
                _ => "unimp",
            }
        }
        SYSTEM => match inst.funct3 {
            0x0 => match inst.imm & 0xfff {
                0x000 => "ecall",
                0x001 => "ebreak",
                0x102 => "sret",
                0x105 => "wfi",
                0x302 => "mret",
                _ => {
                    if inst.funct7 == 0x09 {
                        "sfence.vma"
                    } else {
                        "unimp"
                    }
                }
            },
            0x1 => "csrrw",
            0x2 => "csrrs",
            0x3 => "csrrc",
            0x5 => "csrrwi",
            0x6 => "csrrsi",
            0x7 => "csrrci",
            _ => "unimp",
        },
        _ => "unimp",
    }
}
