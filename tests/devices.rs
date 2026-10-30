//! Device tests: UART, CLINT, syscon, and the ELF loader.

mod common;

use std::rc::Rc;

use common::*;
use riscv_emu::bus::{Bus, Shared};
use riscv_emu::clint::{Clint, Clock};
use riscv_emu::csr::{csr, CsrFile};
use riscv_emu::decoder::opcodes;
use riscv_emu::device::{AccessType, Device};
use riscv_emu::elf::{is_elf, load_elf};
use riscv_emu::platform::Platform;
use riscv_emu::plic::Plic;
use riscv_emu::syscon::{self, Syscon};
use riscv_emu::types::*;
use riscv_emu::uart::Uart;

fn uart() -> (Uart, SharedBuf) {
    let out = SharedBuf::new();
    (Uart::new(Box::new(out.clone())), out)
}

fn clint() -> Clint {
    Clint::new(1, Rc::new(Clock::new()))
}

// --- UART -------------------------------------------------------------------

#[test]
fn uart_writes_appear_on_the_output() {
    let (mut u, out) = uart();
    for c in "hi!".bytes() {
        u.store(0, 1, c as u64).unwrap();
    }
    assert_eq!(out.text(), "hi!");
}

#[test]
fn uart_lsr_reports_transmitter_ready() {
    let (mut u, _) = uart();
    let lsr = u.load(5, 1).unwrap();
    // Bit 5 (THR empty) is always set: a write completes immediately because there is no real serial
    // line to wait for.
    assert_ne!(lsr & (1 << 5), 0);
    // Bit 0 (data ready) is clear until something is queued.
    assert_eq!(lsr & 1, 0);

    u.feed_input(b"x");
    assert_ne!(u.load(5, 1).unwrap() & 1, 0);
}

#[test]
fn uart_reads_queued_input() {
    let (mut u, _) = uart();
    u.feed_input(b"ab");
    assert_eq!(u.load(0, 1).unwrap(), b'a' as u64);
    assert_eq!(u.load(0, 1).unwrap(), b'b' as u64);
    assert_eq!(u.load(0, 1).unwrap(), 0); // nothing left
}

#[test]
fn uart_registers_are_byte_wide() {
    let (mut u, _) = uart();
    assert!(u.load(0, 4).is_err());
    assert!(u.store(0, 2, 0).is_err());
}

#[test]
fn uart_iir_names_the_interrupt_cause() {
    // Bit 0 clear means an interrupt is pending, but that is not the whole answer: bits 3:1 say
    // *which* condition caused it, and a real driver dispatches on that field rather than going
    // looking.
    //
    //   0b001  transmitter holding register empty
    //   0b010  received data available
    //
    // Returning a bare "something happened" leaves the field at 000, which means "modem status change"
    // - so Linux's 8250 driver read the modem status register, found nothing, and never touched the
    // receive buffer. The console printed perfectly and could not be typed at. xv6 never reads IIR at
    // all, which is why that went unnoticed for a whole phase.
    let (mut u, _) = uart();

    // Nothing enabled, nothing waiting: bit 0 set means no interrupt.
    assert_ne!(u.load(2, 1).unwrap() & 1, 0);

    // A queued byte with receive interrupts enabled reports "data available".
    u.store(1, 1, 0x01).unwrap(); // IER: receive
    u.feed_input(b"x");
    let iir = u.load(2, 1).unwrap();
    assert_eq!(iir & 1, 0); // pending
    assert_eq!((iir >> 1) & 0x7, 0x2); // received data available

    // Reading IIR must not consume the byte - only reading RBR does that.
    assert_eq!(u.load(0, 1).unwrap(), b'x' as u64);

    // With the queue drained and transmit interrupts enabled, a sent byte reports "transmitter empty"
    // instead.
    u.store(1, 1, 0x02).unwrap(); // IER: transmit
    u.store(0, 1, b'h' as u64).unwrap(); // THR
    let iir = u.load(2, 1).unwrap();
    assert_eq!(iir & 1, 0);
    assert_eq!((iir >> 1) & 0x7, 0x1); // THR empty

    // And reading IIR acknowledges that one, so it does not repeat.
    assert_ne!(u.load(2, 1).unwrap() & 1, 0);

    // Received data outranks a free transmitter: an unread byte is lost if the next one arrives, while
    // an idle transmitter will still be idle later.
    u.store(1, 1, 0x03).unwrap(); // IER: both
    u.store(0, 1, b'h' as u64).unwrap(); // make the transmitter empty again
    u.feed_input(b"y");
    let iir = u.load(2, 1).unwrap();
    assert_eq!((iir >> 1) & 0x7, 0x2); // receive wins

    // Bits 7:6 advertise working FIFOs, which is what makes a driver identify this as a 16550A rather
    // than an earlier part.
    assert_eq!((iir >> 6) & 0x3, 0x3);
}

