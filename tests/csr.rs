//! CSR and trap-handling tests.
//!
//! The heart of this is that a trap stops being "the emulator halts" and becomes "the
//! guest's handler runs". These tests check the full round trip: an ECALL vectors to mtvec,
//! the handler reads mcause to learn why, adjusts mepc, and MRET returns to the right
//! instruction with the machine's state restored exactly.

mod common;

use common::*;
use riscv_emu::csr::{csr, CsrFile};
use riscv_emu::decoder::opcodes;
use riscv_emu::trap::{Exception, Interrupt};
use riscv_emu::types::{DRAM_BASE, PRIV_LEAST_SUPPORTED, PRIV_MACHINE};

// --- CSR instruction encoders ----------------------------------------------

fn csrrw(rd: u32, a: u32, rs1: u32) -> u32 {
    i_type(opcodes::SYSTEM, rd, 0x1, rs1, a as i32)
}
fn csrrs(rd: u32, a: u32, rs1: u32) -> u32 {
    i_type(opcodes::SYSTEM, rd, 0x2, rs1, a as i32)
}
fn csrrc(rd: u32, a: u32, rs1: u32) -> u32 {
    i_type(opcodes::SYSTEM, rd, 0x3, rs1, a as i32)
}
fn csrrwi(rd: u32, a: u32, uimm: u32) -> u32 {
    i_type(opcodes::SYSTEM, rd, 0x5, uimm, a as i32)
}
fn csrrsi(rd: u32, a: u32, uimm: u32) -> u32 {
    i_type(opcodes::SYSTEM, rd, 0x6, uimm, a as i32)
}
fn ecall() -> u32 {
    i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x000)
}
fn ebreak() -> u32 {
    i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x001)
}
fn mret() -> u32 {
    i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x302)
}
fn wfi() -> u32 {
    i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x105)
}

// --- basic CSR read/write ---------------------------------------------------

#[test]
fn csrrw_swaps() {
    // mscratch is a plain scratch register with no side effects, so it is the clean way to test
    // the swap semantics themselves.
    let mut p = Vec::new();
    load_imm64(&mut p, 1, 0xdead_beef);
    p.push(csrrw(2, csr::MSCRATCH, 1)); // x2 = mscratch(0); mscratch = x1
    p.push(csrrw(3, csr::MSCRATCH, 0)); // x3 = mscratch; mscratch = 0

    let mut m = Machine::new(&p);
    m.run(LOAD_IMM64_STEPS + 2);
    assert_eq!(m.reg(2), 0); // old value
    assert_eq!(m.reg(3), 0xdead_beef); // what we wrote
    assert_eq!(m.cpu.csrs.read(csr::MSCRATCH), 0);
}

#[test]
fn csrrs_and_csrrc_set_and_clear() {
    let mut p = Vec::new();
    load_imm64(&mut p, 1, 0b1010);
    load_imm64(&mut p, 2, 0b0010);
    p.push(csrrs(0, csr::MSCRATCH, 1)); // mscratch |= 0b1010
    p.push(csrrs(3, csr::MSCRATCH, 2)); // x3 = 0b1010; mscratch |= 0b0010
    p.push(csrrc(4, csr::MSCRATCH, 2)); // x4 = 0b1010; mscratch &= ~0b0010

    let mut m = Machine::new(&p);
    m.run(LOAD_IMM64_STEPS * 2 + 3);
    assert_eq!(m.reg(3), 0b1010);
    assert_eq!(m.reg(4), 0b1010);
    assert_eq!(m.cpu.csrs.read(csr::MSCRATCH), 0b1000);
}

#[test]
fn immediate_forms_use_a_five_bit_immediate() {
    // In CSRRWI the rs1 field is a value, not a register number. Reading x31 here instead of
    // using 31 as a literal would give 0.
    let mut m = Machine::new(&[csrrwi(1, csr::MSCRATCH, 31)]);
    m.run(1);
    assert_eq!(m.cpu.csrs.read(csr::MSCRATCH), 31);

    let mut n = Machine::new(&[csrrsi(1, csr::MSCRATCH, 5)]);
    n.run(1);
    assert_eq!(n.cpu.csrs.read(csr::MSCRATCH), 5);
}

// --- the suppression rules --------------------------------------------------

