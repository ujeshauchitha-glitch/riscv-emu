//! Multiple harts.
//!
//! These tests run real RISC-V code on several harts at once and check what the harts did to shared
//! memory - which is the only honest way to test a multiprocessor: unit-testing the pieces proves
//! each works alone, not that they work together.
//!
//! Programs are tiny, hand-assembled with the independent encoders in common. Constants are loaded
//! with load_imm64 (a fixed 17-instruction sequence) so every hart's prologue takes exactly the same
//! number of steps, which the interleaving tests depend on.

mod common;

use std::cell::RefCell;
use std::rc::Rc;

use common::*;
use riscv_emu::bus::{Bus, Shared};
use riscv_emu::clint::{Clint, Clock};
use riscv_emu::csr::{csr, CsrFile};
use riscv_emu::decoder::opcodes;
use riscv_emu::device::{AccessType, Device};
use riscv_emu::platform::Platform;
use riscv_emu::plic::Plic;
use riscv_emu::sbi;
use riscv_emu::syscon::Syscon;
use riscv_emu::types::*;
use riscv_emu::uart::Uart;

const DATA: u64 = DRAM_BASE + 0x10000; // shared scratch memory for results

// --- instruction helpers not in common ---------------------------------------

fn slli(rd: u32, rs1: u32, sh: u32) -> u32 {
    i_type(opcodes::OP_IMM, rd, 0x1, rs1, sh as i32)
}
fn sd(rs2: u32, rs1: u32, off: i32) -> u32 {
    s_type(opcodes::STORE, 0x3, rs1, rs2, off)
}
fn sw(rs2: u32, rs1: u32, off: i32) -> u32 {
    s_type(opcodes::STORE, 0x2, rs1, rs2, off)
}
fn bne(rs1: u32, rs2: u32, off: i32) -> u32 {
    b_type(opcodes::BRANCH, 0x1, rs1, rs2, off)
}
fn csrr(rd: u32, c: u32) -> u32 {
    i_type(opcodes::SYSTEM, rd, 0x2, 0, c as i32)
}
fn csrw(c: u32, rs1: u32) -> u32 {
    i_type(opcodes::SYSTEM, 0, 0x1, rs1, c as i32)
}
fn csrsi(c: u32, uimm: u32) -> u32 {
    i_type(opcodes::SYSTEM, 0, 0x6, uimm, c as i32)
}
const ECALL: u32 = 0x0000_0073;
const WFI: u32 = 0x1050_0073;
fn lr_d(rd: u32, rs1: u32) -> u32 {
    r_type(opcodes::AMO, rd, 0x3, rs1, 0, 0x02 << 2)
}
fn sc_d(rd: u32, rs1: u32, rs2: u32) -> u32 {
    r_type(opcodes::AMO, rd, 0x3, rs1, rs2, 0x03 << 2)
}
fn amoadd_d(rd: u32, rs1: u32, rs2: u32) -> u32 {
    r_type(opcodes::AMO, rd, 0x3, rs1, rs2, 0x00 << 2)
}

// A register name per the ABI, to keep the programs readable.
const ZERO: u32 = 0;
const T0: u32 = 5;
const T1: u32 = 6;
const T2: u32 = 7;
const A0: u32 = 10;
const A1: u32 = 11;
const A2: u32 = 12;
const A6: u32 = 16;
const A7: u32 = 17;
const T3: u32 = 28;
const T4: u32 = 29;
const T5: u32 = 30;
const T6: u32 = 31;

/// A tiny assembler: a vector of words plus forward-reference patching.
#[derive(Default)]
struct Asm {
    w: Vec<u32>,
}

