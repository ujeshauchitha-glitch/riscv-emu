//! Supervisor mode and Sv39 virtual memory.
//!
//! Two themes dominate:
//!
//!   * sstatus, sie and sip are *views* onto mstatus, mie and mip - the same physical bits, not
//!     copies. A separate copy would drift, and a kernel that cleared sstatus.SIE would keep taking
//!     interrupts inside what it believed was a critical section.
//!
//!   * translation and its permission checks, including the ones that exist for security rather than
//!     for addressing: the U bit, SUM and MXR.

mod common;

use common::*;
use riscv_emu::csr::csr;
use riscv_emu::decoder::opcodes;
use riscv_emu::device::AccessType;
use riscv_emu::mmu::{pte, PAGE_SHIFT, PAGE_SIZE};
use riscv_emu::trap::{Exception, Interrupt, Res};
use riscv_emu::types::{DRAM_BASE, PRIV_LEAST_SUPPORTED, PRIV_MACHINE, PRIV_SUPERVISOR, PRIV_USER};

fn csrrw(rd: u32, a: u32, rs1: u32) -> u32 {
    i_type(opcodes::SYSTEM, rd, 0x1, rs1, a as i32)
}
fn csrrs(rd: u32, a: u32, rs1: u32) -> u32 {
    i_type(opcodes::SYSTEM, rd, 0x2, rs1, a as i32)
}
fn csrrc(rd: u32, a: u32, rs1: u32) -> u32 {
    i_type(opcodes::SYSTEM, rd, 0x3, rs1, a as i32)
}
fn ecall() -> u32 {
    i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x000)
}
fn mret() -> u32 {
    i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x302)
}
fn sret() -> u32 {
    i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x102)
}
fn sfence() -> u32 {
    r_type(opcodes::SYSTEM, 0, 0x0, 0, 0, 0x09)
}

/// Translate through the hart's own MMU.
fn translate(m: &mut Machine, va: u64, ty: AccessType, priv_: u32) -> Res<u64> {
    m.cpu.mmu.translate(&m.bus, &m.cpu.csrs, va, ty, priv_)
}

// --- the supervisor views ---------------------------------------------------

#[test]
fn sstatus_is_a_view_not_a_copy() {
    let mut m = Machine::new(&[]);
    let c = &mut m.cpu.csrs;

    // Setting a bit through sstatus sets the same bit in mstatus.
    c.write(csr::SSTATUS, csr::MSTATUS_SIE);
    assert_ne!(c.mstatus() & csr::MSTATUS_SIE, 0);

    // And clearing it through mstatus clears what sstatus reads.
    c.set_mstatus(c.mstatus() & !csr::MSTATUS_SIE);
    assert_eq!(c.read(csr::SSTATUS) & csr::MSTATUS_SIE, 0);

    // Machine-only bits are invisible through the supervisor view: setting MIE in mstatus must not
    // appear in sstatus.
    c.set_mstatus(c.mstatus() | csr::MSTATUS_MIE);
    assert_eq!(c.read(csr::SSTATUS) & csr::MSTATUS_MIE, 0);
}

#[test]
fn sstatus_write_cannot_reach_machine_bits() {
    // A supervisor writing all-ones to sstatus must not gain machine privileges. Only the bits the
    // view exposes may change.
    let mut m = Machine::new(&[]);
    let c = &mut m.cpu.csrs;

    let before = c.mstatus();
    c.write(csr::SSTATUS, !0);

    // MIE and MPP are machine-only and must be untouched.
    assert_eq!(c.mstatus() & csr::MSTATUS_MIE, before & csr::MSTATUS_MIE);
    assert_eq!(c.mstatus() & csr::MSTATUS_MPP, before & csr::MSTATUS_MPP);

    // Reading sstatus back shows only the bits the view actually implements.
    assert_eq!(c.read(csr::SSTATUS) & !csr::SSTATUS_MASK, 0);
}

