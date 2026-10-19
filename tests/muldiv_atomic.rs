//! M and A extension tests.
//!
//! The M extension's interesting behaviour is entirely in its edge cases: RISC-V division
//! never traps, so divide-by-zero and signed overflow have specific defined results that must
//! be produced exactly.
//!
//! The A extension's interesting behaviour is the LR/SC reservation, including the case a
//! single-hart emulator could easily get wrong: a trap between the LR and the SC must make the
//! SC fail.

mod common;

use common::*;
use riscv_emu::csr::csr;
use riscv_emu::decoder::opcodes;
use riscv_emu::trap::Exception;
use riscv_emu::types::DRAM_BASE;

const U64_MAX: u64 = u64::MAX;
const I64_MIN: u64 = 0x8000_0000_0000_0000;

/// x1 = a, x2 = b, then one M-extension instruction into x3.
fn m_op(a: u64, b: u64, opcode: u32, funct3: u32) -> u64 {
    let mut p = Vec::new();
    load_imm64(&mut p, 1, a);
    load_imm64(&mut p, 2, b);
    p.push(r_type(opcode, 3, funct3, 1, 2, 0x01));
    let mut m = Machine::new(&p);
    m.run(LOAD_IMM64_STEPS * 2 + 1);
    m.reg(3)
}

fn op64(a: u64, b: u64, f3: u32) -> u64 {
    m_op(a, b, opcodes::OP, f3)
}
fn op32(a: u64, b: u64, f3: u32) -> u64 {
    m_op(a, b, opcodes::OP_32, f3)
}

// --- multiply ---------------------------------------------------------------

#[test]
fn mul() {
    assert_eq!(op64(6, 7, 0x0), 42);
    assert_eq!(op64(U64_MAX, 2, 0x0), U64_MAX - 1); // wraps, never traps

    // MUL returns the low 64 bits, where signedness makes no difference: -1 * -1 and its
    // unsigned reading give the same low half.
    assert_eq!(op64(U64_MAX, U64_MAX, 0x0), 1);
}

#[test]
fn mulh_family() {
    // 2^32 * 2^32 = 2^64, so the low half is 0 and the high half is 1.
    let big = 1u64 << 32;
    assert_eq!(op64(big, big, 0x0), 0); // MUL
    assert_eq!(op64(big, big, 0x3), 1); // MULHU

    // -1 * -1 = 1: the upper half is 0 when both are read as signed...
    assert_eq!(op64(U64_MAX, U64_MAX, 0x1), 0); // MULH
    // ...but as unsigned this is (2^64-1)^2, whose upper half is 2^64-2.
    assert_eq!(op64(U64_MAX, U64_MAX, 0x3), U64_MAX - 1); // MULHU

    // MULHSU reads rs1 as signed and rs2 as unsigned. -1 * (2^64-1) = -(2^64-1), whose upper 64
    // bits are all ones.
    assert_eq!(op64(U64_MAX, U64_MAX, 0x2), U64_MAX); // MULHSU
    // A positive rs1 makes MULHSU agree with MULHU.
    assert_eq!(op64(big, big, 0x2), 1);
}

// --- division: the defined results ------------------------------------------

#[test]
fn div_by_zero_returns_defined_values() {
    // RISC-V division never traps. Every one of these is a specified result.
    assert_eq!(op64(42, 0, 0x4), U64_MAX); // DIV  by zero -> -1
    assert_eq!(op64(42, 0, 0x5), U64_MAX); // DIVU by zero -> all ones
    assert_eq!(op64(42, 0, 0x6), 42); // REM  by zero -> the dividend
    assert_eq!(op64(42, 0, 0x7), 42); // REMU by zero -> the dividend

    // Including when the dividend is itself zero.
    assert_eq!(op64(0, 0, 0x4), U64_MAX);
    assert_eq!(op64(0, 0, 0x6), 0);
}

#[test]
fn signed_overflow_is_defined() {
    // INT64_MIN / -1 is mathematically 2^63, which does not fit in 64 bits. RISC-V defines the
    // result as INT64_MIN itself (it wraps), and the remainder as 0. Computing it directly would
    // panic in Rust (and raise SIGFPE on x86 in C++).
    assert_eq!(op64(I64_MIN, U64_MAX, 0x4), I64_MIN); // DIV
    assert_eq!(op64(I64_MIN, U64_MAX, 0x6), 0); // REM

    // Read as unsigned there is no overflow, so these are ordinary divisions.
    assert_eq!(op64(I64_MIN, U64_MAX, 0x5), 0); // DIVU
    assert_eq!(op64(I64_MIN, U64_MAX, 0x7), I64_MIN); // REMU
}

