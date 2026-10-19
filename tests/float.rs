//! The F and D extensions.
//!
//! riscv-tests covers the arithmetic thoroughly, so this suite concentrates on the parts that
//! are easy to get subtly wrong and that show up as a wrong number rather than a crash: NaN
//! boxing, the FS enable, and the places where RISC-V's semantics differ from the host's.

mod common;

use common::*;
use riscv_emu::cpu::fp::*;
use riscv_emu::csr::csr;
use riscv_emu::decoder::opcodes;
use riscv_emu::device::AccessType;
use riscv_emu::softfloat::F64;
use riscv_emu::trap::Exception;
use riscv_emu::types::DRAM_BASE;

const CANONICAL_NAN_F64: u64 = 0x7ff8_0000_0000_0000;

// Encoders, written independently of src/decoder.rs for the usual reason: a test that built its
// inputs with the emulator's own decoder would agree with itself however wrong both halves were.
fn fp_op(funct7: u32, rs2: u32, rs1: u32, rm: u32, rd: u32) -> u32 {
    (funct7 << 25) | (rs2 << 20) | (rs1 << 15) | (rm << 12) | (rd << 7) | opcodes::OP_FP
}
fn fadd_d(rd: u32, a: u32, b: u32) -> u32 {
    fp_op(0x01, b, a, 0x7, rd)
}
fn fadd_s(rd: u32, a: u32, b: u32) -> u32 {
    fp_op(0x00, b, a, 0x7, rd)
}
fn fmul_d(rd: u32, a: u32, b: u32) -> u32 {
    fp_op(0x09, b, a, 0x7, rd)
}
fn fdiv_d(rd: u32, a: u32, b: u32) -> u32 {
    fp_op(0x0d, b, a, 0x7, rd)
}
fn fsqrt_d(rd: u32, a: u32) -> u32 {
    fp_op(0x2d, 0, a, 0x7, rd)
}
fn fsgnj_d(rd: u32, a: u32, b: u32) -> u32 {
    fp_op(0x11, b, a, 0x0, rd)
}
fn fsgnjn_d(rd: u32, a: u32, b: u32) -> u32 {
    fp_op(0x11, b, a, 0x1, rd)
}
fn fsgnjx_d(rd: u32, a: u32, b: u32) -> u32 {
    fp_op(0x11, b, a, 0x2, rd)
}
fn fmin_d(rd: u32, a: u32, b: u32) -> u32 {
    fp_op(0x15, b, a, 0x0, rd)
}
fn fmax_d(rd: u32, a: u32, b: u32) -> u32 {
    fp_op(0x15, b, a, 0x1, rd)
}
fn feq_d(rd: u32, a: u32, b: u32) -> u32 {
    fp_op(0x51, b, a, 0x2, rd)
}
fn flt_d(rd: u32, a: u32, b: u32) -> u32 {
    fp_op(0x51, b, a, 0x1, rd)
}
fn fle_d(rd: u32, a: u32, b: u32) -> u32 {
    fp_op(0x51, b, a, 0x0, rd)
}
fn fclass_d(rd: u32, a: u32) -> u32 {
    fp_op(0x71, 0, a, 0x1, rd)
}
fn fmv_x_d(rd: u32, a: u32) -> u32 {
    fp_op(0x71, 0, a, 0x0, rd)
}
fn fmv_d_x(rd: u32, a: u32) -> u32 {
    fp_op(0x79, 0, a, 0x0, rd)
}
fn fmv_x_w(rd: u32, a: u32) -> u32 {
    fp_op(0x70, 0, a, 0x0, rd)
}
fn fmv_w_x(rd: u32, a: u32) -> u32 {
    fp_op(0x78, 0, a, 0x0, rd)
}
fn fcvt_w_d(rd: u32, a: u32, rm: u32) -> u32 {
    fp_op(0x61, 0, a, rm, rd)
}
fn fcvt_d_w(rd: u32, a: u32) -> u32 {
    fp_op(0x69, 0, a, 0x7, rd)
}
fn fcvt_s_d(rd: u32, a: u32) -> u32 {
    fp_op(0x20, 1, a, 0x7, rd)
}
fn fcvt_d_s(rd: u32, a: u32) -> u32 {
    fp_op(0x21, 0, a, 0x7, rd)
}
fn fld(rd: u32, rs1: u32, imm: i32) -> u32 {
    i_type(opcodes::LOAD_FP, rd, 0x3, rs1, imm)
}
fn flw(rd: u32, rs1: u32, imm: i32) -> u32 {
    i_type(opcodes::LOAD_FP, rd, 0x2, rs1, imm)
}
fn fsd(rs1: u32, rs2: u32, imm: i32) -> u32 {
    s_type(opcodes::STORE_FP, 0x3, rs1, rs2, imm)
}
fn fsw(rs1: u32, rs2: u32, imm: i32) -> u32 {
    s_type(opcodes::STORE_FP, 0x2, rs1, rs2, imm)
}

