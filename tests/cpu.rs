//! CPU tests.
//!
//! These cover three behavioural properties that are easy to get wrong:
//!
//!   * OP-IMM dispatches on funct3, so SLTI/SLTIU/XORI/ORI/ANDI/SLLI/SRLI/SRAI do their
//!     own jobs instead of all performing an addition.
//!   * An unrecognised instruction raises IllegalInstruction.
//!   * The PC does not advance when an instruction traps.

mod common;

use common::*;
use riscv_emu::decoder::opcodes;
use riscv_emu::trap::Exception;
use riscv_emu::types::DRAM_BASE;

/// Build an OP-IMM shift: the 12-bit immediate field is funct6 in its top 6 bits and the
/// 6-bit shift amount below it.
fn enc_shift(rd: u32, funct3: u32, rs1: u32, funct6: u32, shamt: u32) -> u32 {
    let imm_field = ((funct6 & 0x3f) << 6) | (shamt & 0x3f);
    (imm_field << 20) | ((rs1 & 0x1f) << 15) | ((funct3 & 0x7) << 12) | ((rd & 0x1f) << 7) | opcodes::OP_IMM
}

#[test]
fn x0_is_hardwired_to_zero() {
    let mut m = Machine::new(&[i_type(opcodes::OP_IMM, 0, 0, 0, 42)]); // addi x0, x0, 42
    assert!(m.cpu.step().is_ok());
    assert_eq!(m.cpu.read_reg(0), 0);

    // Writes through the API are discarded too.
    m.cpu.write_reg(0, 0xdead);
    assert_eq!(m.cpu.read_reg(0), 0);
    assert_eq!(m.cpu.regs[0], 0);
}

/// The core regression test: each of these instructions has a distinct funct3, and an
/// implementation that ignored it would run ADDI for all of them.
#[test]
fn op_imm_dispatches_on_funct3() {
    // Set up x1 = 5 and x2 = -3, then run one OP-IMM per case.
    let set_x1 = i_type(opcodes::OP_IMM, 1, 0, 0, 5); // addi x1, x0, 5
    let set_x2 = i_type(opcodes::OP_IMM, 2, 0, 0, -3); // addi x2, x0, -3

    let cases: &[(&str, u32, usize, u64)] = &[
        // ADDI x3, x1, 3 -> 8. (The control: this one is right even when dispatch is not.)
        ("addi", i_type(opcodes::OP_IMM, 3, 0x0, 1, 3), 3, 8),
        // SLTI x3, x2, 0 -> 1, because -3 < 0 as a signed comparison. As ADDI this would
        // have produced -3.
        ("slti", i_type(opcodes::OP_IMM, 3, 0x2, 2, 0), 3, 1),
        // SLTIU x3, x2, 0 -> 0. The immediate is sign-extended and *then* compared
        // unsigned, so this asks "is 0xFFFF...FD < 0", which is false.
        ("sltiu", i_type(opcodes::OP_IMM, 3, 0x3, 2, 0), 3, 0),
        // SLTIU x3, x1, -1 -> 1. Sign-extension matters: the comparison is against
        // 0xFFFF_FFFF_FFFF_FFFF, not against 0xFFF.
        ("sltiu-sext", i_type(opcodes::OP_IMM, 3, 0x3, 1, -1), 3, 1),
        // XORI x3, x1, 15 -> 5 ^ 15 = 10. As ADDI this would be 20.
        ("xori", i_type(opcodes::OP_IMM, 3, 0x4, 1, 15), 3, 10),
        // ORI x3, x1, 12 -> 5 | 12 = 13 (5 + 12 = 17 would be ADDI).
        ("ori", i_type(opcodes::OP_IMM, 3, 0x6, 1, 12), 3, 13),
        // ANDI x3, x1, 6 -> 5 & 6 = 4. As ADDI this would be 11.
        ("andi", i_type(opcodes::OP_IMM, 3, 0x7, 1, 6), 3, 4),
        // SLLI x3, x1, 4 -> 5 << 4 = 80. As ADDI this would be 9.
        ("slli", enc_shift(3, 0x1, 1, 0x00, 4), 3, 80),
        // SRLI x3, x2, 60 -> logical shift of 0xFFFF_FFFF_FFFF_FFFD, zeros shifted in,
        // giving 0xF.
        ("srli", enc_shift(3, 0x5, 2, 0x00, 60), 3, 0xf),
        // SRAI x3, x2, 60 -> arithmetic shift, sign bits shifted in, giving -1.
        // Distinguishing this from SRLI requires funct6, not funct7.
        ("srai", enc_shift(3, 0x5, 2, 0x10, 60), 3, 0xffff_ffff_ffff_ffff),
    ];

    for &(name, inst, rd, expected) in cases {
        let mut m = Machine::new(&[set_x1, set_x2, inst]);
        assert!(m.cpu.step().is_ok());
        assert!(m.cpu.step().is_ok());
        assert!(m.cpu.step().is_ok(), "{name} trapped unexpectedly");
        let got = m.cpu.read_reg(rd);
        assert_eq!(got, expected, "{name}: got {got:#x}, expected {expected:#x}");
    }
}

