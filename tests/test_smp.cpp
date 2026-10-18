#include <cstring>
#include <memory>
#include <vector>

#include "clint.hpp"
#include "csr.hpp"
#include "fdt.hpp"
#include "machine.hpp"
#include "platform.hpp"
#include "plic.hpp"
#include "sbi.hpp"
#include "syscon.hpp"
#include "test_util.hpp"

// ---------------------------------------------------------------------------
// Multiple harts.
//
// These tests run real RISC-V code on several harts at once and check what the
// harts did to shared memory - which is the only honest way to test a
// multiprocessor: unit-testing the pieces proves each works alone, not that
// they work together.
//
// Programs are tiny, hand-assembled with the independent encoders in
// machine.hpp. Constants are loaded with load_imm64 (a fixed 17-instruction
// sequence) so every hart's prologue takes exactly the same number of steps,
// which the interleaving tests depend on.
// ---------------------------------------------------------------------------

using namespace rvt;

namespace {

constexpr u64 DATA = DRAM_BASE + 0x10000;   // shared scratch memory for results

// --- instruction helpers not in machine.hpp ----------------------------------

u32 SLLI(u32 rd, u32 rs1, u32 sh) { return i_type(opcodes::OP_IMM, rd, 0x1, rs1, static_cast<i32>(sh)); }
u32 SD(u32 rs2, u32 rs1, i32 off)  { return s_type(opcodes::STORE, 0x3, rs1, rs2, off); }
u32 SW(u32 rs2, u32 rs1, i32 off)  { return s_type(opcodes::STORE, 0x2, rs1, rs2, off); }
u32 BNE(u32 rs1, u32 rs2, i32 off) { return b_type(opcodes::BRANCH, 0x1, rs1, rs2, off); }
u32 CSRR(u32 rd, u32 csr)          { return i_type(opcodes::SYSTEM, rd, 0x2, 0, static_cast<i32>(csr)); }
u32 CSRW(u32 csr, u32 rs1)         { return i_type(opcodes::SYSTEM, 0, 0x1, rs1, static_cast<i32>(csr)); }
u32 CSRSI(u32 csr, u32 uimm)       { return i_type(opcodes::SYSTEM, 0, 0x6, uimm, static_cast<i32>(csr)); }
constexpr u32 ECALL = 0x00000073;
constexpr u32 WFI   = 0x10500073;
u32 LR_D(u32 rd, u32 rs1)              { return r_type(opcodes::AMO, rd, 0x3, rs1, 0, 0x02 << 2); }
u32 SC_D(u32 rd, u32 rs1, u32 rs2)     { return r_type(opcodes::AMO, rd, 0x3, rs1, rs2, 0x03 << 2); }
u32 AMOADD_D(u32 rd, u32 rs1, u32 rs2) { return r_type(opcodes::AMO, rd, 0x3, rs1, rs2, 0x00 << 2); }

// A register name per the ABI, to keep the programs readable.
enum : u32 { ZERO = 0, T0 = 5, T1 = 6, T2 = 7, A0 = 10, A1 = 11, A2 = 12, A6 = 16, A7 = 17,
             T3 = 28, T4 = 29, T5 = 30, T6 = 31 };

// A tiny assembler: a vector of words plus forward-reference patching.
struct Asm {
    std::vector<u32> w;
    std::size_t here() const { return w.size(); }
    void emit(u32 x) { w.push_back(x); }
    void li(u32 rd, u64 value) { load_imm64(w, rd, value); }
    void nops(int n) { for (int i = 0; i < n; ++i) emit(NOP()); }
    // `bnez rs, <later>`: returns the index to patch.
    std::size_t bnez_fwd() { emit(0); return here() - 1; }
    void patch_bne(std::size_t at, u32 rs, std::size_t target) {
        w[at] = BNE(rs, ZERO, static_cast<i32>((target - at) * 4));
    }
    // `auipc rd,0 ; addi rd,rd,<later>`: returns the index of the addi.
    std::size_t la_fwd(u32 rd) { emit(AUIPC(rd, 0)); emit(0); return here() - 1; }
    void patch_la(std::size_t addi_at, u32 rd, std::size_t target) {
        w[addi_at] = ADDI(rd, rd, static_cast<i32>((target - (addi_at - 1)) * 4));
    }
    void spin() { emit(HALT()); }   // jump to self
    u64 addr(std::size_t index) const { return DRAM_BASE + index * 4; }
};

// A machine with `harts` cores, a CLINT, a PLIC and syscon, and `program`
// loaded at the start of DRAM.
struct Smp {
    Bus                       bus;
    Dram*                     dram   = nullptr;
    Clint*                    clint  = nullptr;
    Plic*                     plic   = nullptr;
    std::unique_ptr<Platform> platform;