#[test]
fn csrrs_with_x0_source_does_not_write() {
    // `csrr rd, csr` assembles to CSRRS with rs1 = x0 and must be a pure read. Because no write
    // is attempted, it is legal even on a read-only CSR - which is exactly how software reads
    // mhartid.
    let mut m = Machine::new(&[csrrs(1, csr::MHARTID, 0)]);
    assert!(m.cpu.step().is_ok()); // no trap, despite mhartid being read-only
    assert_eq!(m.reg(1), 0); // single hart, id 0
}

#[test]
fn csrrw_to_read_only_csr_traps() {
    // An actual write to a read-only CSR is illegal. Address bits [11:10] == 11 mark the whole
    // 0xC00-0xFFF range as read-only, so no per-register table is needed to know this.
    let mut m = Machine::new(&[csrrw(0, csr::MHARTID, 1)]);
    assert_eq!(m.cpu.step().unwrap_err().cause, Exception::IllegalInstruction);
}

#[test]
fn unimplemented_csr_traps() {
    // Probing an unimplemented CSR and catching the trap is how software detects optional
    // features, so this must fault rather than read zero.
    let mut m = Machine::new(&[csrrs(1, 0x7c0, 0)]); // a custom CSR we do not implement
    assert_eq!(m.cpu.step().unwrap_err().cause, Exception::IllegalInstruction);
}

// --- CSR-specific write masking ---------------------------------------------

#[test]
fn mstatus_masks_unimplemented_bits() {
    // Writing all-ones to mstatus must not make reserved bits read back as set.
    let mut p = Vec::new();
    load_imm64(&mut p, 1, u64::MAX);
    p.push(csrrw(0, csr::MSTATUS, 1));

    let mut m = Machine::new(&p);
    m.run(LOAD_IMM64_STEPS + 1);

    let status = m.cpu.csrs.mstatus();
    assert_ne!(status & csr::MSTATUS_MIE, 0);
    assert_ne!(status & csr::MSTATUS_MPIE, 0);
    assert_eq!((status & csr::MSTATUS_MPP) >> csr::MSTATUS_MPP_SHIFT, PRIV_MACHINE as u64);
    // ...and nothing outside the implemented set is, however the guest writes it.
    let implemented = csr::MSTATUS_MIE
        | csr::MSTATUS_MPIE
        | csr::MSTATUS_MPP
        | csr::MSTATUS_SIE
        | csr::MSTATUS_SPIE
        | csr::MSTATUS_SPP
        | csr::MSTATUS_MPRV
        | csr::MSTATUS_SUM
        | csr::MSTATUS_MXR
        | csr::MSTATUS_TVM
        | csr::MSTATUS_TW
        | csr::MSTATUS_TSR
        | csr::MSTATUS_FS
        | csr::MSTATUS_SD
        | csr::MSTATUS_UXL
        | csr::MSTATUS_SXL; // read-only; see below
    assert_eq!(status & !implemented, 0);

    // UXL and SXL say how wide user and supervisor mode are. This is an RV64-only machine, so
    // both read as 2 (64-bit) and writing all-ones - which just happened - must not move them.
    // (rv64si/csr in the official suite checks that sstatus.UXL reads as 2; a 64-bit kernel
    // consults it to decide whether 32-bit user programs may run.)
    assert_eq!((status & csr::MSTATUS_UXL) >> 32, csr::MSTATUS_XL_RV64);
    assert_eq!((status & csr::MSTATUS_SXL) >> 34, csr::MSTATUS_XL_RV64);

    // The supervisor view exposes UXL (but not SXL, which is machine-only)...
    assert_eq!((m.cpu.csrs.read(csr::SSTATUS) & csr::MSTATUS_UXL) >> 32, csr::MSTATUS_XL_RV64);
    assert_eq!(m.cpu.csrs.read(csr::SSTATUS) & csr::MSTATUS_SXL, 0);
    // ...and cannot change it.
    m.cpu.csrs.write(csr::SSTATUS, 0);
    assert_eq!((m.cpu.csrs.read(csr::SSTATUS) & csr::MSTATUS_UXL) >> 32, csr::MSTATUS_XL_RV64);
    m.cpu.csrs.write(csr::SSTATUS, !0);
    assert_eq!((m.cpu.csrs.read(csr::SSTATUS) & csr::MSTATUS_UXL) >> 32, csr::MSTATUS_XL_RV64);

    m.cpu.csrs.write(csr::MSTATUS, !0); // restore the all-ones state the rest assumes

    // SD is read-only and derived: it is set exactly when FS is Dirty, so it can never
    // disagree with the field it summarises. A context switch reads that one bit rather than
    // picking a field apart.
    assert_eq!(status & csr::MSTATUS_FS, csr::MSTATUS_FS_DIRTY);
    assert_ne!(status & csr::MSTATUS_SD, 0);

    m.cpu.csrs.write(csr::MSTATUS, (status & !csr::MSTATUS_FS) | csr::MSTATUS_FS_CLEAN);
    assert_eq!(m.cpu.csrs.mstatus() & csr::MSTATUS_SD, 0);
}