#[test]
fn signed_division_truncates_toward_zero() {
    let neg7 = (-7i64) as u64;
    let neg2 = (-2i64) as u64;

    assert_eq!(op64(neg7, 2, 0x4), (-3i64) as u64); // -7/2 = -3, not -4
    assert_eq!(op64(neg7, 2, 0x6), (-1i64) as u64); // remainder keeps the dividend's sign
    assert_eq!(op64(7, neg2, 0x4), (-3i64) as u64);
    assert_eq!(op64(7, neg2, 0x6), 1);
}

#[test]
fn signed_and_unsigned_divide_differ() {
    // -2 as unsigned is a huge number, so DIVU gives 0 where DIV gives 1.
    assert_eq!(op64((-2i64) as u64, (-2i64) as u64, 0x4), 1);
    assert_eq!(op64(2, (-2i64) as u64, 0x5), 0);
}

// --- the 32-bit forms -------------------------------------------------------

#[test]
fn muldiv_w_sign_extends() {
    // MULW: 0x10000 * 0x10000 = 2^32, whose low 32 bits are 0.
    assert_eq!(op32(0x10000, 0x10000, 0x0), 0);

    // A 32-bit product with bit 31 set must sign-extend to 64.
    assert_eq!(op32(0x4000_0000, 2, 0x0), 0xffff_ffff_8000_0000);

    // The *W forms ignore bits 32-63 of their operands.
    assert_eq!(op32(0xffff_ffff_0000_0006, 7, 0x0), 42);

    // Divide-by-zero results, sign-extended.
    assert_eq!(op32(42, 0, 0x4), U64_MAX); // DIVW
    assert_eq!(op32(42, 0, 0x5), U64_MAX); // DIVUW - all ones, not 0xFFFFFFFF
    assert_eq!(op32(42, 0, 0x6), 42); // REMW
    assert_eq!(op32(42, 0, 0x7), 42); // REMUW

    // 32-bit signed overflow.
    let i32_min = 0xffff_ffff_8000_0000u64;
    assert_eq!(op32(i32_min, U64_MAX, 0x4), i32_min); // DIVW
    assert_eq!(op32(i32_min, U64_MAX, 0x6), 0); // REMW

    // DIVUW reads its operands as unsigned 32-bit but still sign-extends the result:
    // 0xFFFFFFFF / 1 = 0xFFFFFFFF, which extends to all ones.
    assert_eq!(op32(0xffff_ffff, 1, 0x5), U64_MAX);
}

// --- atomics ----------------------------------------------------------------

fn amo(funct5: u32, rd: u32, rs1: u32, rs2: u32, word: bool) -> u32 {
    r_type(opcodes::AMO, rd, if word { 0x2 } else { 0x3 }, rs1, rs2, funct5 << 2)
}

#[test]
fn amo_returns_original_and_stores_result() {
    let addr = DRAM_BASE + 0x400;

    let mut p = Vec::new();
    load_imm64(&mut p, 1, addr);
    load_imm64(&mut p, 2, 100);
    p.push(s_type(opcodes::STORE, 0x3, 1, 2, 0)); // [addr] = 100
    load_imm64(&mut p, 2, 5);
    p.push(amo(0x00, 3, 1, 2, false)); // amoadd.d x3, x2, (x1)
    p.push(i_type(opcodes::LOAD, 4, 0x3, 1, 0)); // x4 = [addr]

    let mut m = Machine::new(&p);
    m.run(LOAD_IMM64_STEPS * 3 + 3);
    assert_eq!(m.reg(3), 100); // rd gets the value from BEFORE the operation
    assert_eq!(m.reg(4), 105); // memory gets the result
}