impl Asm {
    fn here(&self) -> usize {
        self.w.len()
    }
    fn emit(&mut self, x: u32) {
        self.w.push(x);
    }
    fn li(&mut self, rd: u32, value: u64) {
        load_imm64(&mut self.w, rd, value);
    }
    fn nops(&mut self, n: usize) {
        for _ in 0..n {
            self.emit(nop());
        }
    }
    /// `bnez rs, <later>`: returns the index to patch.
    fn bnez_fwd(&mut self) -> usize {
        self.emit(0);
        self.here() - 1
    }
    fn patch_bne(&mut self, at: usize, rs: u32, target: usize) {
        self.w[at] = bne(rs, ZERO, ((target - at) * 4) as i32);
    }
    /// `auipc rd,0 ; addi rd,rd,<later>`: returns the index of the addi.
    fn la_fwd(&mut self, rd: u32) -> usize {
        self.emit(auipc(rd, 0));
        self.emit(0);
        self.here() - 1
    }
    fn patch_la(&mut self, addi_at: usize, rd: u32, target: usize) {
        self.w[addi_at] = addi(rd, rd, ((target - (addi_at - 1)) * 4) as i32);
    }
    fn spin(&mut self) {
        self.emit(halt()); // jump to self
    }
    fn addr(&self, index: usize) -> u64 {
        DRAM_BASE + index as u64 * 4
    }
    /// A backward `jal x0` to `target`.
    fn jump_back(&mut self, target: usize) {
        let at = self.here();
        self.emit(j_type(opcodes::JAL, 0, ((target as i64 - at as i64) * 4) as i32));
    }
}

/// A machine with `harts` cores, a CLINT, a PLIC and syscon, and `program` loaded at the start of
/// DRAM.
struct Smp {
    platform: Platform,
    clint: Rc<RefCell<Clint>>,
}

impl Smp {
    fn new(program: &[u32], harts: u32) -> Smp {
        Smp::with_quantum(program, harts, 8)
    }

    fn with_quantum(program: &[u32], harts: u32, quantum: u64) -> Smp {
        let mut bus = Bus::new(TEST_DRAM_SIZE);
        let (c, clint) = Shared::new(Clint::new(harts, Rc::new(Clock::new())));
        let (p, plic) = Shared::new(Plic::new(harts));
        let (s, syscon) = Shared::new(Syscon::new());
        assert!(bus.attach(Box::new(c)) && bus.attach(Box::new(p)) && bus.attach(Box::new(s)));

        let bytes: Vec<u8> = program.iter().flat_map(|w| w.to_le_bytes()).collect();
        bus.ram.load_image(DRAM_BASE, &bytes);

        // These tests have no console; the platform just needs something to poll.
        let uart = Rc::new(RefCell::new(Uart::new(Box::new(SharedBuf::new()))));
        let mut platform = Platform::new(Rc::new(bus), harts);
        platform.quantum = quantum;
        platform.wire(clint.clone(), plic, uart, 0, syscon);
        platform.set_idle_on_wfi(true);
        Smp { platform, clint }
    }

    fn mem64(&self, addr: u64) -> u64 {
        self.platform.bus.load(addr, 8, AccessType::Load).unwrap_or(u64::MAX)
    }

    fn run(&mut self, steps: u64) -> bool {
        self.platform.run(steps).is_ok()
    }
}

// --- every hart sees its own id -------------------------------------------------

#[test]
fn each_hart_reads_its_own_id() {
    let mut a = Asm::default();
    a.emit(csrr(T0, 0xf14)); // t0 = mhartid
    a.emit(slli(T1, T0, 3)); // t1 = hartid * 8
    a.li(T2, DATA);
    a.emit(r_type(opcodes::OP, T2, 0x0, T2, T1, 0x00)); // t2 += t1
    a.emit(addi(T3, T0, 100)); // value = hartid + 100
    a.emit(sd(T3, T2, 0));
    a.spin();

    let mut m = Smp::new(&a.w, 4);
    assert!(m.run(2000));
    for h in 0..4 {
        assert_eq!(m.mem64(DATA + 8 * h), 100 + h);
    }
}

// --- atomics from several harts ----------------------------------------------------

#[test]
fn atomic_counter_is_exact_across_harts() {
    // Every hart adds 1 to one shared counter 500 times. With a quantum of 7 the harts interleave in
    // the middle of the loop; an AMO that were not atomic with respect to the other harts would lose
    // updates.
    let mut a = Asm::default();
    a.li(T2, DATA);
    a.emit(addi(T3, ZERO, 1));
    a.emit(addi(T4, ZERO, 500));
    let loop_ = a.here();
    a.emit(amoadd_d(ZERO, T2, T3));
    a.emit(addi(T4, T4, -1));
    let here = a.here();
    a.emit(bne(T4, ZERO, ((loop_ as i64 - here as i64) * 4) as i32));
    a.spin();

    let mut m = Smp::with_quantum(&a.w, 4, 7);
    assert!(m.run(100_000));
    assert_eq!(m.mem64(DATA), 4 * 500);
}

// --- LR/SC reservations are cancelled by another hart's store --------------------------