/// A machine with the FPU switched on, which is what a kernel does before handing the unit to a
/// process.
fn fpu_machine(program: &[u32]) -> Machine {
    let mut m = Machine::new(program);
    let status = m.cpu.csrs.mstatus() | csr::MSTATUS_FS_INITIAL;
    m.cpu.csrs.write(csr::MSTATUS, status);
    m
}

fn f64_bits(d: f64) -> u64 {
    d.to_bits()
}
fn f32_bits(f: f32) -> u32 {
    f.to_bits()
}
fn fflags(m: &Machine) -> u64 {
    m.cpu.csrs.read(csr::FFLAGS)
}

// --- the FS enable ----------------------------------------------------------

#[test]
fn fp_traps_while_fs_is_off() {
    // mstatus.FS comes out of reset as Off, and while it is Off every floating-point instruction
    // is illegal. This is what a kernel with no FPU support relies on to stop user code from
    // quietly corrupting a register file nobody is saving across context switches.
    let mut m = Machine::new(&[fadd_d(0, 1, 2)]);
    assert_eq!(m.cpu.csrs.mstatus() & csr::MSTATUS_FS, csr::MSTATUS_FS_OFF);
    assert_eq!(m.cpu.step().unwrap_err().cause, Exception::IllegalInstruction);

    // fcsr is gated by the same bit - it is part of the state being protected.
    let csrrs_fcsr = i_type(opcodes::SYSTEM, 1, 0x2, 0, csr::FCSR as i32);
    let mut m2 = Machine::new(&[csrrs_fcsr]);
    assert_eq!(m2.cpu.step().unwrap_err().cause, Exception::IllegalInstruction);
}

#[test]
fn writing_an_f_register_marks_the_unit_dirty() {
    // A kernel saves the 32 floating-point registers only when FS says Dirty. An instruction that
    // writes one and does not make that transition is a process silently inheriting another's
    // floating-point state.
    let mut m = fpu_machine(&[fmv_d_x(1, 0)]);
    assert_eq!(m.cpu.csrs.mstatus() & csr::MSTATUS_FS, csr::MSTATUS_FS_INITIAL);
    assert!(m.cpu.step().is_ok());
    assert_eq!(m.cpu.csrs.mstatus() & csr::MSTATUS_FS, csr::MSTATUS_FS_DIRTY);
    // And SD, the read-only summary bit, follows it.
    assert_ne!(m.cpu.csrs.mstatus() & csr::MSTATUS_SD, 0);
}

// --- NaN boxing -------------------------------------------------------------

#[test]
fn a_single_is_nan_boxed_in_the_register() {
    // The two precisions share one register file, so something has to happen when a program writes
    // a float and reads it as a double. The spec's answer is NaN boxing: a valid single has all of
    // its upper 32 bits set, which is the encoding of a quiet NaN - so reading it as a double
    // yields a NaN rather than a plausible-looking wrong number.
    let mut m = fpu_machine(&[addi(1, 0, 0x40), fmv_w_x(0, 1)]);
    assert!(m.run(2).is_none());
    assert_eq!(m.cpu.fregs[0] >> 32, 0xffff_ffff);
    assert_eq!(m.cpu.fregs[0] & 0xffff_ffff, 0x40);

    // Read that register as a double and it is a NaN, by construction.
    assert!(f64::from_bits(m.cpu.fregs[0]).is_nan());
}

#[test]
fn an_unboxed_register_reads_as_canonical_nan() {
    // A 64-bit value whose upper half is not all ones is not a valid single. Every
    // single-precision *arithmetic* instruction must see the canonical NaN rather than the bits
    // that happen to be there.
    assert_eq!(nan_unbox(0x7fff_ffff_1111_1111), CANONICAL_NAN_F32);
    assert_eq!(nan_unbox(nan_box(0x40490fdb)), 0x40490fdb);

    // 1.0 as a double, read as a single: not boxed, so NaN.
    assert_eq!(nan_unbox(f64_bits(1.0)), CANONICAL_NAN_F32);
}