#[test]
fn amo_operations() {
    let addr = DRAM_BASE + 0x500;

    // (name, funct5, memory, source, expected memory afterwards)
    let cases: &[(&str, u32, u64, u64, u64)] = &[
        ("amoswap", 0x01, 100, 7, 7),
        ("amoxor", 0x04, 0b1100, 0b1010, 0b0110),
        ("amoor", 0x08, 0b1100, 0b1010, 0b1110),
        ("amoand", 0x0c, 0b1100, 0b1010, 0b1000),
        ("amomin", 0x10, (-5i64) as u64, 3, (-5i64) as u64),
        ("amomax", 0x14, (-5i64) as u64, 3, 3),
        // The unsigned forms read the same bits the other way: -5 is huge.
        ("amominu", 0x18, (-5i64) as u64, 3, 3),
        ("amomaxu", 0x1c, (-5i64) as u64, 3, (-5i64) as u64),
    ];

    for &(name, funct5, mem, src, expect) in cases {
        let mut p = Vec::new();
        load_imm64(&mut p, 1, addr);
        load_imm64(&mut p, 2, mem);
        p.push(s_type(opcodes::STORE, 0x3, 1, 2, 0));
        load_imm64(&mut p, 2, src);
        p.push(amo(funct5, 3, 1, 2, false));
        p.push(i_type(opcodes::LOAD, 4, 0x3, 1, 0));

        let mut m = Machine::new(&p);
        m.run(LOAD_IMM64_STEPS * 3 + 3);

        assert_eq!(m.reg(4), expect, "{name}: memory holds {:#x}, expected {expect:#x}", m.reg(4));
        assert_eq!(m.reg(3), mem, "{name}: rd must get the original");
    }
}

#[test]
fn amo_word_form_sign_extends() {
    let addr = DRAM_BASE + 0x600;

    let mut p = Vec::new();
    load_imm64(&mut p, 1, addr);
    load_imm64(&mut p, 2, 0x8000_0000); // bit 31 set
    p.push(s_type(opcodes::STORE, 0x2, 1, 2, 0)); // sw
    load_imm64(&mut p, 2, 0);
    p.push(amo(0x00, 3, 1, 2, true)); // amoadd.w x3, x2, (x1)

    let mut m = Machine::new(&p);
    m.run(LOAD_IMM64_STEPS * 3 + 2);
    // The loaded 32-bit value is sign-extended into rd.
    assert_eq!(m.reg(3), 0xffff_ffff_8000_0000);
}

#[test]
fn misaligned_atomic_traps() {
    // Unlike ordinary loads and stores, the spec requires atomics to be naturally aligned.
    let mut p = Vec::new();
    load_imm64(&mut p, 1, DRAM_BASE + 0x401); // not 8-byte aligned
    p.push(amo(0x00, 3, 1, 0, false));

    let mut m = Machine::new(&p);
    let t = m.run(LOAD_IMM64_STEPS + 1).expect("must trap");
    assert_eq!(t.cause, Exception::StoreAMOAddressMisaligned);
}

// --- LR / SC ----------------------------------------------------------------

#[test]
fn lr_sc_succeeds_when_reservation_holds() {
    let addr = DRAM_BASE + 0x700;

    let mut p = Vec::new();
    load_imm64(&mut p, 1, addr);
    load_imm64(&mut p, 2, 11);
    p.push(s_type(opcodes::STORE, 0x3, 1, 2, 0)); // [addr] = 11
    load_imm64(&mut p, 2, 99);
    p.push(amo(0x02, 3, 1, 0, false)); // lr.d  x3, (x1)
    p.push(amo(0x03, 4, 1, 2, false)); // sc.d  x4, x2, (x1)
    p.push(i_type(opcodes::LOAD, 5, 0x3, 1, 0)); // x5 = [addr]

    let mut m = Machine::new(&p);
    m.run(LOAD_IMM64_STEPS * 3 + 4);
    assert_eq!(m.reg(3), 11); // lr returned the old value
    assert_eq!(m.reg(4), 0); // 0 in rd means the store succeeded
    assert_eq!(m.reg(5), 99); // and it really did store
}

#[test]
fn sc_without_lr_fails() {
    // An SC with no reservation must fail and must not write memory.
    let addr = DRAM_BASE + 0x800;

    let mut p = Vec::new();
    load_imm64(&mut p, 1, addr);
    load_imm64(&mut p, 2, 77);
    p.push(amo(0x03, 4, 1, 2, false)); // sc.d with no lr
    p.push(i_type(opcodes::LOAD, 5, 0x3, 1, 0));

    let mut m = Machine::new(&p);
    m.run(LOAD_IMM64_STEPS * 2 + 2);
    assert_ne!(m.reg(4), 0); // non-zero means failure
    assert_eq!(m.reg(5), 0); // memory untouched
}

#[test]
fn sc_to_a_different_address_fails() {
    let a = DRAM_BASE + 0x900;
    let b = DRAM_BASE + 0x908;

    let mut p = Vec::new();
    load_imm64(&mut p, 1, a);
    load_imm64(&mut p, 6, b);
    load_imm64(&mut p, 2, 55);
    p.push(amo(0x02, 3, 1, 0, false)); // lr.d on a
    p.push(amo(0x03, 4, 6, 2, false)); // sc.d on b - different address

    let mut m = Machine::new(&p);
    m.run(LOAD_IMM64_STEPS * 3 + 2);
    assert_ne!(m.reg(4), 0);
}

