//! RV64I instruction tests.
//!
//! The emphasis is on the places where RV64 differs from RV32 and where the spec's
//! fine print is easy to skip:
//!
//!   * every "*W" result is sign-extended from 32 to 64 bits
//!   * 64-bit shifts mask to 6 bits, 32-bit shifts to 5
//!   * signed vs unsigned comparisons and load extensions
//!   * JALR clears bit 0 of the computed target after the add
//!   * a faulting instruction commits nothing

mod common;

use common::*;
use riscv_emu::csr::csr;
use riscv_emu::decoder::opcodes;
use riscv_emu::trap::Exception;
use riscv_emu::types::DRAM_BASE;

/// Build: x1 = a, x2 = b, then `inst`. Returns the value left in x3.
fn alu2(a: u64, b: u64, inst: u32) -> u64 {
    let mut p = Vec::new();
    load_imm64(&mut p, 1, a);
    load_imm64(&mut p, 2, b);
    p.push(inst);
    let mut m = Machine::new(&p);
    m.run(LOAD_IMM64_STEPS * 2 + 1);
    m.reg(3)
}

/// Build: x1 = a, then `inst`. Returns the value left in x3.
fn alu1(a: u64, inst: u32) -> u64 {
    let mut p = Vec::new();
    load_imm64(&mut p, 1, a);
    p.push(inst);
    let mut m = Machine::new(&p);
    m.run(LOAD_IMM64_STEPS + 1);
    m.reg(3)
}

#[test]
fn load_imm64_helper() {
    // The whole suite depends on this working, so check it first.
    assert_eq!(alu1(0x0123_4567_89ab_cdef, nop()), 0); // NOP writes nothing to x3

    let mut p = Vec::new();
    load_imm64(&mut p, 5, 0xdead_beef_cafe_babe);
    let mut m = Machine::new(&p);
    m.run(LOAD_IMM64_STEPS);
    assert_eq!(m.reg(5), 0xdead_beef_cafe_babe);
}

#[test]
fn lui_sign_extends() {
    let mut m = run(&[lui(1, 0x12345)], 1);
    assert_eq!(m.reg(1), 0x1234_5000);

    // Bit 31 set: the result is sign-extended to 64 bits, so this is negative.
    let mut n = run(&[lui(1, 0x80000)], 1);
    assert_eq!(n.reg(1), 0xffff_ffff_8000_0000);
}

#[test]
fn auipc_is_relative_to_itself() {
    // AUIPC adds to the address of the AUIPC itself, not to the next instruction. Put
    // one at DRAM_BASE+4 to catch an off-by-one-instruction.
    let mut m = run(&[nop(), auipc(1, 0x1)], 2);
    assert_eq!(m.reg(1), DRAM_BASE + 4 + 0x1000);

    // Zero immediate makes AUIPC a "where am I" instruction.
    let mut n = run(&[nop(), nop(), auipc(2, 0)], 3);
    assert_eq!(n.reg(2), DRAM_BASE + 8);
}

#[test]
fn jal() {
    // jal x1, +8 skips the ADDI at +4.
    let mut m = run(&[common::jal(1, 8), addi(3, 0, 99), addi(4, 0, 7)], 2);
    assert_eq!(m.reg(1), DRAM_BASE + 4); // link = address of next instruction
    assert_eq!(m.reg(3), 0); // never executed
    assert_eq!(m.reg(4), 7);

    // A backward jump, and rd = x0 (a plain goto with no link).
    let mut n = run(&[common::jal(0, 8), addi(3, 0, 99), addi(4, 0, 1), common::jal(0, -8)], 4);
    assert_eq!(n.reg(0), 0);
    assert_eq!(n.reg(3), 99); // reached on the second pass
}

