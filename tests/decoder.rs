//! Decoder tests.
//!
//! Two complementary strategies:
//!
//!  1. Anchors: a handful of instruction words whose encodings are well known and can be
//!     checked against the ISA manual by hand. These catch a systematically wrong encoder.
//!
//!  2. Round trips: independently written encoders (in `common`) paired with the decoder
//!     in src/. Agreement across a range of values - including the sign-bit boundaries
//!     where off-by-one errors live - gives much broader coverage than a fixed list could.

mod common;

use common::*;
use riscv_emu::decoder::{decode, opcodes, Format};

#[test]
fn field_extraction() {
    // addi x1, x0, 5
    let a = decode(0x00500093);
    assert_eq!(a.opcode, opcodes::OP_IMM);
    assert_eq!(a.rd, 1);
    assert_eq!(a.rs1, 0);
    assert_eq!(a.funct3, 0);
    assert_eq!(a.imm, 5);
    assert_eq!(a.fmt, Format::I);

    // add x3, x1, x2 -- R-type, checks rs2/funct7
    let b = decode(0x002081b3);
    assert_eq!(b.opcode, opcodes::OP);
    assert_eq!(b.rd, 3);
    assert_eq!(b.rs1, 1);
    assert_eq!(b.rs2, 2);
    assert_eq!(b.funct3, 0);
    assert_eq!(b.funct7, 0);
    assert_eq!(b.fmt, Format::R);

    // sub x3, x1, x2 -- same fields but funct7 = 0x20.
    assert_eq!(decode(0x402081b3).funct7, 0x20);
}

#[test]
fn i_immediates() {
    // Anchor: addi x6, x0, -3
    assert_eq!(decode(0xffd00313).imm, -3);

    // Round trip across the full signed 12-bit range boundaries.
    for v in [0, 1, -1, 5, -5, 2047, -2048, 1024, -1024] {
        assert_eq!(decode(i_type(opcodes::OP_IMM, 1, 0, 2, v)).imm, v as i64);
    }
}

#[test]
fn s_immediates() {
    // S-type splits the immediate across two disjoint fields, so a decoder that reuses the
    // I-type extraction produces nonsense here.
    for v in [0, 4, 8, 16, -4, -8, 2047, -2048] {
        let d = decode(s_type(opcodes::STORE, 3, 2, 5, v));
        assert_eq!(d.imm, v as i64);
        assert_eq!(d.fmt, Format::S);
        assert_eq!(d.rs1, 2);
        assert_eq!(d.rs2, 5);
    }
}

#[test]
fn b_immediates() {
    // B immediates are always even: bit 0 is not stored.
    for v in [0, 2, 4, 8, -2, -4, -8, 4094, -4096] {
        let d = decode(b_type(opcodes::BRANCH, 0, 1, 2, v));
        assert_eq!(d.imm, v as i64);
        assert_eq!(d.fmt, Format::B);
    }
}

#[test]
fn u_immediates() {
    // lui x1, 0x12345 -- the immediate occupies the upper 20 bits and the low 12 are zero.
    let a = decode(u_type(opcodes::LUI, 1, 0x12345));
    assert_eq!(a.imm as u64, 0x1234_5000);
    assert_eq!(a.fmt, Format::U);

    // Bit 31 set: the result must be sign-extended to 64 bits, not zero-extended.
    let b = decode(u_type(opcodes::LUI, 1, 0x80000));
    assert_eq!(b.imm, -0x8000_0000i64);
    assert_eq!(b.imm as u64, 0xffff_ffff_8000_0000);
}

#[test]
fn j_immediates() {
    for v in [0, 2, 4, 8, -2, -4, 1048574, -1048576] {
        let d = decode(j_type(opcodes::JAL, 1, v));
        assert_eq!(d.imm, v as i64);
        assert_eq!(d.fmt, Format::J);
    }
}

#[test]
fn shift_fields() {
    // On RV64 a shift immediate is 6 bits, and funct6 (not funct7) selects logical vs
    // arithmetic. srli x13, x6, 60:
    let srli = decode(0x03c35693);
    assert_eq!(srli.funct3, 0x5);
    assert_eq!(srli.funct6(), 0x00);
    assert_eq!(srli.shamt6(), 60);

    // srai x14, x6, 60 -- same funct3, funct6 = 0x10.
    let srai = decode(0x43c35713);
    assert_eq!(srai.funct3, 0x5);
    assert_eq!(srai.funct6(), 0x10);
    assert_eq!(srai.shamt6(), 60);

    // A 6-bit shift amount must survive: shifting by 32..63 is legal on RV64 and would be
    // truncated by a 5-bit RV32-style extraction.
    assert_eq!(decode(i_type(opcodes::OP_IMM, 1, 1, 2, 63)).shamt6(), 63);
}

#[test]
fn unknown_opcode() {
    // An all-zero word is not a valid instruction. It must be reported as Unknown so the
    // CPU can trap, rather than being mistaken for something.
    assert_eq!(decode(0x0000_0000).fmt, Format::Unknown);
}