/// Hart 0 takes a reservation, waits a while, and tries to store conditionally. Hart 1 stores to
/// `hart1_target` in the meantime. If that is the reserved address the SC must fail (result 1); if it
/// is a different address the SC must succeed (result 0). Quantum 1 interleaves the harts one
/// instruction at a time, so "in the meantime" is deterministic.
fn run_lrsc(hart1_target_offset: i32) -> u64 {
    let mut a = Asm::default();
    a.emit(csrr(T0, 0xf14));
    a.li(T2, DATA);
    let to_h1 = a.bnez_fwd();

    // hart 0
    a.emit(lr_d(T3, T2));
    a.nops(40);
    a.emit(addi(T5, ZERO, 9));
    a.emit(sc_d(T4, T2, T5));
    a.emit(sd(T4, T2, 64)); // record the SC result
    a.spin();

    // hart 1
    let h1 = a.here();
    a.patch_bne(to_h1, T0, h1);
    a.nops(4);
    a.emit(addi(T6, ZERO, 7));
    a.emit(sd(T6, T2, hart1_target_offset));
    a.spin();

    let mut m = Smp::with_quantum(&a.w, 2, 1);
    m.run(3000);
    m.mem64(DATA + 64)
}

#[test]
fn other_harts_store_cancels_reservation() {
    assert_eq!(run_lrsc(0), 1); // same address: the SC must fail
    assert_eq!(run_lrsc(16), 0); // different address: the SC succeeds
}

// --- a software interrupt sent through the CLINT ------------------------------------------

#[test]
fn clint_msip_interrupts_another_hart() {
    let mut a = Asm::default();
    a.emit(csrr(T0, 0xf14));
    a.li(T2, DATA);
    let to_h1 = a.bnez_fwd();

    // hart 0: write 1 to hart 1's msip register (CLINT_BASE + 4).
    a.li(T1, CLINT_BASE + 4);
    a.emit(addi(T3, ZERO, 1));
    a.emit(sw(T3, T1, 0));
    a.spin();

    // hart 1: install a handler, enable machine software interrupts, wait.
    let h1 = a.here();
    a.patch_bne(to_h1, T0, h1);
    let la = a.la_fwd(T4);
    a.emit(csrw(0x305, T4)); // mtvec
    a.emit(addi(T5, ZERO, 8)); // MSIE
    a.emit(csrw(0x304, T5)); // mie
    a.emit(csrsi(0x300, 8)); // mstatus.MIE
    a.spin();
    let handler = a.here();
    a.patch_la(la, T4, handler);
    a.emit(addi(T6, ZERO, 1));
    a.emit(sd(T6, T2, 0)); // flag
    a.emit(csrr(T6, 0x342)); // mcause
    a.emit(sd(T6, T2, 8));
    a.spin();

    let mut m = Smp::new(&a.w, 2);
    assert!(m.run(5000));
    assert_eq!(m.mem64(DATA), 1);
    assert_eq!(m.mem64(DATA + 8), 0x8000_0000_0000_0003); // machine software interrupt
}

// --- SBI: start a hart, ask its state, send it an IPI ---------------------------------------