#[test]
fn jalr() {
    // Target = rs1 + imm. The landing pad sits three instructions past the JALR, which
    // itself follows the setup sequence.
    let target = DRAM_BASE + (LOAD_IMM64_STEPS + 3) * 4;

    let mut p = Vec::new();
    load_imm64(&mut p, 1, target);
    p.push(common::jalr(2, 1, 0));
    p.push(addi(3, 0, 99)); // skipped
    p.push(nop()); // skipped
    p.push(addi(4, 0, 5)); // <- target

    let mut m = Machine::new(&p);
    m.run(LOAD_IMM64_STEPS + 2);
    assert_eq!(m.reg(3), 0);
    assert_eq!(m.reg(4), 5);
    // Link register holds the address after the JALR.
    assert_eq!(m.reg(2), DRAM_BASE + LOAD_IMM64_STEPS * 4 + 4);
}

#[test]
fn jalr_clears_bit_zero() {
    // jalr with an odd computed target must clear bit 0 rather than trap. This is
    // defined behaviour, not an alignment check: it lets the C extension put a tag in
    // bit 0 of a function pointer.
    let target = DRAM_BASE + (LOAD_IMM64_STEPS + 2) * 4;

    let mut p = Vec::new();
    load_imm64(&mut p, 1, target);
    p.push(common::jalr(2, 1, 1)); // target + 1, cleared back to target
    p.push(addi(3, 0, 99)); // skipped
    p.push(addi(4, 0, 42)); // <- target

    let mut m = Machine::new(&p);
    assert!(m.run(LOAD_IMM64_STEPS + 2).is_none());
    assert_eq!(m.reg(3), 0);
    assert_eq!(m.reg(4), 42);
}

#[test]
fn jalr_rd_equals_rs1() {
    // `jalr ra, ra, 0` is a real idiom. The target must be computed from the *old* rs1,
    // before rd is overwritten with the link address.
    let mut p = Vec::new();
    load_imm64(&mut p, 1, DRAM_BASE + (LOAD_IMM64_STEPS + 2) * 4);
    p.push(common::jalr(1, 1, 0)); // jump to the instruction after the next one
    p.push(addi(3, 0, 99)); // skipped
    p.push(addi(4, 0, 8)); // landing spot

    let mut m = Machine::new(&p);
    m.run(LOAD_IMM64_STEPS + 2);
    assert_eq!(m.reg(3), 0);
    assert_eq!(m.reg(4), 8);
    // x1 now holds the link address (the instruction after the JALR), not the target.
    assert_eq!(m.reg(1), DRAM_BASE + LOAD_IMM64_STEPS * 4 + 4);
}

#[test]
fn jump_alignment_with_the_c_extension() {
    // Before the C extension a jump to a 2-byte-aligned address trapped with
    // InstructionAddressMisaligned. With C, IALIGN is 16 bits and that target is
    // perfectly legal - compilers emit them constantly.
    {
        let mut p = Vec::new();
        load_imm64(&mut p, 1, DRAM_BASE + 2);
        p.push(common::jalr(2, 1, 0));
        let mut m = Machine::new(&p);
        assert!(m.run(LOAD_IMM64_STEPS + 1).is_none());
        assert_eq!(m.cpu.pc, DRAM_BASE + 2);
    }

    // With IALIGN = 16 a misaligned *jump* has become impossible to express. JAL and the
    // branches have bit 0 of their immediate hardwired to zero, and JALR clears bit 0 of
    // its computed target. An odd value in the register is masked away rather than
    // trapping:
    {
        let mut p = Vec::new();
        load_imm64(&mut p, 1, DRAM_BASE + 3);
        p.push(common::jalr(2, 1, 0));
        let mut m = Machine::new(&p);
        assert!(m.run(LOAD_IMM64_STEPS + 1).is_none());
        assert_eq!(m.cpu.pc, DRAM_BASE + 2); // bit 0 cleared, not a trap
    }
}

/// Runs: x1 = a, x2 = b, branch +8 over an ADDI. Returns true if taken.
fn branch_taken(a: u64, b: u64, funct3: u32) -> bool {
    let mut p = Vec::new();
    load_imm64(&mut p, 1, a);
    load_imm64(&mut p, 2, b);
    p.push(b_type(opcodes::BRANCH, funct3, 1, 2, 8));
    p.push(addi(3, 0, 1)); // executed only if NOT taken
    p.push(nop());
    let mut m = Machine::new(&p);
    m.run(LOAD_IMM64_STEPS * 2 + 3);
    m.reg(3) == 0
}