#[test]
fn uart_dlab_switches_register_bank() {
    // With DLAB set, registers 0 and 1 become the baud-rate divisor rather than data and
    // interrupt-enable. A UART that ignored this would print the divisor bytes a real driver writes
    // during initialisation.
    let (mut u, out) = uart();

    u.store(3, 1, 0x80).unwrap(); // LCR: set DLAB
    u.store(0, 1, 0x03).unwrap(); // this is the divisor low byte, not a char
    u.store(1, 1, 0x00).unwrap();
    assert!(out.text().is_empty()); // nothing was printed

    assert_eq!(u.load(0, 1).unwrap(), 0x03);

    u.store(3, 1, 0x03).unwrap(); // clear DLAB, 8 data bits
    u.store(0, 1, b'Z' as u64).unwrap(); // now this really is a character
    assert_eq!(out.text(), "Z");
}

// --- CLINT ------------------------------------------------------------------

#[test]
fn clint_mtime_advances_with_instructions() {
    let mut c = clint();
    assert_eq!(c.mtime(), 0);
    c.clock().tick(1);
    c.clock().tick(1);
    assert_eq!(c.mtime(), 2);
    assert_eq!(c.load(0xbff8, 8).unwrap(), 2);
}

#[test]
fn clint_timer_divisor_slows_the_clock() {
    // One mtime tick every N instructions. (This once added N ticks per instruction - the inverse -
    // so asking for a slower clock produced an interrupt storm.)
    let c = clint();
    c.clock().set_instructions_per_tick(100);
    c.clock().tick(250);
    assert_eq!(c.mtime(), 2);
    c.clock().tick(50); // 300 total
    assert_eq!(c.mtime(), 3);
}

#[test]
fn clint_raises_timer_interrupt_at_the_deadline() {
    let mut c = clint();
    let mut csrs = CsrFile::new();

    c.store(0x4000, 8, 5).unwrap(); // mtimecmp = 5
    c.update(&mut csrs, 0);
    assert_eq!(csrs.read(csr::MIP) & csr::MIP_MTIP, 0); // not yet

    for _ in 0..5 {
        c.clock().tick(1);
    }
    c.update(&mut csrs, 0);
    assert_ne!(csrs.read(csr::MIP) & csr::MIP_MTIP, 0); // mtime == mtimecmp

    // Moving the deadline forward is how a kernel acknowledges the interrupt - it cannot clear MTIP by
    // writing mip, because the device owns that bit.
    c.store(0x4000, 8, 100).unwrap();
    c.update(&mut csrs, 0);
    assert_eq!(csrs.read(csr::MIP) & csr::MIP_MTIP, 0);
}

#[test]
fn clint_software_interrupt() {
    let mut c = clint();
    let mut csrs = CsrFile::new();
    c.store(0x4000, 8, !0).unwrap(); // push the timer far away

    c.store(0, 4, 1).unwrap(); // msip = 1
    c.update(&mut csrs, 0);
    assert_ne!(csrs.read(csr::MIP) & csr::MIP_MSIP, 0);

    c.store(0, 4, 0).unwrap();
    c.update(&mut csrs, 0);
    assert_eq!(csrs.read(csr::MIP) & csr::MIP_MSIP, 0);
}

#[test]
fn clint_mtimecmp_halves() {
    // 32-bit kernels write mtimecmp as two halves, so both widths must work.
    let mut c = clint();
    c.store(0x4000, 4, 0xdead_beef).unwrap();
    c.store(0x4004, 4, 0x1234_5678).unwrap();
    assert_eq!(c.load(0x4000, 8).unwrap(), 0x1234_5678_dead_beef);
}