#[test]
fn fmv_x_w_does_not_unbox() {
    // The one exception, and it matters: FMV.X.W is a *raw* bit move. It takes bits 31:0 as they are
    // and sign-extends them, applying no boxing rule at all - unboxing here would replace the bits
    // software asked to see with a canonical NaN, defeating the point of the instruction.
    let mut m = fpu_machine(&[fmv_x_w(1, 0)]);
    m.cpu.fregs[0] = 0x7fff_ffff_1111_1111; // deliberately not boxed
    assert!(m.cpu.step().is_ok());
    assert_eq!(m.reg(1), 0x1111_1111);

    // And the sign extension is real: a low half with bit 31 set fills the top.
    let mut m2 = fpu_machine(&[fmv_x_w(1, 0)]);
    m2.cpu.fregs[0] = 0x0000_0000_8000_0001;
    assert!(m2.cpu.step().is_ok());
    assert_eq!(m2.reg(1), 0xffff_ffff_8000_0001);
}

#[test]
fn fsw_stores_only_the_low_half() {
    // FSW writes the 32-bit single; the NaN box is a property of the register, not of the value in
    // memory, so it is not stored. FSD writes all 64 bits, because for a double every one of them is
    // part of the number.
    let mut m = fpu_machine(&[fsw(2, 0, 0), fsd(2, 0, 8)]);
    m.cpu.fregs[0] = nan_box(0xdead_beef);
    m.cpu.regs[2] = DRAM_BASE + 0x100; // clear of the program itself
    assert!(m.run(2).is_none());

    assert_eq!(m.bus.load(DRAM_BASE + 0x100, 4, AccessType::Load).unwrap(), 0xdead_beef);
    assert_eq!(m.bus.load(DRAM_BASE + 0x108, 8, AccessType::Load).unwrap(), nan_box(0xdead_beef));
}

#[test]
fn flw_boxes_what_it_loads() {
    let mut m = fpu_machine(&[flw(0, 2, 0)]);
    m.cpu.regs[2] = DRAM_BASE + 0x100;
    m.bus.store(DRAM_BASE + 0x100, 4, 0x40490fdb).unwrap();
    assert!(m.cpu.step().is_ok());
    assert_eq!(m.cpu.fregs[0], nan_box(0x40490fdb));
}

#[test]
fn fld_loads_all_sixty_four_bits() {
    // The other half of the pair: FLD moves a double, so there is no box to apply - every bit of the
    // register is part of the number.
    let mut m = fpu_machine(&[fld(0, 2, 0)]);
    m.cpu.regs[2] = DRAM_BASE + 0x100;
    m.bus.store(DRAM_BASE + 0x100, 8, f64_bits(-2.75)).unwrap();
    assert!(m.cpu.step().is_ok());
    assert_eq!(m.cpu.fregs[0], f64_bits(-2.75));

    // A double loaded this way is *not* a valid single, which is exactly what NaN boxing is for:
    // reading it as one yields the canonical NaN rather than the low half of a number that means
    // something else.
    assert_eq!(nan_unbox(m.cpu.fregs[0]), CANONICAL_NAN_F32);
}

// --- arithmetic -------------------------------------------------------------

#[test]
fn basic_double_arithmetic() {
    let mut m = fpu_machine(&[fadd_d(3, 1, 2), fmul_d(4, 1, 2), fdiv_d(5, 1, 2), fsqrt_d(6, 1)]);
    m.cpu.fregs[1] = f64_bits(9.0);
    m.cpu.fregs[2] = f64_bits(2.0);
    assert!(m.run(4).is_none());
    assert_eq!(m.cpu.fregs[3], f64_bits(11.0));
    assert_eq!(m.cpu.fregs[4], f64_bits(18.0));
    assert_eq!(m.cpu.fregs[5], f64_bits(4.5));
    assert_eq!(m.cpu.fregs[6], f64_bits(3.0));
}