#[test]
fn mtvec_mode_is_warl() {
    // Only modes 0 and 1 are defined. Mode 2 and 3 are reserved, and because the field is WARL
    // an implementation coerces rather than storing them.
    let mut p = Vec::new();
    load_imm64(&mut p, 1, 0x8000_1000 | 0x3); // base + reserved mode 3
    p.push(csrrw(0, csr::MTVEC, 1));

    let mut m = Machine::new(&p);
    m.run(LOAD_IMM64_STEPS + 1);
    let tvec = m.cpu.csrs.read(csr::MTVEC);
    assert_eq!(tvec & !csr::MTVEC_MODE_MASK, 0x8000_1000);
    assert_eq!(tvec & csr::MTVEC_MODE_MASK, csr::MTVEC_MODE_DIRECT);
}

#[test]
fn mepc_low_bit_is_hardwired_zero() {
    // mepc always holds an instruction address, so bit 0 is dropped. Bit 1 is kept: with the C
    // extension IALIGN is 16 and a 2-byte-aligned address is a perfectly ordinary place for an
    // instruction to be.
    let mut p = Vec::new();
    load_imm64(&mut p, 1, 0x8000_1007);
    p.push(csrrw(0, csr::MEPC, 1));

    let mut m = Machine::new(&p);
    m.run(LOAD_IMM64_STEPS + 1);
    assert_eq!(m.cpu.csrs.read(csr::MEPC), 0x8000_1006);
}

#[test]
fn misa_reports_the_implemented_extensions() {
    let mut m = Machine::new(&[csrrs(1, csr::MISA, 0)]);
    m.run(1);
    let misa = m.reg(1);
    // MXL = 2 means RV64, in the top two bits.
    assert_eq!(misa >> 62, 2);
    // I, M, A, C, F and D are all implemented. misa must not claim more than the emulator
    // delivers, because guest code reads it to decide what it may use - and must not claim
    // less, because a kernel that sees no C bit may refuse to run compressed code it emitted.
    for ext in ['I', 'M', 'A', 'C', 'F', 'D'] {
        assert_ne!(misa & (1u64 << (ext as u8 - b'A')), 0, "misa lacks {ext}");
    }
}

#[test]
fn epc_registers_keep_bit_1_with_the_c_extension() {
    // mepc and sepc hold an instruction address, so bit 0 is hardwired to zero. Bit 1 is *not*,
    // once the C extension is implemented: IALIGN becomes 16 and instructions legitimately live
    // at 2-byte-aligned addresses.
    //
    // This is not a corner case. xv6 built for rv64gc puts `main` at 0x8000_0dee; start() writes
    // that to mepc and returns to it with mret. An implementation that masks bit 1 as well
    // returns two bytes early, into the middle of whichever function the linker happened to
    // place before it.
    let mut csrs = CsrFile::new();

    csrs.write(csr::MEPC, DRAM_BASE + 0xdee);
    assert_eq!(csrs.read(csr::MEPC), DRAM_BASE + 0xdee);
    csrs.write(csr::SEPC, DRAM_BASE + 0xdee);
    assert_eq!(csrs.read(csr::SEPC), DRAM_BASE + 0xdee);

    // Bit 0 is still dropped: there is no such thing as an odd instruction address at any IALIGN.
    csrs.write(csr::MEPC, DRAM_BASE + 0xdef);
    assert_eq!(csrs.read(csr::MEPC), DRAM_BASE + 0xdee);
    csrs.write(csr::SEPC, DRAM_BASE + 0xdef);
    assert_eq!(csrs.read(csr::SEPC), DRAM_BASE + 0xdee);
}

// --- trap dispatch ----------------------------------------------------------
//
// Programs below are laid out by instruction index. `with_handler` emits load_imm64
// (LOAD_IMM64_STEPS instructions, indices 0..k-1) followed by the CSRRW that installs mtvec at
// index k, so the first instruction a test adds sits at index k+1.