#[test]
fn sie_sip_are_views() {
    let mut m = Machine::new(&[]);
    let c = &mut m.cpu.csrs;

    c.write(csr::SIE, csr::MIP_SSIP);
    assert_ne!(c.read(csr::MIE) & csr::MIP_SSIP, 0);

    // Machine interrupt enables are not visible or settable through sie.
    c.write(csr::MIE, csr::MIP_MTIP);
    assert_eq!(c.read(csr::SIE) & csr::MIP_MTIP, 0);
    c.write(csr::SIE, !0);
    assert_eq!(c.read(csr::SIE) & !csr::SIE_SIP_MASK, 0);

    // Clearing through the view works the same way.
    let mut n = Machine::new(&[csrrc(0, csr::SIE, 1), halt()]);
    n.cpu.csrs.write(csr::MIE, csr::MIP_SSIP | csr::MIP_MTIP);
    n.cpu.write_reg(1, csr::MIP_SSIP);
    assert!(n.cpu.step().is_ok());
    assert_eq!(n.cpu.csrs.read(csr::MIE) & csr::MIP_SSIP, 0);
    // ...and cannot reach the machine bits it does not expose.
    assert_ne!(n.cpu.csrs.read(csr::MIE) & csr::MIP_MTIP, 0);
}

// --- privilege transitions --------------------------------------------------

#[test]
fn mret_can_drop_to_supervisor() {
    let k = LOAD_IMM64_STEPS;
    let mut p = Vec::new();
    load_imm64(&mut p, 1, DRAM_BASE + (k + 2) * 4); // where to land
    p.push(csrrw(0, csr::MEPC, 1)); // K   mepc = target
    p.push(mret()); // K+1
    p.push(addi(5, 0, 7)); // K+2 runs in the new mode
    p.push(halt());

    let mut m = Machine::new(&p);
    // Run the setup, then set MPP = S directly. Doing it in guest code would need another 64-bit
    // constant loaded, and anything that wrote mstatus afterwards would clobber the field again.
    m.run(k + 1);
    let status = (m.cpu.csrs.mstatus() & !csr::MSTATUS_MPP) | ((PRIV_SUPERVISOR as u64) << csr::MSTATUS_MPP_SHIFT);
    m.cpu.csrs.set_mstatus(status);
    m.run(50);

    assert_eq!(m.reg(5), 7);
    assert_eq!(m.cpu.priv_, PRIV_SUPERVISOR);
    // MPP is reset to the least-privileged mode, so a later MRET cannot use it to climb back up.
    assert_eq!(m.cpu.csrs.mstatus_mpp(), PRIV_LEAST_SUPPORTED);
}

#[test]
fn sret_returns_to_the_mode_spp_records() {
    let mut m = Machine::new(&[sret()]);
    m.cpu.priv_ = PRIV_SUPERVISOR;
    m.cpu.csrs.write(csr::SEPC, DRAM_BASE + 0x40);
    let c = &mut m.cpu.csrs;
    c.set_mstatus(c.mstatus() & !csr::MSTATUS_SPP); // SPP = 0 -> return to user
    c.set_mstatus(c.mstatus() | csr::MSTATUS_SPIE);

    assert!(m.cpu.step().is_ok());
    assert_eq!(m.cpu.priv_, PRIV_USER);
    assert_eq!(m.cpu.pc, DRAM_BASE + 0x40);
    assert!(m.cpu.csrs.mstatus_sie()); // SIE restored from SPIE
    assert_eq!(m.cpu.csrs.mstatus() & csr::MSTATUS_SPP, 0);
}

#[test]
fn sret_from_user_mode_is_illegal() {
    let mut m = Machine::new(&[sret()]);
    m.cpu.priv_ = PRIV_USER;
    assert_eq!(m.cpu.step().unwrap_err().cause, Exception::IllegalInstruction);
}

#[test]
fn machine_csr_is_unreachable_from_supervisor() {
    // The CSR address itself encodes the minimum privilege, so this needs no per-register table.
    let mut m = Machine::new(&[csrrs(1, csr::MSTATUS, 0)]);
    m.cpu.priv_ = PRIV_SUPERVISOR;
    assert_eq!(m.cpu.step().unwrap_err().cause, Exception::IllegalInstruction);
}