#[test]
fn divide_by_zero_sets_the_flag_and_yields_infinity() {
    // RISC-V floating-point division by zero does not trap - like integer division, it produces a
    // defined value. Unlike integer division it also records that it happened, in a sticky flag
    // software can check later.
    let mut m = fpu_machine(&[fdiv_d(3, 1, 2)]);
    m.cpu.fregs[1] = f64_bits(1.0);
    m.cpu.fregs[2] = f64_bits(0.0);
    assert!(m.cpu.step().is_ok());
    assert!(f64::from_bits(m.cpu.fregs[3]).is_infinite());
    assert_ne!(fflags(&m) & csr::FFLAG_DZ, 0);
}

#[test]
fn exception_flags_are_sticky() {
    // Nothing but an explicit write clears them. That is what lets a program run a long calculation
    // and ask afterwards whether anything went wrong, rather than checking after every operation.
    let mut m = fpu_machine(&[fdiv_d(3, 1, 2), fadd_d(4, 1, 1)]);
    m.cpu.fregs[1] = f64_bits(1.0);
    m.cpu.fregs[2] = f64_bits(0.0);
    assert!(m.run(2).is_none());
    // The clean addition did not clear the earlier division's flag.
    assert_ne!(fflags(&m) & csr::FFLAG_DZ, 0);

    m.cpu.csrs.write(csr::FFLAGS, 0);
    assert_eq!(fflags(&m), 0);
}

#[test]
fn fcsr_fflags_and_frm_are_windows_on_one_register() {
    // Three CSR addresses, one register. Software writes frm alone constantly - a routine that
    // wants round-toward-zero should not have to read-modify-write fcsr and risk clobbering the
    // accrued flags.
    let mut m = fpu_machine(&[nop()]);
    m.cpu.csrs.write(csr::FCSR, 0);
    m.cpu.csrs.write(csr::FFLAGS, csr::FFLAG_NX | csr::FFLAG_OF);
    m.cpu.csrs.write(csr::FRM, csr::FRM_RTZ as u64);

    assert_eq!(fflags(&m), csr::FFLAG_NX | csr::FFLAG_OF);
    assert_eq!(m.cpu.csrs.read(csr::FRM), csr::FRM_RTZ as u64);
    assert_eq!(
        m.cpu.csrs.read(csr::FCSR),
        ((csr::FRM_RTZ as u64) << csr::FCSR_FRM_SHIFT) | csr::FFLAG_NX | csr::FFLAG_OF
    );

    // Writing frm leaves the flags alone, which is the whole point.
    m.cpu.csrs.write(csr::FRM, csr::FRM_RUP as u64);
    assert_eq!(fflags(&m), csr::FFLAG_NX | csr::FFLAG_OF);
}

#[test]
fn rounding_mode_changes_the_result() {
    // 2.5 converted to an integer: round-to-nearest-even gives 2, toward zero gives 2, up gives 3,
    // down gives 2. The tie is what distinguishes RNE from the away-from-zero rounding most people
    // expect.
    for (rm, expect) in [(csr::FRM_RNE, 2u64), (csr::FRM_RTZ, 2), (csr::FRM_RUP, 3), (csr::FRM_RDN, 2)] {
        let mut m = fpu_machine(&[fcvt_w_d(1, 0, rm)]);
        m.cpu.fregs[0] = f64_bits(2.5);
        assert!(m.cpu.step().is_ok());
        assert_eq!(m.reg(1), expect, "rm {rm}");
    }

    // 3.5 rounds to 4 under RNE - to *even*, not always down.
    let mut m = fpu_machine(&[fcvt_w_d(1, 0, csr::FRM_RNE)]);
    m.cpu.fregs[0] = f64_bits(3.5);
    assert!(m.cpu.step().is_ok());
    assert_eq!(m.reg(1), 4);
}

#[test]
fn a_reserved_rounding_mode_is_illegal() {
    // Modes 5 and 6 do not exist. An instruction asking for one has a bug, and guessing at what it
    // meant hides that.
    let mut m = fpu_machine(&[(fadd_d(3, 1, 2) & !(0x7 << 12)) | (5 << 12)]);
    assert_eq!(m.cpu.step().unwrap_err().cause, Exception::IllegalInstruction);
}

