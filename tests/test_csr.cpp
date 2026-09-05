#include <vector>

#include "csr.hpp"
#include "machine.hpp"
#include "test_util.hpp"

// ---------------------------------------------------------------------------
// CSR and trap-handling tests.
//
// The heart of this phase is that a trap stops being "the emulator halts" and
// becomes "the guest's handler runs". These tests check the full round trip:
// an ECALL vectors to mtvec, the handler reads mcause to learn why, adjusts
// mepc, and MRET returns to the right instruction with the machine's state
// restored exactly.
// ---------------------------------------------------------------------------

using namespace rvt;

namespace {

// --- CSR instruction encoders ----------------------------------------------

u32 CSRRW(u32 rd, u32 csr_addr, u32 rs1) {
    return i_type(opcodes::SYSTEM, rd, 0x1, rs1, static_cast<i32>(csr_addr));
}
u32 CSRRS(u32 rd, u32 csr_addr, u32 rs1) {
    return i_type(opcodes::SYSTEM, rd, 0x2, rs1, static_cast<i32>(csr_addr));
}
u32 CSRRC(u32 rd, u32 csr_addr, u32 rs1) {
    return i_type(opcodes::SYSTEM, rd, 0x3, rs1, static_cast<i32>(csr_addr));
}
u32 CSRRWI(u32 rd, u32 csr_addr, u32 uimm) {
    return i_type(opcodes::SYSTEM, rd, 0x5, uimm, static_cast<i32>(csr_addr));
}
u32 CSRRSI(u32 rd, u32 csr_addr, u32 uimm) {
    return i_type(opcodes::SYSTEM, rd, 0x6, uimm, static_cast<i32>(csr_addr));
}
u32 ECALL()  { return i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x000); }
u32 EBREAK() { return i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x001); }
u32 MRET()   { return i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x302); }
u32 WFI()    { return i_type(opcodes::SYSTEM, 0, 0x0, 0, 0x105); }

// --- basic CSR read/write ---------------------------------------------------

