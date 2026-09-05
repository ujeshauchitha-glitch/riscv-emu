#include <vector>

#include "csr.hpp"
#include "machine.hpp"
#include "mmu.hpp"
#include "test_util.hpp"

// ---------------------------------------------------------------------------
// Supervisor mode and Sv39 virtual memory.
//
// Two themes dominate:
//
//   * sstatus, sie and sip are *views* onto mstatus, mie and mip - the same
//     physical bits, not copies. A separate copy would drift, and a kernel that
//     cleared sstatus.SIE would keep taking interrupts inside what it believed
//     was a critical section.
//
//   * translation and its permission checks, including the ones that exist for
//     security rather than for addressing: the U bit, SUM and MXR.
// ---------------------------------------------------------------------------

using namespace rvt;

namespace {

u32 CSRRW(u32 rd, u32 a, u32 rs1)  { return i_type(opcodes::SYSTEM, rd, 0x1, rs1, (i32)a); }
u32 CSRRS(u32 rd, u32 a, u32 rs1)  { return i_type(opcodes::SYSTEM, rd, 0x2, rs1, (i32)a); }
u32 CSRRC(u32 rd, u32 a, u32 rs1)  { return i_type(opcodes::SYSTEM, rd, 0x3, rs1, (i32)a); }
u32 ECALL()  { return i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x000); }
u32 MRET()   { return i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x302); }
u32 SRET()   { return i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x102); }
u32 SFENCE() { return r_type(opcodes::SYSTEM, 0, 0x0, 0, 0, 0x09); }

// --- the supervisor views ---------------------------------------------------

void test_sstatus_is_a_view_not_a_copy() {
    Machine m({});
    CsrFile& c = m.cpu->csrs;

    // Setting a bit through sstatus sets the same bit in mstatus.
    c.write(csr::SSTATUS, csr::MSTATUS_SIE);
    CHECK((c.mstatus() & csr::MSTATUS_SIE) != 0);

    // And clearing it through mstatus clears what sstatus reads.
    c.set_mstatus(c.mstatus() & ~csr::MSTATUS_SIE);
    CHECK((c.read(csr::SSTATUS) & csr::MSTATUS_SIE) == 0);

    // Machine-only bits are invisible through the supervisor view: setting MIE
    // in mstatus must not appear in sstatus.
    c.set_mstatus(c.mstatus() | csr::MSTATUS_MIE);
    CHECK((c.read(csr::SSTATUS) & csr::MSTATUS_MIE) == 0);
}

void test_sstatus_write_cannot_reach_machine_bits() {
    // A supervisor writing all-ones to sstatus must not gain machine
    // privileges. Only the bits the view exposes may change.
    Machine m({});
    CsrFile& c = m.cpu->csrs;

    const u64 before = c.mstatus();
    c.write(csr::SSTATUS, ~0ull);

    // MIE and MPP are machine-only and must be untouched.
    CHECK((c.mstatus() & csr::MSTATUS_MIE) == (before & csr::MSTATUS_MIE));
    CHECK((c.mstatus() & csr::MSTATUS_MPP) == (before & csr::MSTATUS_MPP));

    // Reading sstatus back shows only the bits the view actually implements -
    // nothing outside SSTATUS_MASK may read as set.
    CHECK_EQ_U(c.read(csr::SSTATUS) & ~csr::SSTATUS_MASK, 0);
}

void test_sie_sip_are_views() {
    Machine m({});
    CsrFile& c = m.cpu->csrs;

    c.write(csr::SIE, csr::MIP_SSIP);
    CHECK((c.read(csr::MIE) & csr::MIP_SSIP) != 0);

    // Machine interrupt enables are not visible or settable through sie.
    c.write(csr::MIE, csr::MIP_MTIP);
    CHECK((c.read(csr::SIE) & csr::MIP_MTIP) == 0);
    c.write(csr::SIE, ~0ull);
    CHECK_EQ_U(c.read(csr::SIE) & ~csr::SIE_SIP_MASK, 0);

    // Clearing through the view works the same way.
    Machine n({CSRRC(0, csr::SIE, 1), HALT()});
    n.cpu->csrs.write(csr::MIE, csr::MIP_SSIP | csr::MIP_MTIP);
    n.cpu->write_reg(1, csr::MIP_SSIP);
    CHECK(n.cpu->step());
    CHECK((n.cpu->csrs.read(csr::MIE) & csr::MIP_SSIP) == 0);
    // ...and cannot reach the machine bits it does not expose.
    CHECK((n.cpu->csrs.read(csr::MIE) & csr::MIP_MTIP) != 0);
}