    Smp(const std::vector<u32>& program, u32 harts, u64 quantum = 8) {
        auto d = std::make_unique<Dram>(kTestDramSize);
        dram = d.get();
        bus.attach(std::move(d));
        auto c = std::make_unique<Clint>(harts);
        clint = c.get();
        bus.attach(std::move(c));
        auto p = std::make_unique<Plic>(harts);
        plic = p.get();
        bus.attach(std::move(p));
        auto s = std::make_unique<Syscon>();
        Syscon* syscon = s.get();
        bus.attach(std::move(s));

        std::vector<u8> bytes;
        for (u32 word : program) {
            for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<u8>((word >> (8 * b)) & 0xff));
        }
        dram->load_image(DRAM_BASE, bytes);

        platform = std::make_unique<Platform>(bus, harts);
        platform->quantum = quantum;
        platform->wire(clint, plic, nullptr, 0, syscon);
        platform->set_idle_on_wfi(true);
    }

    u64 mem64(u64 addr) {
        Result<u64> r = bus.load(addr, 8, AccessType::Load);
        return r ? r.value : ~0ull;
    }
    Status run(u64 steps) { return platform->run(steps, nullptr); }
};

// --- every hart sees its own id -------------------------------------------------

void test_each_hart_reads_its_own_id() {
    Asm a;
    a.emit(CSRR(T0, 0xf14));                 // t0 = mhartid
    a.emit(SLLI(T1, T0, 3));                 // t1 = hartid * 8
    a.li(T2, DATA);
    a.emit(r_type(opcodes::OP, T2, 0x0, T2, T1, 0x00));   // t2 += t1
    a.emit(ADDI(T3, T0, 100));               // value = hartid + 100
    a.emit(SD(T3, T2, 0));
    a.spin();

    Smp m(a.w, 4);
    CHECK(m.run(2000));
    for (u32 h = 0; h < 4; ++h) CHECK_EQ_U(m.mem64(DATA + 8 * h), 100 + h);
}

// --- atomics from several harts ----------------------------------------------------

void test_atomic_counter_is_exact_across_harts() {
    // Every hart adds 1 to one shared counter 500 times. With a quantum of 7 the
    // harts interleave in the middle of the loop; an AMO that were not atomic
    // with respect to the other harts would lose updates.
    Asm a;
    a.li(T2, DATA);
    a.emit(ADDI(T3, ZERO, 1));
    a.emit(ADDI(T4, ZERO, 500));
    const std::size_t loop = a.here();
    a.emit(AMOADD_D(ZERO, T2, T3));
    a.emit(ADDI(T4, T4, -1));
    a.emit(BNE(T4, ZERO, static_cast<i32>((loop - a.here()) * 4)));
    a.spin();

    Smp m(a.w, 4, 7);
    CHECK(m.run(100000));
    CHECK_EQ_U(m.mem64(DATA), 4 * 500);
}

// --- LR/SC reservations are cancelled by another hart's store --------------------------

// Hart 0 takes a reservation, waits a while, and tries to store conditionally.
// Hart 1 stores to `hart1_target` in the meantime. If that is the reserved
// address the SC must fail (result 1); if it is a different address the SC must
// succeed (result 0). Quantum 1 interleaves the harts one instruction at a time,
// so "in the meantime" is deterministic.
u64 run_lrsc(u64 hart1_target_offset) {
    Asm a;
    a.emit(CSRR(T0, 0xf14));
    a.li(T2, DATA);
    const std::size_t to_h1 = a.bnez_fwd();

    // hart 0
    a.emit(LR_D(T3, T2));
    a.nops(40);
    a.emit(ADDI(T5, ZERO, 9));
    a.emit(SC_D(T4, T2, T5));
    a.emit(SD(T4, T2, 64));              // record the SC result
    a.spin();

    // hart 1
    const std::size_t h1 = a.here();
    a.patch_bne(to_h1, T0, h1);
    a.nops(4);
    a.emit(ADDI(T6, ZERO, 7));
    a.emit(SD(T6, T2, static_cast<i32>(hart1_target_offset)));
    a.spin();

    Smp m(a.w, 2, 1);
    m.run(3000);
    return m.mem64(DATA + 64);
}