// --- trap delegation --------------------------------------------------------

#[test]
fn delegated_trap_goes_to_supervisor() {
    let mut m = Machine::new(&[ecall(), halt()]);
    m.cpu.priv_ = PRIV_SUPERVISOR;
    m.cpu.csrs.write(csr::MTVEC, DRAM_BASE + 0x100);
    m.cpu.csrs.write(csr::STVEC, DRAM_BASE + 0x200);
    // Delegate ecall-from-S to supervisor mode.
    m.cpu.csrs.write(csr::MEDELEG, 1 << (Exception::ECallFromSMode as u64));

    assert!(m.cpu.step().is_ok());
    let c = &m.cpu.csrs;
    assert_eq!(m.cpu.pc, DRAM_BASE + 0x200); // stvec, not mtvec
    assert_eq!(m.cpu.priv_, PRIV_SUPERVISOR);
    assert_eq!(c.read(csr::SCAUSE), Exception::ECallFromSMode as u64);
    assert_eq!(c.read(csr::SEPC), DRAM_BASE);
    assert_ne!(c.mstatus() & csr::MSTATUS_SPP, 0); // came from supervisor
    assert!(!c.mstatus_sie()); // interrupts off in handler
    // mcause must NOT have been written - the trap never reached machine mode.
    assert_eq!(c.read(csr::MCAUSE), 0);
}

#[test]
fn undelegated_trap_goes_to_machine() {
    let mut m = Machine::new(&[ecall(), halt()]);
    m.cpu.priv_ = PRIV_SUPERVISOR;
    m.cpu.csrs.write(csr::MTVEC, DRAM_BASE + 0x100);
    m.cpu.csrs.write(csr::STVEC, DRAM_BASE + 0x200);
    m.cpu.csrs.write(csr::MEDELEG, 0); // delegate nothing

    assert!(m.cpu.step().is_ok());
    assert_eq!(m.cpu.pc, DRAM_BASE + 0x100);
    assert_eq!(m.cpu.priv_, PRIV_MACHINE);
    assert_eq!(m.cpu.csrs.read(csr::MCAUSE), Exception::ECallFromSMode as u64);
}

#[test]
fn machine_mode_traps_are_never_delegated() {
    // Delegation only applies to traps taken in S or U mode. A trap taken in machine mode has
    // nothing more privileged to delegate from.
    let mut m = Machine::new(&[ecall(), halt()]);
    m.cpu.csrs.write(csr::MTVEC, DRAM_BASE + 0x100);
    m.cpu.csrs.write(csr::STVEC, DRAM_BASE + 0x200);
    m.cpu.csrs.write(csr::MEDELEG, !0); // delegate everything

    assert!(m.cpu.step().is_ok());
    assert_eq!(m.cpu.pc, DRAM_BASE + 0x100); // still machine mode
    assert_eq!(m.cpu.csrs.read(csr::MCAUSE), Exception::ECallFromMMode as u64);
}

#[test]
fn ecall_cause_depends_on_the_calling_mode() {
    for (priv_, cause) in [
        (PRIV_USER, Exception::ECallFromUMode),
        (PRIV_SUPERVISOR, Exception::ECallFromSMode),
        (PRIV_MACHINE, Exception::ECallFromMMode),
    ] {
        let mut m = Machine::new(&[ecall()]);
        m.cpu.priv_ = priv_;
        assert_eq!(m.cpu.step().unwrap_err().cause, cause);
    }
}

// --- Sv39 translation -------------------------------------------------------