// --- privilege transitions --------------------------------------------------

void test_mret_can_drop_to_supervisor() {
    const u64 K = kLoadImm64Steps;
    std::vector<u32> p;
    load_imm64(p, 1, DRAM_BASE + (K + 2) * 4);              // where to land
    p.push_back(CSRRW(0, csr::MEPC, 1));                    // K   mepc = target
    p.push_back(MRET());                                    // K+1
    p.push_back(ADDI(5, 0, 7));                             // K+2 runs in the new mode
    p.push_back(HALT());

    Machine m(p);
    // Run the setup, then set MPP = S directly. Doing it in guest code would
    // need another 64-bit constant loaded, and anything that wrote mstatus
    // afterwards would clobber the field again.
    m.cpu->run(K + 1, nullptr);
    m.cpu->csrs.set_mstatus((m.cpu->csrs.mstatus() & ~csr::MSTATUS_MPP) |
                            (u64(PRIV_SUPERVISOR) << csr::MSTATUS_MPP_SHIFT));
    m.cpu->run(50, nullptr);

    CHECK_EQ_U(m.reg(5), 7);
    CHECK_EQ_U(m.cpu->priv, PRIV_SUPERVISOR);
    // MPP is reset to the least-privileged mode, so a later MRET cannot use it
    // to climb back up.
    CHECK_EQ_U(m.cpu->csrs.mstatus_mpp(), PRIV_LEAST_SUPPORTED);
}

void test_sret_returns_to_the_mode_spp_records() {
    Machine m({SRET()});
    CsrFile& c = m.cpu->csrs;

    m.cpu->priv = PRIV_SUPERVISOR;
    c.write(csr::SEPC, DRAM_BASE + 0x40);
    c.set_mstatus(c.mstatus() & ~csr::MSTATUS_SPP);   // SPP = 0 -> return to user
    c.set_mstatus(c.mstatus() | csr::MSTATUS_SPIE);

    CHECK(m.cpu->step());
    CHECK_EQ_U(m.cpu->priv, PRIV_USER);
    CHECK_EQ_U(m.cpu->pc, DRAM_BASE + 0x40);
    CHECK(m.cpu->csrs.mstatus_sie());                // SIE restored from SPIE
    CHECK((m.cpu->csrs.mstatus() & csr::MSTATUS_SPP) == 0);
}

void test_sret_from_user_mode_is_illegal() {
    Machine m({SRET()});
    m.cpu->priv = PRIV_USER;
    const Status st = m.cpu->step();
    CHECK(!st);
    CHECK(st.trap.cause == Exception::IllegalInstruction);
}

void test_machine_csr_is_unreachable_from_supervisor() {
    // The CSR address itself encodes the minimum privilege, so this needs no
    // per-register table.
    Machine m({CSRRS(1, csr::MSTATUS, 0)});
    m.cpu->priv = PRIV_SUPERVISOR;
    const Status st = m.cpu->step();
    CHECK(!st);
    CHECK(st.trap.cause == Exception::IllegalInstruction);
}

// --- trap delegation --------------------------------------------------------

void test_delegated_trap_goes_to_supervisor() {
    Machine m({ECALL(), HALT()});
    CsrFile& c = m.cpu->csrs;

    m.cpu->priv = PRIV_SUPERVISOR;
    c.write(csr::MTVEC, DRAM_BASE + 0x100);
    c.write(csr::STVEC, DRAM_BASE + 0x200);
    // Delegate ecall-from-S to supervisor mode.
    c.write(csr::MEDELEG, 1ull << u64(Exception::ECallFromSMode));

    CHECK(m.cpu->step());
    CHECK_EQ_U(m.cpu->pc, DRAM_BASE + 0x200);          // stvec, not mtvec
    CHECK_EQ_U(m.cpu->priv, PRIV_SUPERVISOR);
    CHECK_EQ_U(c.read(csr::SCAUSE), u64(Exception::ECallFromSMode));
    CHECK_EQ_U(c.read(csr::SEPC), DRAM_BASE);
    CHECK((c.mstatus() & csr::MSTATUS_SPP) != 0);      // came from supervisor
    CHECK(!c.mstatus_sie());                           // interrupts off in handler
    // mcause must NOT have been written - the trap never reached machine mode.
    CHECK_EQ_U(c.read(csr::MCAUSE), 0);
}