#[test]
fn branches() {
    const NEG: u64 = 0xffff_ffff_ffff_ffff;
    // BEQ / BNE
    assert!(branch_taken(5, 5, 0x0));
    assert!(!branch_taken(5, 6, 0x0));
    assert!(branch_taken(5, 6, 0x1));
    assert!(!branch_taken(5, 5, 0x1));

    // BLT / BGE are signed. -1 < 1.
    assert!(branch_taken(NEG, 1, 0x4));
    assert!(!branch_taken(1, NEG, 0x4));
    assert!(branch_taken(1, NEG, 0x5));
    assert!(branch_taken(5, 5, 0x5)); // BGE is >=

    // BLTU / BGEU are unsigned, so the same bit patterns compare the other way: 0xFFFF...FF
    // is the *largest* unsigned value, not -1.
    assert!(!branch_taken(NEG, 1, 0x6));
    assert!(branch_taken(1, NEG, 0x6));
    assert!(branch_taken(NEG, 1, 0x7));

    // Signed and unsigned disagreeing on the same inputs is the whole point.
    assert!(branch_taken(0x8000_0000_0000_0000, 1, 0x4)); // signed: negative < 1
    assert!(!branch_taken(0x8000_0000_0000_0000, 1, 0x6)); // unsigned: huge > 1
}

#[test]
fn backward_branch_loop() {
    // A real loop: count x3 down from 3 to 0 with a backward BNE.
    let p = [
        addi(3, 0, 3),                           // x3 = 3
        addi(4, 0, 0),                           // x4 = 0
        addi(3, 3, -1),                          // loop: x3 -= 1
        addi(4, 4, 10),                          //       x4 += 10
        b_type(opcodes::BRANCH, 0x1, 3, 0, -8),  // bne x3, x0, loop
    ];
    let mut m = Machine::new(&p);
    m.run(100);
    assert_eq!(m.reg(3), 0);
    assert_eq!(m.reg(4), 30);
}

#[test]
fn store_then_load_all_widths() {
    let addr = DRAM_BASE + 0x400;

    let mut p = Vec::new();
    load_imm64(&mut p, 1, addr);
    load_imm64(&mut p, 2, 0x0123_4567_89ab_cdef);
    p.push(s_type(opcodes::STORE, 0x3, 1, 2, 0)); // sd
    p.push(i_type(opcodes::LOAD, 3, 0x3, 1, 0)); // ld x3
    p.push(i_type(opcodes::LOAD, 4, 0x2, 1, 0)); // lw x4 (signed)
    p.push(i_type(opcodes::LOAD, 5, 0x6, 1, 0)); // lwu x5 (unsigned)
    p.push(i_type(opcodes::LOAD, 6, 0x1, 1, 0)); // lh x6
    p.push(i_type(opcodes::LOAD, 7, 0x0, 1, 0)); // lb x7

    let mut m = Machine::new(&p);
    m.run(LOAD_IMM64_STEPS * 2 + 6);

    assert_eq!(m.reg(3), 0x0123_4567_89ab_cdef);
    // Low word is 0x89abcdef; bit 31 is set, so LW sign-extends and LWU does not.
    assert_eq!(m.reg(4), 0xffff_ffff_89ab_cdef);
    assert_eq!(m.reg(5), 0x0000_0000_89ab_cdef);
    // Low halfword 0xcdef, bit 15 set -> sign-extended.
    assert_eq!(m.reg(6), 0xffff_ffff_ffff_cdef);
    // Low byte 0xef, bit 7 set -> sign-extended.
    assert_eq!(m.reg(7), 0xffff_ffff_ffff_ffef);
}