/// Builds a three-level page table mapping one 4 KiB virtual page to one physical page, with the
/// given permissions. Returns the satp value.
fn map_page(m: &Machine, root_pa: u64, vaddr: u64, paddr: u64, perms: u64) -> u64 {
    let l1 = root_pa + PAGE_SIZE; // second-level table
    let l0 = root_pa + 2 * PAGE_SIZE; // leaf table

    let pte_for = |pa: u64, flags: u64| ((pa >> PAGE_SHIFT) << pte::PPN_SHIFT) | flags;
    let idx = |level: u32| (vaddr >> (PAGE_SHIFT + 9 * level)) & 0x1ff;

    // Interior entries have no R/W/X: that is what marks them as pointers to the next level rather
    // than leaves.
    m.bus.store(root_pa + idx(2) * 8, 8, pte_for(l1, pte::V)).unwrap();
    m.bus.store(l1 + idx(1) * 8, 8, pte_for(l0, pte::V)).unwrap();
    m.bus.store(l0 + idx(0) * 8, 8, pte_for(paddr, pte::V | perms)).unwrap();

    (csr::SATP_MODE_SV39 << csr::SATP_MODE_SHIFT) | (root_pa >> PAGE_SHIFT)
}

#[test]
fn translation_is_off_in_machine_mode() {
    let mut m = Machine::new(&[]);
    // Even with satp set, machine mode addresses physical memory directly.
    m.cpu.csrs.write(csr::SATP, (csr::SATP_MODE_SV39 << csr::SATP_MODE_SHIFT) | 0x99);
    assert_eq!(translate(&mut m, 0x1234, AccessType::Load, PRIV_MACHINE).unwrap(), 0x1234);
}

#[test]
fn bare_mode_is_identity() {
    let mut m = Machine::new(&[]);
    assert_eq!(translate(&mut m, 0x8000_1234, AccessType::Load, PRIV_SUPERVISOR).unwrap(), 0x8000_1234);
}

#[test]
fn sv39_walk_and_permissions() {
    let mut m = Machine::new(&[]);
    let root = DRAM_BASE + 0x10000;
    let va = 0x4000;
    let pa = DRAM_BASE + 0x20000;

    // A read-only supervisor page.
    let satp = map_page(&m, root, va, pa, pte::R);
    m.cpu.csrs.write(csr::SATP, satp);
    m.cpu.mmu.flush();

    // The page offset is carried through.
    assert_eq!(translate(&mut m, va + 0x30, AccessType::Load, PRIV_SUPERVISOR).unwrap(), pa + 0x30);

    // Writing it faults, and the cause distinguishes which access it was.
    let store = translate(&mut m, va, AccessType::Store, PRIV_SUPERVISOR).unwrap_err();
    assert_eq!(store.cause, Exception::StoreAMOPageFault);
    assert_eq!(store.tval, va); // stval holds the faulting virtual address

    // Executing it faults too, with a third distinct cause.
    let fetch = translate(&mut m, va, AccessType::Instruction, PRIV_SUPERVISOR).unwrap_err();
    assert_eq!(fetch.cause, Exception::InstructionPageFault);
}

#[test]
fn unmapped_address_page_faults() {
    let mut m = Machine::new(&[]);
    let root = DRAM_BASE + 0x10000;
    let satp = map_page(&m, root, 0x4000, DRAM_BASE + 0x20000, pte::R);
    m.cpu.csrs.write(csr::SATP, satp);
    m.cpu.mmu.flush();

    let r = translate(&mut m, 0x999000, AccessType::Load, PRIV_SUPERVISOR).unwrap_err();
    assert_eq!(r.cause, Exception::LoadPageFault);
}

#[test]
fn user_bit_separates_the_two_worlds() {
    let mut m = Machine::new(&[]);
    let root = DRAM_BASE + 0x10000;
    let (va, pa) = (0x4000, DRAM_BASE + 0x20000);

    // A supervisor page: no U bit.
    let satp = map_page(&m, root, va, pa, pte::R);
    m.cpu.csrs.write(csr::SATP, satp);
    m.cpu.mmu.flush();
    assert!(translate(&mut m, va, AccessType::Load, PRIV_SUPERVISOR).is_ok());
    // User may not touch it.
    assert!(translate(&mut m, va, AccessType::Load, PRIV_USER).is_err());
}

