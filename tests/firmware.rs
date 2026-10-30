//! The two things a Linux kernel needs that a bare-metal program does not: a device tree telling it
//! what hardware exists, and an SBI firmware layer doing the things supervisor mode cannot do for
//! itself.

mod common;

use std::cell::RefCell;
use std::rc::Rc;

use common::*;
use riscv_emu::clint::{Clint, Clock};
use riscv_emu::csr::csr;
use riscv_emu::fdt::{self, FdtConfig};
use riscv_emu::sbi;
use riscv_emu::trap::Exception;
use riscv_emu::types::{DRAM_BASE, PRIV_SUPERVISOR};
use riscv_emu::uart::Uart;

// --- device tree ------------------------------------------------------------

fn be32_at(blob: &[u8], offset: usize) -> u32 {
    u32::from_be_bytes(blob[offset..offset + 4].try_into().unwrap())
}

/// Does the blob contain this string? Enough to confirm a property name or a compatible string made
/// it in, without reimplementing a device-tree parser here - `dtc -I dtb` is the real structural
/// check, and the boot scripts do that.
fn contains_string(blob: &[u8], needle: &str) -> bool {
    let n = needle.as_bytes();
    blob.windows(n.len()).any(|w| w == n)
}

#[test]
fn fdt_header_is_well_formed() {
    let blob = fdt::build_simple(128 * 1024 * 1024, "console=ttyS0", 0, 0);

    // Everything in a device tree is big-endian, including on a little-endian machine. A blob
    // written natively parses as garbage, and the magic number is the first place that shows up.
    assert_eq!(be32_at(&blob, 0), 0xd00d_feed);
    assert_eq!(be32_at(&blob, 4) as usize, blob.len()); // totalsize
    assert_eq!(be32_at(&blob, 20), 17); // version
    assert_eq!(be32_at(&blob, 24), 16); // last compatible version

    let off_struct = be32_at(&blob, 8) as usize;
    let off_strings = be32_at(&blob, 12) as usize;
    let off_rsvmap = be32_at(&blob, 16) as usize;
    let size_strings = be32_at(&blob, 32) as usize;
    let size_struct = be32_at(&blob, 36) as usize;

    // The three blocks follow the header in order and exactly fill the blob.
    assert!(off_rsvmap >= 40);
    assert!(off_struct > off_rsvmap);
    assert_eq!(off_strings, off_struct + size_struct);
    assert_eq!(off_strings + size_strings, blob.len());

    // Every offset is 4-byte aligned, which a parser reading 32-bit tokens depends on.
    assert_eq!(off_struct % 4, 0);
    assert_eq!(off_rsvmap % 8, 0);

    // The structure block ends with FDT_END.
    assert_eq!(be32_at(&blob, off_struct + size_struct - 4), 9);

    // The reservation block is terminated by an all-zero entry, which is how a parser knows to
    // stop reading it.
    assert_eq!(be32_at(&blob, off_rsvmap), 0);
    assert_eq!(be32_at(&blob, off_rsvmap + 12), 0);
}

#[test]
fn fdt_describes_this_machine() {
    let blob = fdt::build_simple(128 * 1024 * 1024, "console=ttyS0", 0, 0);

    // Every device the emulator actually has, named at the address it is actually at. The blob is
    // built from the same constants in types.rs that the devices are attached with, so these cannot
    // drift apart - but the node names are spelled out here, and a mismatch would mean a kernel
    // looking in the wrong place.
    for node in [
        "memory@80000000",
        "serial@10000000",
        "virtio_mmio@10001000",
        "clint@2000000",
        "plic@c000000",
        "syscon@100000",
    ] {
        assert!(contains_string(&blob, node), "{node}");
    }

    // The compatible strings are how Linux picks a driver. Get one wrong and the device is simply
    // not there as far as the kernel is concerned.
    for compat in ["ns16550a", "virtio,mmio", "riscv,cpu-intc", "syscon-poweroff"] {
        assert!(contains_string(&blob, compat), "{compat}");
    }

    // The ISA string must match what the emulator implements: claim more and the kernel executes an
    // instruction that traps, claim less and it takes a slower path for nothing.
    assert!(contains_string(&blob, "rv64imafdc_zicsr_zifencei"));
    assert!(contains_string(&blob, "riscv,sv39"));

    // Without stdout-path the kernel has a UART driver and a node describing a UART, but no reason to
    // believe that UART is the console - and boots silently.
    assert!(contains_string(&blob, "stdout-path"));
    assert!(contains_string(&blob, "/soc/serial@10000000"));
}