void test_undelegated_trap_goes_to_machine() {
    Machine m({ECALL(), HALT()});
    CsrFile& c = m.cpu->csrs;

    m.cpu->priv = PRIV_SUPERVISOR;
    c.write(csr::MTVEC, DRAM_BASE + 0x100);
    c.write(csr::STVEC, DRAM_BASE + 0x200);
    c.write(csr::MEDELEG, 0);                          // delegate nothing

    CHECK(m.cpu->step());
    CHECK_EQ_U(m.cpu->pc, DRAM_BASE + 0x100);
    CHECK_EQ_U(m.cpu->priv, PRIV_MACHINE);
    CHECK_EQ_U(c.read(csr::MCAUSE), u64(Exception::ECallFromSMode));
}

void test_machine_mode_traps_are_never_delegated() {
    // Delegation only applies to traps taken in S or U mode. A trap taken in
    // machine mode has nothing more privileged to delegate from.
    Machine m({ECALL(), HALT()});
    CsrFile& c = m.cpu->csrs;

    c.write(csr::MTVEC, DRAM_BASE + 0x100);
    c.write(csr::STVEC, DRAM_BASE + 0x200);
    c.write(csr::MEDELEG, ~0ull);                      // delegate everything

    CHECK(m.cpu->step());
    CHECK_EQ_U(m.cpu->pc, DRAM_BASE + 0x100);          // still machine mode
    CHECK_EQ_U(c.read(csr::MCAUSE), u64(Exception::ECallFromMMode));
}

void test_ecall_cause_depends_on_the_calling_mode() {
    for (auto [priv, cause] : std::initializer_list<std::pair<u32, Exception>>{
             {PRIV_USER, Exception::ECallFromUMode},
             {PRIV_SUPERVISOR, Exception::ECallFromSMode},
             {PRIV_MACHINE, Exception::ECallFromMMode}}) {
        Machine m({ECALL()});
        m.cpu->priv = priv;
        const Status st = m.cpu->step();
        CHECK(!st);
        CHECK(st.trap.cause == cause);
    }
}

// --- Sv39 translation -------------------------------------------------------

// Builds a three-level page table mapping one 4 KiB virtual page to one
// physical page, with the given permissions. Returns the satp value.
u64 map_page(Machine& m, u64 root_pa, u64 vaddr, u64 paddr, u64 perms) {
    const u64 l1 = root_pa + PAGE_SIZE;      // second-level table
    const u64 l0 = root_pa + 2 * PAGE_SIZE;  // leaf table

    auto pte_for = [](u64 pa, u64 flags) {
        return ((pa >> PAGE_SHIFT) << pte::PPN_SHIFT) | flags;
    };
    auto idx = [&](int level) { return (vaddr >> (PAGE_SHIFT + 9 * level)) & 0x1ff; };

    // Interior entries have no R/W/X: that is what marks them as pointers to
    // the next level rather than leaves.
    m.bus.store(root_pa + idx(2) * 8, 8, pte_for(l1, pte::V));
    m.bus.store(l1 + idx(1) * 8, 8, pte_for(l0, pte::V));
    m.bus.store(l0 + idx(0) * 8, 8, pte_for(paddr, pte::V | perms));

    return (csr::SATP_MODE_SV39 << csr::SATP_MODE_SHIFT) | (root_pa >> PAGE_SHIFT);
}

void test_translation_is_off_in_machine_mode() {
    Machine m({});
    // Even with satp set, machine mode addresses physical memory directly.
    m.cpu->csrs.write(csr::SATP,
                      (csr::SATP_MODE_SV39 << csr::SATP_MODE_SHIFT) | 0x99);
    auto r = m.cpu->mmu.translate(0x1234, AccessType::Load, PRIV_MACHINE, m.cpu->csrs);
    CHECK(r && r.value == 0x1234);
}

