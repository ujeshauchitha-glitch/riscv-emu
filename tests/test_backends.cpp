#include <array>
#include <random>
#include <vector>

#include "backends.hpp"
#include "machine.hpp"
#include "test_util.hpp"

// ---------------------------------------------------------------------------
// The optional register-file and ALU backends.
//
// What is checked here is the *wiring*: that each instruction reaches the ALU
// backend with the operation and operands it should, that a backend really
// does control the outcome, and that a pass-through backend leaves a program's
// result identical to the plain core. Whether the plain ALU itself is right is
// the job of the existing suites and riscv-tests; the default path runs the
// same alu_reference() code the pass-through backend does.
// ---------------------------------------------------------------------------

using namespace rvt;

namespace {

struct Call {
    AluOp op;
    u64   a, b;
};

// Records every call, and answers with the reference ALU.
struct RecordingAlu : AluBackend {
    std::vector<Call> log;
    u64 exec(AluOp op, u64 a, u64 b) override {
        log.push_back({op, a, b});
        return alu_reference(op, a, b);
    }
};

// A register file that remembers every access and refuses x0.
struct CheckedRegFile : RegFileBackend {
    std::array<u64, NUM_REGS> r{};
    int  bad_index = 0;
    long reads = 0, writes = 0;
    u64 read(u32 i) override {
        if (i == 0 || i >= NUM_REGS) ++bad_index;
        ++reads;
        return r[i % NUM_REGS];
    }
    void write(u32 i, u64 v) override {
        if (i == 0 || i >= NUM_REGS) ++bad_index;
        ++writes;
        r[i % NUM_REGS] = v;
    }
};

const std::array<u64, 12> kEdges = {
    0, 1, 2, 31, 32, 63, 0x7fff'ffffull, 0x8000'0000ull,
    0xffff'ffffull, 0x7fff'ffff'ffff'ffffull, 0x8000'0000'0000'0000ull,
    0xffff'ffff'ffff'ffffull};

u64 pick(std::mt19937_64& rng) {
    return (rng() % 3 == 0) ? kEdges[rng() % kEdges.size()] : rng();
}

// x1 = a, x2 = b, then `inst`, executing exactly that many steps.
struct OneShot {
    Machine      m;
    RecordingAlu alu;
    OneShot(u64 a, u64 b, u32 inst, std::vector<u32> tail = {})
        : m(build(a, b, inst, tail)) {
        m.cpu->alu_backend = &alu;
        m.cpu->run(kLoadImm64Steps * 2 + 1, nullptr);
    }
    static std::vector<u32> build(u64 a, u64 b, u32 inst, const std::vector<u32>& tail) {
        std::vector<u32> p;
        load_imm64(p, 1, a);
        load_imm64(p, 2, b);
        p.push_back(inst);
        p.insert(p.end(), tail.begin(), tail.end());
        return p;
    }
    // The operation the instruction under test sent to the ALU: the last call.
    const Call& last() const { return alu.log.back(); }
};

struct RForm { u32 opcode, f3, f7; AluOp op; };
const RForm kRForms[] = {
    {opcodes::OP, 0, 0x00, AluOp::Add},   {opcodes::OP, 0, 0x20, AluOp::Sub},
    {opcodes::OP, 1, 0x00, AluOp::Sll},   {opcodes::OP, 2, 0x00, AluOp::Slt},
    {opcodes::OP, 3, 0x00, AluOp::Sltu},  {opcodes::OP, 4, 0x00, AluOp::Xor},
    {opcodes::OP, 5, 0x00, AluOp::Srl},   {opcodes::OP, 5, 0x20, AluOp::Sra},
    {opcodes::OP, 6, 0x00, AluOp::Or},    {opcodes::OP, 7, 0x00, AluOp::And},
    {opcodes::OP_32, 0, 0x00, AluOp::AddW}, {opcodes::OP_32, 0, 0x20, AluOp::SubW},
    {opcodes::OP_32, 1, 0x00, AluOp::SllW}, {opcodes::OP_32, 5, 0x00, AluOp::SrlW},
    {opcodes::OP_32, 5, 0x20, AluOp::SraW},
};

void test_register_register_forms_reach_the_alu() {
    std::mt19937_64 rng(1);
    for (const RForm& f : kRForms) {
        for (int i = 0; i < 20; ++i) {
            const u64 a = pick(rng), b = pick(rng);
            OneShot t(a, b, r_type(f.opcode, 3, f.f3, 1, 2, f.f7));
            CHECK_EQ(static_cast<int>(t.last().op), static_cast<int>(f.op));
            CHECK_EQ_U(t.last().a, a);
            CHECK_EQ_U(t.last().b, b);
            CHECK_EQ_U(t.m.reg(3), alu_reference(f.op, a, b));
        }
    }
}

struct IForm { u32 opcode, f3; i32 imm; AluOp op; u64 expect_b; };

void test_immediate_forms_reach_the_alu() {
    std::mt19937_64 rng(2);
    for (int i = 0; i < 40; ++i) {
        const u64 a = pick(rng);
        const i32 imm = static_cast<i32>(rng() % 4096) - 2048;
        const u32 sh6 = rng() % 64, sh5 = rng() % 32;
        const IForm forms[] = {
            {opcodes::OP_IMM, 0, imm, AluOp::Add, static_cast<u64>(static_cast<i64>(imm))},
            {opcodes::OP_IMM, 2, imm, AluOp::Slt, static_cast<u64>(static_cast<i64>(imm))},
            {opcodes::OP_IMM, 3, imm, AluOp::Sltu, static_cast<u64>(static_cast<i64>(imm))},
            {opcodes::OP_IMM, 4, imm, AluOp::Xor, static_cast<u64>(static_cast<i64>(imm))},
            {opcodes::OP_IMM, 6, imm, AluOp::Or, static_cast<u64>(static_cast<i64>(imm))},
            {opcodes::OP_IMM, 7, imm, AluOp::And, static_cast<u64>(static_cast<i64>(imm))},
            {opcodes::OP_IMM, 1, static_cast<i32>(sh6), AluOp::Sll, sh6},
            {opcodes::OP_IMM, 5, static_cast<i32>(sh6), AluOp::Srl, sh6},
            {opcodes::OP_IMM, 5, static_cast<i32>(0x400 | sh6), AluOp::Sra, sh6},
            {opcodes::OP_IMM_32, 0, imm, AluOp::AddW, static_cast<u64>(static_cast<i64>(imm))},
            {opcodes::OP_IMM_32, 1, static_cast<i32>(sh5), AluOp::SllW, sh5},
            {opcodes::OP_IMM_32, 5, static_cast<i32>(sh5), AluOp::SrlW, sh5},
            {opcodes::OP_IMM_32, 5, static_cast<i32>(0x400 | sh5), AluOp::SraW, sh5},
        };
        for (const IForm& f : forms) {
            OneShot t(a, 0, i_type(f.opcode, 3, f.f3, 1, f.imm));
            CHECK_EQ(static_cast<int>(t.last().op), static_cast<int>(f.op));
            CHECK_EQ_U(t.last().a, a);
            CHECK_EQ_U(t.last().b, f.expect_b);
            CHECK_EQ_U(t.m.reg(3), alu_reference(f.op, a, f.expect_b));
        }
    }
}

void test_branches_compare_through_the_alu() {
    struct BForm { u32 f3; AluOp op; bool (*taken)(u64, u64); };
    const BForm forms[] = {
        {0, AluOp::Xor,  [](u64 a, u64 b) { return a == b; }},
        {1, AluOp::Xor,  [](u64 a, u64 b) { return a != b; }},
        {4, AluOp::Slt,  [](u64 a, u64 b) { return (i64)a < (i64)b; }},
        {5, AluOp::Slt,  [](u64 a, u64 b) { return (i64)a >= (i64)b; }},
        {6, AluOp::Sltu, [](u64 a, u64 b) { return a < b; }},
        {7, AluOp::Sltu, [](u64 a, u64 b) { return a >= b; }},
    };
    std::mt19937_64 rng(3);
    const u64 pc_of_branch = DRAM_BASE + 4 * (kLoadImm64Steps * 2);
    for (const BForm& f : forms) {
        for (int i = 0; i < 40; ++i) {
            // Half the draws make the operands equal so BEQ/BNE see both outcomes.
            const u64 a = pick(rng);
            const u64 b = (i % 2) ? a : pick(rng);
            OneShot t(a, b, b_type(opcodes::BRANCH, f.f3, 1, 2, 8));
            CHECK_EQ(static_cast<int>(t.last().op), static_cast<int>(f.op));
            CHECK_EQ_U(t.last().a, a);
            CHECK_EQ_U(t.last().b, b);
            CHECK_EQ_U(t.m.cpu->pc, pc_of_branch + (f.taken(a, b) ? 8 : 4));
        }
    }
}

void test_address_generation_uses_the_alu_add() {
    const u64 base = DRAM_BASE + 0x1000;
    {
        OneShot t(base, 0, i_type(opcodes::LOAD, 3, 3, 1, -16));  // LD x3, -16(x1)
        CHECK_EQ(static_cast<int>(t.last().op), static_cast<int>(AluOp::Add));
        CHECK_EQ_U(t.last().a, base);
        CHECK_EQ_U(t.last().b, static_cast<u64>(-16));
    }
    {
        OneShot t(base, 0x1122, s_type(opcodes::STORE, 3, 1, 2, 24));  // SD x2, 24(x1)
        CHECK_EQ(static_cast<int>(t.last().op), static_cast<int>(AluOp::Add));
        CHECK_EQ_U(t.last().a, base);
        CHECK_EQ_U(t.last().b, 24);
    }
    {
        OneShot t(base, 0, i_type(opcodes::JALR, 3, 0, 1, 5));  // JALR x3, 5(x1)
        CHECK_EQ(static_cast<int>(t.last().op), static_cast<int>(AluOp::Add));
        CHECK_EQ_U(t.last().a, base);
        CHECK_EQ_U(t.last().b, 5);
        CHECK_EQ_U(t.m.cpu->pc, (base + 5) & ~1ull);  // bit 0 still cleared after the add
    }
}

// A backend decides the result: an ALU that corrupts bit 0 of every Add must
// change what ADDI leaves in the register.
void test_backend_controls_the_outcome() {
    struct Flip : AluBackend {
        u64 exec(AluOp op, u64 a, u64 b) override {
            const u64 r = alu_reference(op, a, b);
            return op == AluOp::Add ? r ^ 1 : r;
        }
    } flip;
    // One instruction only: two flipped adds would cancel each other out.
    std::vector<u32> p = {ADDI(1, 0, 100)};
    Machine plain(p), bent(p);
    bent.cpu->alu_backend = &flip;
    plain.cpu->run(1, nullptr);
    bent.cpu->run(1, nullptr);
    CHECK_EQ_U(plain.reg(1), 100);
    CHECK_EQ_U(bent.reg(1), 101);
}

// A random straight-line program over x1..x7, run on the plain core and on a
// core with both backends installed as pass-throughs.
std::vector<u32> random_program(std::mt19937_64& rng, int n) {
    std::vector<u32> p;
    for (u32 r = 1; r <= 7; ++r) load_imm64(p, r, pick(rng));
    for (int i = 0; i < n; ++i) {
        const u32 rd = 1 + rng() % 7, rs1 = 1 + rng() % 7, rs2 = 1 + rng() % 7;
        if (rng() % 2) {
            const RForm& f = kRForms[rng() % (sizeof(kRForms) / sizeof(kRForms[0]))];
            p.push_back(r_type(f.opcode, rd, f.f3, rs1, rs2, f.f7));
        } else {
            const i32 imm = static_cast<i32>(rng() % 4096) - 2048;
            const u32 f3s[] = {0, 2, 3, 4, 6, 7};
            p.push_back(i_type(opcodes::OP_IMM, rd, f3s[rng() % 6], rs1, imm));
        }
    }
    return p;
}

void test_passthrough_backends_match_the_plain_core() {
    std::mt19937_64 rng(4);
    for (int trial = 0; trial < 50; ++trial) {
        const std::vector<u32> prog = random_program(rng, 200);
        const u64 steps = prog.size();

        Machine plain(prog), backed(prog);
        RecordingAlu   alu;
        CheckedRegFile rf;
        backed.cpu->alu_backend = &alu;
        backed.cpu->regfile     = &rf;

        plain.cpu->run(steps, nullptr);
        backed.cpu->run(steps, nullptr);

        for (u32 r = 0; r < NUM_REGS; ++r) CHECK_EQ_U(backed.reg(r), plain.reg(r));
        CHECK_EQ_U(backed.cpu->pc, plain.cpu->pc);
        CHECK_EQ(rf.bad_index, 0);  // the backend never sees x0 or an out-of-range index
        CHECK(rf.reads > 0 && rf.writes > 0);
        CHECK(!alu.log.empty());
        // While a backend is installed the CPU's own array is not touched.
        for (u32 r = 0; r < NUM_REGS; ++r) CHECK_EQ_U(backed.cpu->regs[r], 0);
    }
}

void test_x0_never_reaches_the_register_file_backend() {
    Machine m({ADDI(0, 0, 77), ADDI(1, 0, 5)});
    CheckedRegFile rf;
    m.cpu->regfile = &rf;
    m.cpu->run(2, nullptr);
    CHECK_EQ(rf.bad_index, 0);
    CHECK_EQ_U(m.reg(0), 0);
    CHECK_EQ_U(m.reg(1), 5);
}

}  // namespace

int main() {
    test_register_register_forms_reach_the_alu();
    test_immediate_forms_reach_the_alu();
    test_branches_compare_through_the_alu();
    test_address_generation_uses_the_alu_add();
    test_backend_controls_the_outcome();
    test_passthrough_backends_match_the_plain_core();
    test_x0_never_reaches_the_register_file_backend();
    return testutil::summary("backends");
}