#[test]
fn second_sc_fails_because_the_reservation_is_consumed() {
    // The reservation is consumed by the SC whether it succeeded or not. Leaving it set would let
    // a later SC succeed against a stale reservation.
    let addr = DRAM_BASE + 0xa00;

    let mut p = Vec::new();
    load_imm64(&mut p, 1, addr);
    load_imm64(&mut p, 2, 1);
    p.push(amo(0x02, 3, 1, 0, false)); // lr.d
    p.push(amo(0x03, 4, 1, 2, false)); // sc.d - succeeds
    p.push(amo(0x03, 5, 1, 2, false)); // sc.d again - must fail

    let mut m = Machine::new(&p);
    m.run(LOAD_IMM64_STEPS * 2 + 3);
    assert_eq!(m.reg(4), 0); // first succeeded
    assert_ne!(m.reg(5), 0); // second did not
}

/// Runs: lr.d, then `middle`, then sc.d - with a trap handler installed so an ECALL dispatches
/// and returns rather than halting. Returns the SC result.
fn sc_result_with_middle_instruction(middle: u32) -> u64 {
    let addr = DRAM_BASE + 0xb00;
    let k = LOAD_IMM64_STEPS;
    let handler = DRAM_BASE + (3 * k + 5) * 4;

    let mut p = Vec::new();
    load_imm64(&mut p, 1, addr); // [0, K)
    load_imm64(&mut p, 2, 42); // [K, 2K)
    load_imm64(&mut p, 6, handler); // [2K, 3K)
    p.push(i_type(opcodes::SYSTEM, 0, 0x1, 6, csr::MTVEC as i32)); // 3K   mtvec = x6
    p.push(amo(0x02, 3, 1, 0, false)); // 3K+1 lr.d
    p.push(middle); // 3K+2
    p.push(amo(0x03, 4, 1, 2, false)); // 3K+3 sc.d
    p.push(halt()); // 3K+4
    // handler: step over the faulting instruction and return.
    p.push(i_type(opcodes::SYSTEM, 5, 0x2, 0, csr::MEPC as i32)); // 3K+5 x5 = mepc
    p.push(addi(5, 5, 4)); // 3K+6
    p.push(i_type(opcodes::SYSTEM, 0, 0x1, 5, csr::MEPC as i32)); // 3K+7 mepc = x5
    p.push(i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x302)); // 3K+8 mret

    let mut m = Machine::new(&p);
    m.run(300);
    m.reg(4)
}

#[test]
fn trap_between_lr_and_sc_breaks_the_reservation() {
    // This is the case a single-hart emulator gets wrong by default. Nothing can write memory
    // behind our back, so a reservation would never break and every SC would succeed - but a trap
    // is a context switch, and a thread that resumes must not inherit another thread's
    // reservation.
    //
    // The control makes the claim precise: with a NOP between the LR and the SC the store
    // succeeds, and swapping only that instruction for an ECALL makes it fail. So it is the trap
    // doing it, not the extra instruction.
    assert_eq!(sc_result_with_middle_instruction(nop()), 0); // control: succeeds

    let ecall = i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x000);
    assert_ne!(sc_result_with_middle_instruction(ecall), 0); // trap breaks it
}

#[test]
fn a_misaligned_lr_reports_a_load_fault() {
    // LR is a load and must report LoadAddressMisaligned; SC and the read-modify-write AMOs
    // store, so they report StoreAMOAddressMisaligned. A handler that sees a store fault for an
    // instruction which performed no store has no way to make sense of it.
    let mut m = Machine::new(&[amo(0x02, 1, 2, 0, false)]); // lr.d x1, (x2)
    m.cpu.regs[2] = DRAM_BASE + 4;
    assert_eq!(m.cpu.step().unwrap_err().cause, Exception::LoadAddressMisaligned);

    let mut m2 = Machine::new(&[amo(0x03, 1, 2, 3, false)]); // sc.d x1, x3, (x2)
    m2.cpu.regs[2] = DRAM_BASE + 4;
    assert_eq!(m2.cpu.step().unwrap_err().cause, Exception::StoreAMOAddressMisaligned);

    let mut m3 = Machine::new(&[amo(0x00, 1, 2, 3, false)]); // amoadd.d x1, x3, (x2)
    m3.cpu.regs[2] = DRAM_BASE + 4;
    assert_eq!(m3.cpu.step().unwrap_err().cause, Exception::StoreAMOAddressMisaligned);
}