void test_bare_mode_is_identity() {
    Machine m({});
    auto r = m.cpu->mmu.translate(0x8000'1234, AccessType::Load, PRIV_SUPERVISOR,
                                  m.cpu->csrs);
    CHECK(r && r.value == 0x8000'1234);
}

void test_sv39_walk_and_permissions() {
    Machine m({});
    const u64 root = DRAM_BASE + 0x10000;
    const u64 va   = 0x4000;
    const u64 pa   = DRAM_BASE + 0x20000;

    // A read-only supervisor page.
    m.cpu->csrs.write(csr::SATP, map_page(m, root, va, pa, pte::R));
    m.cpu->mmu.flush();

    auto load = m.cpu->mmu.translate(va + 0x30, AccessType::Load, PRIV_SUPERVISOR,
                                     m.cpu->csrs);
    CHECK(load);
    CHECK_EQ_U(load.value, pa + 0x30);   // the page offset is carried through

    // Writing it faults, and the cause distinguishes which access it was.
    auto store = m.cpu->mmu.translate(va, AccessType::Store, PRIV_SUPERVISOR,
                                      m.cpu->csrs);
    CHECK(!store);
    CHECK(store.trap.cause == Exception::StoreAMOPageFault);
    CHECK_EQ_U(store.trap.tval, va);     // stval holds the faulting virtual address

    // Executing it faults too, with a third distinct cause.
    auto fetch = m.cpu->mmu.translate(va, AccessType::Instruction, PRIV_SUPERVISOR,
                                      m.cpu->csrs);
    CHECK(!fetch);
    CHECK(fetch.trap.cause == Exception::InstructionPageFault);
}

void test_unmapped_address_page_faults() {
    Machine m({});
    const u64 root = DRAM_BASE + 0x10000;
    m.cpu->csrs.write(csr::SATP, map_page(m, root, 0x4000, DRAM_BASE + 0x20000, pte::R));
    m.cpu->mmu.flush();

    auto r = m.cpu->mmu.translate(0x999000, AccessType::Load, PRIV_SUPERVISOR,
                                  m.cpu->csrs);
    CHECK(!r);
    CHECK(r.trap.cause == Exception::LoadPageFault);
}

void test_user_bit_separates_the_two_worlds() {
    Machine m({});
    const u64 root = DRAM_BASE + 0x10000;
    const u64 va = 0x4000, pa = DRAM_BASE + 0x20000;

    // A supervisor page: no U bit.
    m.cpu->csrs.write(csr::SATP, map_page(m, root, va, pa, pte::R));
    m.cpu->mmu.flush();
    CHECK(m.cpu->mmu.translate(va, AccessType::Load, PRIV_SUPERVISOR, m.cpu->csrs));
    // User may not touch it.
    CHECK(!m.cpu->mmu.translate(va, AccessType::Load, PRIV_USER, m.cpu->csrs));
}

void test_sum_controls_supervisor_access_to_user_pages() {
    Machine m({});
    const u64 root = DRAM_BASE + 0x10000;
    const u64 va = 0x4000, pa = DRAM_BASE + 0x20000;

    // A user page: R and U, plus X so the fetch case can be checked too.
    m.cpu->csrs.write(csr::SATP,
                      map_page(m, root, va, pa, pte::R | pte::X | pte::U));
    m.cpu->mmu.flush();

    // Without SUM, a supervisor read of a user page faults. That is deliberate
    // hardening: a kernel tricked into dereferencing a user pointer would
    // otherwise read user memory as itself.
    CHECK(!m.cpu->mmu.translate(va, AccessType::Load, PRIV_SUPERVISOR, m.cpu->csrs));

    m.cpu->csrs.set_mstatus(m.cpu->csrs.mstatus() | csr::MSTATUS_SUM);
    m.cpu->mmu.flush();
    CHECK(m.cpu->mmu.translate(va, AccessType::Load, PRIV_SUPERVISOR, m.cpu->csrs));

    // SUM never permits *executing* a user page, however it is set: the kernel
    // must not run user code as itself.
    CHECK(!m.cpu->mmu.translate(va, AccessType::Instruction, PRIV_SUPERVISOR,
                                m.cpu->csrs));
}

void test_mxr_makes_execute_only_pages_readable() {
    Machine m({});
    const u64 root = DRAM_BASE + 0x10000;
    const u64 va = 0x4000, pa = DRAM_BASE + 0x20000;

    // Execute-only: X without R.
    m.cpu->csrs.write(csr::SATP, map_page(m, root, va, pa, pte::X));
    m.cpu->mmu.flush();

    CHECK(!m.cpu->mmu.translate(va, AccessType::Load, PRIV_SUPERVISOR, m.cpu->csrs));

    m.cpu->csrs.set_mstatus(m.cpu->csrs.mstatus() | csr::MSTATUS_MXR);
    m.cpu->mmu.flush();
    CHECK(m.cpu->mmu.translate(va, AccessType::Load, PRIV_SUPERVISOR, m.cpu->csrs));
}

void test_accessed_and_dirty_bits_are_set() {
    Machine m({});
    const u64 root = DRAM_BASE + 0x10000;
    const u64 va = 0x4000, pa = DRAM_BASE + 0x20000;
    m.cpu->csrs.write(csr::SATP, map_page(m, root, va, pa, pte::R | pte::W));
    m.cpu->mmu.flush();

    const u64 leaf_addr = root + 2 * PAGE_SIZE + (((va >> 12) & 0x1ff) * 8);

    m.cpu->mmu.translate(va, AccessType::Load, PRIV_SUPERVISOR, m.cpu->csrs);
    auto after_load = m.bus.load(leaf_addr, 8, AccessType::Load);
    CHECK((after_load.value & pte::A) != 0);   // accessed
    CHECK((after_load.value & pte::D) == 0);   // but not dirty: it was a read

    // Deliberately *not* flushed in between.
    //
    // The read above filled the TLB, so the store below is a TLB hit and never
    // walks the table again. An implementation that only sets D during the walk
    // leaves this page clean forever - and a kernel scanning for dirty pages
    // then treats modified pages as clean and drops the writes. This test used
    // to flush here, which meant it only ever exercised the walk and could not
    // have caught that.
    m.cpu->mmu.translate(va, AccessType::Store, PRIV_SUPERVISOR, m.cpu->csrs);
    auto after_store = m.bus.load(leaf_addr, 8, AccessType::Load);
    CHECK((after_store.value & pte::D) != 0);

    // And once set, it stays set on a subsequent read - D is sticky until
    // software clears it, which is the whole basis of dirty-page tracking.
    m.cpu->mmu.translate(va, AccessType::Load, PRIV_SUPERVISOR, m.cpu->csrs);
    auto after_reread = m.bus.load(leaf_addr, 8, AccessType::Load);
    CHECK((after_reread.value & pte::D) != 0);
}

void test_non_canonical_address_faults() {
    // Bits 63:39 must all equal bit 38. The unaddressable hole this creates is
    // what separates low user addresses from high kernel ones.
    Machine m({});
    const u64 root = DRAM_BASE + 0x10000;
    m.cpu->csrs.write(csr::SATP, map_page(m, root, 0x4000, DRAM_BASE + 0x20000, pte::R));
    m.cpu->mmu.flush();

    auto r = m.cpu->mmu.translate(0x0000'8000'0000'0000ull, AccessType::Load,
                                  PRIV_SUPERVISOR, m.cpu->csrs);
    CHECK(!r);
    CHECK(r.trap.cause == Exception::LoadPageFault);
}

void test_reserved_write_only_encoding_faults() {
    // W without R is reserved, not a write-only page.
    Machine m({});
    const u64 root = DRAM_BASE + 0x10000;
    m.cpu->csrs.write(csr::SATP, map_page(m, root, 0x4000, DRAM_BASE + 0x20000, pte::W));
    m.cpu->mmu.flush();

    auto r = m.cpu->mmu.translate(0x4000, AccessType::Store, PRIV_SUPERVISOR,
                                  m.cpu->csrs);
    CHECK(!r);
}

void test_tlb_caches_and_sfence_flushes() {
    Machine m({});
    const u64 root = DRAM_BASE + 0x10000;
    const u64 va = 0x4000, pa = DRAM_BASE + 0x20000;
    m.cpu->csrs.write(csr::SATP, map_page(m, root, va, pa, pte::R));
    m.cpu->mmu.flush();

    CHECK(m.cpu->mmu.translate(va, AccessType::Load, PRIV_SUPERVISOR, m.cpu->csrs));
    const u64 misses = m.cpu->mmu.tlb_misses();
    CHECK(m.cpu->mmu.translate(va, AccessType::Load, PRIV_SUPERVISOR, m.cpu->csrs));
    CHECK_EQ_U(m.cpu->mmu.tlb_misses(), misses);   // second time was a hit
    CHECK(m.cpu->mmu.tlb_hits() > 0);

    // Rewriting the page table is not enough on its own - nothing tells the
    // hardware to forget what it cached. That is what SFENCE.VMA is for.
    m.cpu->mmu.flush();
    CHECK(m.cpu->mmu.translate(va, AccessType::Load, PRIV_SUPERVISOR, m.cpu->csrs));
    CHECK(m.cpu->mmu.tlb_misses() > misses);
}

void test_sfence_vma_is_illegal_below_supervisor() {
    Machine m({SFENCE()});
    m.cpu->priv = PRIV_USER;
    const Status st = m.cpu->step();
    CHECK(!st);
    CHECK(st.trap.cause == Exception::IllegalInstruction);
}

void test_satp_rejects_unsupported_modes() {
    Machine m({});
    // Sv48 is not implemented. satp's MODE field is WARL, so the write is
    // ignored rather than stored - which is how a kernel discovers it must fall
    // back to Sv39.
    m.cpu->csrs.write(csr::SATP, csr::SATP_MODE_SV48 << csr::SATP_MODE_SHIFT);
    CHECK_EQ_U(m.cpu->csrs.satp_mode(), csr::SATP_MODE_BARE);

    m.cpu->csrs.write(csr::SATP, csr::SATP_MODE_SV39 << csr::SATP_MODE_SHIFT);
    CHECK_EQ_U(m.cpu->csrs.satp_mode(), csr::SATP_MODE_SV39);
}

void test_misa_advertises_s_and_u() {
    Machine m({});
    const u64 misa = m.cpu->csrs.read(csr::MISA);
    CHECK((misa & (1ull << ('S' - 'A'))) != 0);
    CHECK((misa & (1ull << ('U' - 'A'))) != 0);
}

void test_an_access_crossing_a_page_boundary_translates_both_pages() {
    // Translation is per page, so a misaligned access whose bytes fall in two
    // pages needs two translations. Translating only the starting address and
    // then reading eight contiguous *physical* bytes walks off the end of the
    // first page's frame into whatever follows it - with no fault and no
    // permission check. RISC-V allows misaligned accesses, so ordinary guest
    // code reaches this.
    Machine m({});
    const u64 root = DRAM_BASE + 0x10000;
    const u64 va_a = 0x100000, pa_a = DRAM_BASE + 0x20000;
    const u64 va_b = 0x101000, pa_b = DRAM_BASE + 0x40000;

    u64 satp = map_page(m, root, va_a, pa_a, pte::R | pte::W);
    map_page(m, root, va_b, pa_b, pte::R | pte::W);
    m.cpu->csrs.write(csr::SATP, satp);
    m.cpu->priv = PRIV_SUPERVISOR;
    m.cpu->mmu.flush();

    // Two distinguishable pages, and a third pattern in the physical memory
    // that *follows* the first page - which is what a single translation would
    // wrongly return.
    CHECK(m.bus.store(pa_a + 0xffc, 4, 0xaaaaaaaa));
    CHECK(m.bus.store(pa_b, 4, 0xbbbbbbbb));
    CHECK(m.bus.store(pa_a + 0x1000, 4, 0xcccccccc));

    auto r = m.cpu->mem_load(va_a + 0xffc, 8, AccessType::Load);
    CHECK(r);
    CHECK_EQ_U(r.value, 0xbbbbbbbb'aaaaaaaaull);

    // And a store lands in both pages rather than running past the first.
    CHECK(m.cpu->mem_store(va_a + 0xffc, 8, 0x11111111'22222222ull));
    auto first  = m.bus.load(pa_a + 0xffc, 4, AccessType::Load);
    auto second = m.bus.load(pa_b, 4, AccessType::Load);
    CHECK(first);
    CHECK(second);
    CHECK_EQ_U(first.value, 0x22222222);
    CHECK_EQ_U(second.value, 0x11111111);
    // The memory after the first page is untouched.
    auto beyond = m.bus.load(pa_a + 0x1000, 4, AccessType::Load);
    CHECK(beyond);
    CHECK_EQ_U(beyond.value, 0xcccccccc);
}

void test_a_crossing_access_faults_when_the_second_page_is_unmapped() {
    // The failure that matters: without a second translation there is no fault
    // at all, and the guest silently reads memory it was never given.
    Machine m({});
    const u64 root = DRAM_BASE + 0x10000;
    const u64 va   = 0x100000, pa = DRAM_BASE + 0x20000;

    m.cpu->csrs.write(csr::SATP, map_page(m, root, va, pa, pte::R | pte::W));
    m.cpu->priv = PRIV_SUPERVISOR;
    m.cpu->mmu.flush();

    auto r = m.cpu->mem_load(va + 0xffc, 8, AccessType::Load);
    CHECK(!r);
    CHECK(r.trap.cause == Exception::LoadPageFault);

    const Status st = m.cpu->mem_store(va + 0xffc, 8, 0);
    CHECK(!st);
    CHECK(st.trap.cause == Exception::StoreAMOPageFault);
}

void test_a_delegated_interrupt_does_not_fire_in_machine_mode() {
    // Trap entry into machine mode clears MIE but leaves SIE alone, so a hart
    // that trapped out of supervisor mode sits in M-mode with SIE still set.
    // Testing sstatus.SIE without also checking the current mode then lets a
    // delegated interrupt fire while running in machine mode - and enter_trap,
    // seeing priv > S, delivers a supervisor-cause interrupt to mtvec. With
    // mtvec zero, as under xv6 or --linux, that vectors to address 0.
    Machine m({});
    m.cpu->priv = PRIV_MACHINE;
    m.cpu->csrs.write(csr::MSTATUS,
                      (m.cpu->csrs.mstatus() & ~csr::MSTATUS_MIE) | csr::MSTATUS_SIE);
    m.cpu->csrs.write(csr::MIDELEG, 1ull << 5);        // delegate the S timer
    m.cpu->csrs.write(csr::MIE, csr::MIP_STIP);
    m.cpu->csrs.raise_interrupt(csr::MIP_STIP);

    Interrupt intr;
    CHECK(!m.cpu->next_interrupt(intr));

    // In supervisor mode, with the same state, it does fire - so the guard is
    // not simply disabling the interrupt.
    m.cpu->priv = PRIV_SUPERVISOR;
    CHECK(m.cpu->next_interrupt(intr));
    CHECK(intr == Interrupt::SupervisorTimer);
}

}  // namespace

int main() {
    test_sstatus_is_a_view_not_a_copy();
    test_sstatus_write_cannot_reach_machine_bits();
    test_sie_sip_are_views();
    test_mret_can_drop_to_supervisor();
    test_sret_returns_to_the_mode_spp_records();
    test_sret_from_user_mode_is_illegal();
    test_machine_csr_is_unreachable_from_supervisor();
    test_delegated_trap_goes_to_supervisor();
    test_undelegated_trap_goes_to_machine();
    test_machine_mode_traps_are_never_delegated();
    test_ecall_cause_depends_on_the_calling_mode();
    test_translation_is_off_in_machine_mode();
    test_bare_mode_is_identity();
    test_sv39_walk_and_permissions();
    test_unmapped_address_page_faults();
    test_user_bit_separates_the_two_worlds();
    test_sum_controls_supervisor_access_to_user_pages();
    test_mxr_makes_execute_only_pages_readable();
    test_accessed_and_dirty_bits_are_set();
    test_an_access_crossing_a_page_boundary_translates_both_pages();
    test_a_crossing_access_faults_when_the_second_page_is_unmapped();
    test_a_delegated_interrupt_does_not_fire_in_machine_mode();
    test_non_canonical_address_faults();
    test_reserved_write_only_encoding_faults();
    test_tlb_caches_and_sfence_flushes();
    test_sfence_vma_is_illegal_below_supervisor();
    test_satp_rejects_unsupported_modes();
    test_misa_advertises_s_and_u();
    return testutil::summary("supervisor");
}