void test_other_harts_store_cancels_reservation() {
    CHECK_EQ_U(run_lrsc(0), 1);      // same address: the SC must fail
    CHECK_EQ_U(run_lrsc(16), 0);     // different address: the SC succeeds
}

// --- a software interrupt sent through the CLINT ------------------------------------------

void test_clint_msip_interrupts_another_hart() {
    Asm a;
    a.emit(CSRR(T0, 0xf14));
    a.li(T2, DATA);
    const std::size_t to_h1 = a.bnez_fwd();

    // hart 0: write 1 to hart 1's msip register (CLINT_BASE + 4).
    a.li(T1, CLINT_BASE + 4);
    a.emit(ADDI(T3, ZERO, 1));
    a.emit(SW(T3, T1, 0));
    a.spin();

    // hart 1: install a handler, enable machine software interrupts, wait.
    const std::size_t h1 = a.here();
    a.patch_bne(to_h1, T0, h1);
    const std::size_t la = a.la_fwd(T4);
    a.emit(CSRW(0x305, T4));                 // mtvec
    a.emit(ADDI(T5, ZERO, 8));               // MSIE
    a.emit(CSRW(0x304, T5));                 // mie
    a.emit(CSRSI(0x300, 8));                 // mstatus.MIE
    a.spin();
    const std::size_t handler = a.here();
    a.patch_la(la, T4, handler);
    a.emit(ADDI(T6, ZERO, 1));
    a.emit(SD(T6, T2, 0));                   // flag
    a.emit(CSRR(T6, 0x342));                 // mcause
    a.emit(SD(T6, T2, 8));
    a.spin();

    Smp m(a.w, 2);
    CHECK(m.run(5000));
    CHECK_EQ_U(m.mem64(DATA), 1);
    CHECK_EQ_U(m.mem64(DATA + 8), 0x8000000000000003ull);   // machine software interrupt
}

// --- SBI: start a hart, ask its state, send it an IPI ---------------------------------------

void test_sbi_hart_start_status_and_ipi() {
    Asm a;
    // ---- hart 0, in supervisor mode ----
    a.li(T2, DATA);
    // hart_start(hartid = 1, start_addr, opaque = 0x1234)
    a.li(A7, sbi::EXT_HSM);
    a.li(A6, 0);
    a.li(A0, 1);
    const std::size_t start_li = a.here();
    a.li(A1, 0);                              // patched below with hart 1's entry
    a.li(A2, 0x1234);
    a.emit(ECALL);
    a.emit(SD(A0, T2, 0));                    // error code
    // hart_get_status(1), then (2)
    a.li(A7, sbi::EXT_HSM); a.li(A6, 2); a.li(A0, 1);
    a.emit(ECALL);
    a.emit(SD(A1, T2, 8));
    a.li(A7, sbi::EXT_HSM); a.li(A6, 2); a.li(A0, 2);
    a.emit(ECALL);
    a.emit(SD(A1, T2, 16));
    // start hart 1 a second time: it is already running
    a.li(A7, sbi::EXT_HSM); a.li(A6, 0); a.li(A0, 1); a.li(A1, 0); a.li(A2, 0);
    a.emit(ECALL);
    a.emit(SD(A0, T2, 24));
    // send_ipi(mask = 0b10, base = 0): hart 1
    a.li(A7, sbi::EXT_IPI); a.li(A6, 0); a.li(A0, 2); a.li(A1, 0);
    a.emit(ECALL);
    a.emit(SD(A0, T2, 32));
    a.spin();

    // ---- hart 1: entered by hart_start with a0 = hartid, a1 = opaque ----
    const std::size_t h1 = a.here();
    a.li(T2, DATA);
    a.emit(SD(A0, T2, 64));
    a.emit(SD(A1, T2, 72));
    const std::size_t la = a.la_fwd(T4);
    a.emit(CSRW(0x105, T4));                  // stvec
    a.emit(ADDI(T5, ZERO, 2));                // SSIE
    a.emit(CSRW(0x104, T5));                  // sie
    a.emit(CSRSI(0x100, 2));                  // sstatus.SIE
    a.spin();
    const std::size_t handler = a.here();
    a.patch_la(la, T4, handler);
    a.emit(ADDI(T6, ZERO, 1));
    a.emit(SD(T6, T2, 80));
    a.emit(CSRR(T6, 0x142));                  // scause
    a.emit(SD(T6, T2, 88));
    a.spin();

    // Patch hart 0's hart_start call with the real entry address of hart 1.
    {
        std::vector<u32> tmp;
        load_imm64(tmp, A1, a.addr(h1));
        for (std::size_t i = 0; i < tmp.size(); ++i) a.w[start_li + i] = tmp[i];
    }

    Smp m(a.w, 4);
    m.platform->enter_supervisor(DRAM_BASE, 0);
    CHECK(m.run(20000));

    CHECK_EQ(static_cast<i64>(m.mem64(DATA + 0)), sbi::SUCCESS);                  // hart_start
    CHECK_EQ_U(m.mem64(DATA + 8), sbi::HART_STARTED);                             // hart 1 runs
    CHECK_EQ_U(m.mem64(DATA + 16), sbi::HART_STOPPED);                            // hart 2 never started
    CHECK_EQ(static_cast<i64>(m.mem64(DATA + 24)), sbi::ERR_ALREADY_AVAILABLE);   // started twice
    CHECK_EQ(static_cast<i64>(m.mem64(DATA + 32)), sbi::SUCCESS);                 // send_ipi
    CHECK_EQ_U(m.mem64(DATA + 64), 1);                                            // a0 = hartid
    CHECK_EQ_U(m.mem64(DATA + 72), 0x1234);                                       // a1 = opaque
    CHECK_EQ_U(m.mem64(DATA + 80), 1);                                            // the IPI handler ran
    CHECK_EQ_U(m.mem64(DATA + 88), 0x8000000000000001ull);                        // supervisor software

    // Harts 2 and 3 were never released.
    CHECK(m.platform->hart(2).parked);
    CHECK(m.platform->hart(3).parked);
}

