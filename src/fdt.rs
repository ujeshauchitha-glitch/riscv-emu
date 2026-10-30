//! A flattened device tree (FDT) generator.
//!
//! A RISC-V kernel is entered with a0 = hart id and a1 = the address of a device tree:
//! a binary blob describing the machine - how much RAM, which CPUs, where each device
//! lives and which interrupt it raises. Everything Linux knows about the hardware
//! comes from it. It is generated here from the same constants the devices were
//! attached with, so the description and the machine cannot drift apart.
//!
//! The format (devicetree specification, chapter 5): a 40-byte header, a memory
//! reservation block, a *structure* block of tokens (begin node, property, end node)
//! and a *strings* block of property names. Everything is big-endian, including on a
//! little-endian machine - writing these by hand without a helper is the most common
//! way to produce a blob a kernel silently refuses.

use crate::types::*;

const FDT_MAGIC: u32 = 0xd00d_feed;
const FDT_VERSION: u32 = 17;
const FDT_COMPAT_VER: u32 = 16;

// Structure-block tokens.
const FDT_BEGIN_NODE: u32 = 1;
const FDT_END_NODE: u32 = 2;
const FDT_PROP: u32 = 3;
const FDT_END: u32 = 9;

// Interrupt controller phandles. A phandle is just a unique number a node carries so
// other nodes can refer to it - the device tree's equivalent of a pointer.
const PHANDLE_PLIC: u32 = 1;
const PHANDLE_SYSCON: u32 = 3;
/// Each hart's local interrupt controller needs its own phandle. They start well clear
/// of the small fixed ones above.
const PHANDLE_CPU_INTC_BASE: u32 = 0x100;

pub const DEFAULT_ISA: &str = "rv64imafdc_zicsr_zifencei_zicond_zihintpause_zba_zbb_zbc_zbs";

#[derive(Clone, Debug)]
pub struct FdtConfig {
    pub dram_bytes: u64,
    pub bootargs: String,
    /// The initramfs, if any, as a physical address range.
    pub initrd_start: u64,
    pub initrd_end: u64,
    pub harts: u32,
    /// Bytes for /chosen/rng-seed, which the kernel uses to seed its generator before
    /// any entropy device has probed.
    pub rng_seed: Vec<u8>,
    // Optional devices, described only when present - a node for hardware that is not
    // there would make the kernel probe it.
    pub virtio_net: bool,
    pub virtio_rng: bool,
    pub rtc: bool,
    /// The riscv,isa string. Shorten it to hide an extension from the kernel.
    pub isa: String,
}

impl Default for FdtConfig {
    fn default() -> Self {
        FdtConfig {
            dram_bytes: DRAM_SIZE_DEFAULT,
            bootargs: String::new(),
            initrd_start: 0,
            initrd_end: 0,
            harts: 1,
            rng_seed: Vec::new(),
            virtio_net: false,
            virtio_rng: false,
            rtc: false,
            isa: DEFAULT_ISA.to_string(),
        }
    }
}

fn push_be32(out: &mut Vec<u8>, v: u32) {
    out.extend_from_slice(&v.to_be_bytes());
}

fn push_be64(out: &mut Vec<u8>, v: u64) {
    out.extend_from_slice(&v.to_be_bytes());
}

fn pad_to_4(out: &mut Vec<u8>) {
    while out.len() % 4 != 0 {
        out.push(0);
    }
}

#[derive(Default)]
struct Builder {
    structure: Vec<u8>,
    strings: Vec<u8>,
}

impl Builder {
    /// Offset of `name` in the strings block, adding it if new. A linear scan is fine:
    /// a tree this size has a few dozen distinct property names.
    fn intern(&mut self, name: &str) -> u32 {
        let mut needle = name.as_bytes().to_vec();
        needle.push(0);
        let hay = &self.strings;
        if hay.len() >= needle.len() {
            for i in 0..=hay.len() - needle.len() {
                if hay[i..i + needle.len()] == needle[..] {
                    return i as u32;
                }
            }
        }
        let offset = self.strings.len() as u32;
        self.strings.extend_from_slice(&needle);
        offset
    }

    fn begin_node(&mut self, name: &str) {
        push_be32(&mut self.structure, FDT_BEGIN_NODE);
        self.structure.extend_from_slice(name.as_bytes());
        self.structure.push(0);
        pad_to_4(&mut self.structure);
    }

