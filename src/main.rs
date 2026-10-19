//! The `riscv_emu` command line: build a machine from flags, load an image, run it.

use std::io::Write;
use std::net::Ipv4Addr;
use std::process::exit;
use std::rc::Rc;

use riscv_emu::bus::{Bus, Shared};
use riscv_emu::clint::{Clint, Clock, MAX_HARTS};
use riscv_emu::elf;
use riscv_emu::fdt::{self, FdtConfig};
use riscv_emu::gdb::GdbStub;
use riscv_emu::net::usernet::{self, UserNet};
use riscv_emu::platform::Platform;
use riscv_emu::plic::Plic;
use riscv_emu::rtc::Rtc;
use riscv_emu::syscon::Syscon;
use riscv_emu::types::*;
use riscv_emu::uart::Uart;
use riscv_emu::virtio::blk::VirtioBlk;
use riscv_emu::virtio::net::{LoopbackBackend, NetBackend, VirtioNet};
use riscv_emu::virtio::rng::{host_entropy, VirtioRng};
use riscv_emu::virtio::VirtioMmio;

fn print_usage(argv0: &str) {
    eprint!(
        "usage: {argv0} [options] <image>

  Runs an ELF64 RISC-V image, or a flat binary loaded at {DRAM_BASE:#x}.
  The format is detected from the file's magic number.
  With no image, runs a built-in demo that prints over the UART.

options:
  --trace              print one line per retired instruction (stderr)
  --max-steps N        stop after N instructions (default 100000000)
  --dram-size-mb N     guest RAM size in MiB (default 128)
  --timer-divisor N    instructions per mtime tick (default 1)
  --smp N              number of harts (cores), default 1
  --quantum N          instructions each hart runs per scheduling round
                       (default 256); smaller is finer-grained, larger is faster
  --dump               dump registers when execution stops
  --disk FILE          back the virtio block device with FILE, read into memory
                       (the guest's writes are discarded when the emulator exits)
  --drive FILE         back the block device with FILE, kept on disk: no RAM cost
                       for a big image, and the guest's writes persist in FILE
  --drive-snapshot FILE  like --drive, but writes are held in memory and FILE is
                       never modified

  Debugging:
  --gdb PORT           wait for gdb on PORT before running anything
                       (then: gdb vmlinux -ex 'target remote :PORT'); 0 picks a port

  Devices:
  --net user           a virtio network card on a built-in user-mode network:
                       DHCP, DNS, TCP and UDP to the host's network, no setup
  --net loopback       a network card whose transmitted frames come straight back
  --net-dns IP         the DNS server the guest's lookups are forwarded to
  --rng                a virtio entropy source (a Debian boot waits for one)
  --rng-seed N         make the entropy (and the seed handed to the kernel) reproducible
  --rtc                a real-time clock, so the guest knows today's date
  --rtc-fixed SECONDS  pin the clock to a Unix time instead of the host's


  Booting Linux (see docs/09-booting-linux.md):
  --linux              start in supervisor mode with a generated device
                       tree in a1 and SBI firmware services enabled,
                       which is what a real bootloader hands a kernel
  --bootargs STR       the kernel command line placed in /chosen
  --initrd FILE        load an initramfs and point the kernel at it
  --dump-dtb FILE      write the generated device tree out and exit
  --isa STRING         the riscv,isa string given to the kernel (default advertises
                       everything implemented; shorten it to hide an extension)
  -h, --help           show this message

  To boot xv6, use scripts/boot-xv6.sh - it fetches and builds it
  for you. Note that a real kernel needs a much larger --max-steps
  than the default: xv6 reaches its shell at around 500 million.
"
    );
}

/// strtoull(..., 0): decimal, 0x-prefixed hex, or 0-prefixed octal.
fn parse_num(s: &str) -> Option<u64> {
    let s = s.trim();
    if let Some(h) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
        u64::from_str_radix(h, 16).ok()
    } else if s.len() > 1 && s.starts_with('0') {
        u64::from_str_radix(&s[1..], 8).ok()
    } else {
        s.parse().ok()
    }
}

/// A built-in demo, used when no image is given: write "hello, RISC-V\n" to the UART
/// one byte at a time, then power off through syscon. The smallest program that shows a
/// guest that can be heard from and can stop on its own terms.
fn demo_image() -> Vec<u8> {
    const PROGRAM: [u32; 13] = [
        0x1000_02b7, // lui   t0, 0x10000    -> t0 = UART0_BASE (0x1000_0000)
        // auipc rather than `lui t1, 0x80000`: on RV64 LUI sign-extends from bit 31, so
        // that would give 0xFFFFFFFF80000000. auipc sidesteps it and is what real
        // position-independent code uses anyway.
        0x0000_0317, // auipc t1, 0          -> t1 = address of this instruction
        0x03c3_0313, // addi  t1, t1, 60     -> the message, at byte offset 64
        0x0003_4383, // loop: lbu   t2, 0(t1)
        0x0003_8863, // beqz  t2, done       -> +16
        0x0072_8023, // sb    t2, 0(t0)      -> the byte appears on the console
        0x0013_0313, // addi  t1, t1, 1
        0xff1f_f06f, // j     loop           -> -16
        0x0010_02b7, // done: lui   t0, 0x100 -> t0 = SYSCON_BASE (0x0010_0000)
        0x0000_53b7, // lui   t2, 0x5        -> 0x5000
        0x5553_839b, // addiw t2, t2, 0x555  -> 0x5555, the poweroff word
        0x0072_a023, // sw    t2, 0(t0)      -> stop the machine
        0x0000_006f, // j     .              (never reached)
    ];
    let mut bytes: Vec<u8> = PROGRAM.iter().flat_map(|w| w.to_le_bytes()).collect();
    bytes.resize(64, 0); // the message lives at offset 64
    bytes.extend_from_slice(b"hello, RISC-V\n\0");
    bytes
}

struct Options {
    image_path: String,
    disk_path: String,
    trace: bool,
    dump: bool,
    max_steps: u64,
    dram_size_mb: u64,
    timer_divisor: u64,
    smp: u64,
    quantum: u64,
    linux_boot: bool,
    bootargs: String,
    initrd_path: String,
    dtb_out_path: String,
    drive_path: String,
    drive_snapshot: bool,
    net_mode: String,
    net_dns: String,
    isa_override: String,
    gdb_port: Option<u64>,
    want_rng: bool,
    rng_seed: Option<u64>,
    want_rtc: bool,
    rtc_fixed_seconds: Option<u64>,
}

fn parse_args(args: &[String]) -> Options {
    let argv0 = args.first().map_or("riscv_emu", |s| s.as_str());
    let mut o = Options {
        image_path: String::new(),
        disk_path: String::new(),
        trace: false,
        dump: false,
        max_steps: 100_000_000,
        dram_size_mb: 128,
        timer_divisor: 1,
        smp: 1,
        quantum: 256,
        linux_boot: false,
        bootargs: "console=ttyS0 earlycon=sbi".into(),
        initrd_path: String::new(),
        dtb_out_path: String::new(),
        drive_path: String::new(),
        drive_snapshot: false,
        net_mode: String::new(),
        net_dns: String::new(),
        isa_override: String::new(),
        gdb_port: None,
        want_rng: false,
        rng_seed: None,
        want_rtc: false,
        rtc_fixed_seconds: None,
    };

    let mut i = 1;
    // The value after a flag, or exit with the same message the C++ binary gave.
    macro_rules! value {
        ($arg:expr, $what:expr) => {{
            i += 1;
            match args.get(i) {
                Some(v) => v.clone(),
                None => {
                    eprintln!("error: {} requires {}", $arg, $what);
                    exit(2);
                }
            }
        }};
    }
    macro_rules! number {
        ($arg:expr) => {{
            i += 1;
            match args.get(i) {
                Some(v) => parse_num(v).unwrap_or(0),
                None => {
                    eprintln!("error: {} requires a value", $arg);
                    exit(2);
                }
            }
        }};
    }

    while i < args.len() {
        let arg = args[i].as_str();
        match arg {
            "-h" | "--help" => {
                print_usage(argv0);
                exit(0);
            }
            "--trace" => o.trace = true,
            "--dump" => o.dump = true,
            "--disk" => o.disk_path = value!(arg, "a path"),
            "--drive" | "--drive-snapshot" => {
                o.drive_path = value!(arg, "a path");
                o.drive_snapshot = arg == "--drive-snapshot";
            }
            "--net" => {
                o.net_mode = value!(arg, "'user' or 'loopback'");
                if o.net_mode != "user" && o.net_mode != "loopback" {
                    eprintln!("error: --net takes 'user' or 'loopback', not '{}'", o.net_mode);
                    exit(2);
                }
            }
            "--net-dns" => o.net_dns = value!(arg, "an IPv4 address"),
            "--gdb" => {
                let port = number!(arg);
                if port > 65535 {
                    eprintln!("error: --gdb wants a port number (0 to pick one)");
                    exit(2);
                }
                o.gdb_port = Some(port);
            }
            "--isa" => o.isa_override = value!(arg, "a string"),
            "--rng" => o.want_rng = true,
            "--rng-seed" => o.rng_seed = Some(number!(arg)),
            "--rtc" => o.want_rtc = true,
            "--rtc-fixed" => {
                o.rtc_fixed_seconds = Some(number!(arg));
                o.want_rtc = true;
            }
            "--linux" => o.linux_boot = true,
            "--bootargs" => o.bootargs = value!(arg, "a value"),
            "--initrd" => o.initrd_path = value!(arg, "a path"),
            "--dump-dtb" => o.dtb_out_path = value!(arg, "a path"),
            "--max-steps" => o.max_steps = number!(arg),
            "--dram-size-mb" => o.dram_size_mb = number!(arg),
            "--timer-divisor" => o.timer_divisor = number!(arg),
            "--smp" => o.smp = number!(arg),
            "--quantum" => o.quantum = number!(arg),
            _ if arg.starts_with('-') => {
                eprintln!("error: unknown option {arg}");
                print_usage(argv0);
                exit(2);
            }
            _ => o.image_path = arg.to_string(),
        }
        i += 1;
    }
    o
}

fn fail(msg: impl std::fmt::Display) -> ! {
    eprintln!("error: {msg}");
    exit(1);
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let o = parse_args(&args);

    if o.dram_size_mb == 0 || o.timer_divisor == 0 || o.quantum == 0 {
        eprintln!("error: --dram-size-mb, --timer-divisor and --quantum must be at least 1");
        exit(2);
    }
    if o.smp == 0 || o.smp > MAX_HARTS as u64 {
        eprintln!("error: --smp must be between 1 and {MAX_HARTS}");
        exit(2);
    }
    if !o.disk_path.is_empty() && !o.drive_path.is_empty() {
        eprintln!("error: --disk and --drive both name the block device; use one");
        exit(2);
    }
    let smp = o.smp as u32;
    let dram_bytes = o.dram_size_mb.checked_mul(1024 * 1024).unwrap_or_else(|| fail("--dram-size-mb is too large"));

    // --- build the machine ---------------------------------------------------------
    let mut bus = Bus::new(dram_bytes);
    let ram = bus.ram.clone();

    let (uart_dev, uart) = Shared::new(Uart::new(Box::new(std::io::stdout())));
    let clock = Rc::new(Clock::new());
    let (clint_dev, clint) = Shared::new(Clint::new(smp, clock));
    let (syscon_dev, syscon) = Shared::new(Syscon::new());
    let (plic_dev, plic) = Shared::new(Plic::new(smp));

    // The block device is a bus master - it reads and writes guest memory itself - and
    // raises its interrupt through the PLIC.
    let mut blk = VirtioMmio::new(VIRTIO_BASE, ram.clone(), VirtioBlk::new());
    blk.attach(plic.clone(), VIRTIO0_IRQ);
    if !o.disk_path.is_empty() {
        if blk.dev.load_image(&o.disk_path).is_err() {
            fail(format!("cannot read disk image '{}'", o.disk_path));
        }
        eprintln!("disk: {} ({} sectors)", o.disk_path, blk.dev.sectors());
    }
    if !o.drive_path.is_empty() {
        if blk.dev.open_image(&o.drive_path, o.drive_snapshot).is_err() {
            fail(format!("cannot open drive image '{}'", o.drive_path));
        }
        eprintln!(
            "drive: {} ({} sectors, {})",
            o.drive_path,
            blk.dev.sectors(),
            if o.drive_snapshot { "snapshot: writes discarded" } else { "writes persist" }
        );
    }

    let mut attached = bus.attach(Box::new(uart_dev))
        && bus.attach(Box::new(clint_dev))
        && bus.attach(Box::new(syscon_dev))
        && bus.attach(Box::new(plic_dev))
        && bus.attach(Box::new(blk));
    if !attached {
        fail("failed to build the machine's address map");
    }

    // --- optional devices ------------------------------------------------------------
    //
    // These live on the bus beside the block device, at consecutive virtio slots, and
    // are described to the kernel in the device tree only when present - a node for
    // hardware that is not there would make the kernel probe it.
    let net_backend: Option<Box<dyn NetBackend>> = match o.net_mode.as_str() {
        "user" => {
            let mut cfg = usernet::Config::default();
            if !o.net_dns.is_empty() {
                match o.net_dns.parse::<Ipv4Addr>() {
                    Ok(ip) => cfg.dns_server = u32::from(ip),
                    Err(_) => {
                        eprintln!("error: --net-dns wants an IPv4 address like 1.1.1.1");
                        exit(2);
                    }
                }
            }
            Some(Box::new(UserNet::new(cfg)))
        }
        "loopback" => Some(Box::new(LoopbackBackend::new())),
        _ => None,
    };
    let want_net = net_backend.is_some();
    if let Some(backend) = net_backend {
        // The MAC address is locally administered (bit 1 of the first byte set, bit 0
        // clear), which is how a made-up address announces that it is one.
        let mac = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];
        let mut net = VirtioMmio::new(VIRTIO_NET_BASE, ram.clone(), VirtioNet::new(backend, mac));
        net.attach(plic.clone(), VIRTIO_NET_IRQ);
        attached = bus.attach(Box::new(net));
        if !attached {
            fail("cannot attach the network card");
        }
        eprintln!("net: virtio-net on a {} network", o.net_mode);
    }
    if o.want_rng {
        let mut rng = VirtioRng::new();
        if let Some(seed) = o.rng_seed {
            rng.seed(seed);
        }
        let mut dev = VirtioMmio::new(VIRTIO_RNG_BASE, ram.clone(), rng);
        dev.attach(plic.clone(), VIRTIO_RNG_IRQ);
        if !bus.attach(Box::new(dev)) {
            fail("cannot attach the entropy device");
        }
    }
    if o.want_rtc {
        let mut rtc = Rtc::new();
        rtc.attach(plic.clone(), RTC_IRQ);
        if let Some(s) = o.rtc_fixed_seconds {
            rtc.set_fixed_time(s.wrapping_mul(1_000_000_000));
        }
        if !bus.attach(Box::new(rtc)) {
            fail("cannot attach the clock");
        }
    }

    // --- load the image -----------------------------------------------------------------
    let mut entry = DRAM_BASE;
    let mut tohost = 0u64;
    if o.image_path.is_empty() {
        ram.load_image(DRAM_BASE, &demo_image());
        eprintln!("no image given; running the built-in UART demo");
    } else {
        let bytes = std::fs::read(&o.image_path).unwrap_or_else(|_| fail(format!("cannot read image '{}'", o.image_path)));
        if elf::is_elf(&bytes) {
            let img = elf::load_elf(&bytes, &bus).unwrap_or_else(|e| fail(e));
            entry = img.entry;
            tohost = img.tohost;
        } else {
            // A flat binary normally goes at the start of DRAM. A Linux kernel Image is
            // the exception: it carries a header saying where it wants to be.
            //
            //   offset 0x00  code0, code1   a jump, so the header is executable
            //   offset 0x08  text_offset    where to load it, relative to the 2 MiB-aligned
            //                               start of memory
            //   offset 0x38  magic2         "RSC\x05"
            //
            // text_offset is 2 MiB on RV64, because that is where firmware conventionally
            // ends. Loading the kernel at the start of DRAM instead puts every symbol 2 MiB
            // away from where it was linked, and it dies on its first absolute reference.
            let mut load_at = DRAM_BASE;
            if bytes.len() >= 0x40 {
                let magic2 = u32::from_le_bytes(bytes[0x38..0x3c].try_into().unwrap());
                if magic2 == 0x0543_5352 {
                    let text_offset = u64::from_le_bytes(bytes[8..16].try_into().unwrap());
                    load_at = DRAM_BASE.wrapping_add(text_offset);
                    entry = load_at;
                    eprintln!("linux kernel image, text_offset {text_offset:#x} -> loading at {load_at:#x}");
                }
            }
            if !ram.load_image(load_at, &bytes) {
                fail(format!("image ({} bytes) does not fit in {} MiB of DRAM", bytes.len(), o.dram_size_mb));
            }
        }
    }

    // --- Linux boot: device tree, initramfs, and supervisor mode -----------------------------
    //
    // A RISC-V kernel expects to be entered by firmware with a0 = the hart ID and a1 =
    // the address of a device tree. Everything it knows about the machine comes from that
    // blob, so it is built here from the same constants the devices were attached with
    // and cannot drift out of agreement.
    let mut dtb_addr = 0u64;
    if o.linux_boot || !o.dtb_out_path.is_empty() {
        // The initramfs goes high in memory, well clear of the kernel, and the device tree
        // just below it. Putting them at the top rather than immediately after the kernel
        // means their placement does not depend on how big the kernel turned out to be.
        let (mut initrd_start, mut initrd_end) = (0u64, 0u64);
        if !o.initrd_path.is_empty() {
            let initrd = std::fs::read(&o.initrd_path)
                .unwrap_or_else(|_| fail(format!("cannot read initrd '{}'", o.initrd_path)));
            initrd_start = (DRAM_BASE + dram_bytes)
                .checked_sub(64 * 1024 * 1024)
                .unwrap_or_else(|| fail("initrd does not fit in DRAM"));
            initrd_end = initrd_start + initrd.len() as u64;
            if !ram.load_image(initrd_start, &initrd) {
                fail("initrd does not fit in DRAM");
            }
            eprintln!("initrd: {} ({} bytes at {initrd_start:#x})", o.initrd_path, initrd.len());
        }

        let mut cfg = FdtConfig {
            dram_bytes,
            bootargs: o.bootargs.clone(),
            initrd_start,
            initrd_end,
            harts: smp,
            virtio_net: want_net,
            virtio_rng: o.want_rng,
            rtc: o.want_rtc,
            ..FdtConfig::default()
        };
        // 32 bytes for /chosen/rng-seed: from the host's entropy source, or from --rng-seed
        // when a reproducible boot is wanted (splitmix64 expands the seed).
        let mut s = o.rng_seed.unwrap_or_else(host_entropy);
        for _ in 0..32 {
            s = s.wrapping_add(0x9e37_79b9_7f4a_7c15);
            let mut z = s;
            z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
            z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
            cfg.rng_seed.push(((z ^ (z >> 31)) >> 24) as u8);
        }
        if !o.isa_override.is_empty() {
            cfg.isa = o.isa_override.clone();
        }
        let dtb = fdt::build(&cfg);

        if !o.dtb_out_path.is_empty() {
            if std::fs::write(&o.dtb_out_path, &dtb).is_err() {
                fail(format!("cannot write '{}'", o.dtb_out_path));
            }
            eprintln!("device tree written to {} ({} bytes)", o.dtb_out_path, dtb.len());
            if !o.linux_boot {
                return;
            }
        }

        // Two megabytes below the initramfs, or below the top of memory when there is none.
        let top = if initrd_start != 0 { initrd_start } else { DRAM_BASE + dram_bytes };
        dtb_addr = top.saturating_sub(2 * 1024 * 1024) & !0xfff;
        if !ram.load_image(dtb_addr, &dtb) {
            fail("device tree does not fit in DRAM");
        }
    }

    let mut platform = Platform::new(Rc::new(bus), smp);
    platform.quantum = o.quantum;
    platform.wire(clint.clone(), plic, uart.clone(), UART0_IRQ, syscon.clone());
    platform.htif_tohost_addr = tohost;

    // On bare metal every hart comes out of reset at the same address and sorts itself
    // out by reading mhartid, as on real hardware.
    for h in platform.harts.iter_mut() {
        h.trace = o.trace;
        h.pc = entry;
    }
    if o.linux_boot {
        // Enter the kernel the way OpenSBI would: in supervisor mode, with the hart ID in
        // a0 and the device tree in a1, and with SBI available for the things supervisor
        // mode cannot do for itself. Secondary harts are parked until the kernel starts
        // them.
        platform.enter_supervisor(entry, dtb_addr);
        eprint!("device tree at {dtb_addr:#x}, entering supervisor mode");
        if smp > 1 {
            eprint!(" ({smp} harts)");
        }
        eprint!("\n\n");
    }

    // The console becomes bidirectional here. Everything before this point could print;
    // from now on a guest shell can also be typed at.
    uart.borrow_mut().attach_host_stdin();
    platform.clock.set_instructions_per_tick(o.timer_divisor);

    let mut st: Result<(), riscv_emu::trap::Trap> = Ok(());
    let mut debugger_ended_session = false;

    if let Some(port) = o.gdb_port {
        // Nothing has executed yet. The machine is fully built and sitting at its first
        // instruction, which is the state a debugger wants to find it in.
        let carry_on = {
            let mut stub = GdbStub::new(&mut platform);
            let bound = stub.listen(port as u16);
            if bound == 0 {
                fail(format!("cannot listen for gdb on port {port}"));
            }
            eprintln!("gdb: waiting for a debugger on port {bound}  (gdb -ex 'target remote :{bound}')");
            if !stub.accept_client() {
                fail("no debugger connected");
            }
            eprintln!("gdb: debugger connected");
            stub.serve()
        };
        if carry_on {
            eprintln!("gdb: debugger detached; running on");
            st = platform.run(o.max_steps);
        } else {
            debugger_ended_session = !platform.halted();
        }
    } else {
        st = platform.run(o.max_steps);
    }
    let retired = platform.retired();

    if platform.htif_tohost_value != 0 {
        // The HTIF result convention used by riscv-tests: bit 0 marks the word as valid,
        // and the rest is the failing check's number (0 = all passed).
        let code = platform.htif_tohost_value >> 1;
        if code == 0 {
            eprintln!("\nPASS after {retired} instruction(s)");
        } else {
            eprintln!("\nFAIL: test {code} failed, after {retired} instruction(s)");
        }
    } else if platform.sbi_shutdown() {
        eprintln!("\nguest requested shutdown through SBI, after {retired} instruction(s)");
    } else if platform.user_quit() {
        eprintln!("\nleft the machine after {retired} instruction(s)");
    } else if platform.halted() {
        eprint!("\nmachine powered off after {retired} instruction(s)");
        let code = syscon.borrow().exit_code();
        if code != 0 {
            eprint!(" (exit code {code})");
        }
        eprintln!();
    } else if let Err(t) = &st {
        let faulting = &platform.harts[platform.fault_hart() as usize];
        eprint!(
            "\nstopped after {retired} instruction(s): {} (cause {}, tval {:#x}) at pc {:#x}",
            t.cause.name(),
            t.cause_code(),
            t.tval,
            faulting.pc
        );
        if smp > 1 {
            eprint!(" on hart {}", platform.fault_hart());
        }
        eprintln!();
    } else if debugger_ended_session {
        eprintln!("\nthe debugger ended the session after {retired} instruction(s)");
    } else if platform.stalled() {
        eprintln!("\nevery hart is idle and nothing can wake them (deadlock), after {retired} instruction(s)");
    } else {
        eprintln!("\nstep budget exhausted after {retired} instruction(s)");
    }

    if o.dump {
        let mut out = std::io::stdout();
        let n = platform.num_harts();
        for (i, h) in platform.harts.iter_mut().enumerate() {
            if n > 1 {
                let _ = writeln!(out, "--- hart {i} ---");
            }
            h.dump_registers(&mut out);
        }
        let _ = out.flush();
    }

    let code = if platform.htif_tohost_value != 0 {
        ((platform.htif_tohost_value >> 1) != 0) as i32
    } else if platform.user_quit() {
        0
    } else if platform.halted() {
        syscon.borrow().exit_code() as i32
    } else {
        st.is_err() as i32
    };
    // Drop the machine first so the console is restored before we exit: `exit` does not
    // run destructors, and a raw-mode terminal outlives the process.
    drop(platform);
    drop(uart);
    exit(code);
}
