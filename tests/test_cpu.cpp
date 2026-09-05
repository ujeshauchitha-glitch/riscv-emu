#include <memory>
#include <vector>

#include "bus.hpp"
#include "cpu.hpp"
#include "dram.hpp"
#include "test_util.hpp"

// ---------------------------------------------------------------------------
// CPU tests.
//
// These cover the three behavioural fixes this phase makes to the committed
// code:
//
//   * OP-IMM dispatches on funct3, so SLTI/SLTIU/XORI/ORI/ANDI/SLLI/SRLI/SRAI
//     do their own jobs instead of all performing an addition.
//   * An unrecognised instruction raises IllegalInstruction.
//   * The PC does not advance when an instruction traps.
// ---------------------------------------------------------------------------

namespace {

constexpr u64 kTestDramSize = 1024 * 1024;

// A machine wired up for a single test, with a program loaded at DRAM_BASE.
struct Machine {
    Bus                  bus;
    std::unique_ptr<Cpu> cpu;

    explicit Machine(const std::vector<u32>& program) {
        auto dram     = std::make_unique<Dram>(kTestDramSize);
        Dram* dram_ptr = dram.get();
        bus.attach(std::move(dram));

        std::vector<u8> bytes;
        bytes.reserve(program.size() * 4);
        for (u32 w : program) {
            bytes.push_back(static_cast<u8>(w & 0xff));
            bytes.push_back(static_cast<u8>((w >> 8) & 0xff));
            bytes.push_back(static_cast<u8>((w >> 16) & 0xff));
            bytes.push_back(static_cast<u8>((w >> 24) & 0xff));
        }
        dram_ptr->load_image(DRAM_BASE, bytes);

        cpu = std::make_unique<Cpu>(bus);
    }
};

u32 enc_i(u32 opcode, u32 rd, u32 funct3, u32 rs1, i32 imm) {
    return ((static_cast<u32>(imm) & 0xfff) << 20) | ((rs1 & 0x1f) << 15) |
           ((funct3 & 0x7) << 12) | ((rd & 0x1f) << 7) | (opcode & 0x7f);
}

// Build an OP-IMM shift: the 12-bit immediate field is funct6 in its top 6 bits
// and the 6-bit shift amount below it.
u32 enc_shift(u32 rd, u32 funct3, u32 rs1, u32 funct6, u32 shamt) {
    const u32 imm_field = ((funct6 & 0x3f) << 6) | (shamt & 0x3f);
    return (imm_field << 20) | ((rs1 & 0x1f) << 15) | ((funct3 & 0x7) << 12) |
           ((rd & 0x1f) << 7) | opcodes::OP_IMM;
}

void test_x0_is_hardwired_to_zero() {
    Machine m({enc_i(opcodes::OP_IMM, 0, 0, 0, 42)});  // addi x0, x0, 42
    CHECK(m.cpu->step());
    CHECK_EQ_U(m.cpu->read_reg(0), 0);

    // Writes through the API are discarded too.
    m.cpu->write_reg(0, 0xdead);
    CHECK_EQ_U(m.cpu->read_reg(0), 0);
    CHECK_EQ_U(m.cpu->regs[0], 0);
}

// The core regression test for this phase. Each of these eight instructions has
// a distinct funct3; the previous implementation ran ADDI for all of them.
void test_op_imm_dispatches_on_funct3() {
    // Set up x1 = 5 and x2 = -3, then run one OP-IMM per case.
    const u32 set_x1 = enc_i(opcodes::OP_IMM, 1, 0, 0, 5);   // addi x1, x0, 5
    const u32 set_x2 = enc_i(opcodes::OP_IMM, 2, 0, 0, -3);  // addi x2, x0, -3

    struct Case {
        const char* name;
        u32         inst;
        u32         rd;
        u64         expected;
    };

    const Case cases[] = {
        // ADDI x3, x1, 3 -> 8. (Also the control: this one was always right.)
        {"addi", enc_i(opcodes::OP_IMM, 3, 0x0, 1, 3), 3, 8},

        // SLTI x3, x2, 0 -> 1, because -3 < 0 as a signed comparison.
        // As ADDI this would have produced -3.
        {"slti", enc_i(opcodes::OP_IMM, 3, 0x2, 2, 0), 3, 1},

        // SLTIU x3, x2, 0 -> 0. The immediate is sign-extended and *then*
        // compared unsigned, so this asks "is 0xFFFF...FD < 0", which is false.
        {"sltiu", enc_i(opcodes::OP_IMM, 3, 0x3, 2, 0), 3, 0},

        // SLTIU x3, x1, -1 -> 1. Sign-extension matters: the comparison is
        // against 0xFFFF_FFFF_FFFF_FFFF, not against 0xFFF.
        {"sltiu-sext", enc_i(opcodes::OP_IMM, 3, 0x3, 1, -1), 3, 1},

        // XORI x3, x1, 15 -> 5 ^ 15 = 10. As ADDI this would be 20.
        {"xori", enc_i(opcodes::OP_IMM, 3, 0x4, 1, 15), 3, 10},

        // ORI x3, x1, 8 -> 5 | 8 = 13. As ADDI this would also be 13, so use a
        // value where they differ instead.
        {"ori", enc_i(opcodes::OP_IMM, 3, 0x6, 1, 12), 3, 13},  // 5|12=13, 5+12=17

        // ANDI x3, x1, 6 -> 5 & 6 = 4. As ADDI this would be 11.
        {"andi", enc_i(opcodes::OP_IMM, 3, 0x7, 1, 6), 3, 4},

        // SLLI x3, x1, 4 -> 5 << 4 = 80. As ADDI this would be 9.
        {"slli", enc_shift(3, 0x1, 1, 0x00, 4), 3, 80},

        // SRLI x3, x2, 60 -> logical shift of 0xFFFF_FFFF_FFFF_FFFD, zeros
        // shifted in, giving 0xF.
        {"srli", enc_shift(3, 0x5, 2, 0x00, 60), 3, 0xf},

        // SRAI x3, x2, 60 -> arithmetic shift, sign bits shifted in, giving -1.
        // Distinguishing this from SRLI requires funct6, not funct7.
        {"srai", enc_shift(3, 0x5, 2, 0x10, 60), 3, 0xffff'ffff'ffff'ffffull},
    };

    for (const Case& c : cases) {
        Machine m({set_x1, set_x2, c.inst});
        CHECK(m.cpu->step());
        CHECK(m.cpu->step());
        const Status st = m.cpu->step();
        if (!st) {
            testutil::report_failure(__FILE__, __LINE__,
                                     std::string(c.name) + " trapped unexpectedly");
            continue;
        }
        ++testutil::g_checks;
        if (m.cpu->read_reg(c.rd) != c.expected) {
            testutil::report_failure(
                __FILE__, __LINE__,
                std::string(c.name) + ": got " + testutil::hex(m.cpu->read_reg(c.rd)) +
                    ", expected " + testutil::hex(c.expected));
        }
    }
}

void test_addi_wraps_without_trapping() {
    // Overflow in ADDI is defined to wrap; it must never trap.
    Machine m({
        enc_i(opcodes::OP_IMM, 1, 0, 0, -1),  // addi x1, x0, -1  -> 0xFFFF...FF
        enc_i(opcodes::OP_IMM, 2, 0, 1, 1),   // addi x2, x1, 1   -> wraps to 0
    });
    CHECK(m.cpu->step());
    CHECK(m.cpu->step());
    CHECK_EQ_U(m.cpu->read_reg(1), 0xffff'ffff'ffff'ffffull);
    CHECK_EQ_U(m.cpu->read_reg(2), 0);
}

void test_shift_uses_six_bits_on_rv64() {
    // Shifting by 32 or more is meaningful on RV64 and must not be truncated to
    // 5 bits the way RV32 would.
    Machine m({
        enc_i(opcodes::OP_IMM, 1, 0, 0, 1),    // addi x1, x0, 1
        enc_shift(2, 0x1, 1, 0x00, 40),        // slli x2, x1, 40
    });
    CHECK(m.cpu->step());
    CHECK(m.cpu->step());
    CHECK_EQ_U(m.cpu->read_reg(2), 1ull << 40);
}

void test_unknown_instruction_traps() {
    Machine m({0x00000000});  // not a valid instruction

    const Status st = m.cpu->step();
    CHECK(!st);
    CHECK(st.trap.cause == Exception::IllegalInstruction);
    // tval carries the offending instruction bits, per the privileged spec.
    CHECK_EQ_U(st.trap.tval, 0x00000000);
}

// The other half of the same fix: a trapping instruction must leave the PC
// where it is. The old code advanced past unknown opcodes, so a wrong jump
// would grind through unmapped memory instead of stopping at the mistake.
void test_pc_does_not_advance_on_trap() {
    Machine m({
        enc_i(opcodes::OP_IMM, 1, 0, 0, 1),  // addi x1, x0, 1  (ok)
        0xffffffff,                          // illegal
    });

    CHECK(m.cpu->step());
    CHECK_EQ_U(m.cpu->pc, DRAM_BASE + 4);

    const Status st = m.cpu->step();
    CHECK(!st);
    CHECK_EQ_U(m.cpu->pc, DRAM_BASE + 4);  // still on the faulting instruction
    CHECK_EQ_U(m.cpu->instret, 1);         // and it did not count as retired
}

void test_reserved_shift_encoding_is_illegal() {
    // funct6 must be 0x00 for SLLI. Any other value is reserved and must trap
    // rather than being executed as a shift.
    Machine m({enc_shift(1, 0x1, 0, 0x20, 4)});
    const Status st = m.cpu->step();
    CHECK(!st);
    CHECK(st.trap.cause == Exception::IllegalInstruction);
}

void test_misaligned_fetch_traps() {
    // IALIGN is 16 bits with the C extension, so only an odd PC is misaligned.
    // No jump can produce one - JALR clears bit 0 and the other transfers have
    // it hardwired to zero - so this is a backstop for a PC set some other way,
    // which is exactly what the test does.
    Machine m({enc_i(opcodes::OP_IMM, 1, 0, 0, 1)});
    m.cpu->pc = DRAM_BASE + 1;

    const Status st = m.cpu->step();
    CHECK(!st);
    CHECK(st.trap.cause == Exception::InstructionAddressMisaligned);
    CHECK_EQ_U(st.trap.tval, DRAM_BASE + 1);
}

void test_fetch_from_unmapped_memory_traps() {
    Machine m({enc_i(opcodes::OP_IMM, 1, 0, 0, 1)});
    m.cpu->pc = 0x1000;  // below DRAM, nothing mapped there

    const Status st = m.cpu->step();
    CHECK(!st);
    // Fetch failures are InstructionAccessFault, not LoadAccessFault.
    CHECK(st.trap.cause == Exception::InstructionAccessFault);
}

void test_run_stops_at_first_trap() {
    Machine m({
        enc_i(opcodes::OP_IMM, 1, 0, 0, 1),
        enc_i(opcodes::OP_IMM, 2, 0, 0, 2),
        0xffffffff,
        enc_i(opcodes::OP_IMM, 3, 0, 0, 3),  // must never execute
    });

    u64          retired = 0;
    const Status st      = m.cpu->run(100, &retired);
    CHECK(!st);
    CHECK_EQ_U(retired, 2);
    CHECK_EQ_U(m.cpu->read_reg(3), 0);
}

}  // namespace

int main() {
    test_x0_is_hardwired_to_zero();
    test_op_imm_dispatches_on_funct3();
    test_addi_wraps_without_trapping();
    test_shift_uses_six_bits_on_rv64();
    test_unknown_instruction_traps();
    test_pc_does_not_advance_on_trap();
    test_reserved_shift_encoding_is_illegal();
    test_misaligned_fetch_traps();
    test_fetch_from_unmapped_memory_traps();
    test_run_stops_at_first_trap();
    return testutil::summary("cpu");
}