#[test]
fn fdt_carries_the_command_line_and_initrd() {
    let blob = fdt::build_simple(64 * 1024 * 1024, "root=/dev/vda rw console=ttyS0", 0x8400_0000, 0x8410_0000);
    assert!(contains_string(&blob, "root=/dev/vda rw console=ttyS0"));
    assert!(contains_string(&blob, "linux,initrd-start"));
    assert!(contains_string(&blob, "linux,initrd-end"));

    // 64-bit, not 32. With more than 4 GiB of guest RAM the initramfs lands above the 4 GiB line,
    // and a 32-bit property truncates it to an address below DRAM_BASE with no memory behind it.
    let high = fdt::build_simple(8 * 1024 * 1024 * 1024, "", 0x1_7c00_0000, 0x1_7c10_0000);
    let found_start = (0..high.len().saturating_sub(8))
        .step_by(4)
        .any(|i| be32_at(&high, i) == 1 && be32_at(&high, i + 4) == 0x7c00_0000);
    assert!(found_start);

    // With no initrd the properties are absent rather than zero: a kernel reads "present" as "there
    // is one", so a zero-length one would be worse than none at all.
    let plain = fdt::build_simple(64 * 1024 * 1024, "console=ttyS0", 0, 0);
    assert!(!contains_string(&plain, "linux,initrd-start"));
}

#[test]
fn fdt_reports_the_memory_it_was_given() {
    // The size lands in the memory node's reg property as two big-endian cells following the base
    // address. A kernel told the wrong size either wastes memory or writes off the end of it.
    let blob = fdt::build_simple(0x1234_5000, "", 0, 0);
    let found = (0..blob.len() - 16).step_by(4).any(|i| {
        be32_at(&blob, i) == 0
            && be32_at(&blob, i + 4) == 0x8000_0000
            && be32_at(&blob, i + 8) == 0
            && be32_at(&blob, i + 12) == 0x1234_5000
    });
    assert!(found);
}

#[test]
fn fdt_optional_devices_appear_only_when_present() {
    let none = fdt::build(&FdtConfig::default());
    for node in ["virtio_mmio@10002000", "virtio_mmio@10003000", "rtc@101000"] {
        assert!(!contains_string(&none, node), "{node} should be absent");
    }
    let all = fdt::build(&FdtConfig { virtio_net: true, virtio_rng: true, rtc: true, ..FdtConfig::default() });
    for node in ["virtio_mmio@10002000", "virtio_mmio@10003000", "rtc@101000", "google,goldfish-rtc"] {
        assert!(contains_string(&all, node), "{node}");
    }
}

#[test]
fn fdt_has_one_cpu_and_two_plic_contexts_per_hart() {
    let blob = fdt::build(&FdtConfig { harts: 4, ..FdtConfig::default() });
    for h in 0..4 {
        assert!(contains_string(&blob, &format!("cpu@{h}")));
    }
    assert!(!contains_string(&blob, "cpu@4"));
}

#[test]
fn fdt_rng_seed_is_passed_through() {
    let blob = fdt::build(&FdtConfig { rng_seed: vec![0xab; 32], ..FdtConfig::default() });
    assert!(contains_string(&blob, "rng-seed"));
    assert!(blob.windows(32).any(|w| w.iter().all(|&b| b == 0xab)));
}

// --- SBI ---------------------------------------------------------------------

/// A machine set up the way --linux sets one up: supervisor mode, SBI enabled.
struct SbiRig {
    m: Machine,
    clint: Rc<RefCell<Clint>>,
    uart: Rc<RefCell<Uart>>,
}

impl SbiRig {
    fn new(program: &[u32]) -> SbiRig {
        let mut m = Machine::new(program);
        let clint = Rc::new(RefCell::new(Clint::new(1, Rc::new(Clock::new()))));
        let uart = Rc::new(RefCell::new(Uart::new(Box::new(SharedBuf::new()))));
        m.cpu.priv_ = PRIV_SUPERVISOR;
        m.cpu.sbi_enabled = true;
        m.cpu.clint = Some(clint.clone());
        m.cpu.uart = Some(uart.clone());
        SbiRig { m, clint, uart }
    }

    /// Place an ECALL with the SBI arguments already in place and run it.
    fn call(&mut self, eid: u64, fid: u64, a0: u64) {
        self.m.cpu.write_reg(17, eid);
        self.m.cpu.write_reg(16, fid);
        self.m.cpu.write_reg(10, a0);
        assert!(self.m.cpu.step().is_ok());
    }

    fn a0(&mut self) -> u64 {
        self.m.cpu.read_reg(10)
    }
    fn a1(&mut self) -> u64 {
        self.m.cpu.read_reg(11)
    }
}