#[test]
fn conversion_saturates_instead_of_being_undefined() {
    // C says an out-of-range float-to-integer conversion is undefined. RISC-V says it saturates to
    // the largest or smallest representable value and raises Invalid - so the result is always
    // something specific.
    let mut m = fpu_machine(&[fcvt_w_d(1, 0, csr::FRM_RTZ)]);
    m.cpu.fregs[0] = f64_bits(1e18);
    assert!(m.cpu.step().is_ok());
    assert_eq!(m.reg(1), i32::MAX as i64 as u64);
    assert_ne!(fflags(&m) & csr::FFLAG_NV, 0);

    let mut m2 = fpu_machine(&[fcvt_w_d(1, 0, csr::FRM_RTZ)]);
    m2.cpu.fregs[0] = f64_bits(-1e18);
    assert!(m2.cpu.step().is_ok());
    assert_eq!(m2.reg(1), i32::MIN as i64 as u64);

    // A NaN converts to the *maximum positive* value, not to zero. That is surprising and it is what
    // the spec says.
    let mut m3 = fpu_machine(&[fcvt_w_d(1, 0, csr::FRM_RTZ)]);
    m3.cpu.fregs[0] = CANONICAL_NAN_F64;
    assert!(m3.cpu.step().is_ok());
    assert_eq!(m3.reg(1), i32::MAX as i64 as u64);
    assert_ne!(fflags(&m3) & csr::FFLAG_NV, 0);
}

// --- sign injection ---------------------------------------------------------

#[test]
fn sign_injection_is_bit_manipulation() {
    // FSGNJ and friends never round, never raise a flag, and never treat a NaN specially - which is
    // exactly what makes them the canonical encodings of fabs, fneg and a register move.
    let mut m = fpu_machine(&[fsgnj_d(3, 1, 2), fsgnjn_d(4, 1, 2), fsgnjx_d(5, 1, 2)]);
    m.cpu.fregs[1] = f64_bits(-3.0);
    m.cpu.fregs[2] = f64_bits(1.0); // positive
    assert!(m.run(3).is_none());
    assert_eq!(m.cpu.fregs[3], f64_bits(3.0)); // sign of rs2
    assert_eq!(m.cpu.fregs[4], f64_bits(-3.0)); // inverted
    assert_eq!(m.cpu.fregs[5], f64_bits(-3.0)); // xor of the two signs
    assert_eq!(fflags(&m), 0);

    // fabs is FSGNJX with the same register twice: x ^ x = 0, so the sign is always cleared.
    let mut m2 = fpu_machine(&[fsgnjx_d(3, 1, 1)]);
    m2.cpu.fregs[1] = f64_bits(-7.5);
    assert!(m2.cpu.step().is_ok());
    assert_eq!(m2.cpu.fregs[3], f64_bits(7.5));
}

// --- min/max and comparison -------------------------------------------------

#[test]
fn fmin_fmax_nan_and_zero_rules() {
    // FMIN/FMAX are neither C's fmin/fmax nor a comparison. If one operand is NaN the result is the
    // *other* one; if both are, it is the canonical NaN.
    let mut m = fpu_machine(&[fmin_d(3, 1, 2), fmax_d(4, 1, 2)]);
    m.cpu.fregs[1] = CANONICAL_NAN_F64;
    m.cpu.fregs[2] = f64_bits(2.0);
    assert!(m.run(2).is_none());
    assert_eq!(m.cpu.fregs[3], f64_bits(2.0));
    assert_eq!(m.cpu.fregs[4], f64_bits(2.0));

    let mut m2 = fpu_machine(&[fmin_d(3, 1, 2)]);
    m2.cpu.fregs[1] = CANONICAL_NAN_F64;
    m2.cpu.fregs[2] = CANONICAL_NAN_F64;
    assert!(m2.cpu.step().is_ok());
    assert_eq!(m2.cpu.fregs[3], CANONICAL_NAN_F64);

    // -0.0 is defined to be less than +0.0 here, which no comparison reports: -0.0 == +0.0 is true.
    let mut m3 = fpu_machine(&[fmin_d(3, 1, 2), fmax_d(4, 1, 2)]);
    m3.cpu.fregs[1] = f64_bits(-0.0);
    m3.cpu.fregs[2] = f64_bits(0.0);
    assert!(m3.run(2).is_none());
    assert_eq!(m3.cpu.fregs[3], f64_bits(-0.0));
    assert_eq!(m3.cpu.fregs[4], f64_bits(0.0));
}

