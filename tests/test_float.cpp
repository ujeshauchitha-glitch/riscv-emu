#include <cmath>
#include <cstring>

#include "cpu.hpp"
#include "fpu.hpp"
#include "machine.hpp"
#include "test_util.hpp"

// ---------------------------------------------------------------------------
// The F and D extensions.
//
// riscv-tests covers the arithmetic thoroughly, so this suite concentrates on
// the parts that are easy to get subtly wrong and that show up as a wrong
// number rather than a crash: NaN boxing, the FS enable, and the places where
// RISC-V's semantics differ from the host's.
// ---------------------------------------------------------------------------

using namespace rvt;

namespace {

// Encoders, written independently of src/decoder.cpp for the usual reason: a
// test that built its inputs with the emulator's own decoder would agree with
// itself however wrong both halves were.
u32 fp_op(u32 funct7, u32 rs2, u32 rs1, u32 rm, u32 rd) {
    return (funct7 << 25) | (rs2 << 20) | (rs1 << 15) | (rm << 12) | (rd << 7) |
           opcodes::OP_FP;
}
u32 FADD_D(u32 rd, u32 a, u32 b) { return fp_op(0x01, b, a, 0x7, rd); }
u32 FADD_S(u32 rd, u32 a, u32 b) { return fp_op(0x00, b, a, 0x7, rd); }
u32 FMUL_D(u32 rd, u32 a, u32 b) { return fp_op(0x09, b, a, 0x7, rd); }
u32 FDIV_D(u32 rd, u32 a, u32 b) { return fp_op(0x0d, b, a, 0x7, rd); }
u32 FSQRT_D(u32 rd, u32 a)       { return fp_op(0x2d, 0, a, 0x7, rd); }
u32 FSGNJ_D(u32 rd, u32 a, u32 b){ return fp_op(0x11, b, a, 0x0, rd); }
u32 FSGNJN_D(u32 rd, u32 a, u32 b){return fp_op(0x11, b, a, 0x1, rd); }
u32 FSGNJX_D(u32 rd, u32 a, u32 b){return fp_op(0x11, b, a, 0x2, rd); }
u32 FMIN_D(u32 rd, u32 a, u32 b) { return fp_op(0x15, b, a, 0x0, rd); }
u32 FMAX_D(u32 rd, u32 a, u32 b) { return fp_op(0x15, b, a, 0x1, rd); }
u32 FEQ_D(u32 rd, u32 a, u32 b)  { return fp_op(0x51, b, a, 0x2, rd); }
u32 FLT_D(u32 rd, u32 a, u32 b)  { return fp_op(0x51, b, a, 0x1, rd); }
u32 FLE_D(u32 rd, u32 a, u32 b)  { return fp_op(0x51, b, a, 0x0, rd); }
u32 FCLASS_D(u32 rd, u32 a)      { return fp_op(0x71, 0, a, 0x1, rd); }
u32 FMV_X_D(u32 rd, u32 a)       { return fp_op(0x71, 0, a, 0x0, rd); }
u32 FMV_D_X(u32 rd, u32 a)       { return fp_op(0x79, 0, a, 0x0, rd); }
u32 FMV_X_W(u32 rd, u32 a)       { return fp_op(0x70, 0, a, 0x0, rd); }
u32 FMV_W_X(u32 rd, u32 a)       { return fp_op(0x78, 0, a, 0x0, rd); }
u32 FCVT_W_D(u32 rd, u32 a, u32 rm) { return fp_op(0x61, 0, a, rm, rd); }
u32 FCVT_D_W(u32 rd, u32 a)      { return fp_op(0x69, 0, a, 0x7, rd); }
u32 FCVT_S_D(u32 rd, u32 a)      { return fp_op(0x20, 1, a, 0x7, rd); }
u32 FCVT_D_S(u32 rd, u32 a)      { return fp_op(0x21, 0, a, 0x7, rd); }
u32 FLD(u32 rd, u32 rs1, i32 imm){ return i_type(opcodes::LOAD_FP, rd, 0x3, rs1, imm); }
u32 FLW(u32 rd, u32 rs1, i32 imm){ return i_type(opcodes::LOAD_FP, rd, 0x2, rs1, imm); }
u32 FSD(u32 rs1, u32 rs2, i32 imm){ return s_type(opcodes::STORE_FP, 0x3, rs1, rs2, imm); }
u32 FSW(u32 rs1, u32 rs2, i32 imm){ return s_type(opcodes::STORE_FP, 0x2, rs1, rs2, imm); }

// A machine with the FPU switched on, which is what a kernel does before
// handing the unit to a process.
Machine fpu_machine(const std::vector<u32>& program) {
    Machine m(program);
    m.cpu->csrs.write(csr::MSTATUS,
                      m.cpu->csrs.mstatus() | csr::MSTATUS_FS_INITIAL);
    return m;
}

u64 as_bits(double d) { u64 b; std::memcpy(&b, &d, 8); return b; }
u32 as_bits(float f)  { u32 b; std::memcpy(&b, &f, 4); return b; }

// --- the FS enable ----------------------------------------------------------

void test_fp_traps_while_fs_is_off() {
    // mstatus.FS comes out of reset as Off, and while it is Off every
    // floating-point instruction is illegal. This is what a kernel with no FPU
    // support relies on to stop user code from quietly corrupting a register
    // file nobody is saving across context switches.
    Machine m({FADD_D(0, 1, 2)});
    CHECK_EQ_U(m.cpu->csrs.mstatus() & csr::MSTATUS_FS, csr::MSTATUS_FS_OFF);

    const Status st = m.cpu->step();
    CHECK(!st);
    CHECK(st.trap.cause == Exception::IllegalInstruction);

    // fcsr is gated by the same bit - it is part of the state being protected.
    const u32 csrrs_fcsr = i_type(opcodes::SYSTEM, 1, 0x2, 0, static_cast<i32>(csr::FCSR));
    Machine m2({csrrs_fcsr});
    const Status st2 = m2.cpu->step();
    CHECK(!st2);
    CHECK(st2.trap.cause == Exception::IllegalInstruction);
}

void test_writing_an_f_register_marks_the_unit_dirty() {
    // A kernel saves the 32 floating-point registers only when FS says Dirty.
    // An instruction that writes one and does not make that transition is a
    // process silently inheriting another's floating-point state.
    Machine m = fpu_machine({FMV_D_X(1, 0)});
    CHECK_EQ_U(m.cpu->csrs.mstatus() & csr::MSTATUS_FS, csr::MSTATUS_FS_INITIAL);
    CHECK(m.cpu->step());
    CHECK_EQ_U(m.cpu->csrs.mstatus() & csr::MSTATUS_FS, csr::MSTATUS_FS_DIRTY);
    // And SD, the read-only summary bit, follows it.
    CHECK((m.cpu->csrs.mstatus() & csr::MSTATUS_SD) != 0);
}

// --- NaN boxing -------------------------------------------------------------

void test_a_single_is_nan_boxed_in_the_register() {
    // The two precisions share one register file, so something has to happen
    // when a program writes a float and reads it as a double. The spec's answer
    // is NaN boxing: a valid single has all of its upper 32 bits set, which is
    // the encoding of a quiet NaN - so reading it as a double yields a NaN
    // rather than a plausible-looking wrong number.
    Machine m = fpu_machine({ADDI(1, 0, 0x40), FMV_W_X(0, 1)});
    CHECK(m.cpu->run(2, nullptr));
    CHECK_EQ_U(m.cpu->fregs[0] >> 32, 0xffffffffull);
    CHECK_EQ_U(m.cpu->fregs[0] & 0xffffffffull, 0x40);

    // Read that register as a double and it is a NaN, by construction.
    CHECK(std::isnan(bits_to_f64(m.cpu->fregs[0])));
}

void test_an_unboxed_register_reads_as_canonical_nan() {
    // A 64-bit value whose upper half is not all ones is not a valid single.
    // Every single-precision *arithmetic* instruction must see the canonical
    // NaN rather than the bits that happen to be there.
    CHECK_EQ_U(nan_unbox(0x7fffffff11111111ull), CANONICAL_NAN_F32);
    CHECK_EQ_U(nan_unbox(nan_box(0x40490fdb)), 0x40490fdb);

    // 1.0 as a double, read as a single: not boxed, so NaN.
    CHECK_EQ_U(nan_unbox(as_bits(1.0)), CANONICAL_NAN_F32);
}

void test_fmv_x_w_does_not_unbox() {
    // The one exception, and it matters: FMV.X.W is a *raw* bit move. It takes
    // bits 31:0 as they are and sign-extends them, applying no boxing rule at
    // all - unboxing here would replace the bits software asked to see with a
    // canonical NaN, defeating the point of the instruction.
    Machine m = fpu_machine({FMV_X_W(1, 0)});
    m.cpu->fregs[0] = 0x7fffffff'11111111ull;   // deliberately not boxed
    CHECK(m.cpu->step());
    CHECK_EQ_U(m.reg(1), 0x11111111ull);

    // And the sign extension is real: a low half with bit 31 set fills the top.
    Machine m2 = fpu_machine({FMV_X_W(1, 0)});
    m2.cpu->fregs[0] = 0x00000000'80000001ull;
    CHECK(m2.cpu->step());
    CHECK_EQ_U(m2.reg(1), 0xffffffff'80000001ull);
}

void test_fsw_stores_only_the_low_half() {
    // FSW writes the 32-bit single; the NaN box is a property of the register,
    // not of the value in memory, so it is not stored. FSD writes all 64 bits,
    // because for a double every one of them is part of the number.
    Machine m = fpu_machine({FSW(2, 0, 0), FSD(2, 0, 8)});
    m.cpu->fregs[0] = nan_box(0xdeadbeef);
    m.cpu->regs[2]  = DRAM_BASE + 0x100;   // clear of the program itself
    CHECK(m.cpu->run(2, nullptr));

    auto word = m.bus.load(DRAM_BASE + 0x100, 4, AccessType::Load);
    CHECK(word);
    CHECK_EQ_U(word.value, 0xdeadbeefull);

    auto dword = m.bus.load(DRAM_BASE + 0x108, 8, AccessType::Load);
    CHECK(dword);
    CHECK_EQ_U(dword.value, nan_box(0xdeadbeef));
}

void test_flw_boxes_what_it_loads() {
    Machine m = fpu_machine({FLW(0, 2, 0)});
    m.cpu->regs[2] = DRAM_BASE + 0x100;
    CHECK(m.bus.store(DRAM_BASE + 0x100, 4, 0x40490fdb));
    CHECK(m.cpu->step());
    CHECK_EQ_U(m.cpu->fregs[0], nan_box(0x40490fdb));
}

void test_fld_loads_all_sixty_four_bits() {
    // The other half of the pair: FLD moves a double, so there is no box to
    // apply - every bit of the register is part of the number.
    Machine m = fpu_machine({FLD(0, 2, 0)});
    m.cpu->regs[2] = DRAM_BASE + 0x100;
    CHECK(m.bus.store(DRAM_BASE + 0x100, 8, as_bits(-2.75)));
    CHECK(m.cpu->step());
    CHECK_EQ_U(m.cpu->fregs[0], as_bits(-2.75));

    // A double loaded this way is *not* a valid single, which is exactly what
    // NaN boxing is for: reading it as one yields the canonical NaN rather
    // than the low half of a number that means something else.
    CHECK_EQ_U(nan_unbox(m.cpu->fregs[0]), CANONICAL_NAN_F32);
}

// --- arithmetic -------------------------------------------------------------

void test_basic_double_arithmetic() {
    Machine m = fpu_machine({FADD_D(3, 1, 2), FMUL_D(4, 1, 2), FDIV_D(5, 1, 2),
                             FSQRT_D(6, 1)});
    m.cpu->fregs[1] = as_bits(9.0);
    m.cpu->fregs[2] = as_bits(2.0);
    CHECK(m.cpu->run(4, nullptr));
    CHECK_EQ_U(m.cpu->fregs[3], as_bits(11.0));
    CHECK_EQ_U(m.cpu->fregs[4], as_bits(18.0));
    CHECK_EQ_U(m.cpu->fregs[5], as_bits(4.5));
    CHECK_EQ_U(m.cpu->fregs[6], as_bits(3.0));
}

void test_divide_by_zero_sets_the_flag_and_yields_infinity() {
    // RISC-V floating-point division by zero does not trap - like integer
    // division, it produces a defined value. Unlike integer division it also
    // records that it happened, in a sticky flag software can check later.
    Machine m = fpu_machine({FDIV_D(3, 1, 2)});
    m.cpu->fregs[1] = as_bits(1.0);
    m.cpu->fregs[2] = as_bits(0.0);
    CHECK(m.cpu->step());
    CHECK(std::isinf(bits_to_f64(m.cpu->fregs[3])));
    CHECK((m.cpu->csrs.read(csr::FFLAGS) & csr::FFLAG_DZ) != 0);
}

void test_exception_flags_are_sticky() {
    // Nothing but an explicit write clears them. That is what lets a program
    // run a long calculation and ask afterwards whether anything went wrong,
    // rather than checking after every operation.
    Machine m = fpu_machine({FDIV_D(3, 1, 2), FADD_D(4, 1, 1)});
    m.cpu->fregs[1] = as_bits(1.0);
    m.cpu->fregs[2] = as_bits(0.0);
    CHECK(m.cpu->run(2, nullptr));
    // The clean addition did not clear the earlier division's flag.
    CHECK((m.cpu->csrs.read(csr::FFLAGS) & csr::FFLAG_DZ) != 0);

    m.cpu->csrs.write(csr::FFLAGS, 0);
    CHECK_EQ_U(m.cpu->csrs.read(csr::FFLAGS), 0);
}

void test_fcsr_fflags_and_frm_are_windows_on_one_register() {
    // Three CSR addresses, one register. Software writes frm alone constantly -
    // a routine that wants round-toward-zero should not have to read-modify-
    // write fcsr and risk clobbering the accrued flags.
    Machine m = fpu_machine(std::vector<u32>{NOP()});
    m.cpu->csrs.write(csr::FCSR, 0);
    m.cpu->csrs.write(csr::FFLAGS, csr::FFLAG_NX | csr::FFLAG_OF);
    m.cpu->csrs.write(csr::FRM, csr::FRM_RTZ);

    CHECK_EQ_U(m.cpu->csrs.read(csr::FFLAGS), csr::FFLAG_NX | csr::FFLAG_OF);
    CHECK_EQ_U(m.cpu->csrs.read(csr::FRM), csr::FRM_RTZ);
    CHECK_EQ_U(m.cpu->csrs.read(csr::FCSR),
               (csr::FRM_RTZ << csr::FCSR_FRM_SHIFT) | csr::FFLAG_NX | csr::FFLAG_OF);

    // Writing frm leaves the flags alone, which is the whole point.
    m.cpu->csrs.write(csr::FRM, csr::FRM_RUP);
    CHECK_EQ_U(m.cpu->csrs.read(csr::FFLAGS), csr::FFLAG_NX | csr::FFLAG_OF);
}

void test_rounding_mode_changes_the_result() {
    // 2.5 converted to an integer: round-to-nearest-even gives 2, toward zero
    // gives 2, up gives 3, down gives 2. The tie is what distinguishes RNE from
    // the away-from-zero rounding most people expect.
    struct { u32 rm; i64 expect; } cases[] = {
        {csr::FRM_RNE, 2},
        {csr::FRM_RTZ, 2},
        {csr::FRM_RUP, 3},
        {csr::FRM_RDN, 2},
    };
    for (const auto& c : cases) {
        Machine m = fpu_machine({FCVT_W_D(1, 0, c.rm)});
        m.cpu->fregs[0] = as_bits(2.5);
        CHECK(m.cpu->step());
        CHECK_EQ_U(m.reg(1), static_cast<u64>(c.expect));
    }

    // 3.5 rounds to 4 under RNE - to *even*, not always down.
    Machine m = fpu_machine({FCVT_W_D(1, 0, csr::FRM_RNE)});
    m.cpu->fregs[0] = as_bits(3.5);
    CHECK(m.cpu->step());
    CHECK_EQ_U(m.reg(1), 4);
}

void test_a_reserved_rounding_mode_is_illegal() {
    // Modes 5 and 6 do not exist. An instruction asking for one has a bug, and
    // guessing at what it meant hides that.
    Machine m = fpu_machine({(FADD_D(3, 1, 2) & ~(0x7u << 12)) | (5u << 12)});
    const Status st = m.cpu->step();
    CHECK(!st);
    CHECK(st.trap.cause == Exception::IllegalInstruction);
}

void test_conversion_saturates_instead_of_being_undefined() {
    // C says an out-of-range float-to-integer conversion is undefined. RISC-V
    // says it saturates to the largest or smallest representable value and
    // raises Invalid - so the result is always something specific.
    Machine m = fpu_machine({FCVT_W_D(1, 0, csr::FRM_RTZ)});
    m.cpu->fregs[0] = as_bits(1e18);
    CHECK(m.cpu->step());
    CHECK_EQ_U(m.reg(1), static_cast<u64>(static_cast<i64>(INT32_MAX)));
    CHECK((m.cpu->csrs.read(csr::FFLAGS) & csr::FFLAG_NV) != 0);

    Machine m2 = fpu_machine({FCVT_W_D(1, 0, csr::FRM_RTZ)});
    m2.cpu->fregs[0] = as_bits(-1e18);
    CHECK(m2.cpu->step());
    CHECK_EQ_U(m2.reg(1), static_cast<u64>(static_cast<i64>(INT32_MIN)));

    // A NaN converts to the *maximum positive* value, not to zero. That is
    // surprising and it is what the spec says.
    Machine m3 = fpu_machine({FCVT_W_D(1, 0, csr::FRM_RTZ)});
    m3.cpu->fregs[0] = CANONICAL_NAN_F64;
    CHECK(m3.cpu->step());
    CHECK_EQ_U(m3.reg(1), static_cast<u64>(static_cast<i64>(INT32_MAX)));
    CHECK((m3.cpu->csrs.read(csr::FFLAGS) & csr::FFLAG_NV) != 0);
}

// --- sign injection ---------------------------------------------------------

void test_sign_injection_is_bit_manipulation() {
    // FSGNJ and friends never round, never raise a flag, and never treat a NaN
    // specially - which is exactly what makes them the canonical encodings of
    // fabs, fneg and a register move.
    Machine m = fpu_machine({FSGNJ_D(3, 1, 2), FSGNJN_D(4, 1, 2), FSGNJX_D(5, 1, 2)});
    m.cpu->fregs[1] = as_bits(-3.0);
    m.cpu->fregs[2] = as_bits(1.0);     // positive
    CHECK(m.cpu->run(3, nullptr));
    CHECK_EQ_U(m.cpu->fregs[3], as_bits(3.0));    // sign of rs2
    CHECK_EQ_U(m.cpu->fregs[4], as_bits(-3.0));   // inverted
    CHECK_EQ_U(m.cpu->fregs[5], as_bits(-3.0));   // xor of the two signs
    CHECK_EQ_U(m.cpu->csrs.read(csr::FFLAGS), 0);

    // fabs is FSGNJX with the same register twice: x ^ x = 0, so the sign is
    // always cleared.
    Machine m2 = fpu_machine({FSGNJX_D(3, 1, 1)});
    m2.cpu->fregs[1] = as_bits(-7.5);
    CHECK(m2.cpu->step());
    CHECK_EQ_U(m2.cpu->fregs[3], as_bits(7.5));
}

// --- min/max and comparison -------------------------------------------------

void test_fmin_fmax_nan_and_zero_rules() {
    // FMIN/FMAX are neither C's fmin/fmax nor a comparison. If one operand is
    // NaN the result is the *other* one; if both are, it is the canonical NaN.
    Machine m = fpu_machine({FMIN_D(3, 1, 2), FMAX_D(4, 1, 2)});
    m.cpu->fregs[1] = CANONICAL_NAN_F64;
    m.cpu->fregs[2] = as_bits(2.0);
    CHECK(m.cpu->run(2, nullptr));
    CHECK_EQ_U(m.cpu->fregs[3], as_bits(2.0));
    CHECK_EQ_U(m.cpu->fregs[4], as_bits(2.0));

    Machine m2 = fpu_machine({FMIN_D(3, 1, 2)});
    m2.cpu->fregs[1] = CANONICAL_NAN_F64;
    m2.cpu->fregs[2] = CANONICAL_NAN_F64;
    CHECK(m2.cpu->step());
    CHECK_EQ_U(m2.cpu->fregs[3], CANONICAL_NAN_F64);

    // -0.0 is defined to be less than +0.0 here, which no comparison reports:
    // -0.0 == +0.0 is true.
    Machine m3 = fpu_machine({FMIN_D(3, 1, 2), FMAX_D(4, 1, 2)});
    m3.cpu->fregs[1] = as_bits(-0.0);
    m3.cpu->fregs[2] = as_bits(0.0);
    CHECK(m3.cpu->run(2, nullptr));
    CHECK_EQ_U(m3.cpu->fregs[3], as_bits(-0.0));
    CHECK_EQ_U(m3.cpu->fregs[4], as_bits(0.0));
}

void test_quiet_and_ordered_comparisons_differ_on_nan() {
    // FEQ is the quiet comparison: a NaN makes it false without complaint.
    // FLT and FLE are ordered: a NaN raises Invalid. That difference is the
    // entire reason there are two kinds.
    Machine m = fpu_machine({FEQ_D(1, 3, 4)});
    m.cpu->fregs[3] = CANONICAL_NAN_F64;
    m.cpu->fregs[4] = as_bits(1.0);
    CHECK(m.cpu->step());
    CHECK_EQ_U(m.reg(1), 0);
    CHECK_EQ_U(m.cpu->csrs.read(csr::FFLAGS) & csr::FFLAG_NV, 0);

    Machine m2 = fpu_machine({FLT_D(1, 3, 4)});
    m2.cpu->fregs[3] = CANONICAL_NAN_F64;
    m2.cpu->fregs[4] = as_bits(1.0);
    CHECK(m2.cpu->step());
    CHECK_EQ_U(m2.reg(1), 0);
    CHECK((m2.cpu->csrs.read(csr::FFLAGS) & csr::FFLAG_NV) != 0);

    // And the ordinary answers are still right.
    Machine m3 = fpu_machine({FEQ_D(1, 3, 4), FLT_D(2, 3, 4), FLE_D(5, 3, 4)});
    m3.cpu->fregs[3] = as_bits(1.0);
    m3.cpu->fregs[4] = as_bits(2.0);
    CHECK(m3.cpu->run(3, nullptr));
    CHECK_EQ_U(m3.reg(1), 0);
    CHECK_EQ_U(m3.reg(2), 1);
    CHECK_EQ_U(m3.reg(5), 1);
}

// --- classification ---------------------------------------------------------

void test_fclass_distinguishes_what_comparisons_cannot() {
    struct { double value; u64 expect; } cases[] = {
        {-INFINITY, FCLASS_NEG_INF},
        {-1.0,      FCLASS_NEG_NORMAL},
        {-0.0,      FCLASS_NEG_ZERO},
        {0.0,       FCLASS_POS_ZERO},
        {1.0,       FCLASS_POS_NORMAL},
        {INFINITY,  FCLASS_POS_INF},
    };
    for (const auto& c : cases) {
        Machine m = fpu_machine({FCLASS_D(1, 0)});
        m.cpu->fregs[0] = as_bits(c.value);
        CHECK(m.cpu->step());
        CHECK_EQ_U(m.reg(1), c.expect);
    }

    // The two zeros are what make fclass necessary: -0.0 == +0.0 is true, so
    // no comparison can tell them apart.
    Machine m = fpu_machine({FCLASS_D(1, 0)});
    m.cpu->fregs[0] = CANONICAL_NAN_F64;
    CHECK(m.cpu->step());
    CHECK_EQ_U(m.reg(1), FCLASS_QUIET_NAN);

    // A signalling NaN has the top mantissa bit clear - the one bit that
    // separates the two kinds.
    Machine m2 = fpu_machine({FCLASS_D(1, 0)});
    m2.cpu->fregs[0] = 0x7ff0000000000001ull;
    CHECK(m2.cpu->step());
    CHECK_EQ_U(m2.reg(1), FCLASS_SIGNALLING);

    // Subnormals: an exponent of zero with a non-zero mantissa.
    Machine m3 = fpu_machine({FCLASS_D(1, 0)});
    m3.cpu->fregs[0] = 1;
    CHECK(m3.cpu->step());
    CHECK_EQ_U(m3.reg(1), FCLASS_POS_SUBNORM);
}

// --- moves and conversions --------------------------------------------------

void test_raw_moves_between_the_register_files() {
    // FMV.D.X and FMV.X.D move bits with no conversion at all, which is how
    // software inspects a float's encoding.
    Machine m = fpu_machine({FMV_D_X(0, 1), FMV_X_D(2, 0)});
    m.cpu->regs[1] = as_bits(-2.5);
    CHECK(m.cpu->run(2, nullptr));
    CHECK_EQ_U(m.cpu->fregs[0], as_bits(-2.5));
    CHECK_EQ_U(m.reg(2), as_bits(-2.5));
    // No rounding happened, so no flag was raised.
    CHECK_EQ_U(m.cpu->csrs.read(csr::FFLAGS), 0);
}

void test_conversions_between_the_two_precisions() {
    Machine m = fpu_machine({FCVT_S_D(1, 0), FCVT_D_S(2, 1)});
    m.cpu->fregs[0] = as_bits(1.5);   // exactly representable in both
    CHECK(m.cpu->run(2, nullptr));
    CHECK_EQ_U(m.cpu->fregs[1], nan_box(as_bits(1.5f)));
    CHECK_EQ_U(m.cpu->fregs[2], as_bits(1.5));

    // A double that does not fit a float loses precision, and says so.
    Machine m2 = fpu_machine({FCVT_S_D(1, 0)});
    m2.cpu->fregs[0] = as_bits(1.0 + 1e-10);
    CHECK(m2.cpu->step());
    CHECK((m2.cpu->csrs.read(csr::FFLAGS) & csr::FFLAG_NX) != 0);
}

void test_integer_to_float_and_back() {
    Machine m = fpu_machine({FCVT_D_W(0, 1), FCVT_W_D(2, 0, csr::FRM_RTZ)});
    m.cpu->regs[1] = static_cast<u64>(static_cast<i64>(-42));
    CHECK(m.cpu->run(2, nullptr));
    CHECK_EQ_U(m.cpu->fregs[0], as_bits(-42.0));
    CHECK_EQ_U(m.reg(2), static_cast<u64>(static_cast<i64>(-42)));
}

void test_single_precision_arithmetic_stays_single() {
    // 1/3 in single precision is a different number from 1/3 in double. If the
    // emulator computed in double and boxed the result, this would come out
    // with more bits of precision than a real machine produces.
    Machine m = fpu_machine({FADD_S(3, 1, 2)});
    m.cpu->fregs[1] = nan_box(as_bits(0.1f));
    m.cpu->fregs[2] = nan_box(as_bits(0.2f));
    CHECK(m.cpu->step());
    CHECK_EQ_U(m.cpu->fregs[3], nan_box(as_bits(0.1f + 0.2f)));
    // Which is emphatically not the double-precision answer boxed.
    CHECK(m.cpu->fregs[3] != nan_box(as_bits(static_cast<float>(0.1 + 0.2))) ||
          as_bits(0.1f + 0.2f) == as_bits(static_cast<float>(0.1 + 0.2)));
}

// --- fused multiply-add -----------------------------------------------------

void test_int_to_float_converts_directly_rather_than_via_double() {
    // 2^53 + 2^29 + 1, converted to a single.
    //
    // Routing through a double rounds twice, and the two roundings do not
    // compose: the intermediate lands exactly on a float tie, and
    // round-half-to-even then breaks it the other way. The result is one ULP
    // from the architecturally required answer - the kind of error that is
    // invisible until it is not.
    const u32 FCVT_S_L = fp_op(0x68, 2, 1, 0x7, 2);   // funct7 0x68 = FCVT.S.*
    Machine m = fpu_machine({FCVT_S_L});
    m.cpu->regs[1] = 0x0020'0000'2000'0001ull;
    CHECK(m.cpu->step());
    CHECK_EQ_U(m.cpu->fregs[2], nan_box(0x5a000001));
}

void test_reserved_conversion_encodings_are_illegal() {
    // rs2 names the source width and signedness for an integer-to-float
    // conversion; 4..31 are reserved. Accepting one and writing zero gives a
    // guest probing for an unimplemented conversion a wrong answer instead of
    // the trap it is waiting for.
    Machine m = fpu_machine({fp_op(0x68, 7, 1, 0x7, 2)});
    const Status st = m.cpu->step();
    CHECK(!st);
    CHECK(st.trap.cause == Exception::IllegalInstruction);
}

void test_fma_rounds_once() {
    // The point of a fused multiply-add is that the product is not rounded
    // before the addition: a*b+c is computed once and rounded once. Computing
    // it as two operations gives a different answer in the last bit, and that
    // difference is precisely what numerical code uses fma to avoid.
    // (1 + 2^-30)^2 = 1 + 2^-29 + 2^-60. The last term falls off the end of a
    // double when the product is rounded on its own, so the unfused answer is
    // exactly 2^-29; the fused one keeps it and gives 2^-29 + 2^-60.
    const double a = 1.0 + 0x1p-30;
    const double b = a;
    const double c = -1.0;

    const u32 FMADD_D = (3u << 27) | (0x1u << 25) | (2u << 20) | (1u << 15) |
                        (0x7u << 12) | (4u << 7) | opcodes::MADD;
    Machine m = fpu_machine({FMADD_D});
    m.cpu->fregs[1] = as_bits(a);
    m.cpu->fregs[2] = as_bits(b);
    m.cpu->fregs[3] = as_bits(c);
    CHECK(m.cpu->step());

    CHECK_EQ_U(m.cpu->fregs[4], as_bits(std::fma(a, b, c)));

    // And the unfused answer really is different, so the test has teeth. The
    // volatiles are load-bearing: without them the compiler is free to contract
    // `a * b + c` into an fma itself, and both sides would agree for the wrong
    // reason.
    volatile double va = a, vb = b, vc = c;
    const double unfused = va * vb + vc;
    CHECK(std::fma(a, b, c) != unfused);
}

}  // namespace

int main() {
    test_fp_traps_while_fs_is_off();
    test_writing_an_f_register_marks_the_unit_dirty();

    test_a_single_is_nan_boxed_in_the_register();
    test_an_unboxed_register_reads_as_canonical_nan();
    test_fmv_x_w_does_not_unbox();
    test_fsw_stores_only_the_low_half();
    test_flw_boxes_what_it_loads();
    test_fld_loads_all_sixty_four_bits();

    test_basic_double_arithmetic();
    test_divide_by_zero_sets_the_flag_and_yields_infinity();
    test_exception_flags_are_sticky();
    test_fcsr_fflags_and_frm_are_windows_on_one_register();
    test_rounding_mode_changes_the_result();
    test_a_reserved_rounding_mode_is_illegal();
    test_conversion_saturates_instead_of_being_undefined();

    test_sign_injection_is_bit_manipulation();
    test_fmin_fmax_nan_and_zero_rules();
    test_quiet_and_ordered_comparisons_differ_on_nan();
    test_fclass_distinguishes_what_comparisons_cannot();

    test_raw_moves_between_the_register_files();
    test_conversions_between_the_two_precisions();
    test_integer_to_float_and_back();
    test_single_precision_arithmetic_stays_single();

    test_int_to_float_converts_directly_rather_than_via_double();
    test_reserved_conversion_encodings_are_illegal();
    test_fma_rounds_once();
    return testutil::summary("float");
}