#[test]
fn load_zero_extension() {
    let addr = DRAM_BASE + 0x500;

    let mut p = Vec::new();
    load_imm64(&mut p, 1, addr);
    load_imm64(&mut p, 2, 0xffff_ffff_ffff_ffff);
    p.push(s_type(opcodes::STORE, 0x3, 1, 2, 0)); // sd all-ones
    p.push(i_type(opcodes::LOAD, 3, 0x4, 1, 0)); // lbu
    p.push(i_type(opcodes::LOAD, 4, 0x5, 1, 0)); // lhu
    p.push(i_type(opcodes::LOAD, 5, 0x6, 1, 0)); // lwu

    let mut m = Machine::new(&p);
    m.run(LOAD_IMM64_STEPS * 2 + 4);

    assert_eq!(m.reg(3), 0xff);
    assert_eq!(m.reg(4), 0xffff);
    assert_eq!(m.reg(5), 0xffff_ffff);
}

#[test]
fn store_widths_write_only_their_bytes() {
    let addr = DRAM_BASE + 0x600;

    let mut p = Vec::new();
    load_imm64(&mut p, 1, addr);
    load_imm64(&mut p, 2, 0xffff_ffff_ffff_ffff);
    p.push(s_type(opcodes::STORE, 0x3, 1, 2, 0)); // sd all-ones
    load_imm64(&mut p, 2, 0xaa);
    p.push(s_type(opcodes::STORE, 0x0, 1, 2, 0)); // sb 0xaa over byte 0
    p.push(i_type(opcodes::LOAD, 3, 0x3, 1, 0)); // ld

    let mut m = Machine::new(&p);
    m.run(LOAD_IMM64_STEPS * 3 + 3);
    // Only the lowest byte changed.
    assert_eq!(m.reg(3), 0xffff_ffff_ffff_ffaa);
}

#[test]
fn store_load_negative_offset() {
    let addr = DRAM_BASE + 0x700;

    let mut p = Vec::new();
    load_imm64(&mut p, 1, addr);
    load_imm64(&mut p, 2, 0x1234);
    p.push(s_type(opcodes::STORE, 0x3, 1, 2, -8)); // sd x2, -8(x1)
    p.push(i_type(opcodes::LOAD, 3, 0x3, 1, -8)); // ld x3, -8(x1)

    let mut m = Machine::new(&p);
    m.run(LOAD_IMM64_STEPS * 2 + 2);
    assert_eq!(m.reg(3), 0x1234);
}

#[test]
fn faulting_load_leaves_rd_untouched() {
    // A load from unmapped memory must trap and leave the destination register alone -
    // committing a partial result would be much harder to debug.
    let mut p = Vec::new();
    load_imm64(&mut p, 1, 0x1000); // below DRAM: nothing mapped
    p.push(addi(3, 0, 77)); // x3 = 77
    p.push(i_type(opcodes::LOAD, 3, 0x3, 1, 0)); // ld x3, 0(x1) -> faults

    let mut m = Machine::new(&p);
    let trap = m.run(LOAD_IMM64_STEPS + 2).expect("the load must fault");
    assert_eq!(trap.cause, Exception::LoadAccessFault);
    assert_eq!(m.reg(3), 77); // unchanged
}

#[test]
fn op_add_sub() {
    assert_eq!(alu2(5, 3, r_type(opcodes::OP, 3, 0x0, 1, 2, 0x00)), 8); // add
    assert_eq!(alu2(5, 3, r_type(opcodes::OP, 3, 0x0, 1, 2, 0x20)), 2); // sub
    // Wrapping, not trapping.
    assert_eq!(alu2(0xffff_ffff_ffff_ffff, 1, r_type(opcodes::OP, 3, 0x0, 1, 2, 0x00)), 0);
    assert_eq!(alu2(0, 1, r_type(opcodes::OP, 3, 0x0, 1, 2, 0x20)), 0xffff_ffff_ffff_ffff);
}

#[test]
fn op_logical_and_compare() {
    assert_eq!(alu2(0b1100, 0b1010, r_type(opcodes::OP, 3, 0x4, 1, 2, 0x00)), 0b0110); // xor
    assert_eq!(alu2(0b1100, 0b1010, r_type(opcodes::OP, 3, 0x6, 1, 2, 0x00)), 0b1110); // or
    assert_eq!(alu2(0b1100, 0b1010, r_type(opcodes::OP, 3, 0x7, 1, 2, 0x00)), 0b1000); // and

    // SLT signed vs SLTU unsigned, on the same bit patterns.
    let neg = 0xffff_ffff_ffff_ffffu64;
    assert_eq!(alu2(neg, 1, r_type(opcodes::OP, 3, 0x2, 1, 2, 0x00)), 1); // slt: -1 < 1
    assert_eq!(alu2(neg, 1, r_type(opcodes::OP, 3, 0x3, 1, 2, 0x00)), 0); // sltu: huge > 1
}