#[test]
fn sum_controls_supervisor_access_to_user_pages() {
    let mut m = Machine::new(&[]);
    let root = DRAM_BASE + 0x10000;
    let (va, pa) = (0x4000, DRAM_BASE + 0x20000);

    // A user page: R and U, plus X so the fetch case can be checked too.
    let satp = map_page(&m, root, va, pa, pte::R | pte::X | pte::U);
    m.cpu.csrs.write(csr::SATP, satp);
    m.cpu.mmu.flush();

    // Without SUM, a supervisor read of a user page faults. That is deliberate hardening: a kernel
    // tricked into dereferencing a user pointer would otherwise read user memory as itself.
    assert!(translate(&mut m, va, AccessType::Load, PRIV_SUPERVISOR).is_err());

    let status = m.cpu.csrs.mstatus() | csr::MSTATUS_SUM;
    m.cpu.csrs.set_mstatus(status);
    m.cpu.mmu.flush();
    assert!(translate(&mut m, va, AccessType::Load, PRIV_SUPERVISOR).is_ok());

    // SUM never permits *executing* a user page, however it is set: the kernel must not run user
    // code as itself.
    assert!(translate(&mut m, va, AccessType::Instruction, PRIV_SUPERVISOR).is_err());
}

#[test]
fn mxr_makes_execute_only_pages_readable() {
    let mut m = Machine::new(&[]);
    let root = DRAM_BASE + 0x10000;
    let (va, pa) = (0x4000, DRAM_BASE + 0x20000);

    // Execute-only: X without R.
    let satp = map_page(&m, root, va, pa, pte::X);
    m.cpu.csrs.write(csr::SATP, satp);
    m.cpu.mmu.flush();

    assert!(translate(&mut m, va, AccessType::Load, PRIV_SUPERVISOR).is_err());

    let status = m.cpu.csrs.mstatus() | csr::MSTATUS_MXR;
    m.cpu.csrs.set_mstatus(status);
    m.cpu.mmu.flush();
    assert!(translate(&mut m, va, AccessType::Load, PRIV_SUPERVISOR).is_ok());
}

#[test]
fn accessed_and_dirty_bits_are_set() {
    let mut m = Machine::new(&[]);
    let root = DRAM_BASE + 0x10000;
    let (va, pa) = (0x4000, DRAM_BASE + 0x20000);
    let satp = map_page(&m, root, va, pa, pte::R | pte::W);
    m.cpu.csrs.write(csr::SATP, satp);
    m.cpu.mmu.flush();

    let leaf_addr = root + 2 * PAGE_SIZE + (((va >> 12) & 0x1ff) * 8);

    translate(&mut m, va, AccessType::Load, PRIV_SUPERVISOR).unwrap();
    let after_load = m.bus.load(leaf_addr, 8, AccessType::Load).unwrap();
    assert_ne!(after_load & pte::A, 0); // accessed
    assert_eq!(after_load & pte::D, 0); // but not dirty: it was a read

    // Deliberately *not* flushed in between.
    //
    // The read above filled the TLB, so the store below is a TLB hit and never walks the table
    // again. An implementation that only sets D during the walk leaves this page clean forever - and
    // a kernel scanning for dirty pages then treats modified pages as clean and drops the writes.
    translate(&mut m, va, AccessType::Store, PRIV_SUPERVISOR).unwrap();
    let after_store = m.bus.load(leaf_addr, 8, AccessType::Load).unwrap();
    assert_ne!(after_store & pte::D, 0);

    // And once set, it stays set on a subsequent read - D is sticky until software clears it, which
    // is the whole basis of dirty-page tracking.
    translate(&mut m, va, AccessType::Load, PRIV_SUPERVISOR).unwrap();
    let after_reread = m.bus.load(leaf_addr, 8, AccessType::Load).unwrap();
    assert_ne!(after_reread & pte::D, 0);
}