void test_csrrw_swaps() {
    // mscratch is a plain scratch register with no side effects, so it is the
    // clean way to test the swap semantics themselves.
    std::vector<u32> p;
    load_imm64(p, 1, 0xdead'beefull);
    p.push_back(CSRRW(2, csr::MSCRATCH, 1));   // x2 = mscratch(0); mscratch = x1
    p.push_back(CSRRW(3, csr::MSCRATCH, 0));   // x3 = mscratch; mscratch = 0

    Machine m(p);
    m.cpu->run(kLoadImm64Steps + 2, nullptr);
    CHECK_EQ_U(m.reg(2), 0);                  // old value
    CHECK_EQ_U(m.reg(3), 0xdead'beefull);     // what we wrote
    CHECK_EQ_U(m.cpu->csrs.read(csr::MSCRATCH), 0);
}

void test_csrrs_and_csrrc_set_and_clear() {
    std::vector<u32> p;
    load_imm64(p, 1, 0b1010);
    load_imm64(p, 2, 0b0010);
    p.push_back(CSRRS(0, csr::MSCRATCH, 1));  // mscratch |= 0b1010
    p.push_back(CSRRS(3, csr::MSCRATCH, 2));  // x3 = 0b1010; mscratch |= 0b0010
    p.push_back(CSRRC(4, csr::MSCRATCH, 2));  // x4 = 0b1010; mscratch &= ~0b0010

    Machine m(p);
    m.cpu->run(kLoadImm64Steps * 2 + 3, nullptr);
    CHECK_EQ_U(m.reg(3), 0b1010);
    CHECK_EQ_U(m.reg(4), 0b1010);
    CHECK_EQ_U(m.cpu->csrs.read(csr::MSCRATCH), 0b1000);
}

void test_immediate_forms_use_a_five_bit_immediate() {
    // In CSRRWI the rs1 field is a value, not a register number. Reading x31
    // here instead of using 31 as a literal would give 0.
    Machine m({CSRRWI(1, csr::MSCRATCH, 31)});
    m.cpu->run(1, nullptr);
    CHECK_EQ_U(m.cpu->csrs.read(csr::MSCRATCH), 31);

    Machine n({CSRRSI(1, csr::MSCRATCH, 5)});
    n.cpu->run(1, nullptr);
    CHECK_EQ_U(n.cpu->csrs.read(csr::MSCRATCH), 5);
}

// --- the suppression rules --------------------------------------------------

void test_csrrs_with_x0_source_does_not_write() {
    // `csrr rd, csr` assembles to CSRRS with rs1 = x0 and must be a pure read.
    // Because no write is attempted, it is legal even on a read-only CSR -
    // which is exactly how software reads mhartid.
    Machine m({CSRRS(1, csr::MHARTID, 0)});
    const Status st = m.cpu->step();
    CHECK(st);                      // no trap, despite mhartid being read-only
    CHECK_EQ_U(m.reg(1), 0);        // single hart, id 0
}

void test_csrrw_to_read_only_csr_traps() {
    // An actual write to a read-only CSR is illegal. Address bits [11:10] == 11
    // mark the whole 0xC00-0xFFF range as read-only, so no per-register table
    // is needed to know this.
    Machine m({CSRRW(0, csr::MHARTID, 1)});
    const Status st = m.cpu->step();
    CHECK(!st);
    CHECK(st.trap.cause == Exception::IllegalInstruction);
}

void test_unimplemented_csr_traps() {
    // Probing an unimplemented CSR and catching the trap is how software
    // detects optional features, so this must fault rather than read zero.
    Machine m({CSRRS(1, 0x7c0, 0)});   // a custom CSR we do not implement
    const Status st = m.cpu->step();
    CHECK(!st);
    CHECK(st.trap.cause == Exception::IllegalInstruction);
}

// --- CSR-specific write masking ---------------------------------------------

void test_mstatus_masks_unimplemented_bits() {
    // Writing all-ones to mstatus must not make reserved bits read back as set.
    std::vector<u32> p;
    load_imm64(p, 1, 0xffff'ffff'ffff'ffffull);
    p.push_back(CSRRW(0, csr::MSTATUS, 1));

    Machine m(p);
    m.cpu->run(kLoadImm64Steps + 1, nullptr);

    const u64 status = m.cpu->csrs.mstatus();
    // The bits this phase implements are set...
    CHECK((status & csr::MSTATUS_MIE) != 0);
    CHECK((status & csr::MSTATUS_MPIE) != 0);
    CHECK_EQ_U((status & csr::MSTATUS_MPP) >> csr::MSTATUS_MPP_SHIFT, PRIV_MACHINE);
    // ...and nothing else is. Phase 6 added the supervisor bits, so the set of
    // implemented bits grew; anything outside it must still read back as zero
    // however the guest writes it.
    const u64 implemented =
        csr::MSTATUS_MIE | csr::MSTATUS_MPIE | csr::MSTATUS_MPP |
        csr::MSTATUS_SIE | csr::MSTATUS_SPIE | csr::MSTATUS_SPP |
        csr::MSTATUS_MPRV | csr::MSTATUS_SUM | csr::MSTATUS_MXR |
        csr::MSTATUS_TVM | csr::MSTATUS_TW | csr::MSTATUS_TSR |
        csr::MSTATUS_FS | csr::MSTATUS_SD;
    CHECK_EQ_U(status & ~implemented, 0);

    // SD is read-only and derived: it is set exactly when FS is Dirty, so it
    // can never disagree with the field it summarises. A context switch reads
    // that one bit rather than picking a field apart.
    CHECK_EQ_U(status & csr::MSTATUS_FS, csr::MSTATUS_FS_DIRTY);
    CHECK((status & csr::MSTATUS_SD) != 0);

    m.cpu->csrs.write(csr::MSTATUS,
                      (status & ~csr::MSTATUS_FS) | csr::MSTATUS_FS_CLEAN);
    CHECK((m.cpu->csrs.mstatus() & csr::MSTATUS_SD) == 0);
}

void test_mtvec_mode_is_warl() {
    // Only modes 0 and 1 are defined. Mode 2 and 3 are reserved, and because
    // the field is WARL an implementation coerces rather than storing them.
    std::vector<u32> p;
    load_imm64(p, 1, 0x8000'1000ull | 0x3);   // base + reserved mode 3
    p.push_back(CSRRW(0, csr::MTVEC, 1));

    Machine m(p);
    m.cpu->run(kLoadImm64Steps + 1, nullptr);
    const u64 tvec = m.cpu->csrs.read(csr::MTVEC);
    CHECK_EQ_U(tvec & ~csr::MTVEC_MODE_MASK, 0x8000'1000ull);
    CHECK_EQ_U(tvec & csr::MTVEC_MODE_MASK, csr::MTVEC_MODE_DIRECT);
}

void test_mepc_low_bit_is_hardwired_zero() {
    // mepc always holds an instruction address, so bit 0 is dropped. Bit 1 is
    // kept: with the C extension IALIGN is 16 and a 2-byte-aligned address is
    // a perfectly ordinary place for an instruction to be. See
    // test_epc_registers_keep_bit_1_with_the_c_extension for why that matters.
    std::vector<u32> p;
    load_imm64(p, 1, 0x8000'1007ull);
    p.push_back(CSRRW(0, csr::MEPC, 1));

    Machine m(p);
    m.cpu->run(kLoadImm64Steps + 1, nullptr);
    CHECK_EQ_U(m.cpu->csrs.read(csr::MEPC), 0x8000'1006ull);
}

void test_misa_reports_rv64i() {
    Machine m({CSRRS(1, csr::MISA, 0)});
    m.cpu->run(1, nullptr);
    const u64 misa = m.reg(1);
    // MXL = 2 means RV64, in the top two bits.
    CHECK_EQ_U(misa >> 62, 2);
    // I, M, A, C, F and D are all implemented. misa must not claim more than
    // the emulator delivers, because guest code reads it to decide what it may
    // use - and must not claim less, because a kernel that sees no C bit may
    // refuse to run compressed code it emitted.
    CHECK((misa & (1ull << ('I' - 'A'))) != 0);
    CHECK((misa & (1ull << ('M' - 'A'))) != 0);
    CHECK((misa & (1ull << ('A' - 'A'))) != 0);
    CHECK((misa & (1ull << ('C' - 'A'))) != 0);
    CHECK((misa & (1ull << ('F' - 'A'))) != 0);
    CHECK((misa & (1ull << ('D' - 'A'))) != 0);
}

void test_epc_registers_keep_bit_1_with_the_c_extension() {
    // mepc and sepc hold an instruction address, so bit 0 is hardwired to zero.
    // Bit 1 is *not*, once the C extension is implemented: IALIGN becomes 16
    // and instructions legitimately live at 2-byte-aligned addresses.
    //
    // This is not a corner case. xv6 built for rv64gc puts `main` at
    // 0x8000_0dee; start() writes that to mepc and returns to it with mret. An
    // implementation that masks bit 1 as well returns two bytes early, into the
    // middle of whichever function the linker happened to place before it.
    CsrFile csrs;

    csrs.write(csr::MEPC, DRAM_BASE + 0xdee);
    CHECK_EQ_U(csrs.read(csr::MEPC), DRAM_BASE + 0xdee);
    csrs.write(csr::SEPC, DRAM_BASE + 0xdee);
    CHECK_EQ_U(csrs.read(csr::SEPC), DRAM_BASE + 0xdee);

    // Bit 0 is still dropped: there is no such thing as an odd instruction
    // address at any IALIGN.
    csrs.write(csr::MEPC, DRAM_BASE + 0xdef);
    CHECK_EQ_U(csrs.read(csr::MEPC), DRAM_BASE + 0xdee);
    csrs.write(csr::SEPC, DRAM_BASE + 0xdef);
    CHECK_EQ_U(csrs.read(csr::SEPC), DRAM_BASE + 0xdee);
}

// --- trap dispatch ----------------------------------------------------------
//
// Programs below are laid out by instruction index. `with_handler` emits
// load_imm64 (kLoadImm64Steps instructions, indices 0..k-1) followed by the
// CSRRW that installs mtvec at index k, so the first instruction a test adds
// sits at index k+1.

// Installs `handler_addr` into mtvec. Returns the program so far.
std::vector<u32> with_handler(u64 handler_addr) {
    std::vector<u32> p;
    load_imm64(p, 1, handler_addr);
    p.push_back(CSRRW(0, csr::MTVEC, 1));
    return p;
}

// Address of instruction index `i`.
u64 at(u64 i) { return DRAM_BASE + i * 4; }

constexpr u64 K = kLoadImm64Steps;

void test_ecall_vectors_to_mtvec() {
    std::vector<u32> p = with_handler(at(K + 4));
    p.push_back(ECALL());          // K+1
    p.push_back(ADDI(5, 0, 99));   // K+2  must not run
    p.push_back(HALT());           // K+3
    p.push_back(ADDI(6, 0, 42));   // K+4  handler
    p.push_back(HALT());           // K+5

    Machine m(p);
    m.cpu->run(200, nullptr);

    CHECK_EQ_U(m.reg(6), 42);   // the handler ran
    CHECK_EQ_U(m.reg(5), 0);    // the instruction after ecall did not
}

void test_trap_records_cause_epc_and_status() {
    std::vector<u32> p = with_handler(at(K + 3));
    p.push_back(ECALL());          // K+1
    p.push_back(ADDI(5, 0, 99));   // K+2
    p.push_back(HALT());           // K+3  handler: spin without trapping again

    Machine m(p);
    m.cpu->run(200, nullptr);

    // mcause says why we trapped. ECALL from machine mode is cause 11.
    CHECK_EQ_U(m.cpu->csrs.read(csr::MCAUSE),
               static_cast<u64>(Exception::ECallFromMMode));

    // mepc points at the ECALL itself, not past it. A handler that wants to
    // resume after the call must add 4 - which is exactly how a syscall
    // return works.
    CHECK_EQ_U(m.cpu->csrs.read(csr::MEPC), at(K + 1));

    // MPP records the privilege the trap came from.
    CHECK_EQ_U(m.cpu->csrs.mstatus_mpp(), PRIV_MACHINE);

    // Interrupts are disabled on entry, so the handler cannot be re-entered
    // by the source that just fired.
    CHECK(!m.cpu->csrs.mstatus_mie());
}

void test_mret_returns_and_restores() {
    // The full round trip: enable interrupts, ECALL, the handler advances mepc
    // past the ECALL and returns, execution resumes at the next instruction
    // with MIE restored.
    std::vector<u32> p = with_handler(at(K + 5));
    p.push_back(CSRRSI(0, csr::MSTATUS, 0x8));  // K+1  mstatus.MIE = 1
    p.push_back(ECALL());                       // K+2
    p.push_back(ADDI(5, 0, 7));                 // K+3  must run after MRET
    p.push_back(HALT());                        // K+4
    p.push_back(CSRRS(1, csr::MEPC, 0));        // K+5  handler: x1 = mepc
    p.push_back(ADDI(1, 1, 4));                 // K+6  skip past the ecall
    p.push_back(CSRRW(0, csr::MEPC, 1));        // K+7  mepc = x1
    p.push_back(ADDI(6, 0, 3));                 // K+8  mark the handler ran
    p.push_back(MRET());                        // K+9

    Machine m(p);
    m.cpu->run(200, nullptr);

    CHECK_EQ_U(m.reg(6), 3);   // handler ran
    CHECK_EQ_U(m.reg(5), 7);   // and we returned to the right instruction

    // MRET restores MIE from MPIE, and sets MPIE.
    CHECK(m.cpu->csrs.mstatus_mie());
    CHECK((m.cpu->csrs.mstatus() & csr::MSTATUS_MPIE) != 0);
    // MPP is reset to the least-privileged supported mode.
    CHECK_EQ_U(m.cpu->csrs.mstatus_mpp(), PRIV_LEAST_SUPPORTED);
}

void test_trap_is_fatal_when_no_handler_installed() {
    // With mtvec still zero a trap would vector to address 0, fault on the
    // fetch, and vector to 0 again forever. Real hardware does exactly that;
    // we stop and report instead, which is far more useful while bringing a
    // system up.
    Machine m({EBREAK()});
    const Status st = m.cpu->step();
    CHECK(!st);
    CHECK(st.trap.cause == Exception::Breakpoint);
    CHECK_EQ_U(m.cpu->pc, DRAM_BASE);   // PC left on the faulting instruction
}

void test_vectored_mtvec_applies_to_interrupts_only() {
    // In vectored mode interrupts land at base + 4*cause, but exceptions still
    // go to the base address. Getting this backwards sends exception handling
    // off into the middle of a jump table.
    std::vector<u32> p;
    load_imm64(p, 1, at(K + 3) | csr::MTVEC_MODE_VECTORED);
    p.push_back(CSRRW(0, csr::MTVEC, 1));  // K
    p.push_back(ECALL());                  // K+1  an exception, cause 11
    p.push_back(ADDI(5, 0, 99));           // K+2
    p.push_back(ADDI(6, 0, 42));           // K+3  handler base
    p.push_back(HALT());                   // K+4

    Machine m(p);
    m.cpu->run(200, nullptr);
    CHECK_EQ_U(m.reg(6), 42);   // landed at base, not at base + 4*11
    CHECK_EQ_U(m.reg(5), 0);
}

// --- interrupts -------------------------------------------------------------

void test_interrupt_fires_when_pending_and_enabled() {
    std::vector<u32> p = with_handler(at(K + 5));
    p.push_back(CSRRSI(0, csr::MSTATUS, 0x8));  // K+1  mstatus.MIE = 1
    p.push_back(ADDI(5, 0, 1));                 // K+2  interrupted before this
    p.push_back(ADDI(5, 5, 1));                 // K+3
    p.push_back(HALT());                        // K+4
    p.push_back(ADDI(6, 0, 77));                // K+5  handler
    p.push_back(HALT());                        // K+6

    Machine m(p);
    // Enable and raise the machine timer interrupt, as the CLINT will do for
    // real in phase 4.
    m.cpu->csrs.write(csr::MIE, csr::MIE_MTIE);
    m.cpu->csrs.raise_interrupt(csr::MIP_MTIP);

    m.cpu->run(200, nullptr);
    CHECK_EQ_U(m.reg(6), 77);   // the handler ran

    // mcause has the interrupt bit set, plus the timer cause number.
    const u64 cause = m.cpu->csrs.read(csr::MCAUSE);
    CHECK((cause >> 63) != 0);
    CHECK_EQ_U(cause & 0x3f, static_cast<u64>(Interrupt::MachineTimer));
}

void test_interrupt_is_gated_by_mstatus_mie() {
    // Pending in mip and enabled in mie, but the global enable is off, so
    // nothing fires and the normal path runs.
    std::vector<u32> p = with_handler(at(K + 4));
    p.push_back(ADDI(5, 0, 1));   // K+1
    p.push_back(HALT());          // K+2
    p.push_back(HALT());          // K+3
    p.push_back(ADDI(6, 0, 99));  // K+4  handler, must not run

    Machine m(p);
    m.cpu->csrs.write(csr::MIE, csr::MIE_MTIE);
    m.cpu->csrs.raise_interrupt(csr::MIP_MTIP);
    // mstatus.MIE deliberately left clear.

    m.cpu->run(200, nullptr);
    CHECK_EQ_U(m.reg(5), 1);   // the normal path ran
    CHECK_EQ_U(m.reg(6), 0);   // the handler did not
}

void test_interrupt_mepc_points_at_the_uninterrupted_instruction() {
    // An interrupt is not caused by an instruction - it happens between them.
    // So mepc must point at the instruction that has NOT run yet, and MRET
    // resumes exactly there rather than skipping it.
    std::vector<u32> p = with_handler(at(K + 3));
    p.push_back(CSRRSI(0, csr::MSTATUS, 0x8));  // K+1  enables interrupts
    p.push_back(ADDI(5, 0, 1));                 // K+2  never runs
    p.push_back(HALT());                        // K+3  handler

    Machine m(p);
    m.cpu->csrs.write(csr::MIE, csr::MIE_MTIE);
    m.cpu->csrs.raise_interrupt(csr::MIP_MTIP);
    m.cpu->run(200, nullptr);

    // The earliest point the interrupt can fire is after the CSRRSI that
    // enabled it, so mepc is the instruction following it.
    CHECK_EQ_U(m.cpu->csrs.read(csr::MEPC), at(K + 2));
    CHECK_EQ_U(m.reg(5), 0);   // that instruction did not run
}

// --- WFI and counters -------------------------------------------------------

void test_wfi_is_a_nop() {
    // The spec explicitly permits implementing WFI as a no-op; software must
    // re-check the condition it was waiting on regardless.
    Machine m({WFI(), ADDI(1, 0, 5), EBREAK()});
    m.cpu->run(10, nullptr);
    CHECK_EQ_U(m.reg(1), 5);
}

void test_instret_counts_retired_instructions() {
    Machine m({ADDI(1, 0, 1), ADDI(2, 0, 2), ADDI(3, 0, 3),
               CSRRS(4, csr::INSTRET, 0)});
    m.cpu->run(4, nullptr);
    // Three ADDIs retired before the CSR read.
    CHECK_EQ_U(m.reg(4), 3);
}

}  // namespace

int main() {
    test_csrrw_swaps();
    test_csrrs_and_csrrc_set_and_clear();
    test_immediate_forms_use_a_five_bit_immediate();
    test_csrrs_with_x0_source_does_not_write();
    test_csrrw_to_read_only_csr_traps();
    test_unimplemented_csr_traps();
    test_mstatus_masks_unimplemented_bits();
    test_mtvec_mode_is_warl();
    test_mepc_low_bit_is_hardwired_zero();
    test_misa_reports_rv64i();
    test_epc_registers_keep_bit_1_with_the_c_extension();
    test_ecall_vectors_to_mtvec();
    test_trap_records_cause_epc_and_status();
    test_mret_returns_and_restores();
    test_trap_is_fatal_when_no_handler_installed();
    test_vectored_mtvec_applies_to_interrupts_only();
    test_interrupt_fires_when_pending_and_enabled();
    test_interrupt_is_gated_by_mstatus_mie();
    test_interrupt_mepc_points_at_the_uninterrupted_instruction();
    test_wfi_is_a_nop();
    test_instret_counts_retired_instructions();
    return testutil::summary("csr");
}