#[test]
fn clint_with_several_harts_has_a_deadline_and_msip_per_hart() {
    let mut c = Clint::new(4, Rc::new(Clock::new()));
    for h in 0..4u64 {
        c.store(0x4000 + h * 8, 8, 100 + h).unwrap();
        assert_eq!(c.load(0x4000 + h * 8, 8).unwrap(), 100 + h);
    }
    c.store(4 * 2, 4, 1).unwrap(); // msip of hart 2
    assert!(c.msip(2));
    assert!(!c.msip(1));
    // msip of a hart that does not exist reads as zero and ignores writes.
    c.store(4 * 9, 4, 1).unwrap();
    assert_eq!(c.load(4 * 9, 4).unwrap(), 0);
}

// --- syscon -----------------------------------------------------------------

#[test]
fn syscon_poweroff_and_exit_code() {
    let mut s = Syscon::new();
    assert!(!s.poweroff_requested());
    s.store(0, 4, syscon::POWEROFF).unwrap();
    assert!(s.poweroff_requested());
    assert_eq!(s.exit_code(), 0);

    let mut t = Syscon::new();
    // riscv-tests packs a failure code into the upper bits.
    t.store(0, 4, (3 << 16) | syscon::POWEROFF).unwrap();
    assert!(t.poweroff_requested());
    assert_eq!(t.exit_code(), 3);

    let mut u = Syscon::new();
    u.store(0, 4, syscon::REBOOT).unwrap();
    assert!(u.reboot_requested());
    assert!(!u.poweroff_requested());
}

// --- integration: a guest program driving the devices -----------------------

/// A machine with all four devices, run by the platform scheduler.
struct FullMachine {
    platform: Platform,
    out: SharedBuf,
}

impl FullMachine {
    fn new(program: &[u32]) -> FullMachine {
        let out = SharedBuf::new();
        let mut bus = Bus::new(1024 * 1024);
        let (u, uart) = Shared::new(Uart::new(Box::new(out.clone())));
        let (c, clint) = Shared::new(Clint::new(1, Rc::new(Clock::new())));
        let (s, syscon) = Shared::new(Syscon::new());
        let (p, plic) = Shared::new(Plic::new(1));
        assert!(bus.attach(Box::new(u)) && bus.attach(Box::new(c)) && bus.attach(Box::new(s)) && bus.attach(Box::new(p)));

        let bytes: Vec<u8> = program.iter().flat_map(|w| w.to_le_bytes()).collect();
        bus.ram.load_image(DRAM_BASE, &bytes);

        let mut platform = Platform::new(Rc::new(bus), 1);
        platform.wire(clint, plic, uart, UART0_IRQ, syscon);
        FullMachine { platform, out }
    }
}

#[test]
fn guest_can_print_and_power_off() {
    let mut p = Vec::new();
    load_imm64(&mut p, 1, UART0_BASE);
    load_imm64(&mut p, 2, b'O' as u64);
    p.push(s_type(opcodes::STORE, 0x0, 1, 2, 0)); // sb 'O' -> console
    load_imm64(&mut p, 2, b'K' as u64);
    p.push(s_type(opcodes::STORE, 0x0, 1, 2, 0));
    load_imm64(&mut p, 1, SYSCON_BASE);
    load_imm64(&mut p, 2, syscon::POWEROFF);
    p.push(s_type(opcodes::STORE, 0x2, 1, 2, 0)); // sw poweroff
    p.push(halt()); // must not be reached

    let mut m = FullMachine::new(&p);
    assert!(m.platform.run(500).is_ok()); // powering off is not a failure
    assert!(m.platform.halted());
    assert_eq!(m.out.text(), "OK");
}