void test_sbi_rejects_unknown_hart() {
    Asm a;
    a.li(T2, DATA);
    a.li(A7, sbi::EXT_HSM); a.li(A6, 0); a.li(A0, 9); a.li(A1, 0); a.li(A2, 0);
    a.emit(ECALL);
    a.emit(SD(A0, T2, 0));
    a.li(A7, sbi::EXT_HSM); a.li(A6, 2); a.li(A0, 9);
    a.emit(ECALL);
    a.emit(SD(A0, T2, 8));
    a.spin();

    Smp m(a.w, 2);
    m.platform->enter_supervisor(DRAM_BASE, 0);
    CHECK(m.run(2000));
    CHECK_EQ(static_cast<i64>(m.mem64(DATA)),     sbi::ERR_INVALID_PARAM);
    CHECK_EQ(static_cast<i64>(m.mem64(DATA + 8)), sbi::ERR_INVALID_PARAM);
}

// --- idle: WFI sleeps, and an idle machine skips to the next deadline ---------------------------

void test_wfi_idles_and_the_clock_skips_to_the_deadline() {
    Asm a;
    a.li(T2, DATA);
    a.li(T0, CLINT_BASE + 0x4000);           // hart 0's mtimecmp
    a.li(T1, 1000000);
    a.emit(SD(T1, T0, 0));                   // fire after a million ticks
    const std::size_t la = a.la_fwd(T4);
    a.emit(CSRW(0x305, T4));                 // mtvec
    a.emit(ADDI(T5, ZERO, 0x80));            // MTIE
    a.emit(CSRW(0x304, T5));
    a.emit(CSRSI(0x300, 8));                 // MIE
    const std::size_t sleep = a.here();
    a.emit(WFI);
    a.emit(j_type(opcodes::JAL, 0, static_cast<i32>((sleep - a.here()) * 4)));

    const std::size_t handler = a.here();
    a.patch_la(la, T4, handler);
    a.emit(ADDI(T6, ZERO, 1));
    a.emit(SD(T6, T2, 0));                   // flag: the timer fired
    a.emit(ADDI(T6, ZERO, -1));
    a.emit(SD(T6, T0, 0));                   // disarm the timer
    const std::size_t wait = a.here();
    a.emit(WFI);
    a.emit(j_type(opcodes::JAL, 0, static_cast<i32>((wait - a.here()) * 4)));

    Smp m(a.w, 1);
    CHECK(m.run(50'000'000));

    CHECK_EQ_U(m.mem64(DATA), 1);                       // the interrupt was delivered
    CHECK(m.clint->mtime() >= 1000000);                 // the clock really got there
    // ...without executing a million instructions to do it. This is the point of
    // idling: the guest slept a million ticks and cost a few hundred steps.
    CHECK(m.platform->retired() < 1000);
    // After the handler the timer is disarmed and nothing else can ever wake the
    // hart: the machine is deadlocked, and says so rather than hanging.
    CHECK(m.platform->stalled());
}

// --- the interrupt controllers know about every hart --------------------------------------------