#[test]
fn non_canonical_address_faults() {
    // Bits 63:39 must all equal bit 38. The unaddressable hole this creates is what separates low
    // user addresses from high kernel ones.
    let mut m = Machine::new(&[]);
    let root = DRAM_BASE + 0x10000;
    let satp = map_page(&m, root, 0x4000, DRAM_BASE + 0x20000, pte::R);
    m.cpu.csrs.write(csr::SATP, satp);
    m.cpu.mmu.flush();

    let r = translate(&mut m, 0x0000_8000_0000_0000, AccessType::Load, PRIV_SUPERVISOR).unwrap_err();
    assert_eq!(r.cause, Exception::LoadPageFault);
}

#[test]
fn reserved_write_only_encoding_faults() {
    // W without R is reserved, not a write-only page.
    let mut m = Machine::new(&[]);
    let root = DRAM_BASE + 0x10000;
    let satp = map_page(&m, root, 0x4000, DRAM_BASE + 0x20000, pte::W);
    m.cpu.csrs.write(csr::SATP, satp);
    m.cpu.mmu.flush();

    assert!(translate(&mut m, 0x4000, AccessType::Store, PRIV_SUPERVISOR).is_err());
}

#[test]
fn tlb_caches_and_sfence_flushes() {
    let mut m = Machine::new(&[]);
    let root = DRAM_BASE + 0x10000;
    let (va, pa) = (0x4000, DRAM_BASE + 0x20000);
    let satp = map_page(&m, root, va, pa, pte::R);
    m.cpu.csrs.write(csr::SATP, satp);
    m.cpu.mmu.flush();

    assert!(translate(&mut m, va, AccessType::Load, PRIV_SUPERVISOR).is_ok());
    let misses = m.cpu.mmu.tlb_misses();
    assert!(translate(&mut m, va, AccessType::Load, PRIV_SUPERVISOR).is_ok());
    assert_eq!(m.cpu.mmu.tlb_misses(), misses); // second time was a hit
    assert!(m.cpu.mmu.tlb_hits() > 0);

    // Rewriting the page table is not enough on its own - nothing tells the hardware to forget what
    // it cached. That is what SFENCE.VMA is for.
    m.cpu.mmu.flush();
    assert!(translate(&mut m, va, AccessType::Load, PRIV_SUPERVISOR).is_ok());
    assert!(m.cpu.mmu.tlb_misses() > misses);
}

#[test]
fn sfence_vma_is_illegal_below_supervisor() {
    let mut m = Machine::new(&[sfence()]);
    m.cpu.priv_ = PRIV_USER;
    assert_eq!(m.cpu.step().unwrap_err().cause, Exception::IllegalInstruction);
}

#[test]
fn satp_rejects_unsupported_modes() {
    let mut m = Machine::new(&[]);
    // Sv48 is not implemented. satp's MODE field is WARL, so the write is ignored rather than stored
    // - which is how a kernel discovers it must fall back to Sv39.
    m.cpu.csrs.write(csr::SATP, csr::SATP_MODE_SV48 << csr::SATP_MODE_SHIFT);
    assert_eq!(m.cpu.csrs.satp_mode(), csr::SATP_MODE_BARE);

    m.cpu.csrs.write(csr::SATP, csr::SATP_MODE_SV39 << csr::SATP_MODE_SHIFT);
    assert_eq!(m.cpu.csrs.satp_mode(), csr::SATP_MODE_SV39);
}

#[test]
fn misa_advertises_s_and_u() {
    let m = Machine::new(&[]);
    let misa = m.cpu.csrs.read(csr::MISA);
    assert_ne!(misa & (1 << (b'S' - b'A')), 0);
    assert_ne!(misa & (1 << (b'U' - b'A')), 0);
}