    fn end_node(&mut self) {
        push_be32(&mut self.structure, FDT_END_NODE);
    }

    fn prop_header(&mut self, name: &str, len: usize) {
        let off = self.intern(name);
        push_be32(&mut self.structure, FDT_PROP);
        push_be32(&mut self.structure, len as u32);
        push_be32(&mut self.structure, off);
    }

    /// A property with no value. Its presence is the information - "ranges" on a bus
    /// node means "addresses pass through unchanged".
    fn prop_empty(&mut self, name: &str) {
        self.prop_header(name, 0);
    }

    fn prop_u32(&mut self, name: &str, v: u32) {
        self.prop_header(name, 4);
        push_be32(&mut self.structure, v);
    }

    fn prop_u64(&mut self, name: &str, v: u64) {
        self.prop_header(name, 8);
        push_be64(&mut self.structure, v);
    }

    fn prop_string(&mut self, name: &str, v: &str) {
        self.prop_header(name, v.len() + 1);
        self.structure.extend_from_slice(v.as_bytes());
        self.structure.push(0);
        pad_to_4(&mut self.structure);
    }

    /// Several NUL-terminated strings back to back. "compatible" uses this to name a
    /// device from most specific to least, so a kernel can fall back to a generic driver
    /// when it has no exact match.
    fn prop_stringlist(&mut self, name: &str, values: &[&str]) {
        let len: usize = values.iter().map(|v| v.len() + 1).sum();
        self.prop_header(name, len);
        for v in values {
            self.structure.extend_from_slice(v.as_bytes());
            self.structure.push(0);
        }
        pad_to_4(&mut self.structure);
    }

    fn prop_bytes(&mut self, name: &str, bytes: &[u8]) {
        self.prop_header(name, bytes.len());
        self.structure.extend_from_slice(bytes);
        pad_to_4(&mut self.structure);
    }

    fn prop_cells(&mut self, name: &str, cells: &[u32]) {
        self.prop_header(name, cells.len() * 4);
        for &c in cells {
            push_be32(&mut self.structure, c);
        }
    }

    /// With #address-cells = 2 and #size-cells = 2, a reg entry is four 32-bit cells:
    /// the address in two, the size in two. That is what lets a 64-bit machine describe
    /// memory above 4 GiB in a format designed for 32-bit ones.
    fn prop_reg(&mut self, name: &str, address: u64, size: u64) {
        self.prop_header(name, 16);
        push_be64(&mut self.structure, address);
        push_be64(&mut self.structure, size);
    }

    fn finish(mut self) -> Vec<u8> {
        push_be32(&mut self.structure, FDT_END);

        // The reservation block always ends with an all-zero entry, even when there is
        // nothing reserved. A parser reads entries until it sees that.
        let mut reservations = Vec::new();
        push_be64(&mut reservations, 0);
        push_be64(&mut reservations, 0);

        const HEADER_SIZE: u32 = 40;
        let off_reservations = HEADER_SIZE;
        let off_structure = off_reservations + reservations.len() as u32;
        let off_strings = off_structure + self.structure.len() as u32;
        let total = off_strings + self.strings.len() as u32;

        let mut out = Vec::with_capacity(total as usize);
        push_be32(&mut out, FDT_MAGIC);
        push_be32(&mut out, total);
        push_be32(&mut out, off_structure);
        push_be32(&mut out, off_strings);
        push_be32(&mut out, off_reservations);
        push_be32(&mut out, FDT_VERSION);
        push_be32(&mut out, FDT_COMPAT_VER);
        push_be32(&mut out, 0); // boot cpuid
        push_be32(&mut out, self.strings.len() as u32);
        push_be32(&mut out, self.structure.len() as u32);

        out.extend_from_slice(&reservations);
        out.extend_from_slice(&self.structure);
        out.extend_from_slice(&self.strings);
        out
    }
}

