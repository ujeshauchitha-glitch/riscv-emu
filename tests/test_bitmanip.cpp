#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "bitmanip.hpp"
#include "machine.hpp"
#include "test_util.hpp"

// ---------------------------------------------------------------------------
// Zba, Zbb, Zbc, Zbs and Zicond.
//
// Two layers of checking, because each catches what the other cannot:
//
//   1. Hand-computed cases with real assembler encodings. These pin the meaning
//      of a few instructions against numbers worked out on paper and against
//      encodings taken from actual toolchain output, so a mistake in how the
//      decoder reads an encoding cannot hide behind a model that shares it.
//
//   2. A differential run against tests/gen_bitmanip_vectors.py, an independent
//      model that produces tens of thousands of random and edge-case
//      instruction/operand/result triples. The vectors are generated at build
//      time; run this program with the path to the file as its argument.
// ---------------------------------------------------------------------------

using namespace rvt;

namespace {

// Execute one instruction with a0..: x11 = rs1, x12 = rs2, result in x10.
struct Exec {
    Machine m{std::vector<u32>{HALT()}};

    Status run(u32 insn, u64 rs1, u64 rs2, u64* rd = nullptr) {
        m.dram->store(0, 4, insn);
        m.cpu->pc = DRAM_BASE;
        m.cpu->write_reg(10, 0xdeadbeefcafef00dull);   // so an instruction that forgets to write rd is noticed
        m.cpu->write_reg(11, rs1);
        m.cpu->write_reg(12, rs2);
        Status st = m.cpu->step();
        if (rd) *rd = m.cpu->read_reg(10);
        return st;
    }
};

// --- known encodings ---------------------------------------------------------------------

void test_encodings_match_real_assembler_output() {
    Exec e;
    u64 rd;

    // These words are what a RISC-V assembler emits for the named instruction
    // with a0 as destination and a1, a2 as sources.
    CHECK(e.run(0x20c5a533, 5, 100, &rd));  CHECK_EQ_U(rd, (5u << 1) + 100);        // sh1add
    CHECK(e.run(0x20c5c533, 5, 100, &rd));  CHECK_EQ_U(rd, (5u << 2) + 100);        // sh2add
    CHECK(e.run(0x20c5e533, 5, 100, &rd));  CHECK_EQ_U(rd, (5u << 3) + 100);        // sh3add
    CHECK(e.run(0x08c5853b, 0xffffffff00000007ull, 1, &rd));
    CHECK_EQ_U(rd, 8);                                                               // add.uw drops rs1's upper half: 7 + 1
    CHECK(e.run(0x60059513, 0x0000000000f00000ull, 0, &rd));  CHECK_EQ_U(rd, 40);   // clz
    CHECK(e.run(0x6b85d513, 0x0102030405060708ull, 0, &rd));
    CHECK_EQ_U(rd, 0x0807060504030201ull);                                           // rev8
    CHECK(e.run(0x0ec5d533, 77, 0, &rd));   CHECK_EQ_U(rd, 0);                       // czero.eqz, condition zero
    CHECK(e.run(0x0ec5d533, 77, 1, &rd));   CHECK_EQ_U(rd, 77);                      // condition non-zero
}

// --- hand-computed arithmetic ----------------------------------------------------------------

void test_counting_and_byte_operations() {
    using namespace bitmanip;
    CHECK_EQ_U(clz64(0), 64);
    CHECK_EQ_U(clz64(1), 63);
    CHECK_EQ_U(clz64(~0ull), 0);
    CHECK_EQ_U(ctz64(0), 64);
    CHECK_EQ_U(ctz64(0x8000000000000000ull), 63);
    CHECK_EQ_U(cpop64(0xff00ff00ff00ff00ull), 32);
    // The 32-bit forms count within the low word only.
    CHECK_EQ_U(clz32(0xffffffff00000001ull), 31);
    CHECK_EQ_U(ctz32(0xffffffff00000000ull), 32);
    CHECK_EQ_U(cpop32(0xffffffff0000000full), 4);

    CHECK_EQ_U(rev8(0x0102030405060708ull), 0x0807060504030201ull);
    CHECK_EQ_U(orc_b(0x0100ff0000800000ull), 0xff00ff0000ff0000ull);
    CHECK_EQ_U(orc_b(0), 0);
}

void test_carryless_multiply() {
    using namespace bitmanip;
    // (x + 1) * (x + 1) = x^2 + 1 over GF(2): no carry, no middle term.
    CHECK_EQ_U(clmul(3, 3), 5);
    CHECK_EQ_U(clmul(0, 0xdeadbeef), 0);
    CHECK_EQ_U(clmul(1, 0xdeadbeefcafef00dull), 0xdeadbeefcafef00dull);   // multiplying by 1 is the identity
    // x^63 * x^63 = x^126: bit 62 of the high half, and bit 63 of the "reversed" product.
    CHECK_EQ_U(clmulh(1ull << 63, 1ull << 63), 1ull << 62);
    CHECK_EQ_U(clmulr(1ull << 63, 1ull << 63), 1ull << 63);
    CHECK_EQ_U(clmul(1ull << 63, 1ull << 63), 0);
}

void test_rotates_and_word_forms() {
    using namespace bitmanip;
    CHECK_EQ_U(rol64(0x8000000000000001ull, 1), 3);
    CHECK_EQ_U(ror64(1, 1), 0x8000000000000000ull);
    CHECK_EQ_U(rol64(0x123, 64), 0x123);              // a rotate by the width is no rotate (and no UB)
    CHECK_EQ_U(rol32(0x80000001u, 1), 3);
    CHECK_EQ_U(ror32(1u, 1), 0x80000000u);

    // ROLW/RORW sign-extend their 32-bit result.
    Exec e;
    u64 rd;
    CHECK(e.run(0x60c5953b, 0x80000001, 1, &rd));      // rolw a0,a1,a2
    CHECK_EQ_U(rd, 3);
    CHECK(e.run(0x60c5d53b, 1, 1, &rd));               // rorw a0,a1,a2
    CHECK_EQ_U(rd, 0xffffffff80000000ull);
    // The upper half of rs1 is ignored by the word forms.
    CHECK(e.run(0x60c5953b, 0xdeadbeef80000001ull, 1, &rd));
    CHECK_EQ_U(rd, 3);
}

void test_extensions_and_zeroing() {
    Exec e;
    u64 rd;
    CHECK(e.run(0x60459513, 0x80, 0, &rd));  CHECK_EQ_U(rd, 0xffffffffffffff80ull);   // sext.b
    CHECK(e.run(0x60559513, 0x8000, 0, &rd)); CHECK_EQ_U(rd, 0xffffffffffff8000ull);  // sext.h
    CHECK(e.run(0x0805c53b, 0x12345678abcdull, 0, &rd)); CHECK_EQ_U(rd, 0xabcd);      // zext.h a0,a1
    // czero.nez is the mirror image.
    CHECK(e.run(0x0ec5f533, 77, 0, &rd));   CHECK_EQ_U(rd, 77);
    CHECK(e.run(0x0ec5f533, 77, 5, &rd));   CHECK_EQ_U(rd, 0);
}

void test_reserved_neighbours_still_trap() {
    // Encodings next to the new ones, which no extension defines, must still be
    // illegal: adding instructions must not turn unallocated space into NOPs.
    Exec e;
    // OP-IMM funct3=1 with imm12 = 0x603 (between cpop and sext.b): reserved.
    CHECK(!e.run(0x60359513, 1, 0));
    // A shift-immediate with a reserved function code: funct6 = 0x2a on SLLI's funct3.
    CHECK(!e.run(0xa0059513, 1, 0));
    // OP with funct7 = 0x07 (Zicond's) but funct3 = 4: not an instruction.
    CHECK(!e.run(0x0ec5c533, 1, 1));
    // ZEXT.H's encoding with a non-zero rs2 is PACK (Zbkb), which is not implemented.
    CHECK(!e.run(0x08c5c53b, 1, 1));
}

void test_misa_advertises_the_b_extension() {
    Machine m({HALT()});
    CHECK((m.cpu->csrs.read(csr::MISA) & (1ull << ('B' - 'A'))) != 0);
}

// --- the differential run --------------------------------------------------------------------

struct Totals { std::size_t run = 0, bad = 0; };

void run_vectors(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "test_bitmanip: cannot open vector file %s\n", path.c_str());
        testutil::report_failure(__FILE__, __LINE__, "vector file missing");
        return;
    }

    Exec e;
    Totals t;
    std::map<std::string, std::size_t> per_instruction;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream in(line);
        std::string insn_s, rs1_s, rs2_s, want_s, name;
        in >> insn_s >> rs1_s >> rs2_s >> want_s >> name;
        const u32 insn = static_cast<u32>(std::strtoull(insn_s.c_str(), nullptr, 16));
        const u64 rs1  = std::strtoull(rs1_s.c_str(), nullptr, 16);
        const u64 rs2  = std::strtoull(rs2_s.c_str(), nullptr, 16);
        const u64 want = std::strtoull(want_s.c_str(), nullptr, 16);

        u64 got = 0;
        Status st = e.run(insn, rs1, rs2, &got);
        ++t.run;
        ++per_instruction[name];
        ++testutil::g_checks;

        // The instruction must also leave its sources alone.
        const bool sources_intact = e.m.cpu->read_reg(11) == rs1 && e.m.cpu->read_reg(12) == rs2;
        if (!st || got != want || !sources_intact) {
            ++t.bad;
            if (t.bad <= 20) {
                std::fprintf(stderr,
                             "MISMATCH %-10s insn=%08x rs1=%016llx rs2=%016llx: got %016llx want %016llx%s%s\n",
                             name.c_str(), insn, static_cast<unsigned long long>(rs1),
                             static_cast<unsigned long long>(rs2), static_cast<unsigned long long>(got),
                             static_cast<unsigned long long>(want), st ? "" : " (trapped)",
                             sources_intact ? "" : " (clobbered a source)");
            }
        }
    }

    std::fprintf(stderr, "test_bitmanip: %zu vectors across %zu instructions, %zu mismatches\n", t.run,
                 per_instruction.size(), t.bad);
    if (t.bad != 0) testutil::report_failure(__FILE__, __LINE__, "differential mismatches");
    // Guard against the generator silently producing nothing.
    CHECK(t.run > 10000);
    CHECK(per_instruction.size() >= 40);
}

}  // namespace

int main(int argc, char** argv) {
    test_encodings_match_real_assembler_output();
    test_counting_and_byte_operations();
    test_carryless_multiply();
    test_rotates_and_word_forms();
    test_extensions_and_zeroing();
    test_reserved_neighbours_still_trap();
    test_misa_advertises_the_b_extension();
    if (argc > 1) run_vectors(argv[1]);
    else std::fprintf(stderr, "test_bitmanip: no vector file given; skipping the differential run\n");
    return testutil::summary("bitmanip");
}