#[test]
fn quiet_and_ordered_comparisons_differ_on_nan() {
    // FEQ is the quiet comparison: a NaN makes it false without complaint. FLT and FLE are ordered: a
    // NaN raises Invalid. That difference is the entire reason there are two kinds.
    let mut m = fpu_machine(&[feq_d(1, 3, 4)]);
    m.cpu.fregs[3] = CANONICAL_NAN_F64;
    m.cpu.fregs[4] = f64_bits(1.0);
    assert!(m.cpu.step().is_ok());
    assert_eq!(m.reg(1), 0);
    assert_eq!(fflags(&m) & csr::FFLAG_NV, 0);

    let mut m2 = fpu_machine(&[flt_d(1, 3, 4)]);
    m2.cpu.fregs[3] = CANONICAL_NAN_F64;
    m2.cpu.fregs[4] = f64_bits(1.0);
    assert!(m2.cpu.step().is_ok());
    assert_eq!(m2.reg(1), 0);
    assert_ne!(fflags(&m2) & csr::FFLAG_NV, 0);

    // And the ordinary answers are still right.
    let mut m3 = fpu_machine(&[feq_d(1, 3, 4), flt_d(2, 3, 4), fle_d(5, 3, 4)]);
    m3.cpu.fregs[3] = f64_bits(1.0);
    m3.cpu.fregs[4] = f64_bits(2.0);
    assert!(m3.run(3).is_none());
    assert_eq!(m3.reg(1), 0);
    assert_eq!(m3.reg(2), 1);
    assert_eq!(m3.reg(5), 1);
}

// --- classification ---------------------------------------------------------

#[test]
fn fclass_distinguishes_what_comparisons_cannot() {
    for (value, expect) in [
        (f64::NEG_INFINITY, FCLASS_NEG_INF),
        (-1.0, FCLASS_NEG_NORMAL),
        (-0.0, FCLASS_NEG_ZERO),
        (0.0, FCLASS_POS_ZERO),
        (1.0, FCLASS_POS_NORMAL),
        (f64::INFINITY, FCLASS_POS_INF),
    ] {
        let mut m = fpu_machine(&[fclass_d(1, 0)]);
        m.cpu.fregs[0] = f64_bits(value);
        assert!(m.cpu.step().is_ok());
        assert_eq!(m.reg(1), expect, "{value}");
        // The software helper agrees with the instruction.
        assert_eq!(fclass(f64_bits(value), F64), expect);
    }

    // The two zeros are what make fclass necessary: -0.0 == +0.0 is true, so no comparison can tell
    // them apart.
    let mut m = fpu_machine(&[fclass_d(1, 0)]);
    m.cpu.fregs[0] = CANONICAL_NAN_F64;
    assert!(m.cpu.step().is_ok());
    assert_eq!(m.reg(1), FCLASS_QUIET_NAN);

    // A signalling NaN has the top mantissa bit clear - the one bit that separates the two kinds.
    let mut m2 = fpu_machine(&[fclass_d(1, 0)]);
    m2.cpu.fregs[0] = 0x7ff0_0000_0000_0001;
    assert!(m2.cpu.step().is_ok());
    assert_eq!(m2.reg(1), FCLASS_SIGNALLING);

    // Subnormals: an exponent of zero with a non-zero mantissa.
    let mut m3 = fpu_machine(&[fclass_d(1, 0)]);
    m3.cpu.fregs[0] = 1;
    assert!(m3.cpu.step().is_ok());
    assert_eq!(m3.reg(1), FCLASS_POS_SUBNORM);
}

// --- moves and conversions --------------------------------------------------

#[test]
fn raw_moves_between_the_register_files() {
    // FMV.D.X and FMV.X.D move bits with no conversion at all, which is how software inspects a
    // float's encoding.
    let mut m = fpu_machine(&[fmv_d_x(0, 1), fmv_x_d(2, 0)]);
    m.cpu.regs[1] = f64_bits(-2.5);
    assert!(m.run(2).is_none());
    assert_eq!(m.cpu.fregs[0], f64_bits(-2.5));
    assert_eq!(m.reg(2), f64_bits(-2.5));
    // No rounding happened, so no flag was raised.
    assert_eq!(fflags(&m), 0);
}