#[test]
fn addi_wraps_without_trapping() {
    // Overflow in ADDI is defined to wrap; it must never trap.
    let mut m = Machine::new(&[
        i_type(opcodes::OP_IMM, 1, 0, 0, -1), // addi x1, x0, -1  -> 0xFFFF...FF
        i_type(opcodes::OP_IMM, 2, 0, 1, 1),  // addi x2, x1, 1   -> wraps to 0
    ]);
    assert!(m.cpu.step().is_ok());
    assert!(m.cpu.step().is_ok());
    assert_eq!(m.cpu.read_reg(1), 0xffff_ffff_ffff_ffff);
    assert_eq!(m.cpu.read_reg(2), 0);
}

#[test]
fn shift_uses_six_bits_on_rv64() {
    // Shifting by 32 or more is meaningful on RV64 and must not be truncated to 5 bits the
    // way RV32 would.
    let mut m = Machine::new(&[
        i_type(opcodes::OP_IMM, 1, 0, 0, 1), // addi x1, x0, 1
        enc_shift(2, 0x1, 1, 0x00, 40),      // slli x2, x1, 40
    ]);
    assert!(m.cpu.step().is_ok());
    assert!(m.cpu.step().is_ok());
    assert_eq!(m.cpu.read_reg(2), 1u64 << 40);
}

#[test]
fn unknown_instruction_traps() {
    let mut m = Machine::new(&[0x0000_0000]); // not a valid instruction
    let t = m.cpu.step().unwrap_err();
    assert_eq!(t.cause, Exception::IllegalInstruction);
    // tval carries the offending instruction bits, per the privileged spec.
    assert_eq!(t.tval, 0);
}

/// The other half: a trapping instruction must leave the PC where it is, so a wrong jump
/// stops at the mistake instead of grinding through unmapped memory.
#[test]
fn pc_does_not_advance_on_trap() {
    let mut m = Machine::new(&[
        i_type(opcodes::OP_IMM, 1, 0, 0, 1), // addi x1, x0, 1  (ok)
        0xffff_ffff,                         // illegal
    ]);

    assert!(m.cpu.step().is_ok());
    assert_eq!(m.cpu.pc, DRAM_BASE + 4);

    assert!(m.cpu.step().is_err());
    assert_eq!(m.cpu.pc, DRAM_BASE + 4); // still on the faulting instruction
    assert_eq!(m.cpu.instret, 1); // and it did not count as retired
}

#[test]
fn reserved_shift_encoding_is_illegal() {
    // funct6 must be 0x00 for SLLI. Any other value is reserved and must trap rather than
    // being executed as a shift.
    let mut m = Machine::new(&[enc_shift(1, 0x1, 0, 0x20, 4)]);
    assert_eq!(m.cpu.step().unwrap_err().cause, Exception::IllegalInstruction);
}

#[test]
fn misaligned_fetch_traps() {
    // IALIGN is 16 bits with the C extension, so only an odd PC is misaligned. No jump can
    // produce one - JALR clears bit 0 and the other transfers have it hardwired to zero - so
    // this is a backstop for a PC set some other way, which is exactly what the test does.
    let mut m = Machine::new(&[i_type(opcodes::OP_IMM, 1, 0, 0, 1)]);
    m.cpu.pc = DRAM_BASE + 1;

    let t = m.cpu.step().unwrap_err();
    assert_eq!(t.cause, Exception::InstructionAddressMisaligned);
    assert_eq!(t.tval, DRAM_BASE + 1);
}

#[test]
fn fetch_from_unmapped_memory_traps() {
    let mut m = Machine::new(&[i_type(opcodes::OP_IMM, 1, 0, 0, 1)]);
    m.cpu.pc = 0x1000; // below DRAM, nothing mapped there

    // Fetch failures are InstructionAccessFault, not LoadAccessFault.
    assert_eq!(m.cpu.step().unwrap_err().cause, Exception::InstructionAccessFault);
}

#[test]
fn run_stops_at_first_trap() {
    let mut m = Machine::new(&[
        i_type(opcodes::OP_IMM, 1, 0, 0, 1),
        i_type(opcodes::OP_IMM, 2, 0, 0, 2),
        0xffff_ffff,
        i_type(opcodes::OP_IMM, 3, 0, 0, 3), // must never execute
    ]);

    let (retired, trap) = m.cpu.run(100);
    assert!(trap.is_some());
    assert_eq!(retired, 2);
    assert_eq!(m.cpu.read_reg(3), 0);
}