/// Installs `handler_addr` into mtvec. Returns the program so far.
fn with_handler(handler_addr: u64) -> Vec<u32> {
    let mut p = Vec::new();
    load_imm64(&mut p, 1, handler_addr);
    p.push(csrrw(0, csr::MTVEC, 1));
    p
}

/// Address of instruction index `i`.
fn at(i: u64) -> u64 {
    DRAM_BASE + i * 4
}

const K: u64 = LOAD_IMM64_STEPS;

#[test]
fn ecall_vectors_to_mtvec() {
    let mut p = with_handler(at(K + 4));
    p.push(ecall()); // K+1
    p.push(addi(5, 0, 99)); // K+2  must not run
    p.push(halt()); // K+3
    p.push(addi(6, 0, 42)); // K+4  handler
    p.push(halt()); // K+5

    let mut m = Machine::new(&p);
    m.run(200);

    assert_eq!(m.reg(6), 42); // the handler ran
    assert_eq!(m.reg(5), 0); // the instruction after ecall did not
}

#[test]
fn trap_records_cause_epc_and_status() {
    let mut p = with_handler(at(K + 3));
    p.push(ecall()); // K+1
    p.push(addi(5, 0, 99)); // K+2
    p.push(halt()); // K+3  handler: spin without trapping again

    let mut m = Machine::new(&p);
    m.run(200);

    // mcause says why we trapped. ECALL from machine mode is cause 11.
    assert_eq!(m.cpu.csrs.read(csr::MCAUSE), Exception::ECallFromMMode as u64);

    // mepc points at the ECALL itself, not past it. A handler that wants to resume after the
    // call must add 4 - which is exactly how a syscall return works.
    assert_eq!(m.cpu.csrs.read(csr::MEPC), at(K + 1));

    // MPP records the privilege the trap came from.
    assert_eq!(m.cpu.csrs.mstatus_mpp(), PRIV_MACHINE);

    // Interrupts are disabled on entry, so the handler cannot be re-entered by the source that
    // just fired.
    assert!(!m.cpu.csrs.mstatus_mie());
}

#[test]
fn mret_returns_and_restores() {
    // The full round trip: enable interrupts, ECALL, the handler advances mepc past the ECALL
    // and returns, execution resumes at the next instruction with MIE restored.
    let mut p = with_handler(at(K + 5));
    p.push(csrrsi(0, csr::MSTATUS, 0x8)); // K+1  mstatus.MIE = 1
    p.push(ecall()); // K+2
    p.push(addi(5, 0, 7)); // K+3  must run after MRET
    p.push(halt()); // K+4
    p.push(csrrs(1, csr::MEPC, 0)); // K+5  handler: x1 = mepc
    p.push(addi(1, 1, 4)); // K+6  skip past the ecall
    p.push(csrrw(0, csr::MEPC, 1)); // K+7  mepc = x1
    p.push(addi(6, 0, 3)); // K+8  mark the handler ran
    p.push(mret()); // K+9

    let mut m = Machine::new(&p);
    m.run(200);

    assert_eq!(m.reg(6), 3); // handler ran
    assert_eq!(m.reg(5), 7); // and we returned to the right instruction

    // MRET restores MIE from MPIE, and sets MPIE.
    assert!(m.cpu.csrs.mstatus_mie());
    assert_ne!(m.cpu.csrs.mstatus() & csr::MSTATUS_MPIE, 0);
    // MPP is reset to the least-privileged supported mode.
    assert_eq!(m.cpu.csrs.mstatus_mpp(), PRIV_LEAST_SUPPORTED);
}

#[test]
fn trap_is_fatal_when_no_handler_installed() {
    // With mtvec still zero a trap would vector to address 0, fault on the fetch, and vector to
    // 0 again forever. Real hardware does exactly that; we stop and report instead, which is
    // far more useful while bringing a system up.
    let mut m = Machine::new(&[ebreak()]);
    assert_eq!(m.cpu.step().unwrap_err().cause, Exception::Breakpoint);
    assert_eq!(m.cpu.pc, DRAM_BASE); // PC left on the faulting instruction
}

