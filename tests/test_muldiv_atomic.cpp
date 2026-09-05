#include <vector>

#include "csr.hpp"
#include "machine.hpp"
#include "test_util.hpp"

// ---------------------------------------------------------------------------
// M and A extension tests.
//
// The M extension's interesting behaviour is entirely in its edge cases: RISC-V
// division never traps, so divide-by-zero and signed overflow have specific
// defined results that must be produced exactly.
//
// The A extension's interesting behaviour is the LR/SC reservation, including
// the case a single-hart emulator could easily get wrong: a trap between the LR
// and the SC must make the SC fail.
// ---------------------------------------------------------------------------

using namespace rvt;

namespace {

constexpr u64 U64_MAX = 0xffff'ffff'ffff'ffffull;
constexpr u64 I64_MIN = 0x8000'0000'0000'0000ull;

// x1 = a, x2 = b, then one M-extension instruction into x3.
u64 m_op(u64 a, u64 b, u32 opcode, u32 funct3) {
    std::vector<u32> p;
    load_imm64(p, 1, a);
    load_imm64(p, 2, b);
    p.push_back(r_type(opcode, 3, funct3, 1, 2, 0x01));
    Machine m(p);
    m.cpu->run(kLoadImm64Steps * 2 + 1, nullptr);
    return m.reg(3);
}

u64 op64(u64 a, u64 b, u32 f3) { return m_op(a, b, opcodes::OP, f3); }
u64 op32(u64 a, u64 b, u32 f3) { return m_op(a, b, opcodes::OP_32, f3); }

// --- multiply ---------------------------------------------------------------

void test_mul() {
    CHECK_EQ_U(op64(6, 7, 0x0), 42);
    CHECK_EQ_U(op64(U64_MAX, 2, 0x0), U64_MAX - 1);   // wraps, never traps

    // MUL returns the low 64 bits, where signedness makes no difference:
    // -1 * -1 and its unsigned reading give the same low half.
    CHECK_EQ_U(op64(U64_MAX, U64_MAX, 0x0), 1);
}

void test_mulh_family() {
    // 2^32 * 2^32 = 2^64, so the low half is 0 and the high half is 1.
    const u64 big = 1ull << 32;
    CHECK_EQ_U(op64(big, big, 0x0), 0);          // MUL
    CHECK_EQ_U(op64(big, big, 0x3), 1);          // MULHU

    // -1 * -1 = 1: the upper half is 0 when both are read as signed...
    CHECK_EQ_U(op64(U64_MAX, U64_MAX, 0x1), 0);  // MULH
    // ...but as unsigned this is (2^64-1)^2, whose upper half is 2^64-2.
    CHECK_EQ_U(op64(U64_MAX, U64_MAX, 0x3), U64_MAX - 1);  // MULHU

    // MULHSU reads rs1 as signed and rs2 as unsigned. -1 * (2^64-1) = -(2^64-1),
    // whose upper 64 bits are all ones.
    CHECK_EQ_U(op64(U64_MAX, U64_MAX, 0x2), U64_MAX);      // MULHSU
    // A positive rs1 makes MULHSU agree with MULHU.
    CHECK_EQ_U(op64(big, big, 0x2), 1);
}

// --- division: the defined results ------------------------------------------

void test_div_by_zero_returns_defined_values() {
    // RISC-V division never traps. Every one of these is a specified result.
    CHECK_EQ_U(op64(42, 0, 0x4), U64_MAX);   // DIV  by zero -> -1
    CHECK_EQ_U(op64(42, 0, 0x5), U64_MAX);   // DIVU by zero -> all ones
    CHECK_EQ_U(op64(42, 0, 0x6), 42);        // REM  by zero -> the dividend
    CHECK_EQ_U(op64(42, 0, 0x7), 42);        // REMU by zero -> the dividend

    // Including when the dividend is itself zero.
    CHECK_EQ_U(op64(0, 0, 0x4), U64_MAX);
    CHECK_EQ_U(op64(0, 0, 0x6), 0);
}

void test_signed_overflow_is_defined() {
    // INT64_MIN / -1 is mathematically 2^63, which does not fit in 64 bits.
    // RISC-V defines the result as INT64_MIN itself (it wraps), and the
    // remainder as 0. This is the case C++ cannot express: computing it
    // directly is undefined behaviour and raises SIGFPE on x86.
    CHECK_EQ_U(op64(I64_MIN, U64_MAX, 0x4), I64_MIN);  // DIV
    CHECK_EQ_U(op64(I64_MIN, U64_MAX, 0x6), 0);        // REM

    // Read as unsigned there is no overflow, so these are ordinary divisions.
    CHECK_EQ_U(op64(I64_MIN, U64_MAX, 0x5), 0);        // DIVU
    CHECK_EQ_U(op64(I64_MIN, U64_MAX, 0x7), I64_MIN);  // REMU
}

void test_signed_division_truncates_toward_zero() {
    const u64 neg7 = static_cast<u64>(-7);
    const u64 neg2 = static_cast<u64>(-2);

    CHECK_EQ_U(op64(neg7, 2, 0x4), static_cast<u64>(-3));  // -7/2 = -3, not -4
    CHECK_EQ_U(op64(neg7, 2, 0x6), static_cast<u64>(-1));  // remainder keeps
                                                           // the dividend's sign
    CHECK_EQ_U(op64(7, neg2, 0x4), static_cast<u64>(-3));
    CHECK_EQ_U(op64(7, neg2, 0x6), 1);
}

void test_signed_and_unsigned_divide_differ() {
    // -2 as unsigned is a huge number, so DIVU gives 0 where DIV gives 1.
    CHECK_EQ_U(op64(static_cast<u64>(-2), static_cast<u64>(-2), 0x4), 1);
    CHECK_EQ_U(op64(2, static_cast<u64>(-2), 0x5), 0);
}

// --- the 32-bit forms -------------------------------------------------------

void test_muldiv_w_sign_extends() {
    // MULW: 0x10000 * 0x10000 = 2^32, whose low 32 bits are 0.
    CHECK_EQ_U(op32(0x10000, 0x10000, 0x0), 0);

    // A 32-bit product with bit 31 set must sign-extend to 64.
    CHECK_EQ_U(op32(0x40000000, 2, 0x0), 0xffff'ffff'8000'0000ull);

    // The *W forms ignore bits 32-63 of their operands.
    CHECK_EQ_U(op32(0xffff'ffff'0000'0006ull, 7, 0x0), 42);

    // Divide-by-zero results, sign-extended.
    CHECK_EQ_U(op32(42, 0, 0x4), U64_MAX);   // DIVW
    CHECK_EQ_U(op32(42, 0, 0x5), U64_MAX);   // DIVUW - all ones, not 0xFFFFFFFF
    CHECK_EQ_U(op32(42, 0, 0x6), 42);        // REMW
    CHECK_EQ_U(op32(42, 0, 0x7), 42);        // REMUW

    // 32-bit signed overflow.
    const u64 i32_min = 0xffff'ffff'8000'0000ull;
    CHECK_EQ_U(op32(i32_min, U64_MAX, 0x4), i32_min);  // DIVW
    CHECK_EQ_U(op32(i32_min, U64_MAX, 0x6), 0);        // REMW

    // DIVUW reads its operands as unsigned 32-bit but still sign-extends the
    // result: 0xFFFFFFFF / 1 = 0xFFFFFFFF, which extends to all ones.
    CHECK_EQ_U(op32(0xffff'ffffull, 1, 0x5), U64_MAX);
}

// --- atomics ----------------------------------------------------------------

u32 amo(u32 funct5, u32 rd, u32 rs1, u32 rs2, bool word) {
    return r_type(opcodes::AMO, rd, word ? 0x2 : 0x3, rs1, rs2, funct5 << 2);
}

void test_amo_returns_original_and_stores_result() {
    const u64 addr = DRAM_BASE + 0x400;

    std::vector<u32> p;
    load_imm64(p, 1, addr);
    load_imm64(p, 2, 100);
    p.push_back(s_type(opcodes::STORE, 0x3, 1, 2, 0));   // [addr] = 100
    load_imm64(p, 2, 5);
    p.push_back(amo(0x00, 3, 1, 2, false));              // amoadd.d x3, x2, (x1)
    p.push_back(i_type(opcodes::LOAD, 4, 0x3, 1, 0));    // x4 = [addr]

    Machine m(p);
    m.cpu->run(kLoadImm64Steps * 3 + 3, nullptr);
    CHECK_EQ_U(m.reg(3), 100);   // rd gets the value from BEFORE the operation
    CHECK_EQ_U(m.reg(4), 105);   // memory gets the result
}

void test_amo_operations() {
    const u64 addr = DRAM_BASE + 0x500;

    struct Case { const char* name; u32 funct5; u64 mem; u64 src; u64 expect; };
    const Case cases[] = {
        {"amoswap", 0x01, 100, 7, 7},
        {"amoxor",  0x04, 0b1100, 0b1010, 0b0110},
        {"amoor",   0x08, 0b1100, 0b1010, 0b1110},
        {"amoand",  0x0c, 0b1100, 0b1010, 0b1000},
        {"amomin",  0x10, static_cast<u64>(-5), 3, static_cast<u64>(-5)},
        {"amomax",  0x14, static_cast<u64>(-5), 3, 3},
        // The unsigned forms read the same bits the other way: -5 is huge.
        {"amominu", 0x18, static_cast<u64>(-5), 3, 3},
        {"amomaxu", 0x1c, static_cast<u64>(-5), 3, static_cast<u64>(-5)},
    };

    for (const Case& c : cases) {
        std::vector<u32> p;
        load_imm64(p, 1, addr);
        load_imm64(p, 2, c.mem);
        p.push_back(s_type(opcodes::STORE, 0x3, 1, 2, 0));
        load_imm64(p, 2, c.src);
        p.push_back(amo(c.funct5, 3, 1, 2, false));
        p.push_back(i_type(opcodes::LOAD, 4, 0x3, 1, 0));

        Machine m(p);
        m.cpu->run(kLoadImm64Steps * 3 + 3, nullptr);

        ++testutil::g_checks;
        if (m.reg(4) != c.expect) {
            testutil::report_failure(__FILE__, __LINE__,
                std::string(c.name) + ": memory holds " + testutil::hex(m.reg(4)) +
                ", expected " + testutil::hex(c.expect));
        }
        CHECK_EQ_U(m.reg(3), c.mem);   // rd always gets the original
    }
}

void test_amo_word_form_sign_extends() {
    const u64 addr = DRAM_BASE + 0x600;

    std::vector<u32> p;
    load_imm64(p, 1, addr);
    load_imm64(p, 2, 0x8000'0000ull);                    // bit 31 set
    p.push_back(s_type(opcodes::STORE, 0x2, 1, 2, 0));   // sw
    load_imm64(p, 2, 0);
    p.push_back(amo(0x00, 3, 1, 2, true));               // amoadd.w x3, x2, (x1)

    Machine m(p);
    m.cpu->run(kLoadImm64Steps * 3 + 2, nullptr);
    // The loaded 32-bit value is sign-extended into rd.
    CHECK_EQ_U(m.reg(3), 0xffff'ffff'8000'0000ull);
}

void test_misaligned_atomic_traps() {
    // Unlike ordinary loads and stores, the spec requires atomics to be
    // naturally aligned.
    std::vector<u32> p;
    load_imm64(p, 1, DRAM_BASE + 0x401);   // not 8-byte aligned
    p.push_back(amo(0x00, 3, 1, 0, false));

    Machine m(p);
    const Status st = m.cpu->run(kLoadImm64Steps + 1, nullptr);
    CHECK(!st);
    CHECK(st.trap.cause == Exception::StoreAMOAddressMisaligned);
}

// --- LR / SC ----------------------------------------------------------------

void test_lr_sc_succeeds_when_reservation_holds() {
    const u64 addr = DRAM_BASE + 0x700;

    std::vector<u32> p;
    load_imm64(p, 1, addr);
    load_imm64(p, 2, 11);
    p.push_back(s_type(opcodes::STORE, 0x3, 1, 2, 0));   // [addr] = 11
    load_imm64(p, 2, 99);
    p.push_back(amo(0x02, 3, 1, 0, false));              // lr.d  x3, (x1)
    p.push_back(amo(0x03, 4, 1, 2, false));              // sc.d  x4, x2, (x1)
    p.push_back(i_type(opcodes::LOAD, 5, 0x3, 1, 0));    // x5 = [addr]

    Machine m(p);
    m.cpu->run(kLoadImm64Steps * 3 + 4, nullptr);
    CHECK_EQ_U(m.reg(3), 11);   // lr returned the old value
    CHECK_EQ_U(m.reg(4), 0);    // 0 in rd means the store succeeded
    CHECK_EQ_U(m.reg(5), 99);   // and it really did store
}

void test_sc_without_lr_fails() {
    // An SC with no reservation must fail and must not write memory.
    const u64 addr = DRAM_BASE + 0x800;

    std::vector<u32> p;
    load_imm64(p, 1, addr);
    load_imm64(p, 2, 77);
    p.push_back(amo(0x03, 4, 1, 2, false));            // sc.d with no lr
    p.push_back(i_type(opcodes::LOAD, 5, 0x3, 1, 0));

    Machine m(p);
    m.cpu->run(kLoadImm64Steps * 2 + 2, nullptr);
    CHECK(m.reg(4) != 0);       // non-zero means failure
    CHECK_EQ_U(m.reg(5), 0);    // memory untouched
}

void test_sc_to_a_different_address_fails() {
    const u64 a = DRAM_BASE + 0x900;
    const u64 b = DRAM_BASE + 0x908;

    std::vector<u32> p;
    load_imm64(p, 1, a);
    load_imm64(p, 6, b);
    load_imm64(p, 2, 55);
    p.push_back(amo(0x02, 3, 1, 0, false));   // lr.d on a
    p.push_back(amo(0x03, 4, 6, 2, false));   // sc.d on b - different address

    Machine m(p);
    m.cpu->run(kLoadImm64Steps * 3 + 2, nullptr);
    CHECK(m.reg(4) != 0);
}

void test_second_sc_fails_because_the_reservation_is_consumed() {
    // The reservation is consumed by the SC whether it succeeded or not.
    // Leaving it set would let a later SC succeed against a stale reservation.
    const u64 addr = DRAM_BASE + 0xa00;

    std::vector<u32> p;
    load_imm64(p, 1, addr);
    load_imm64(p, 2, 1);
    p.push_back(amo(0x02, 3, 1, 0, false));   // lr.d
    p.push_back(amo(0x03, 4, 1, 2, false));   // sc.d - succeeds
    p.push_back(amo(0x03, 5, 1, 2, false));   // sc.d again - must fail

    Machine m(p);
    m.cpu->run(kLoadImm64Steps * 2 + 3, nullptr);
    CHECK_EQ_U(m.reg(4), 0);   // first succeeded
    CHECK(m.reg(5) != 0);      // second did not
}

// Runs: lr.d, then `middle`, then sc.d - with a trap handler installed so an
// ECALL dispatches and returns rather than halting. Returns the SC result.
u64 sc_result_with_middle_instruction(u32 middle) {
    const u64 addr = DRAM_BASE + 0xb00;
    const u64 K = kLoadImm64Steps;
    const u64 handler = DRAM_BASE + (3 * K + 5) * 4;

    std::vector<u32> p;
    load_imm64(p, 1, addr);        // [0, K)
    load_imm64(p, 2, 42);          // [K, 2K)
    load_imm64(p, 6, handler);     // [2K, 3K)
    p.push_back(i_type(opcodes::SYSTEM, 0, 0x1, 6, csr::MTVEC));  // 3K   mtvec = x6
    p.push_back(amo(0x02, 3, 1, 0, false));                       // 3K+1 lr.d
    p.push_back(middle);                                          // 3K+2
    p.push_back(amo(0x03, 4, 1, 2, false));                       // 3K+3 sc.d
    p.push_back(HALT());                                          // 3K+4
    // handler: step over the faulting instruction and return.
    p.push_back(i_type(opcodes::SYSTEM, 5, 0x2, 0, csr::MEPC));   // 3K+5 x5 = mepc
    p.push_back(ADDI(5, 5, 4));                                   // 3K+6
    p.push_back(i_type(opcodes::SYSTEM, 0, 0x1, 5, csr::MEPC));   // 3K+7 mepc = x5
    p.push_back(i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x302));       // 3K+8 mret

    Machine m(p);
    m.cpu->run(300, nullptr);
    return m.reg(4);
}

void test_trap_between_lr_and_sc_breaks_the_reservation() {
    // This is the case a single-hart emulator gets wrong by default. Nothing
    // can write memory behind our back, so a reservation would never break and
    // every SC would succeed - but a trap is a context switch, and a thread
    // that resumes must not inherit another thread's reservation.
    //
    // The control makes the claim precise: with a NOP between the LR and the SC
    // the store succeeds, and swapping only that instruction for an ECALL makes
    // it fail. So it is the trap doing it, not the extra instruction.
    CHECK_EQ_U(sc_result_with_middle_instruction(NOP()), 0);   // control: succeeds

    const u32 ecall = i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x000);
    CHECK(sc_result_with_middle_instruction(ecall) != 0);      // trap breaks it
}

void test_a_misaligned_lr_reports_a_load_fault() {
    // LR is a load and must report LoadAddressMisaligned; SC and the
    // read-modify-write AMOs store, so they report StoreAMOAddressMisaligned.
    // A handler that sees a store fault for an instruction which performed no
    // store has no way to make sense of it.
    Machine m({amo(0x02, 1, 2, 0, false)});   // lr.d x1, (x2)
    m.cpu->regs[2] = DRAM_BASE + 4;
    const Status lr = m.cpu->step();
    CHECK(!lr);
    CHECK(lr.trap.cause == Exception::LoadAddressMisaligned);

    Machine m2({amo(0x03, 1, 2, 3, false)});  // sc.d x1, x3, (x2)
    m2.cpu->regs[2] = DRAM_BASE + 4;
    const Status sc = m2.cpu->step();
    CHECK(!sc);
    CHECK(sc.trap.cause == Exception::StoreAMOAddressMisaligned);

    Machine m3({amo(0x00, 1, 2, 3, false)});  // amoadd.d x1, x3, (x2)
    m3.cpu->regs[2] = DRAM_BASE + 4;
    const Status amo = m3.cpu->step();
    CHECK(!amo);
    CHECK(amo.trap.cause == Exception::StoreAMOAddressMisaligned);
}

}  // namespace

int main() {
    test_mul();
    test_mulh_family();
    test_div_by_zero_returns_defined_values();
    test_signed_overflow_is_defined();
    test_signed_division_truncates_toward_zero();
    test_signed_and_unsigned_divide_differ();
    test_muldiv_w_sign_extends();
    test_amo_returns_original_and_stores_result();
    test_amo_operations();
    test_amo_word_form_sign_extends();
    test_misaligned_atomic_traps();
    test_lr_sc_succeeds_when_reservation_holds();
    test_sc_without_lr_fails();
    test_sc_to_a_different_address_fails();
    test_second_sc_fails_because_the_reservation_is_consumed();
    test_trap_between_lr_and_sc_breaks_the_reservation();
    test_a_misaligned_lr_reports_a_load_fault();
    return testutil::summary("muldiv_atomic");
}