#[test]
fn timer_interrupt_reaches_the_guest_handler() {
    // The full path: the CLINT's counter passes mtimecmp, the device asserts MTIP, the CPU notices it
    // before the next fetch, and the guest's handler runs. Nothing here writes mip directly.
    let k = LOAD_IMM64_STEPS;
    let handler = DRAM_BASE + (4 * k + 5) * 4;

    let mut p = Vec::new();
    load_imm64(&mut p, 1, handler); // [0,K)
    load_imm64(&mut p, 2, CLINT_BASE + 0x4000); // [K,2K)
    load_imm64(&mut p, 3, 50); // [2K,3K)  deadline
    load_imm64(&mut p, 4, csr::MIP_MTIP); // [3K,4K)  (mie.MTIE is the same bit)
    p.push(i_type(opcodes::SYSTEM, 0, 0x1, 1, csr::MTVEC as i32)); // 4K   mtvec
    p.push(s_type(opcodes::STORE, 0x3, 2, 3, 0)); // 4K+1 mtimecmp = 50
    p.push(i_type(opcodes::SYSTEM, 0, 0x1, 4, csr::MIE as i32)); // 4K+2 mie = MTIE
    p.push(i_type(opcodes::SYSTEM, 0, 0x6, 8, csr::MSTATUS as i32)); // 4K+3 mstatus.MIE
    p.push(halt()); // 4K+4 spin
    p.push(addi(9, 0, 123)); // 4K+5 handler
    p.push(halt()); // 4K+6

    let mut m = FullMachine::new(&p);
    m.platform.run(500).unwrap();
    assert_eq!(m.platform.harts[0].read_reg(9), 123);
}

// --- ELF loader -------------------------------------------------------------

/// Builds a minimal ELF64 RISC-V file with one PT_LOAD segment.
fn make_elf(entry: u64, paddr: u64, payload: &[u8], memsz_extra: u64) -> Vec<u8> {
    let mut e = vec![0u8; 64 + 56];
    let put16 = |e: &mut Vec<u8>, o: usize, v: u16| e[o..o + 2].copy_from_slice(&v.to_le_bytes());
    let put32 = |e: &mut Vec<u8>, o: usize, v: u32| e[o..o + 4].copy_from_slice(&v.to_le_bytes());
    let put64 = |e: &mut Vec<u8>, o: usize, v: u64| e[o..o + 8].copy_from_slice(&v.to_le_bytes());

    e[0..4].copy_from_slice(&[0x7f, b'E', b'L', b'F']);
    e[4] = 2; // ELFCLASS64
    e[5] = 1; // little-endian
    e[6] = 1; // version
    put16(&mut e, 16, 2); // ET_EXEC
    put16(&mut e, 18, 243); // EM_RISCV
    put32(&mut e, 20, 1);
    put64(&mut e, 24, entry);
    put64(&mut e, 32, 64); // e_phoff
    put16(&mut e, 52, 64); // e_ehsize
    put16(&mut e, 54, 56); // e_phentsize
    put16(&mut e, 56, 1); // e_phnum

    let ph = 64;
    put32(&mut e, ph, 1); // PT_LOAD
    put32(&mut e, ph + 4, 5); // flags: R+X
    put64(&mut e, ph + 8, 120); // p_offset (payload follows)
    put64(&mut e, ph + 16, paddr); // p_vaddr
    put64(&mut e, ph + 24, paddr); // p_paddr
    put64(&mut e, ph + 32, payload.len() as u64); // p_filesz
    put64(&mut e, ph + 40, payload.len() as u64 + memsz_extra); // p_memsz
    put64(&mut e, ph + 48, 8); // p_align

    e.extend_from_slice(payload);
    e
}

#[test]
fn elf_detection() {
    assert!(is_elf(&[0x7f, b'E', b'L', b'F', 0]));
    assert!(!is_elf(&[0x00, 0x50, 0x02, 0x93])); // a plain instruction word
    assert!(!is_elf(&[]));
}

#[test]
fn elf_loads_segment_at_paddr_and_reports_entry() {
    let bus = Bus::new(1024 * 1024);
    let elf = make_elf(DRAM_BASE + 0x100, DRAM_BASE + 0x200, &[0xde, 0xad, 0xbe, 0xef], 0);

    let img = load_elf(&elf, &bus).unwrap();
    assert_eq!(img.entry, DRAM_BASE + 0x100);
    assert_eq!(bus.load(DRAM_BASE + 0x200, 4, AccessType::Load).unwrap(), 0xefbe_adde);
}