#[test]
fn vectored_mtvec_applies_to_interrupts_only() {
    // In vectored mode interrupts land at base + 4*cause, but exceptions still go to the base
    // address. Getting this backwards sends exception handling off into the middle of a jump
    // table.
    let mut p = Vec::new();
    load_imm64(&mut p, 1, at(K + 3) | csr::MTVEC_MODE_VECTORED);
    p.push(csrrw(0, csr::MTVEC, 1)); // K
    p.push(ecall()); // K+1  an exception, cause 11
    p.push(addi(5, 0, 99)); // K+2
    p.push(addi(6, 0, 42)); // K+3  handler base
    p.push(halt()); // K+4

    let mut m = Machine::new(&p);
    m.run(200);
    assert_eq!(m.reg(6), 42); // landed at base, not at base + 4*11
    assert_eq!(m.reg(5), 0);
}

// --- interrupts -------------------------------------------------------------

#[test]
fn interrupt_fires_when_pending_and_enabled() {
    let mut p = with_handler(at(K + 5));
    p.push(csrrsi(0, csr::MSTATUS, 0x8)); // K+1  mstatus.MIE = 1
    p.push(addi(5, 0, 1)); // K+2  interrupted before this
    p.push(addi(5, 5, 1)); // K+3
    p.push(halt()); // K+4
    p.push(addi(6, 0, 77)); // K+5  handler
    p.push(halt()); // K+6

    let mut m = Machine::new(&p);
    // Enable and raise the machine timer interrupt, as the CLINT does for real.
    m.cpu.csrs.write(csr::MIE, csr::MIP_MTIP);
    m.cpu.csrs.raise_interrupt(csr::MIP_MTIP);

    m.run(200);
    assert_eq!(m.reg(6), 77); // the handler ran

    // mcause has the interrupt bit set, plus the timer cause number.
    let cause = m.cpu.csrs.read(csr::MCAUSE);
    assert_ne!(cause >> 63, 0);
    assert_eq!(cause & 0x3f, Interrupt::MachineTimer as u64);
}

#[test]
fn interrupt_is_gated_by_mstatus_mie() {
    // Pending in mip and enabled in mie, but the global enable is off, so nothing fires and the
    // normal path runs.
    let mut p = with_handler(at(K + 4));
    p.push(addi(5, 0, 1)); // K+1
    p.push(halt()); // K+2
    p.push(halt()); // K+3
    p.push(addi(6, 0, 99)); // K+4  handler, must not run

    let mut m = Machine::new(&p);
    m.cpu.csrs.write(csr::MIE, csr::MIP_MTIP);
    m.cpu.csrs.raise_interrupt(csr::MIP_MTIP);
    // mstatus.MIE deliberately left clear.

    m.run(200);
    assert_eq!(m.reg(5), 1); // the normal path ran
    assert_eq!(m.reg(6), 0); // the handler did not
}

#[test]
fn interrupt_mepc_points_at_the_uninterrupted_instruction() {
    // An interrupt is not caused by an instruction - it happens between them. So mepc must point
    // at the instruction that has NOT run yet, and MRET resumes exactly there rather than
    // skipping it.
    let mut p = with_handler(at(K + 3));
    p.push(csrrsi(0, csr::MSTATUS, 0x8)); // K+1  enables interrupts
    p.push(addi(5, 0, 1)); // K+2  never runs
    p.push(halt()); // K+3  handler

    let mut m = Machine::new(&p);
    m.cpu.csrs.write(csr::MIE, csr::MIP_MTIP);
    m.cpu.csrs.raise_interrupt(csr::MIP_MTIP);
    m.run(200);

    // The earliest point the interrupt can fire is after the CSRRSI that enabled it, so mepc is
    // the instruction following it.
    assert_eq!(m.cpu.csrs.read(csr::MEPC), at(K + 2));
    assert_eq!(m.reg(5), 0); // that instruction did not run
}

// --- WFI and counters -------------------------------------------------------

#[test]
fn wfi_is_a_nop() {
    // The spec explicitly permits implementing WFI as a no-op; software must re-check the
    // condition it was waiting on regardless.
    let mut m = Machine::new(&[wfi(), addi(1, 0, 5), ebreak()]);
    m.run(10);
    assert_eq!(m.reg(1), 5);
}

#[test]
fn instret_counts_retired_instructions() {
    let mut m = Machine::new(&[addi(1, 0, 1), addi(2, 0, 2), addi(3, 0, 3), csrrs(4, csr::INSTRET, 0)]);
    m.run(4);
    // Three ADDIs retired before the CSR read.
    assert_eq!(m.reg(4), 3);
}