void test_plic_routes_each_interrupt_to_the_right_hart_context() {
    Plic plic(2);                               // contexts 0..3: h0 M, h0 S, h1 M, h1 S
    CHECK_EQ_U(plic.num_contexts(), 4);

    auto w32 = [&](u64 off, u32 v) { plic.store(off, 4, v); };
    w32(4 * 5, 1);                              // source 5, priority 1
    w32(0x2000 + 0x80 * 3, 1u << 5);            // enable source 5 for context 3 (hart 1, S)
    plic.set_pending(5, true);

    CsrFile hart0, hart1;
    plic.update(hart0, 0);
    plic.update(hart1, 1);
    CHECK((hart0.read(csr::MIP) & csr::MIP_SEIP) == 0);     // hart 0 is not interested
    CHECK((hart1.read(csr::MIP) & csr::MIP_SEIP) != 0);     // hart 1's supervisor is
    CHECK((hart1.read(csr::MIP) & csr::MIP_MEIP) == 0);     // ...but not its machine mode

    // Claiming happens through that context's own register block.
    Result<u64> claim = plic.load(0x200000 + 0x1000 * 3 + 4, 4);
    CHECK(claim);
    CHECK_EQ_U(claim.value, 5);
    Result<u64> other = plic.load(0x200000 + 0x1000 * 1 + 4, 4);
    CHECK_EQ_U(other.value, 0);                              // hart 0's context sees nothing
}

void test_clint_has_independent_timers_and_ipis_per_hart() {
    Clint clint(2);
    CHECK_EQ_U(clint.harts(), 2);

    clint.store(0x4000 + 8 * 0, 8, 100);        // hart 0: soon
    clint.store(0x4000 + 8 * 1, 8, 5000);       // hart 1: much later
    clint.tick(200);

    CsrFile hart0, hart1;
    clint.update(hart0, 0);
    clint.update(hart1, 1);
    CHECK((hart0.read(csr::MIP) & csr::MIP_MTIP) != 0);
    CHECK((hart1.read(csr::MIP) & csr::MIP_MTIP) == 0);

    // An IPI to one hart does not disturb the other.
    clint.store(4 * 1, 4, 1);
    clint.update(hart0, 0);
    clint.update(hart1, 1);
    CHECK((hart1.read(csr::MIP) & csr::MIP_MSIP) != 0);
    CHECK((hart0.read(csr::MIP) & csr::MIP_MSIP) == 0);

    CHECK_EQ_U(clint.load(0x4000 + 8, 8).value, 5000);
    CHECK_EQ_U(clint.load(4, 4).value, 1);
}

void test_device_tree_describes_every_hart() {
    FdtConfig c;
    c.harts = 3;
    c.virtio_net = true;
    c.virtio_rng = true;
    c.rtc = true;
    const std::vector<u8> blob = Fdt::build(c);

    auto has = [&](const char* needle) {
        const std::size_t n = std::strlen(needle);
        for (std::size_t i = 0; i + n <= blob.size(); ++i) {
            if (std::memcmp(blob.data() + i, needle, n) == 0) return true;
        }
        return false;
    };
    CHECK(has("cpu@0"));
    CHECK(has("cpu@1"));
    CHECK(has("cpu@2"));
    CHECK(!has("cpu@3"));
    CHECK(has("virtio_mmio@10002000"));
    CHECK(has("virtio_mmio@10003000"));
    CHECK(has("google,goldfish-rtc"));

    // And a tree built for one hart without the optional devices has none of them.
    FdtConfig plain;
    const std::vector<u8> small = Fdt::build(plain);
    bool found_net = false;
    const char* needle = "virtio_mmio@10002000";
    for (std::size_t i = 0; i + std::strlen(needle) <= small.size(); ++i) {
        if (std::memcmp(small.data() + i, needle, std::strlen(needle)) == 0) found_net = true;
    }
    CHECK(!found_net);
}

}  // namespace

int main() {
    test_each_hart_reads_its_own_id();
    test_atomic_counter_is_exact_across_harts();
    test_other_harts_store_cancels_reservation();
    test_clint_msip_interrupts_another_hart();
    test_sbi_hart_start_status_and_ipi();
    test_sbi_rejects_unknown_hart();
    test_wfi_idles_and_the_clock_skips_to_the_deadline();
    test_plic_routes_each_interrupt_to_the_right_hart_context();
    test_clint_has_independent_timers_and_ipis_per_hart();
    test_device_tree_describes_every_hart();
    return testutil::summary("smp");
}