#[test]
fn elf_zero_fills_bss() {
    // p_memsz > p_filesz means .bss: storage the program expects to exist and be zero, but which takes
    // no space in the file. Skipping the zero-fill leaves a kernel's globals full of whatever was in
    // RAM.
    let bus = Bus::new(1024 * 1024);

    // Dirty the region first so a missing zero-fill would be visible.
    for i in 0..16 {
        bus.store(DRAM_BASE + 0x300 + i, 1, 0xff).unwrap();
    }

    let elf = make_elf(DRAM_BASE, DRAM_BASE + 0x300, &[1, 2, 3, 4], 8);
    load_elf(&elf, &bus).unwrap();

    assert_eq!(bus.load(DRAM_BASE + 0x304, 8, AccessType::Load).unwrap(), 0);
}

#[test]
fn elf_rejects_bad_images() {
    let bus = Bus::new(1024 * 1024);

    assert!(load_elf(&[1, 2, 3], &bus).is_err()); // not ELF
    let mut e32 = make_elf(DRAM_BASE, DRAM_BASE, &[1], 0);
    e32[4] = 1; // ELFCLASS32
    assert!(load_elf(&e32, &bus).is_err());
    let mut ex86 = make_elf(DRAM_BASE, DRAM_BASE, &[1], 0);
    ex86[18] = 62; // EM_X86_64
    assert!(load_elf(&ex86, &bus).is_err());
    // A segment aimed at unmapped memory must be refused, not silently dropped.
    assert!(load_elf(&make_elf(0, 0x1000, &[1, 2, 3, 4], 0), &bus).is_err());
}

#[test]
fn elf_header_fields_cannot_overflow_the_bounds_checks() {
    // A crafted p_offset near 2^64 wraps `offset + length` to something small, which would pass a
    // naive bounds check and read far outside the file. The loader subtracts instead.
    let bus = Bus::new(1024 * 1024);
    let mut e = make_elf(DRAM_BASE, DRAM_BASE, &[1, 2, 3, 4], 0);
    e[64 + 8..64 + 16].copy_from_slice(&(u64::MAX - 1).to_le_bytes()); // p_offset
    assert!(load_elf(&e, &bus).is_err());
    let mut e = make_elf(DRAM_BASE, DRAM_BASE, &[1, 2, 3, 4], 0);
    e[32..40].copy_from_slice(&(u64::MAX - 8).to_le_bytes()); // e_phoff
    assert!(load_elf(&e, &bus).is_err());
}


#[test]
fn elf_with_an_enormous_memsz_is_refused_quickly() {
    let bus = Bus::new(1024 * 1024);
    let mut e = make_elf(DRAM_BASE, DRAM_BASE, &[1, 2, 3, 4], 0);
    e[64 + 40..64 + 48].copy_from_slice(&(1u64 << 60).to_le_bytes()); // p_memsz
    assert!(load_elf(&e, &bus).is_err());
    let mut e = make_elf(DRAM_BASE, u64::MAX - 2, &[1, 2, 3, 4], 0); // p_paddr + memsz wraps
    e[64 + 40..64 + 48].copy_from_slice(&16u64.to_le_bytes());
    assert!(load_elf(&e, &bus).is_err());
}

#[test]
fn an_impossible_ram_size_is_an_error_not_an_abort() {
    assert!(Bus::try_new(u64::MAX).is_none());
    assert!(Bus::try_new(1 << 62).is_none());
    assert!(Bus::try_new(1024 * 1024).is_some());
}

#[test]
fn a_guest_polling_an_empty_uart_asks_the_machine_to_yield() {
    let (mut u, _) = uart();
    for _ in 0..63 {
        u.load(5, 1).unwrap();
    }
    assert!(!u.take_yield_request());
    u.load(5, 1).unwrap();
    assert!(u.take_yield_request());
    assert!(!u.take_yield_request()); // the count was reset

    // Data arriving resets the streak.
    for _ in 0..40 {
        u.load(5, 1).unwrap();
    }
    u.feed_input(b"x");
    u.load(5, 1).unwrap();
    for _ in 0..40 {
        u.load(5, 1).unwrap();
    }
    assert!(!u.take_yield_request());
}
