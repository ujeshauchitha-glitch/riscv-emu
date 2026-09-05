#include "decoder.hpp"
#include "test_util.hpp"

// ---------------------------------------------------------------------------
// Decoder tests.
//
// Two complementary strategies:
//
//  1. Anchors: a handful of instruction words whose encodings are well known
//     and can be checked against the ISA manual by hand. These catch a
//     systematically wrong encoder.
//
//  2. Round trips: independently written encoders here in the test file,
//     paired with the decoder in src/. Agreement across a range of values —
//     including the sign-bit boundaries where off-by-one errors live — gives
//     much broader coverage than a fixed list could.
// ---------------------------------------------------------------------------

namespace {

u32 enc_i(u32 opcode, u32 rd, u32 funct3, u32 rs1, i32 imm) {
    return ((static_cast<u32>(imm) & 0xfff) << 20) | ((rs1 & 0x1f) << 15) |
           ((funct3 & 0x7) << 12) | ((rd & 0x1f) << 7) | (opcode & 0x7f);
}

u32 enc_s(u32 opcode, u32 funct3, u32 rs1, u32 rs2, i32 imm) {
    const u32 i = static_cast<u32>(imm);
    return (((i >> 5) & 0x7f) << 25) | ((rs2 & 0x1f) << 20) | ((rs1 & 0x1f) << 15) |
           ((funct3 & 0x7) << 12) | ((i & 0x1f) << 7) | (opcode & 0x7f);
}

u32 enc_b(u32 opcode, u32 funct3, u32 rs1, u32 rs2, i32 imm) {
    const u32 i = static_cast<u32>(imm);
    return (((i >> 12) & 0x1) << 31) | (((i >> 5) & 0x3f) << 25) | ((rs2 & 0x1f) << 20) |
           ((rs1 & 0x1f) << 15) | ((funct3 & 0x7) << 12) | (((i >> 1) & 0xf) << 8) |
           (((i >> 11) & 0x1) << 7) | (opcode & 0x7f);
}

u32 enc_u(u32 opcode, u32 rd, u32 imm_hi20) {
    return ((imm_hi20 & 0xf'ffff) << 12) | ((rd & 0x1f) << 7) | (opcode & 0x7f);
}

u32 enc_j(u32 opcode, u32 rd, i32 imm) {
    const u32 i = static_cast<u32>(imm);
    return (((i >> 20) & 0x1) << 31) | (((i >> 1) & 0x3ff) << 21) | (((i >> 11) & 0x1) << 20) |
           (((i >> 12) & 0xff) << 12) | ((rd & 0x1f) << 7) | (opcode & 0x7f);
}

void test_field_extraction() {
    // addi x1, x0, 5 -- the one instruction the emulator could already run.
    const DecodedInst a = decode(0x00500093);
    CHECK_EQ(a.opcode, opcodes::OP_IMM);
    CHECK_EQ(a.rd, 1);
    CHECK_EQ(a.rs1, 0);
    CHECK_EQ(a.funct3, 0);
    CHECK_EQ(a.imm, 5);
    CHECK(a.fmt == Format::I);

    // add x3, x1, x2 -- R-type, checks rs2/funct7 which the old decode()
    // extracted but could not pass to execute().
    const DecodedInst b = decode(0x002081b3);
    CHECK_EQ(b.opcode, opcodes::OP);
    CHECK_EQ(b.rd, 3);
    CHECK_EQ(b.rs1, 1);
    CHECK_EQ(b.rs2, 2);
    CHECK_EQ(b.funct3, 0);
    CHECK_EQ(b.funct7, 0);
    CHECK(b.fmt == Format::R);

    // sub x3, x1, x2 -- same fields but funct7 = 0x20.
    const DecodedInst c = decode(0x402081b3);
    CHECK_EQ(c.funct7, 0x20);
}

void test_i_immediates() {
    // Anchor: addi x6, x0, -3
    CHECK_EQ(decode(0xffd00313).imm, -3);

    // Round trip across the full signed 12-bit range boundaries.
    const i32 values[] = {0, 1, -1, 5, -5, 2047, -2048, 1024, -1024};
    for (i32 v : values) {
        const DecodedInst d = decode(enc_i(opcodes::OP_IMM, 1, 0, 2, v));
        CHECK_EQ(d.imm, v);
    }
}

void test_s_immediates() {
    // S-type splits the immediate across two disjoint fields, so a decoder that
    // reuses the I-type extraction produces nonsense here.
    const i32 values[] = {0, 4, 8, 16, -4, -8, 2047, -2048};
    for (i32 v : values) {
        const DecodedInst d = decode(enc_s(opcodes::STORE, 3, 2, 5, v));
        CHECK_EQ(d.imm, v);
        CHECK(d.fmt == Format::S);
        CHECK_EQ(d.rs1, 2);
        CHECK_EQ(d.rs2, 5);
    }
}

void test_b_immediates() {
    // B immediates are always even: bit 0 is not stored.
    const i32 values[] = {0, 2, 4, 8, -2, -4, -8, 4094, -4096};
    for (i32 v : values) {
        const DecodedInst d = decode(enc_b(opcodes::BRANCH, 0, 1, 2, v));
        CHECK_EQ(d.imm, v);
        CHECK(d.fmt == Format::B);
    }
}

void test_u_immediates() {
    // lui x1, 0x12345 -- the immediate occupies the upper 20 bits and the low
    // 12 are zero.
    const DecodedInst a = decode(enc_u(opcodes::LUI, 1, 0x12345));
    CHECK_EQ_U(a.imm, 0x12345000ull);
    CHECK(a.fmt == Format::U);

    // Bit 31 set: the result must be sign-extended to 64 bits, not
    // zero-extended. lui with 0x80000 yields a negative value.
    const DecodedInst b = decode(enc_u(opcodes::LUI, 1, 0x80000));
    CHECK_EQ(b.imm, -static_cast<i64>(0x80000000ll));
    CHECK_EQ_U(static_cast<u64>(b.imm), 0xffff'ffff'8000'0000ull);
}

void test_j_immediates() {
    const i32 values[] = {0, 2, 4, 8, -2, -4, 1048574, -1048576};
    for (i32 v : values) {
        const DecodedInst d = decode(enc_j(opcodes::JAL, 1, v));
        CHECK_EQ(d.imm, v);
        CHECK(d.fmt == Format::J);
    }
}

void test_shift_fields() {
    // On RV64 a shift immediate is 6 bits, and funct6 (not funct7) selects
    // logical vs arithmetic. srli x13, x6, 60:
    const DecodedInst srli = decode(0x03c35693);
    CHECK_EQ(srli.funct3, 0x5);
    CHECK_EQ(srli.funct6(), 0x00);
    CHECK_EQ(srli.shamt6(), 60);

    // srai x14, x6, 60 -- same funct3, funct6 = 0x10.
    const DecodedInst srai = decode(0x43c35713);
    CHECK_EQ(srai.funct3, 0x5);
    CHECK_EQ(srai.funct6(), 0x10);
    CHECK_EQ(srai.shamt6(), 60);

    // A 6-bit shift amount must survive: shifting by 32..63 is legal on RV64
    // and would be truncated by a 5-bit RV32-style extraction.
    CHECK_EQ(decode(enc_i(opcodes::OP_IMM, 1, 1, 2, 63)).shamt6(), 63);
}

void test_unknown_opcode() {
    // An all-zero word is not a valid instruction. It must be reported as
    // Unknown so the CPU can trap, rather than being mistaken for something.
    const DecodedInst d = decode(0x00000000);
    CHECK(d.fmt == Format::Unknown);
}

}  // namespace

int main() {
    test_field_extraction();
    test_i_immediates();
    test_s_immediates();
    test_b_immediates();
    test_u_immediates();
    test_j_immediates();
    test_shift_fields();
    test_unknown_opcode();
    return testutil::summary("decoder");
}