#[test]
fn op_shifts_mask_to_six_bits() {
    // Only the low 6 bits of rs2 are used on RV64. A shift by 64 is a shift by 0, not
    // undefined behaviour and not a zeroed register.
    assert_eq!(alu2(0x1234, 64, r_type(opcodes::OP, 3, 0x1, 1, 2, 0x00)), 0x1234); // sll
    assert_eq!(alu2(1, 63, r_type(opcodes::OP, 3, 0x1, 1, 2, 0x00)), 0x8000_0000_0000_0000);

    // SRL zero-fills, SRA sign-fills.
    let neg = 0xffff_ffff_ffff_ffffu64;
    assert_eq!(alu2(neg, 60, r_type(opcodes::OP, 3, 0x5, 1, 2, 0x00)), 0xf);
    assert_eq!(alu2(neg, 60, r_type(opcodes::OP, 3, 0x5, 1, 2, 0x20)), neg);
}

// --- the *W (32-bit) instruction family ---------------------------------------
//
// Every one of these must sign-extend its 32-bit result to 64 bits. This is the single
// most common source of silent RV64 emulator bugs.

#[test]
fn addw_sign_extends() {
    // 0x7FFFFFFF + 1 = 0x80000000 as a 32-bit value, which sign-extends to a negative
    // 64-bit number. A zero-extending implementation gives 0x0000000080000000 and nothing
    // complains until much later.
    assert_eq!(alu2(0x7fff_ffff, 1, r_type(opcodes::OP_32, 3, 0x0, 1, 2, 0x00)), 0xffff_ffff_8000_0000);

    // Bits above 32 in the operands are ignored entirely.
    assert_eq!(alu2(0xffff_ffff_0000_0005, 3, r_type(opcodes::OP_32, 3, 0x0, 1, 2, 0x00)), 8);

    // subw
    assert_eq!(alu2(0, 1, r_type(opcodes::OP_32, 3, 0x0, 1, 2, 0x20)), 0xffff_ffff_ffff_ffff);
}

#[test]
fn w_shifts_use_five_bits() {
    // 32-bit shifts mask rs2 to 5 bits, so a shift by 32 is a shift by 0.
    assert_eq!(alu2(0x1234, 32, r_type(opcodes::OP_32, 3, 0x1, 1, 2, 0x00)), 0x1234);

    // sllw producing a value with bit 31 set -> sign-extended.
    assert_eq!(alu2(1, 31, r_type(opcodes::OP_32, 3, 0x1, 1, 2, 0x00)), 0xffff_ffff_8000_0000);

    // srlw operates on the low 32 bits only: the upper bits of rs1 must not leak in.
    assert_eq!(alu2(0xffff_ffff_8000_0000, 28, r_type(opcodes::OP_32, 3, 0x5, 1, 2, 0x00)), 0x8);
    // sraw sign-fills from bit 31, then the 32-bit result is sign-extended.
    assert_eq!(
        alu2(0xffff_ffff_8000_0000, 28, r_type(opcodes::OP_32, 3, 0x5, 1, 2, 0x20)),
        0xffff_ffff_ffff_fff8
    );
}