#[test]
fn sbi_hart_start_status_and_ipi() {
    let mut a = Asm::default();
    // ---- hart 0, in supervisor mode ----
    a.li(T2, DATA);
    // hart_start(hartid = 1, start_addr, opaque = 0x1234)
    a.li(A7, sbi::EXT_HSM);
    a.li(A6, 0);
    a.li(A0, 1);
    let start_li = a.here();
    a.li(A1, 0); // patched below with hart 1's entry
    a.li(A2, 0x1234);
    a.emit(ECALL);
    a.emit(sd(A0, T2, 0)); // error code
    // hart_get_status(1), then (2)
    a.li(A7, sbi::EXT_HSM);
    a.li(A6, 2);
    a.li(A0, 1);
    a.emit(ECALL);
    a.emit(sd(A1, T2, 8));
    a.li(A7, sbi::EXT_HSM);
    a.li(A6, 2);
    a.li(A0, 2);
    a.emit(ECALL);
    a.emit(sd(A1, T2, 16));
    // start hart 1 a second time: it is already running
    a.li(A7, sbi::EXT_HSM);
    a.li(A6, 0);
    a.li(A0, 1);
    a.li(A1, 0);
    a.li(A2, 0);
    a.emit(ECALL);
    a.emit(sd(A0, T2, 24));
    // send_ipi(mask = 0b10, base = 0): hart 1
    a.li(A7, sbi::EXT_IPI);
    a.li(A6, 0);
    a.li(A0, 2);
    a.li(A1, 0);
    a.emit(ECALL);
    a.emit(sd(A0, T2, 32));
    a.spin();

    // ---- hart 1: entered by hart_start with a0 = hartid, a1 = opaque ----
    let h1 = a.here();
    a.li(T2, DATA);
    a.emit(sd(A0, T2, 64));
    a.emit(sd(A1, T2, 72));
    let la = a.la_fwd(T4);
    a.emit(csrw(0x105, T4)); // stvec
    a.emit(addi(T5, ZERO, 2)); // SSIE
    a.emit(csrw(0x104, T5)); // sie
    a.emit(csrsi(0x100, 2)); // sstatus.SIE
    a.spin();
    let handler = a.here();
    a.patch_la(la, T4, handler);
    a.emit(addi(T6, ZERO, 1));
    a.emit(sd(T6, T2, 80));
    a.emit(csrr(T6, 0x142)); // scause
    a.emit(sd(T6, T2, 88));
    a.spin();

    // Patch hart 0's hart_start call with the real entry address of hart 1.
    let mut tmp = Vec::new();
    load_imm64(&mut tmp, A1, a.addr(h1));
    for (i, w) in tmp.iter().enumerate() {
        a.w[start_li + i] = *w;
    }

    let mut m = Smp::new(&a.w, 4);
    m.platform.enter_supervisor(DRAM_BASE, 0);
    assert!(m.run(20000));

    assert_eq!(m.mem64(DATA) as i64, sbi::SUCCESS); // hart_start
    assert_eq!(m.mem64(DATA + 8), sbi::HART_STARTED); // hart 1 runs
    assert_eq!(m.mem64(DATA + 16), sbi::HART_STOPPED); // hart 2 never started
    assert_eq!(m.mem64(DATA + 24) as i64, sbi::ERR_ALREADY_AVAILABLE); // started twice
    assert_eq!(m.mem64(DATA + 32) as i64, sbi::SUCCESS); // send_ipi
    assert_eq!(m.mem64(DATA + 64), 1); // a0 = hartid
    assert_eq!(m.mem64(DATA + 72), 0x1234); // a1 = opaque
    assert_eq!(m.mem64(DATA + 80), 1); // the IPI handler ran
    assert_eq!(m.mem64(DATA + 88), 0x8000_0000_0000_0001); // supervisor software

    // Harts 2 and 3 were never released.
    assert!(m.platform.harts[2].parked());
    assert!(m.platform.harts[3].parked());
}

#[test]
fn sbi_rejects_unknown_hart() {
    let mut a = Asm::default();
    a.li(T2, DATA);
    a.li(A7, sbi::EXT_HSM);
    a.li(A6, 0);
    a.li(A0, 9);
    a.li(A1, 0);
    a.li(A2, 0);
    a.emit(ECALL);
    a.emit(sd(A0, T2, 0));
    a.li(A7, sbi::EXT_HSM);
    a.li(A6, 2);
    a.li(A0, 9);
    a.emit(ECALL);
    a.emit(sd(A0, T2, 8));
    a.spin();

    let mut m = Smp::new(&a.w, 2);
    m.platform.enter_supervisor(DRAM_BASE, 0);
    assert!(m.run(2000));
    assert_eq!(m.mem64(DATA) as i64, sbi::ERR_INVALID_PARAM);
    assert_eq!(m.mem64(DATA + 8) as i64, sbi::ERR_INVALID_PARAM);
}

// --- idle: WFI sleeps, and an idle machine skips to the next deadline ---------------------------