#[test]
fn conversions_between_the_two_precisions() {
    let mut m = fpu_machine(&[fcvt_s_d(1, 0), fcvt_d_s(2, 1)]);
    m.cpu.fregs[0] = f64_bits(1.5); // exactly representable in both
    assert!(m.run(2).is_none());
    assert_eq!(m.cpu.fregs[1], nan_box(f32_bits(1.5)));
    assert_eq!(m.cpu.fregs[2], f64_bits(1.5));

    // A double that does not fit a float loses precision, and says so.
    let mut m2 = fpu_machine(&[fcvt_s_d(1, 0)]);
    m2.cpu.fregs[0] = f64_bits(1.0 + 1e-10);
    assert!(m2.cpu.step().is_ok());
    assert_ne!(fflags(&m2) & csr::FFLAG_NX, 0);
}

#[test]
fn integer_to_float_and_back() {
    let mut m = fpu_machine(&[fcvt_d_w(0, 1), fcvt_w_d(2, 0, csr::FRM_RTZ)]);
    m.cpu.regs[1] = (-42i64) as u64;
    assert!(m.run(2).is_none());
    assert_eq!(m.cpu.fregs[0], f64_bits(-42.0));
    assert_eq!(m.reg(2), (-42i64) as u64);
}

#[test]
fn single_precision_arithmetic_stays_single() {
    // 0.1f + 0.2f in single precision is a different number from the double-precision sum. If the
    // emulator computed in double and boxed the result, this would come out with more bits of
    // precision than a real machine produces.
    let mut m = fpu_machine(&[fadd_s(3, 1, 2)]);
    m.cpu.fregs[1] = nan_box(f32_bits(0.1));
    m.cpu.fregs[2] = nan_box(f32_bits(0.2));
    assert!(m.cpu.step().is_ok());
    assert_eq!(m.cpu.fregs[3], nan_box(f32_bits(0.1f32 + 0.2f32)));
}

// --- fused multiply-add and conversions ---------------------------------------

#[test]
fn int_to_float_converts_directly_rather_than_via_double() {
    // 2^53 + 2^29 + 1, converted to a single.
    //
    // Routing through a double rounds twice, and the two roundings do not compose: the intermediate
    // lands exactly on a float tie, and round-half-to-even then breaks it the other way. The result
    // is one ULP from the architecturally required answer - the kind of error that is invisible
    // until it is not.
    let fcvt_s_l = fp_op(0x68, 2, 1, 0x7, 2); // funct7 0x68 = FCVT.S.*
    let mut m = fpu_machine(&[fcvt_s_l]);
    m.cpu.regs[1] = 0x0020_0000_2000_0001;
    assert!(m.cpu.step().is_ok());
    assert_eq!(m.cpu.fregs[2], nan_box(0x5a000001));
}

#[test]
fn reserved_conversion_encodings_are_illegal() {
    // rs2 names the source width and signedness for an integer-to-float conversion; 4..31 are
    // reserved. Accepting one and writing zero gives a guest probing for an unimplemented
    // conversion a wrong answer instead of the trap it is waiting for.
    let mut m = fpu_machine(&[fp_op(0x68, 7, 1, 0x7, 2)]);
    assert_eq!(m.cpu.step().unwrap_err().cause, Exception::IllegalInstruction);
}

#[test]
fn fma_rounds_once() {
    // The point of a fused multiply-add is that the product is not rounded before the addition:
    // a*b+c is computed once and rounded once. Computing it as two operations gives a different
    // answer in the last bit, and that difference is precisely what numerical code uses fma to
    // avoid. (1 + 2^-30)^2 = 1 + 2^-29 + 2^-60. The last term falls off the end of a double when the
    // product is rounded on its own, so the unfused answer is exactly 2^-29; the fused one keeps it
    // and gives 2^-29 + 2^-60.
    let a = 1.0 + 2f64.powi(-30);
    let b = a;
    let c = -1.0;

    let fmadd_d = (3u32 << 27) | (0x1 << 25) | (2 << 20) | (1 << 15) | (0x7 << 12) | (4 << 7) | opcodes::MADD;
    let mut m = fpu_machine(&[fmadd_d]);
    m.cpu.fregs[1] = f64_bits(a);
    m.cpu.fregs[2] = f64_bits(b);
    m.cpu.fregs[3] = f64_bits(c);
    assert!(m.cpu.step().is_ok());

    assert_eq!(m.cpu.fregs[4], f64_bits(a.mul_add(b, c)));

    // And the unfused answer really is different, so the test has teeth.
    let unfused = std::hint::black_box(a) * std::hint::black_box(b) + c;
    assert_ne!(a.mul_add(b, c), unfused);
}