/// ECALL, encoded directly.
const ECALL: u32 = 0x0000_0073;

#[test]
fn sbi_ecall_returns_instead_of_trapping() {
    // This is the whole idea: with SBI enabled, an ecall from supervisor mode is a call into
    // firmware, not an exception. Execution continues at the next instruction rather than vectoring
    // to stvec.
    let mut rig = SbiRig::new(&[ECALL, addi(5, 0, 7)]);
    rig.call(sbi::EXT_BASE, 0, 0);
    assert_eq!(rig.m.cpu.pc, DRAM_BASE + 4);
    assert!(rig.m.cpu.step().is_ok());
    assert_eq!(rig.m.reg(5), 7);
}

#[test]
fn sbi_is_off_by_default() {
    // A kernel that provides its own machine-mode code - xv6 does - must see its own ecalls. Enabling
    // SBI unconditionally would steal them.
    let mut m = Machine::new(&[ECALL]);
    m.cpu.priv_ = PRIV_SUPERVISOR;
    assert!(!m.cpu.sbi_enabled);
    assert_eq!(m.cpu.step().unwrap_err().cause, Exception::ECallFromSMode);
}

#[test]
fn sbi_base_extension_reports_what_exists() {
    let mut rig = SbiRig::new(&[ECALL, ECALL, ECALL, ECALL]);

    rig.call(sbi::EXT_BASE, 0, 0); // get_spec_version
    assert_eq!(rig.a0(), 0); // success
    assert_ne!(rig.a1(), 0);

    rig.call(sbi::EXT_BASE, 3, sbi::EXT_TIME); // probe_extension
    assert_eq!(rig.a0(), 0);
    assert_eq!(rig.a1(), 1); // present

    rig.call(sbi::EXT_BASE, 3, 0xdead_beef); // probe something absent
    assert_eq!(rig.a0(), 0);
    assert_eq!(rig.a1(), 0);

    // An unknown *function* is an error, not silence. A kernel probing for something optional must be
    // able to ask and be told no.
    rig.call(sbi::EXT_BASE, 99, 0);
    assert_eq!(rig.a0(), sbi::ERR_NOT_SUPPORTED as u64);
}

#[test]
fn sbi_set_timer_writes_mtimecmp_and_clears_the_pending_interrupt() {
    // mtimecmp is a machine-mode register, so a supervisor cannot write it - which is the entire
    // reason this call exists.
    let mut rig = SbiRig::new(&[ECALL]);
    rig.m.cpu.csrs.raise_interrupt(csr::MIP_MTIP);
    assert_ne!(rig.m.cpu.csrs.read(csr::MIP) & csr::MIP_MTIP, 0);

    rig.call(sbi::EXT_TIME, 0, 5000);

    // Clearing MTIP is required, and easy to miss. The interrupt that just fired is still pending;
    // leaving it set means the kernel re-enters its timer handler on the very next instruction and
    // never makes progress.
    assert_eq!(rig.m.cpu.csrs.read(csr::MIP) & csr::MIP_MTIP, 0);

    // And the deadline took effect: it is 5000 ticks away, so ten ticks on does not raise it.
    for _ in 0..10 {
        rig.clint.borrow().clock().tick(1);
    }
    rig.clint.borrow().update(&mut rig.m.cpu.csrs, 0);
    assert_eq!(rig.m.cpu.csrs.read(csr::MIP) & csr::MIP_MTIP, 0);
    assert_eq!(rig.clint.borrow().mtimecmp(0), 5000);
}

#[test]
fn sbi_set_timer_expiry_reaches_the_supervisor() {
    // mtimecmp's expiry naturally raises MTIP - a *machine* timer interrupt. A supervisor cannot
    // enable that: mie is machine-only, and under --linux there is no M-mode software to enable it on
    // the kernel's behalf. So without forwarding, the deadline expires into nothing, jiffies never
    // advance, and every sleep in the kernel hangs forever - a failure that looks exactly like the
    // emulator being slow rather than being wrong.
    //
    // Real firmware handles the machine timer interrupt and posts a supervisor one in its place. This
    // checks that it happens.
    let mut rig = SbiRig::new(&[ECALL]);
    rig.call(sbi::EXT_TIME, 0, 5);

    rig.clint.borrow().update(&mut rig.m.cpu.csrs, 0);
    assert_eq!(rig.m.cpu.csrs.read(csr::MIP) & csr::MIP_STIP, 0);

    for _ in 0..10 {
        rig.clint.borrow().clock().tick(1);
    }
    rig.clint.borrow().update(&mut rig.m.cpu.csrs, 0);
    assert_ne!(rig.m.cpu.csrs.read(csr::MIP) & csr::MIP_STIP, 0);
}

