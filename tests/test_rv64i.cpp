#include <vector>

#include "machine.hpp"
#include "test_util.hpp"

// ---------------------------------------------------------------------------
// RV64I instruction tests.
//
// The emphasis is on the places where RV64 differs from RV32 and where the
// spec's fine print is easy to skip:
//
//   * every "*W" result is sign-extended from 32 to 64 bits
//   * 64-bit shifts mask to 6 bits, 32-bit shifts to 5
//   * signed vs unsigned comparisons and load extensions
//   * JALR clears bit 0 of the computed target after the add
//   * a faulting instruction commits nothing
// ---------------------------------------------------------------------------

using namespace rvt;

namespace {

// Run a program that first materialises inputs, then executes one instruction
// under test. Returns the machine so registers can be inspected.
Machine run(const std::vector<u32>& prog, u64 steps) {
    Machine m(prog);
    m.cpu->run(steps, nullptr);
    return m;
}

// Build: x1 = a, x2 = b, then `inst`. Returns the value left in x3.
u64 alu2(u64 a, u64 b, u32 inst) {
    std::vector<u32> p;
    load_imm64(p, 1, a);
    load_imm64(p, 2, b);
    p.push_back(inst);
    Machine m(p);
    m.cpu->run(kLoadImm64Steps * 2 + 1, nullptr);
    return m.reg(3);
}

// Build: x1 = a, then `inst`. Returns the value left in x3.
u64 alu1(u64 a, u32 inst) {
    std::vector<u32> p;
    load_imm64(p, 1, a);
    p.push_back(inst);
    Machine m(p);
    m.cpu->run(kLoadImm64Steps + 1, nullptr);
    return m.reg(3);
}

// --- helper sanity ----------------------------------------------------------

void test_load_imm64_helper() {
    // The whole suite depends on this working, so check it first.
    CHECK_EQ_U(alu1(0x0123'4567'89ab'cdefull, NOP()), 0);  // NOP writes nothing to x3

    std::vector<u32> p;
    load_imm64(p, 5, 0xdead'beef'cafe'babeull);
    Machine m(p);
    m.cpu->run(kLoadImm64Steps, nullptr);
    CHECK_EQ_U(m.reg(5), 0xdead'beef'cafe'babeull);
}

// --- LUI / AUIPC ------------------------------------------------------------

void test_lui() {
    Machine m = run({LUI(1, 0x12345)}, 1);
    CHECK_EQ_U(m.reg(1), 0x1234'5000ull);

    // Bit 31 set: the result is sign-extended to 64 bits, so this is negative.
    Machine n = run({LUI(1, 0x80000)}, 1);
    CHECK_EQ_U(n.reg(1), 0xffff'ffff'8000'0000ull);
}

void test_auipc() {
    // AUIPC adds to the address of the AUIPC itself, not to the next
    // instruction. Put one at DRAM_BASE+4 to catch an off-by-one-instruction.
    Machine m = run({NOP(), AUIPC(1, 0x1)}, 2);
    CHECK_EQ_U(m.reg(1), DRAM_BASE + 4 + 0x1000);

    // Zero immediate makes AUIPC a "where am I" instruction.
    Machine n = run({NOP(), NOP(), AUIPC(2, 0)}, 3);
    CHECK_EQ_U(n.reg(2), DRAM_BASE + 8);
}

// --- jumps ------------------------------------------------------------------

void test_jal() {
    // jal x1, +8  skips the ADDI at +4.
    Machine m = run({
        JAL(1, 8),
        ADDI(3, 0, 99),   // skipped
        ADDI(4, 0, 7),
    }, 2);
    CHECK_EQ_U(m.reg(1), DRAM_BASE + 4);  // link = address of next instruction
    CHECK_EQ_U(m.reg(3), 0);              // never executed
    CHECK_EQ_U(m.reg(4), 7);

    // A backward jump, and rd = x0 (a plain goto with no link).
    Machine n = run({
        JAL(0, 8),
        ADDI(3, 0, 99),
        ADDI(4, 0, 1),
        JAL(0, -8),       // back to the ADDI at +4... which we skipped first
    }, 4);
    CHECK_EQ_U(n.reg(0), 0);
    CHECK_EQ_U(n.reg(3), 99);  // reached on the second pass
}

void test_jalr() {
    // Target = rs1 + imm. The landing pad sits three instructions past the
    // JALR, which itself follows the setup sequence.
    const u64 target = DRAM_BASE + (kLoadImm64Steps + 3) * 4;

    std::vector<u32> p;
    load_imm64(p, 1, target);
    p.push_back(JALR(2, 1, 0));        // index kLoadImm64Steps
    p.push_back(ADDI(3, 0, 99));       // skipped
    p.push_back(NOP());                // skipped
    p.push_back(ADDI(4, 0, 5));        // <- target

    Machine m(p);
    m.cpu->run(kLoadImm64Steps + 2, nullptr);
    CHECK_EQ_U(m.reg(3), 0);
    CHECK_EQ_U(m.reg(4), 5);
    // Link register holds the address after the JALR.
    CHECK_EQ_U(m.reg(2), DRAM_BASE + kLoadImm64Steps * 4 + 4);
}

void test_jalr_clears_bit_zero() {
    // jalr with an odd computed target must clear bit 0 rather than trap. This
    // is defined behaviour, not an alignment check: it lets the C extension put
    // a tag in bit 0 of a function pointer.
    // rs1 holds an even address; adding 1 makes the sum odd, and bit 0 is then
    // cleared, landing back on the even target.
    const u64 target = DRAM_BASE + (kLoadImm64Steps + 2) * 4;

    std::vector<u32> p;
    load_imm64(p, 1, target);
    p.push_back(JALR(2, 1, 1));     // target + 1, cleared back to target
    p.push_back(ADDI(3, 0, 99));    // skipped
    p.push_back(ADDI(4, 0, 42));    // <- target

    Machine m(p);
    const Status st = m.cpu->run(kLoadImm64Steps + 2, nullptr);
    CHECK(st);
    CHECK_EQ_U(m.reg(3), 0);
    CHECK_EQ_U(m.reg(4), 42);
}

void test_jalr_rd_equals_rs1() {
    // `jalr ra, ra, 0` is a real idiom. The target must be computed from the
    // *old* rs1, before rd is overwritten with the link address.
    std::vector<u32> p;
    load_imm64(p, 1, DRAM_BASE + (kLoadImm64Steps + 2) * 4);
    p.push_back(JALR(1, 1, 0));   // jump to the instruction after the next one
    p.push_back(ADDI(3, 0, 99));  // skipped
    p.push_back(ADDI(4, 0, 8));   // landing spot

    Machine m(p);
    m.cpu->run(kLoadImm64Steps + 2, nullptr);
    CHECK_EQ_U(m.reg(3), 0);
    CHECK_EQ_U(m.reg(4), 8);
    // x1 now holds the link address (the instruction after the JALR), not the
    // target it jumped to.
    CHECK_EQ_U(m.reg(1), DRAM_BASE + kLoadImm64Steps * 4 + 4);
}

void test_misaligned_jump_traps_on_the_jump() {
    // The trap must be reported on the jump instruction, with the *target* in
    // tval, and the PC left on the jump - not on the target.
    std::vector<u32> p;
    load_imm64(p, 1, DRAM_BASE + 2);   // 2-byte aligned: illegal without C
    p.push_back(JALR(2, 1, 0));

    Machine m(p);
    const Status st = m.cpu->run(kLoadImm64Steps + 1, nullptr);
    CHECK(!st);
    CHECK(st.trap.cause == Exception::InstructionAddressMisaligned);
    CHECK_EQ_U(st.trap.tval, DRAM_BASE + 2);
    // PC still on the JALR itself.
    CHECK_EQ_U(m.cpu->pc, DRAM_BASE + kLoadImm64Steps * 4);
    // And the link register was not written.
    CHECK_EQ_U(m.reg(2), 0);
}

// --- branches ---------------------------------------------------------------

// Runs: x1 = a, x2 = b, branch +8 over an ADDI. Returns true if taken.
bool branch_taken(u64 a, u64 b, u32 funct3) {
    std::vector<u32> p;
    load_imm64(p, 1, a);
    load_imm64(p, 2, b);
    p.push_back(b_type(opcodes::BRANCH, funct3, 1, 2, 8));
    p.push_back(ADDI(3, 0, 1));   // executed only if NOT taken
    p.push_back(NOP());
    Machine m(p);
    m.cpu->run(kLoadImm64Steps * 2 + 3, nullptr);
    return m.reg(3) == 0;
}

void test_branches() {
    // BEQ / BNE
    CHECK(branch_taken(5, 5, 0x0));
    CHECK(!branch_taken(5, 6, 0x0));
    CHECK(branch_taken(5, 6, 0x1));
    CHECK(!branch_taken(5, 5, 0x1));

    // BLT / BGE are signed. -1 < 1.
    CHECK(branch_taken(0xffff'ffff'ffff'ffffull, 1, 0x4));
    CHECK(!branch_taken(1, 0xffff'ffff'ffff'ffffull, 0x4));
    CHECK(branch_taken(1, 0xffff'ffff'ffff'ffffull, 0x5));
    CHECK(branch_taken(5, 5, 0x5));  // BGE is >=

    // BLTU / BGEU are unsigned, so the same bit patterns compare the other way:
    // 0xFFFF...FF is the *largest* unsigned value, not -1.
    CHECK(!branch_taken(0xffff'ffff'ffff'ffffull, 1, 0x6));
    CHECK(branch_taken(1, 0xffff'ffff'ffff'ffffull, 0x6));
    CHECK(branch_taken(0xffff'ffff'ffff'ffffull, 1, 0x7));

    // Signed and unsigned disagreeing on the same inputs is the whole point.
    CHECK(branch_taken(0x8000'0000'0000'0000ull, 1, 0x4));   // signed: negative < 1
    CHECK(!branch_taken(0x8000'0000'0000'0000ull, 1, 0x6));  // unsigned: huge > 1
}

void test_backward_branch_loop() {
    // A real loop: count x3 down from 3 to 0 with a backward BNE.
    std::vector<u32> p;
    p.push_back(ADDI(3, 0, 3));                              // x3 = 3
    p.push_back(ADDI(4, 0, 0));                              // x4 = 0
    p.push_back(ADDI(3, 3, -1));                             // loop: x3 -= 1
    p.push_back(ADDI(4, 4, 10));                             //       x4 += 10
    p.push_back(b_type(opcodes::BRANCH, 0x1, 3, 0, -8));     // bne x3, x0, loop
    Machine m(p);
    m.cpu->run(100, nullptr);
    CHECK_EQ_U(m.reg(3), 0);
    CHECK_EQ_U(m.reg(4), 30);
}

// --- loads and stores -------------------------------------------------------

void test_store_then_load_all_widths() {
    const u64 addr = DRAM_BASE + 0x400;

    std::vector<u32> p;
    load_imm64(p, 1, addr);
    load_imm64(p, 2, 0x0123'4567'89ab'cdefull);
    p.push_back(s_type(opcodes::STORE, 0x3, 1, 2, 0));      // sd
    p.push_back(i_type(opcodes::LOAD, 3, 0x3, 1, 0));       // ld x3
    p.push_back(i_type(opcodes::LOAD, 4, 0x2, 1, 0));       // lw x4  (signed)
    p.push_back(i_type(opcodes::LOAD, 5, 0x6, 1, 0));       // lwu x5 (unsigned)
    p.push_back(i_type(opcodes::LOAD, 6, 0x1, 1, 0));       // lh x6
    p.push_back(i_type(opcodes::LOAD, 7, 0x0, 1, 0));       // lb x7

    Machine m(p);
    m.cpu->run(kLoadImm64Steps * 2 + 6, nullptr);

    CHECK_EQ_U(m.reg(3), 0x0123'4567'89ab'cdefull);
    // Low word is 0x89abcdef; bit 31 is set, so LW sign-extends and LWU does not.
    CHECK_EQ_U(m.reg(4), 0xffff'ffff'89ab'cdefull);
    CHECK_EQ_U(m.reg(5), 0x0000'0000'89ab'cdefull);
    // Low halfword 0xcdef, bit 15 set -> sign-extended.
    CHECK_EQ_U(m.reg(6), 0xffff'ffff'ffff'cdefull);
    // Low byte 0xef, bit 7 set -> sign-extended.
    CHECK_EQ_U(m.reg(7), 0xffff'ffff'ffff'ffefull);
}

void test_load_zero_extension() {
    const u64 addr = DRAM_BASE + 0x500;

    std::vector<u32> p;
    load_imm64(p, 1, addr);
    load_imm64(p, 2, 0xffff'ffff'ffff'ffffull);
    p.push_back(s_type(opcodes::STORE, 0x3, 1, 2, 0));   // sd all-ones
    p.push_back(i_type(opcodes::LOAD, 3, 0x4, 1, 0));    // lbu
    p.push_back(i_type(opcodes::LOAD, 4, 0x5, 1, 0));    // lhu
    p.push_back(i_type(opcodes::LOAD, 5, 0x6, 1, 0));    // lwu

    Machine m(p);
    m.cpu->run(kLoadImm64Steps * 2 + 4, nullptr);

    CHECK_EQ_U(m.reg(3), 0xffull);
    CHECK_EQ_U(m.reg(4), 0xffffull);
    CHECK_EQ_U(m.reg(5), 0xffff'ffffull);
}

void test_store_widths_write_only_their_bytes() {
    const u64 addr = DRAM_BASE + 0x600;

    std::vector<u32> p;
    load_imm64(p, 1, addr);
    load_imm64(p, 2, 0xffff'ffff'ffff'ffffull);
    p.push_back(s_type(opcodes::STORE, 0x3, 1, 2, 0));   // sd all-ones
    load_imm64(p, 2, 0x0000'0000'0000'00aaull);
    p.push_back(s_type(opcodes::STORE, 0x0, 1, 2, 0));   // sb 0xaa over byte 0
    p.push_back(i_type(opcodes::LOAD, 3, 0x3, 1, 0));    // ld

    Machine m(p);
    m.cpu->run(kLoadImm64Steps * 3 + 3, nullptr);
    // Only the lowest byte changed.
    CHECK_EQ_U(m.reg(3), 0xffff'ffff'ffff'ffaaull);
}

void test_store_load_negative_offset() {
    const u64 addr = DRAM_BASE + 0x700;

    std::vector<u32> p;
    load_imm64(p, 1, addr);
    load_imm64(p, 2, 0x1234ull);
    p.push_back(s_type(opcodes::STORE, 0x3, 1, 2, -8));  // sd x2, -8(x1)
    p.push_back(i_type(opcodes::LOAD, 3, 0x3, 1, -8));   // ld x3, -8(x1)

    Machine m(p);
    m.cpu->run(kLoadImm64Steps * 2 + 2, nullptr);
    CHECK_EQ_U(m.reg(3), 0x1234ull);
}

void test_faulting_load_leaves_rd_untouched() {
    // A load from unmapped memory must trap and leave the destination register
    // alone - committing a partial result would be much harder to debug.
    std::vector<u32> p;
    load_imm64(p, 1, 0x1000);            // below DRAM: nothing mapped
    p.push_back(ADDI(3, 0, 77));         // x3 = 77
    p.push_back(i_type(opcodes::LOAD, 3, 0x3, 1, 0));  // ld x3, 0(x1) -> faults

    Machine m(p);
    const Status st = m.cpu->run(kLoadImm64Steps + 2, nullptr);
    CHECK(!st);
    CHECK(st.trap.cause == Exception::LoadAccessFault);
    CHECK_EQ_U(m.reg(3), 77);  // unchanged
}

// --- register-register ALU --------------------------------------------------

void test_op_add_sub() {
    CHECK_EQ_U(alu2(5, 3, r_type(opcodes::OP, 3, 0x0, 1, 2, 0x00)), 8);   // add
    CHECK_EQ_U(alu2(5, 3, r_type(opcodes::OP, 3, 0x0, 1, 2, 0x20)), 2);   // sub
    // Wrapping, not trapping.
    CHECK_EQ_U(alu2(0xffff'ffff'ffff'ffffull, 1, r_type(opcodes::OP, 3, 0x0, 1, 2, 0x00)), 0);
    CHECK_EQ_U(alu2(0, 1, r_type(opcodes::OP, 3, 0x0, 1, 2, 0x20)),
               0xffff'ffff'ffff'ffffull);
}

void test_op_logical_and_compare() {
    CHECK_EQ_U(alu2(0b1100, 0b1010, r_type(opcodes::OP, 3, 0x4, 1, 2, 0x00)), 0b0110); // xor
    CHECK_EQ_U(alu2(0b1100, 0b1010, r_type(opcodes::OP, 3, 0x6, 1, 2, 0x00)), 0b1110); // or
    CHECK_EQ_U(alu2(0b1100, 0b1010, r_type(opcodes::OP, 3, 0x7, 1, 2, 0x00)), 0b1000); // and

    // SLT signed vs SLTU unsigned, on the same bit patterns.
    const u64 neg = 0xffff'ffff'ffff'ffffull;
    CHECK_EQ_U(alu2(neg, 1, r_type(opcodes::OP, 3, 0x2, 1, 2, 0x00)), 1);  // slt: -1 < 1
    CHECK_EQ_U(alu2(neg, 1, r_type(opcodes::OP, 3, 0x3, 1, 2, 0x00)), 0);  // sltu: huge > 1
}

void test_op_shifts_mask_to_six_bits() {
    // Only the low 6 bits of rs2 are used on RV64. A shift by 64 is a shift by
    // 0, not undefined behaviour and not a zeroed register.
    CHECK_EQ_U(alu2(0x1234, 64, r_type(opcodes::OP, 3, 0x1, 1, 2, 0x00)), 0x1234);  // sll
    CHECK_EQ_U(alu2(1, 63, r_type(opcodes::OP, 3, 0x1, 1, 2, 0x00)),
               0x8000'0000'0000'0000ull);

    // SRL zero-fills, SRA sign-fills.
    const u64 neg = 0xffff'ffff'ffff'ffffull;
    CHECK_EQ_U(alu2(neg, 60, r_type(opcodes::OP, 3, 0x5, 1, 2, 0x00)), 0xf);
    CHECK_EQ_U(alu2(neg, 60, r_type(opcodes::OP, 3, 0x5, 1, 2, 0x20)), neg);
}

// --- the *W (32-bit) instruction family -------------------------------------
//
// Every one of these must sign-extend its 32-bit result to 64 bits. This is the
// single most common source of silent RV64 emulator bugs.

void test_addw_sign_extends() {
    // 0x7FFFFFFF + 1 = 0x80000000 as a 32-bit value, which sign-extends to a
    // negative 64-bit number. A zero-extending implementation gives
    // 0x0000000080000000 and nothing complains until much later.
    CHECK_EQ_U(alu2(0x7fff'ffff, 1, r_type(opcodes::OP_32, 3, 0x0, 1, 2, 0x00)),
               0xffff'ffff'8000'0000ull);

    // Bits above 32 in the operands are ignored entirely.
    CHECK_EQ_U(alu2(0xffff'ffff'0000'0005ull, 3,
                    r_type(opcodes::OP_32, 3, 0x0, 1, 2, 0x00)),
               8);

    // subw
    CHECK_EQ_U(alu2(0, 1, r_type(opcodes::OP_32, 3, 0x0, 1, 2, 0x20)),
               0xffff'ffff'ffff'ffffull);
}

void test_w_shifts_use_five_bits() {
    // 32-bit shifts mask rs2 to 5 bits, so a shift by 32 is a shift by 0.
    CHECK_EQ_U(alu2(0x1234, 32, r_type(opcodes::OP_32, 3, 0x1, 1, 2, 0x00)), 0x1234);

    // sllw producing a value with bit 31 set -> sign-extended.
    CHECK_EQ_U(alu2(1, 31, r_type(opcodes::OP_32, 3, 0x1, 1, 2, 0x00)),
               0xffff'ffff'8000'0000ull);

    // srlw operates on the low 32 bits only: the upper bits of rs1 must not
    // leak into the result.
    CHECK_EQ_U(alu2(0xffff'ffff'8000'0000ull, 28,
                    r_type(opcodes::OP_32, 3, 0x5, 1, 2, 0x00)),
               0x8);
    // sraw sign-fills from bit 31, then the 32-bit result is sign-extended.
    CHECK_EQ_U(alu2(0xffff'ffff'8000'0000ull, 28,
                    r_type(opcodes::OP_32, 3, 0x5, 1, 2, 0x20)),
               0xffff'ffff'ffff'fff8ull);
}

void test_op_imm_32() {
    // addiw
    CHECK_EQ_U(alu1(0x7fff'ffff, i_type(opcodes::OP_IMM_32, 3, 0x0, 1, 1)),
               0xffff'ffff'8000'0000ull);
    // addiw also truncates its source to 32 bits first.
    CHECK_EQ_U(alu1(0xffff'ffff'0000'0005ull, i_type(opcodes::OP_IMM_32, 3, 0x0, 1, 3)), 8);

    // slliw / srliw / sraiw. Shift amount is 5 bits, selector is funct7.
    auto shiftw = [](u32 funct3, u32 funct7, u32 shamt) {
        return i_type(opcodes::OP_IMM_32, 3, funct3, 1,
                      static_cast<i32>((funct7 << 5) | (shamt & 0x1f)));
    };
    CHECK_EQ_U(alu1(1, shiftw(0x1, 0x00, 31)), 0xffff'ffff'8000'0000ull);   // slliw
    CHECK_EQ_U(alu1(0xffff'ffff'8000'0000ull, shiftw(0x5, 0x00, 28)), 0x8); // srliw
    CHECK_EQ_U(alu1(0xffff'ffff'8000'0000ull, shiftw(0x5, 0x20, 28)),
               0xffff'ffff'ffff'fff8ull);                                    // sraiw
}

// --- SYSTEM and MISC-MEM ----------------------------------------------------

void test_fence_is_a_nop() {
    // We execute strictly in order and complete each access before the next
    // instruction, so the ordering FENCE requests already holds.
    Machine m = run({i_type(opcodes::MISC_MEM, 0, 0x0, 0, 0), ADDI(1, 0, 5)}, 2);
    CHECK_EQ_U(m.reg(1), 5);
    CHECK_EQ_U(m.cpu->pc, DRAM_BASE + 8);
}

void test_ecall_and_ebreak_trap() {
    Machine m({i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x000)});
    const Status ec = m.cpu->step();
    CHECK(!ec);
    CHECK(ec.trap.cause == Exception::ECallFromMMode);

    Machine n({i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x001)});
    const Status eb = n.cpu->step();
    CHECK(!eb);
    CHECK(eb.trap.cause == Exception::Breakpoint);
    CHECK_EQ_U(eb.trap.tval, DRAM_BASE);
}

void test_not_yet_implemented_traps_rather_than_misexecuting() {
    // The M extension shares the OP opcode and is selected by funct7 == 0x01.
    // Until phase 3 it must trap, not be mistaken for ADD.
    Machine m({r_type(opcodes::OP, 3, 0x0, 1, 2, 0x01)});  // mul
    const Status st = m.cpu->step();
    CHECK(!st);
    CHECK(st.trap.cause == Exception::IllegalInstruction);

    // CSR instructions share the SYSTEM opcode (funct3 != 0) - phase 2.
    Machine n({i_type(opcodes::SYSTEM, 3, 0x1, 0, 0x300)});  // csrrw
    const Status cs = n.cpu->step();
    CHECK(!cs);
    CHECK(cs.trap.cause == Exception::IllegalInstruction);

    // MRET is SYSTEM/funct3=0 but not ECALL or EBREAK - phase 2.
    Machine o({i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x302)});
    const Status mr = o.cpu->step();
    CHECK(!mr);
    CHECK(mr.trap.cause == Exception::IllegalInstruction);
}

// --- an end-to-end program --------------------------------------------------

void test_sum_loop() {
    // Sum 1..10 with a loop, storing the result to memory and loading it back.
    // Exercises branches, arithmetic, and memory together.
    const u64 addr = DRAM_BASE + 0x800;

    std::vector<u32> p;
    load_imm64(p, 10, addr);
    p.push_back(ADDI(1, 0, 0));                            // sum = 0
    p.push_back(ADDI(2, 0, 1));                            // i = 1
    p.push_back(ADDI(3, 0, 11));                           // limit = 11
    // loop:
    p.push_back(r_type(opcodes::OP, 1, 0x0, 1, 2, 0x00));  // sum += i
    p.push_back(ADDI(2, 2, 1));                            // i += 1
    p.push_back(b_type(opcodes::BRANCH, 0x1, 2, 3, -8));   // bne i, limit, loop
    p.push_back(s_type(opcodes::STORE, 0x3, 10, 1, 0));    // sd sum, (addr)
    p.push_back(i_type(opcodes::LOAD, 4, 0x3, 10, 0));     // ld x4, (addr)
    p.push_back(i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x001));// ebreak - stop here

    // Run with a generous budget: the program terminates itself with EBREAK, so
    // reaching that trap (rather than the budget, or an illegal instruction in
    // the zeroed memory past the program) is itself the check that control flow
    // went where it should.
    Machine m(p);
    const Status st = m.cpu->run(500, nullptr);
    CHECK(!st);
    CHECK(st.trap.cause == Exception::Breakpoint);
    CHECK_EQ_U(m.reg(1), 55);
    CHECK_EQ_U(m.reg(4), 55);
}

}  // namespace

int main() {
    test_load_imm64_helper();
    test_lui();
    test_auipc();
    test_jal();
    test_jalr();
    test_jalr_clears_bit_zero();
    test_jalr_rd_equals_rs1();
    test_misaligned_jump_traps_on_the_jump();
    test_branches();
    test_backward_branch_loop();
    test_store_then_load_all_widths();
    test_load_zero_extension();
    test_store_widths_write_only_their_bytes();
    test_store_load_negative_offset();
    test_faulting_load_leaves_rd_untouched();
    test_op_add_sub();
    test_op_logical_and_compare();
    test_op_shifts_mask_to_six_bits();
    test_addw_sign_extends();
    test_w_shifts_use_five_bits();
    test_op_imm_32();
    test_fence_is_a_nop();
    test_ecall_and_ebreak_trap();
    test_not_yet_implemented_traps_rather_than_misexecuting();
    test_sum_loop();
    return testutil::summary("rv64i");
}