#[test]
fn an_access_crossing_a_page_boundary_translates_both_pages() {
    // Translation is per page, so a misaligned access whose bytes fall in two pages needs two
    // translations. Translating only the starting address and then reading eight contiguous
    // *physical* bytes walks off the end of the first page's frame into whatever follows it - with no
    // fault and no permission check. RISC-V allows misaligned accesses, so ordinary guest code
    // reaches this.
    let mut m = Machine::new(&[]);
    let root = DRAM_BASE + 0x10000;
    let (va_a, pa_a) = (0x100000, DRAM_BASE + 0x20000);
    let (va_b, pa_b) = (0x101000, DRAM_BASE + 0x40000);

    let satp = map_page(&m, root, va_a, pa_a, pte::R | pte::W);
    map_page(&m, root, va_b, pa_b, pte::R | pte::W);
    m.cpu.csrs.write(csr::SATP, satp);
    m.cpu.priv_ = PRIV_SUPERVISOR;
    m.cpu.mmu.flush();

    // Two distinguishable pages, and a third pattern in the physical memory that *follows* the first
    // page - which is what a single translation would wrongly return.
    m.bus.store(pa_a + 0xffc, 4, 0xaaaa_aaaa).unwrap();
    m.bus.store(pa_b, 4, 0xbbbb_bbbb).unwrap();
    m.bus.store(pa_a + 0x1000, 4, 0xcccc_cccc).unwrap();

    assert_eq!(m.cpu.mem_load(va_a + 0xffc, 8, AccessType::Load).unwrap(), 0xbbbb_bbbb_aaaa_aaaa);

    // And a store lands in both pages rather than running past the first.
    m.cpu.mem_store(va_a + 0xffc, 8, 0x1111_1111_2222_2222).unwrap();
    assert_eq!(m.bus.load(pa_a + 0xffc, 4, AccessType::Load).unwrap(), 0x2222_2222);
    assert_eq!(m.bus.load(pa_b, 4, AccessType::Load).unwrap(), 0x1111_1111);
    // The memory after the first page is untouched.
    assert_eq!(m.bus.load(pa_a + 0x1000, 4, AccessType::Load).unwrap(), 0xcccc_cccc);
}

#[test]
fn a_crossing_access_faults_when_the_second_page_is_unmapped() {
    // The failure that matters: without a second translation there is no fault at all, and the guest
    // silently reads memory it was never given.
    let mut m = Machine::new(&[]);
    let root = DRAM_BASE + 0x10000;
    let (va, pa) = (0x100000, DRAM_BASE + 0x20000);

    let satp = map_page(&m, root, va, pa, pte::R | pte::W);
    m.cpu.csrs.write(csr::SATP, satp);
    m.cpu.priv_ = PRIV_SUPERVISOR;
    m.cpu.mmu.flush();

    assert_eq!(m.cpu.mem_load(va + 0xffc, 8, AccessType::Load).unwrap_err().cause, Exception::LoadPageFault);
    assert_eq!(m.cpu.mem_store(va + 0xffc, 8, 0).unwrap_err().cause, Exception::StoreAMOPageFault);
}

#[test]
fn a_delegated_interrupt_does_not_fire_in_machine_mode() {
    // Trap entry into machine mode clears MIE but leaves SIE alone, so a hart that trapped out of
    // supervisor mode sits in M-mode with SIE still set. Testing sstatus.SIE without also checking
    // the current mode then lets a delegated interrupt fire while running in machine mode - and
    // enter_trap, seeing priv > S, delivers a supervisor-cause interrupt to mtvec. With mtvec zero,
    // as under xv6 or --linux, that vectors to address 0.
    let mut m = Machine::new(&[]);
    m.cpu.priv_ = PRIV_MACHINE;
    let status = (m.cpu.csrs.mstatus() & !csr::MSTATUS_MIE) | csr::MSTATUS_SIE;
    m.cpu.csrs.write(csr::MSTATUS, status);
    m.cpu.csrs.write(csr::MIDELEG, 1 << 5); // delegate the S timer
    m.cpu.csrs.write(csr::MIE, csr::MIP_STIP);
    m.cpu.csrs.raise_interrupt(csr::MIP_STIP);

    assert!(m.cpu.next_interrupt().is_none());

    // In supervisor mode, with the same state, it does fire - so the guard is not simply disabling the
    // interrupt.
    m.cpu.priv_ = PRIV_SUPERVISOR;
    assert_eq!(m.cpu.next_interrupt(), Some(Interrupt::SupervisorTimer));
}