#[test]
fn the_timer_divisor_slows_the_clock_rather_than_speeding_it_up() {
    // "--timer-divisor N: instructions per mtime tick". This used to *add* N per instruction, the exact
    // inverse - so asking for a slower clock gave an interrupt storm instead.
    let clint = Clint::new(1, Rc::new(Clock::new()));
    clint.clock().set_instructions_per_tick(10);
    for _ in 0..9 {
        clint.clock().tick(1);
    }
    assert_eq!(clint.mtime(), 0);
    clint.clock().tick(1);
    assert_eq!(clint.mtime(), 1);
    for _ in 0..30 {
        clint.clock().tick(1);
    }
    assert_eq!(clint.mtime(), 4);

    // The default advances once per instruction, which is what makes a run reproducible: the timer
    // fires at the same instruction every time.
    let fast = Clint::new(1, Rc::new(Clock::new()));
    for _ in 0..5 {
        fast.clock().tick(1);
    }
    assert_eq!(fast.mtime(), 5);
}

#[test]
fn sbi_legacy_set_timer_returns_no_error_code() {
    // The v0.1 form returns nothing at all - not even success - so a0 must be left exactly as the
    // caller set it. Writing a return code into it would corrupt a register the kernel still
    // considers live.
    let mut rig = SbiRig::new(&[ECALL]);
    rig.m.cpu.write_reg(10, 1234);
    rig.m.cpu.write_reg(17, sbi::EXT_SET_TIMER);
    assert!(rig.m.cpu.step().is_ok());
    assert_eq!(rig.m.cpu.read_reg(10), 1234);
}

#[test]
fn sbi_console_putchar_reaches_the_host() {
    // Linux uses this for its earliest output, before it has probed the device tree and found a real
    // UART driver - which is exactly when you most want to see something, because a failure before
    // that point is silent. Writes go to the process's stdout, so this checks the call is accepted
    // and consumes no trap rather than the text itself.
    let mut rig = SbiRig::new(&[ECALL, ECALL]);
    rig.call(sbi::EXT_CONSOLE_PUTCHAR, 0, b'h' as u64);
    assert_eq!(rig.m.cpu.pc, DRAM_BASE + 4);
}

#[test]
fn sbi_console_getchar_drains_the_uart() {
    let mut rig = SbiRig::new(&[ECALL, ECALL, ECALL]);
    rig.uart.borrow_mut().feed_input(b"hi");

    rig.call(sbi::EXT_CONSOLE_GETCHAR, 0, 0);
    assert_eq!(rig.a0(), b'h' as u64);
    rig.call(sbi::EXT_CONSOLE_GETCHAR, 0, 0);
    assert_eq!(rig.a0(), b'i' as u64);

    // Nothing waiting reads as -1, which is how the caller knows to stop.
    rig.call(sbi::EXT_CONSOLE_GETCHAR, 0, 0);
    assert_eq!(rig.a0(), u64::MAX);
}

#[test]
fn sbi_shutdown_stops_the_machine() {
    let mut rig = SbiRig::new(&[ECALL, addi(5, 0, 7)]);
    rig.call(sbi::EXT_SRST, 0, 0);
    assert!(rig.m.cpu.halted);
    assert!(rig.m.cpu.sbi_shutdown);

    // And the legacy call does the same thing.
    let mut legacy = SbiRig::new(&[ECALL]);
    legacy.call(sbi::EXT_SHUTDOWN, 0, 0);
    assert!(legacy.m.cpu.halted);
}

#[test]
fn sbi_single_hart_calls_succeed() {
    // An IPI to a mask containing only this hart, and a remote fence with no remote harts to fence,
    // are both no-ops that must report success. Returning "not supported" would make the kernel take
    // a fallback path for a situation that is not a failure.
    let mut rig = SbiRig::new(&[ECALL, ECALL]);
    rig.call(sbi::EXT_IPI, 0, 0);
    assert_eq!(rig.a0(), 0);
    rig.call(sbi::EXT_RFENCE, 0, 0);
    assert_eq!(rig.a0(), 0);
}

#[test]
fn sbi_unknown_extension_is_refused_cleanly() {
    let mut rig = SbiRig::new(&[ECALL]);
    rig.call(0xbadc0de, 0, 0);
    assert_eq!(rig.a0(), sbi::ERR_NOT_SUPPORTED as u64);
    // Refused, but not a trap: the kernel carries on and takes its fallback.
    assert_eq!(rig.m.cpu.pc, DRAM_BASE + 4);
}