/// Build the device tree blob for a machine.
pub fn build(config: &FdtConfig) -> Vec<u8> {
    let harts = config.harts.max(1);
    let mut t = Builder::default();

    t.begin_node("");
    // Two cells for an address and two for a size: 64-bit values, in a format whose
    // cells are 32 bits wide. Every `reg` in the tree is read according to these, so
    // getting them wrong misplaces every device at once.
    t.prop_u32("#address-cells", 2);
    t.prop_u32("#size-cells", 2);
    t.prop_string("compatible", "riscv-virtio");
    t.prop_string("model", "riscv-emu,virt");

    // --- /chosen: what the bootloader wants to tell the kernel -------------------
    t.begin_node("chosen");
    t.prop_string("bootargs", &config.bootargs);
    // Where to print. Without this Linux boots silently - it has a UART driver and a
    // node describing the UART, but no reason to believe that particular UART is the
    // console.
    t.prop_string("stdout-path", "/soc/serial@10000000");
    if !config.rng_seed.is_empty() {
        t.prop_bytes("rng-seed", &config.rng_seed);
    }
    if config.initrd_end > config.initrd_start {
        // 64-bit, not 32. With more than 4 GiB of guest RAM the initramfs is placed
        // above the 4 GiB line, and a 32-bit property truncates that to an address with
        // no memory behind it - Linux then reserves the wrong region and either fails to
        // unpack the initramfs or corrupts whatever is there.
        t.prop_u64("linux,initrd-start", config.initrd_start);
        t.prop_u64("linux,initrd-end", config.initrd_end);
    }
    t.end_node();

    // --- /memory ------------------------------------------------------------------
    t.begin_node("memory@80000000");
    t.prop_string("device_type", "memory");
    t.prop_reg("reg", DRAM_BASE, config.dram_bytes);
    t.end_node();

    // --- /cpus --------------------------------------------------------------------
    t.begin_node("cpus");
    t.prop_u32("#address-cells", 1);
    t.prop_u32("#size-cells", 0);
    // The timer frequency: what the kernel divides mtime ticks by to get seconds. The
    // guest's idea of wall-clock time is only as good as this estimate.
    t.prop_u32("timebase-frequency", 10_000_000);
    // One node per hart. Linux brings the others up through SBI's hart_start, naming
    // each by the `reg` value here.
    for h in 0..harts {
        t.begin_node(&format!("cpu@{h}"));
        t.prop_string("device_type", "cpu");
        t.prop_u32("reg", h);
        t.prop_string("status", "okay");
        t.prop_string("compatible", "riscv");
        t.prop_string("riscv,isa", &config.isa);
        t.prop_string("mmu-type", "riscv,sv39");
        t.begin_node("interrupt-controller");
        t.prop_u32("#interrupt-cells", 1);
        t.prop_empty("interrupt-controller");
        t.prop_string("compatible", "riscv,cpu-intc");
        t.prop_u32("phandle", PHANDLE_CPU_INTC_BASE + h);
        t.end_node();
        t.end_node();
    }
    t.end_node();

    // --- /soc: the memory-mapped devices ------------------------------------------
    t.begin_node("soc");
    t.prop_u32("#address-cells", 2);
    t.prop_u32("#size-cells", 2);
    t.prop_stringlist("compatible", &["simple-bus"]);
    // "ranges" with no value means addresses inside this bus are the same as outside it.
    t.prop_empty("ranges");

    // The console.
    t.begin_node("serial@10000000");
    t.prop_stringlist("compatible", &["ns16550a"]);
    t.prop_reg("reg", UART0_BASE, 0x100);
    t.prop_u32("interrupt-parent", PHANDLE_PLIC);
    t.prop_u32("interrupts", UART0_IRQ);
    t.prop_u32("clock-frequency", 3_686_400);
    t.end_node();

    // The disk.
    t.begin_node("virtio_mmio@10001000");
    t.prop_stringlist("compatible", &["virtio,mmio"]);
    t.prop_reg("reg", VIRTIO_BASE, 0x1000);
    t.prop_u32("interrupt-parent", PHANDLE_PLIC);
    t.prop_u32("interrupts", VIRTIO0_IRQ);
    t.end_node();

    // Optional virtio devices, one 4 KiB slot each.
    if config.virtio_net {
        t.begin_node("virtio_mmio@10002000");
        t.prop_stringlist("compatible", &["virtio,mmio"]);
        t.prop_reg("reg", VIRTIO_NET_BASE, 0x1000);
        t.prop_u32("interrupt-parent", PHANDLE_PLIC);
        t.prop_u32("interrupts", VIRTIO_NET_IRQ);
        t.end_node();
    }
    if config.virtio_rng {
        t.begin_node("virtio_mmio@10003000");
        t.prop_stringlist("compatible", &["virtio,mmio"]);
        t.prop_reg("reg", VIRTIO_RNG_BASE, 0x1000);
        t.prop_u32("interrupt-parent", PHANDLE_PLIC);
        t.prop_u32("interrupts", VIRTIO_RNG_IRQ);
        t.end_node();
    }
    if config.rtc {
        // Wall-clock time. Without it a guest boots into 1970, and anything that checks
        // certificate or repository validity (apt, TLS) refuses to work.
        t.begin_node("rtc@101000");
        t.prop_stringlist("compatible", &["google,goldfish-rtc"]);
        t.prop_reg("reg", RTC_BASE, 0x1000);
        t.prop_u32("interrupt-parent", PHANDLE_PLIC);
        t.prop_u32("interrupts", RTC_IRQ);
        t.end_node();
    }

    // The timer and software-interrupt block. Its `interrupts-extended` names the
    // per-hart controller twice, once for the software interrupt (3) and once for the
    // timer (7) - the two things a hart raises for itself.
    t.begin_node("clint@2000000");
    t.prop_stringlist("compatible", &["sifive,clint0", "riscv,clint0"]);
    t.prop_reg("reg", CLINT_BASE, 0x10000);
    let mut cells = Vec::new();
    for h in 0..harts {
        cells.extend_from_slice(&[PHANDLE_CPU_INTC_BASE + h, 3]); // software
        cells.extend_from_slice(&[PHANDLE_CPU_INTC_BASE + h, 7]); // timer
    }
    t.prop_cells("interrupts-extended", &cells);
    t.end_node();

    // The external interrupt controller.
    t.begin_node("plic@c000000");
    t.prop_u32("#address-cells", 0);
    t.prop_u32("#interrupt-cells", 1);
    t.prop_stringlist("compatible", &["sifive,plic-1.0.0", "riscv,plic0"]);
    t.prop_empty("interrupt-controller");
    t.prop_reg("reg", PLIC_BASE, 0x400000);
    // Two contexts per hart, in order: machine external (cause 11), then supervisor
    // external (cause 9). The position in this list *is* the context number, which is
    // why it matches the PLIC's 2h / 2h+1 layout.
    let mut cells = Vec::new();
    for h in 0..harts {
        cells.extend_from_slice(&[PHANDLE_CPU_INTC_BASE + h, 11]);
        cells.extend_from_slice(&[PHANDLE_CPU_INTC_BASE + h, 9]);
    }
    t.prop_cells("interrupts-extended", &cells);
    t.prop_u32("riscv,ndev", 31);
    t.prop_u32("phandle", PHANDLE_PLIC);
    t.end_node();

    t.end_node(); // /soc

    // --- /poweroff and /reboot ----------------------------------------------------
    // How the kernel turns the machine off. Without these, `poweroff` prints "System
    // halted" and spins forever, which looks like a hang.
    t.begin_node("poweroff");
    t.prop_u32("value", 0x5555);
    t.prop_u32("offset", 0);
    t.prop_u32("regmap", PHANDLE_SYSCON);
    t.prop_string("compatible", "syscon-poweroff");
    t.end_node();

    t.begin_node("reboot");
    t.prop_u32("value", 0x7777);
    t.prop_u32("offset", 0);
    t.prop_u32("regmap", PHANDLE_SYSCON);
    t.prop_string("compatible", "syscon-reboot");
    t.end_node();

    t.begin_node("syscon@100000");
    t.prop_stringlist("compatible", &["syscon"]);
    t.prop_reg("reg", SYSCON_BASE, 0x1000);
    t.prop_u32("phandle", PHANDLE_SYSCON);
    t.end_node();

    t.end_node(); // /
    t.finish()
}

/// A device tree with only the essentials (RAM size, command line, initrd range).
pub fn build_simple(dram_bytes: u64, bootargs: &str, initrd_start: u64, initrd_end: u64) -> Vec<u8> {
    build(&FdtConfig {
        dram_bytes,
        bootargs: bootargs.to_string(),
        initrd_start,
        initrd_end,
        ..FdtConfig::default()
    })
}