#[test]
fn op_imm_32() {
    // addiw
    assert_eq!(alu1(0x7fff_ffff, i_type(opcodes::OP_IMM_32, 3, 0x0, 1, 1)), 0xffff_ffff_8000_0000);
    // addiw also truncates its source to 32 bits first.
    assert_eq!(alu1(0xffff_ffff_0000_0005, i_type(opcodes::OP_IMM_32, 3, 0x0, 1, 3)), 8);

    // slliw / srliw / sraiw. Shift amount is 5 bits, selector is funct7.
    let shiftw = |funct3: u32, funct7: u32, shamt: u32| {
        i_type(opcodes::OP_IMM_32, 3, funct3, 1, ((funct7 << 5) | (shamt & 0x1f)) as i32)
    };
    assert_eq!(alu1(1, shiftw(0x1, 0x00, 31)), 0xffff_ffff_8000_0000); // slliw
    assert_eq!(alu1(0xffff_ffff_8000_0000, shiftw(0x5, 0x00, 28)), 0x8); // srliw
    assert_eq!(alu1(0xffff_ffff_8000_0000, shiftw(0x5, 0x20, 28)), 0xffff_ffff_ffff_fff8); // sraiw
}

#[test]
fn fence_is_a_nop() {
    // We execute strictly in order and complete each access before the next instruction,
    // so the ordering FENCE requests already holds.
    let mut m = run(&[i_type(opcodes::MISC_MEM, 0, 0x0, 0, 0), addi(1, 0, 5)], 2);
    assert_eq!(m.reg(1), 5);
    assert_eq!(m.cpu.pc, DRAM_BASE + 8);
}

#[test]
fn ecall_and_ebreak_trap() {
    let mut m = Machine::new(&[i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x000)]);
    let ec = m.cpu.step().expect_err("ecall traps");
    assert_eq!(ec.cause, Exception::ECallFromMMode);

    let mut n = Machine::new(&[i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x001)]);
    let eb = n.cpu.step().expect_err("ebreak traps");
    assert_eq!(eb.cause, Exception::Breakpoint);
    assert_eq!(eb.tval, DRAM_BASE);
}

#[test]
fn later_extensions_share_opcodes_without_being_mistaken_for_base_instructions() {
    // The M extension shares the OP opcode, selected by funct7 == 0x01. What matters here
    // is that `mul` is not mistaken for ADD.
    let mut m = Machine::new(&[addi(1, 0, 6), addi(2, 0, 7), r_type(opcodes::OP, 3, 0x0, 1, 2, 0x01)]);
    m.run(3);
    assert_eq!(m.reg(3), 42); // 6 * 7, not 6 + 7

    // CSR instructions share the SYSTEM opcode and execute.
    let mut n = Machine::new(&[i_type(opcodes::SYSTEM, 3, 0x1, 0, csr::MSTATUS as i32)]); // csrrw
    assert!(n.cpu.step().is_ok());

    // An unallocated SYSTEM immediate still traps.
    let mut o = Machine::new(&[i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x7ff)]);
    let sr = o.cpu.step().expect_err("unallocated SYSTEM traps");
    assert_eq!(sr.cause, Exception::IllegalInstruction);
}

#[test]
fn sum_loop() {
    // Sum 1..10 with a loop, storing the result to memory and loading it back. Exercises
    // branches, arithmetic, and memory together.
    let addr = DRAM_BASE + 0x800;

    let mut p = Vec::new();
    load_imm64(&mut p, 10, addr);
    p.push(addi(1, 0, 0)); // sum = 0
    p.push(addi(2, 0, 1)); // i = 1
    p.push(addi(3, 0, 11)); // limit = 11
    // loop:
    p.push(r_type(opcodes::OP, 1, 0x0, 1, 2, 0x00)); // sum += i
    p.push(addi(2, 2, 1)); // i += 1
    p.push(b_type(opcodes::BRANCH, 0x1, 2, 3, -8)); // bne i, limit, loop
    p.push(s_type(opcodes::STORE, 0x3, 10, 1, 0)); // sd sum, (addr)
    p.push(i_type(opcodes::LOAD, 4, 0x3, 10, 0)); // ld x4, (addr)
    p.push(i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x001)); // ebreak - stop here

    // The program terminates itself with EBREAK, so reaching that trap (rather than the
    // budget, or an illegal instruction in the zeroed memory past the program) is itself
    // the check that control flow went where it should.
    let mut m = Machine::new(&p);
    let trap = m.run(500).expect("ebreak stops the run");
    assert_eq!(trap.cause, Exception::Breakpoint);
    assert_eq!(m.reg(1), 55);
    assert_eq!(m.reg(4), 55);
}