#[test]
fn wfi_idles_and_the_clock_skips_to_the_deadline() {
    let mut a = Asm::default();
    a.li(T2, DATA);
    a.li(T0, CLINT_BASE + 0x4000); // hart 0's mtimecmp
    a.li(T1, 1_000_000);
    a.emit(sd(T1, T0, 0)); // fire after a million ticks
    let la = a.la_fwd(T4);
    a.emit(csrw(0x305, T4)); // mtvec
    a.emit(addi(T5, ZERO, 0x80)); // MTIE
    a.emit(csrw(0x304, T5));
    a.emit(csrsi(0x300, 8)); // MIE
    let sleep = a.here();
    a.emit(WFI);
    a.jump_back(sleep);

    let handler = a.here();
    a.patch_la(la, T4, handler);
    a.emit(addi(T6, ZERO, 1));
    a.emit(sd(T6, T2, 0)); // flag: the timer fired
    a.emit(addi(T6, ZERO, -1));
    a.emit(sd(T6, T0, 0)); // disarm the timer
    let wait = a.here();
    a.emit(WFI);
    a.jump_back(wait);

    let mut m = Smp::new(&a.w, 1);
    assert!(m.run(50_000_000));

    assert_eq!(m.mem64(DATA), 1); // the interrupt was delivered
    assert!(m.clint.borrow().mtime() >= 1_000_000); // the clock really got there
    // ...without executing a million instructions to do it. This is the point of idling: the guest
    // slept a million ticks and cost a few hundred steps.
    assert!(m.platform.retired() < 1000, "retired {}", m.platform.retired());
    // After the handler the timer is disarmed and nothing else can ever wake the hart: the machine is
    // deadlocked, and says so rather than hanging.
    assert!(m.platform.stalled());
}

// --- the interrupt controllers know about every hart --------------------------------------------

#[test]
fn plic_routes_each_interrupt_to_the_right_hart_context() {
    let mut plic = Plic::new(2); // contexts 0..3: h0 M, h0 S, h1 M, h1 S
    assert_eq!(plic.num_contexts(), 4);

    plic.store(4 * 5, 4, 1).unwrap(); // source 5, priority 1
    plic.store(0x2000 + 0x80 * 3, 4, 1 << 5).unwrap(); // enable source 5 for context 3 (hart 1, S)
    plic.set_pending(5, true);

    let (mut hart0, mut hart1) = (CsrFile::new(), CsrFile::new());
    plic.update(&mut hart0, 0);
    plic.update(&mut hart1, 1);
    assert_eq!(hart0.read(csr::MIP) & csr::MIP_SEIP, 0); // hart 0 is not interested
    assert_ne!(hart1.read(csr::MIP) & csr::MIP_SEIP, 0); // hart 1's supervisor is
    assert_eq!(hart1.read(csr::MIP) & csr::MIP_MEIP, 0); // ...but not its machine mode

    // Claiming happens through that context's own register block.
    assert_eq!(plic.load(0x200000 + 0x1000 * 3 + 4, 4).unwrap(), 5);
    assert_eq!(plic.load(0x200000 + 0x1000 + 4, 4).unwrap(), 0); // hart 0's context sees nothing
}

#[test]
fn clint_has_independent_timers_and_ipis_per_hart() {
    let mut clint = Clint::new(2, Rc::new(Clock::new()));
    assert_eq!(clint.harts(), 2);

    clint.store(0x4000, 8, 100).unwrap(); // hart 0: soon
    clint.store(0x4000 + 8, 8, 5000).unwrap(); // hart 1: much later
    clint.clock().tick(200);

    let (mut hart0, mut hart1) = (CsrFile::new(), CsrFile::new());
    clint.update(&mut hart0, 0);
    clint.update(&mut hart1, 1);
    assert_ne!(hart0.read(csr::MIP) & csr::MIP_MTIP, 0);
    assert_eq!(hart1.read(csr::MIP) & csr::MIP_MTIP, 0);

    // An IPI to one hart does not disturb the other.
    clint.store(4, 4, 1).unwrap();
    clint.update(&mut hart0, 0);
    clint.update(&mut hart1, 1);
    assert_ne!(hart1.read(csr::MIP) & csr::MIP_MSIP, 0);
    assert_eq!(hart0.read(csr::MIP) & csr::MIP_MSIP, 0);

    assert_eq!(clint.load(0x4000 + 8, 8).unwrap(), 5000);
    assert_eq!(clint.load(4, 4).unwrap(), 1);
}

#[test]
fn a_hart_that_has_not_been_started_does_not_run() {
    // Secondary harts come out of reset parked under --linux and sit there until SBI releases them.
    let mut a = Asm::default();
    a.li(T2, DATA);
    a.emit(addi(T3, ZERO, 5));
    a.emit(sd(T3, T2, 0));
    a.spin();
    let mut m = Smp::new(&a.w, 2);
    m.platform.enter_supervisor(DRAM_BASE, 0);
    assert!(m.run(1000));
    assert_eq!(m.mem64(DATA), 5);
    assert!(m.platform.harts[1].parked());
    assert_eq!(m.platform.harts[1].instret, 0);
}
