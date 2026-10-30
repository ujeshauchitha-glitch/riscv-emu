//! The PLIC and the virtio block device.
//!
//! Both are harder to test than the UART or the CLINT, because neither is a simple register file.
//! The PLIC's behaviour is a protocol - claim, then complete - and the virtio device's behaviour is a
//! data structure it walks in guest memory. So these tests drive them the way a driver would: they
//! build a virtqueue by hand and check that the disk contents come back.

use std::cell::RefCell;
use std::rc::Rc;

use riscv_emu::bus::{Bus, Shared};
use riscv_emu::csr::{csr, CsrFile};
use riscv_emu::device::{AccessType, Device};
use riscv_emu::plic::Plic;
use riscv_emu::types::*;
use riscv_emu::virtio::blk::{VirtioBlk, SECTOR_SIZE};
use riscv_emu::virtio::VirtioMmio;

// PLIC register offsets, from the SiFive layout that QEMU's virt machine uses.
const PLIC_PRIORITY: u64 = 0x000000; // + irq * 4
const PLIC_PENDING: u64 = 0x001000;
const PLIC_ENABLE: u64 = 0x002000; // + context * 0x80
const PLIC_CONTEXT: u64 = 0x200000; // + context * 0x1000
const PLIC_THRESHOLD: u64 = 0x0; // within a context block
const PLIC_CLAIM: u64 = 0x4;

const CTX_M: u64 = 0;
const CTX_S: u64 = 1;

fn ctx_off(context: u64, reg: u64) -> u64 {
    PLIC_CONTEXT + context * 0x1000 + reg
}

/// Give a context an interest in one IRQ: non-zero priority, enabled, and a threshold low enough to
/// let it through.
fn enable_irq(plic: &mut Plic, context: u64, irq: u32, priority: u32) {
    plic.store(PLIC_PRIORITY + irq as u64 * 4, 4, priority as u64).unwrap();
    // Read-modify-write: the enable register is a bitmap of 32 sources, so storing a bare `1 << irq`
    // would disable everything else in the same word.
    let at = PLIC_ENABLE + context * 0x80 + (irq as u64 / 32) * 4;
    let cur = plic.load(at, 4).unwrap();
    plic.store(at, 4, cur | (1 << (irq % 32))).unwrap();
    plic.store(ctx_off(context, PLIC_THRESHOLD), 4, 0).unwrap();
}

fn read32(plic: &mut Plic, offset: u64) -> u32 {
    plic.load(offset, 4).unwrap() as u32
}

// --- PLIC -------------------------------------------------------------------

#[test]
fn plic_reports_nothing_when_idle() {
    let mut plic = Plic::new(1);
    let mut csrs = CsrFile::new();
    plic.update(&mut csrs, 0);
    assert_eq!(csrs.read(csr::MIP) & csr::MIP_SEIP, 0);
    // Claiming with nothing pending yields IRQ 0, the reserved "none" value.
    assert_eq!(read32(&mut plic, ctx_off(CTX_S, PLIC_CLAIM)), 0);
}

#[test]
fn plic_pending_irq_drives_seip() {
    let mut plic = Plic::new(1);
    let mut csrs = CsrFile::new();
    enable_irq(&mut plic, CTX_S, UART0_IRQ, 1);

    plic.set_pending(UART0_IRQ, true);
    plic.update(&mut csrs, 0);
    assert_ne!(csrs.read(csr::MIP) & csr::MIP_SEIP, 0);

    // Dropping the line drops the interrupt: the PLIC is level-triggered, so a source that goes quiet
    // before the handler runs simply disappears.
    plic.set_pending(UART0_IRQ, false);
    plic.update(&mut csrs, 0);
    assert_eq!(csrs.read(csr::MIP) & csr::MIP_SEIP, 0);
}

#[test]
fn plic_ignores_a_disabled_or_zero_priority_source() {
    let mut plic = Plic::new(1);
    let mut csrs = CsrFile::new();

    // Enabled, but priority 0 - which the spec defines as "never interrupt".
    plic.store(PLIC_ENABLE + CTX_S * 0x80, 4, 1 << UART0_IRQ).unwrap();
    plic.set_pending(UART0_IRQ, true);
    plic.update(&mut csrs, 0);
    assert_eq!(csrs.read(csr::MIP) & csr::MIP_SEIP, 0);

    // Priority set, but below the context's threshold. A threshold of N means "only tell me about
    // priorities strictly greater than N".
    plic.store(PLIC_PRIORITY + UART0_IRQ as u64 * 4, 4, 3).unwrap();
    plic.store(ctx_off(CTX_S, PLIC_THRESHOLD), 4, 3).unwrap();
    plic.update(&mut csrs, 0);
    assert_eq!(csrs.read(csr::MIP) & csr::MIP_SEIP, 0);

    plic.store(ctx_off(CTX_S, PLIC_THRESHOLD), 4, 2).unwrap();
    plic.update(&mut csrs, 0);
    assert_ne!(csrs.read(csr::MIP) & csr::MIP_SEIP, 0);
}

#[test]
fn plic_claim_and_complete() {
    let mut plic = Plic::new(1);
    let mut csrs = CsrFile::new();
    enable_irq(&mut plic, CTX_S, UART0_IRQ, 1);
    plic.set_pending(UART0_IRQ, true);

    assert_eq!(read32(&mut plic, PLIC_PENDING), 1 << UART0_IRQ);

    // Claim: the handler learns which source fired.
    assert_eq!(read32(&mut plic, ctx_off(CTX_S, PLIC_CLAIM)), UART0_IRQ);

    // While it is in service the same source must not be offered again, even though its line is
    // still asserted. This is the property that stops a level-triggered device from re-entering its
    // own handler forever.
    plic.update(&mut csrs, 0);
    assert_eq!(csrs.read(csr::MIP) & csr::MIP_SEIP, 0);
    assert_eq!(read32(&mut plic, ctx_off(CTX_S, PLIC_CLAIM)), 0);

    // Complete, with the line still up: it becomes claimable again.
    plic.store(ctx_off(CTX_S, PLIC_CLAIM), 4, UART0_IRQ as u64).unwrap();
    plic.update(&mut csrs, 0);
    assert_ne!(csrs.read(csr::MIP) & csr::MIP_SEIP, 0);
    assert_eq!(read32(&mut plic, ctx_off(CTX_S, PLIC_CLAIM)), UART0_IRQ);
}

#[test]
fn plic_picks_the_highest_priority_source() {
    let mut plic = Plic::new(1);
    enable_irq(&mut plic, CTX_S, UART0_IRQ, 1);
    enable_irq(&mut plic, CTX_S, VIRTIO0_IRQ, 7);

    plic.set_pending(UART0_IRQ, true);
    plic.set_pending(VIRTIO0_IRQ, true);

    // Higher priority wins regardless of IRQ number - virtio is IRQ 1 here and the UART is IRQ 10, so
    // a scan that returned the lowest number would give the same answer by accident. Flipping the
    // priorities catches that.
    assert_eq!(read32(&mut plic, ctx_off(CTX_S, PLIC_CLAIM)), VIRTIO0_IRQ);

    let mut other = Plic::new(1);
    enable_irq(&mut other, CTX_S, UART0_IRQ, 7);
    enable_irq(&mut other, CTX_S, VIRTIO0_IRQ, 1);
    other.set_pending(UART0_IRQ, true);
    other.set_pending(VIRTIO0_IRQ, true);
    assert_eq!(read32(&mut other, ctx_off(CTX_S, PLIC_CLAIM)), UART0_IRQ);
}

#[test]
fn plic_contexts_are_independent() {
    let mut plic = Plic::new(1);
    let mut csrs = CsrFile::new();
    // Only machine mode is interested in this source.
    enable_irq(&mut plic, CTX_M, VIRTIO0_IRQ, 1);
    plic.set_pending(VIRTIO0_IRQ, true);
    plic.update(&mut csrs, 0);

    assert_ne!(csrs.read(csr::MIP) & csr::MIP_MEIP, 0);
    assert_eq!(csrs.read(csr::MIP) & csr::MIP_SEIP, 0);
    assert_eq!(read32(&mut plic, ctx_off(CTX_S, PLIC_CLAIM)), 0);
    assert_eq!(read32(&mut plic, ctx_off(CTX_M, PLIC_CLAIM)), VIRTIO0_IRQ);
}

#[test]
fn plic_serves_each_hart_its_own_pair_of_contexts() {
    // Hart h owns contexts 2h (M-mode) and 2h+1 (S-mode).
    let mut plic = Plic::new(2);
    let mut csrs0 = CsrFile::new();
    let mut csrs1 = CsrFile::new();
    enable_irq(&mut plic, 3, UART0_IRQ, 1); // hart 1, supervisor
    plic.set_pending(UART0_IRQ, true);
    plic.update(&mut csrs0, 0);
    plic.update(&mut csrs1, 1);
    assert_eq!(csrs0.read(csr::MIP) & csr::MIP_SEIP, 0);
    assert_ne!(csrs1.read(csr::MIP) & csr::MIP_SEIP, 0);
}

// --- virtio-blk -------------------------------------------------------------

// Guest-memory addresses for a hand-built virtqueue. The layout is ours to choose; a real driver picks
// these too.
const DESC_ADDR: u64 = DRAM_BASE + 0x1000;
const AVAIL_ADDR: u64 = DRAM_BASE + 0x2000;
const USED_ADDR: u64 = DRAM_BASE + 0x3000;
const HEADER_ADDR: u64 = DRAM_BASE + 0x4000;
const DATA_ADDR: u64 = DRAM_BASE + 0x5000;
const STATUS_ADDR: u64 = DRAM_BASE + 0x6000;

const DESC_F_NEXT: u16 = 1;
const DESC_F_WRITE: u16 = 2;

/// A test rig: bus + DRAM + PLIC + a virtio device backed by a synthetic disk.
struct DiskRig {
    bus: Bus,
    plic: Rc<RefCell<Plic>>,
    blk: Rc<RefCell<VirtioMmio<VirtioBlk>>>,
}

impl DiskRig {
    fn new(sectors: u64) -> DiskRig {
        let mut bus = Bus::new(1024 * 1024);
        let (plic_dev, plic) = Shared::new(Plic::new(1));

        let mut dev = VirtioMmio::new(VIRTIO_BASE, bus.ram.clone(), VirtioBlk::new());
        dev.attach(plic.clone(), VIRTIO0_IRQ);
        // Fill the disk with a recognisable pattern: byte i of sector s is s + i, so a wrong sector or a
        // wrong offset both show up.
        let disk = dev.dev.data_mut();
        disk.resize((sectors * SECTOR_SIZE) as usize, 0);
        for s in 0..sectors {
            for i in 0..SECTOR_SIZE {
                disk[(s * SECTOR_SIZE + i) as usize] = (s + i) as u8;
            }
        }
        let (blk_dev, blk) = Shared::new(dev);
        assert!(bus.attach(Box::new(plic_dev)));
        assert!(bus.attach(Box::new(blk_dev)));
        DiskRig { bus, plic, blk }
    }

    fn poke(&self, addr: u64, size: usize, value: u64) {
        self.bus.store(addr, size, value).unwrap();
    }
    fn peek(&self, addr: u64, size: usize) -> u64 {
        self.bus.load(addr, size, AccessType::Load).unwrap()
    }
    fn reg_store(&self, offset: u64, value: u64) {
        self.blk.borrow_mut().store(offset, 4, value).unwrap();
    }
    fn reg_load(&self, offset: u64) -> u64 {
        self.blk.borrow_mut().load(offset, 4).unwrap()
    }
    fn disk(&self, offset: usize) -> u8 {
        self.blk.borrow().dev.data()[offset]
    }

    /// Write one 16-byte descriptor: {addr, len, flags, next}.
    fn descriptor(&self, index: u64, addr: u64, len: u32, flags: u16, next: u16) {
        let at = DESC_ADDR + index * 16;
        self.poke(at, 8, addr);
        self.poke(at + 8, 4, len as u64);
        self.poke(at + 12, 2, flags as u64);
        self.poke(at + 14, 2, next as u64);
    }

    /// Point the device at the rings and mark the queue ready, the way a driver's initialisation does.
    fn setup_queue(&self) {
        self.reg_store(0x030, 0); // QueueSel = 0
        self.reg_store(0x038, 8); // QueueNum
        self.reg_store(0x080, DESC_ADDR & 0xffff_ffff);
        self.reg_store(0x084, DESC_ADDR >> 32);
        self.reg_store(0x090, AVAIL_ADDR & 0xffff_ffff);
        self.reg_store(0x094, AVAIL_ADDR >> 32);
        self.reg_store(0x0a0, USED_ADDR & 0xffff_ffff);
        self.reg_store(0x0a4, USED_ADDR >> 32);
        self.reg_store(0x044, 1); // QueueReady
    }

    /// Build the standard three-descriptor block request and notify the device. `kind` is 0 for a read
    /// and 1 for a write.
    fn submit(&self, kind: u32, sector: u64, avail_idx: u64) {
        self.poke(HEADER_ADDR, 4, kind as u64);
        self.poke(HEADER_ADDR + 4, 4, 0);
        self.poke(HEADER_ADDR + 8, 8, sector);
        self.poke(STATUS_ADDR, 1, 0xff); // poison, so "0 = OK" means something

        self.descriptor(0, HEADER_ADDR, 16, DESC_F_NEXT, 1);
        self.descriptor(1, DATA_ADDR, SECTOR_SIZE as u32, DESC_F_NEXT | if kind == 0 { DESC_F_WRITE } else { 0 }, 2);
        self.descriptor(2, STATUS_ADDR, 1, DESC_F_WRITE, 0);

        self.poke(AVAIL_ADDR + 4 + (avail_idx % 8) * 2, 2, 0); // ring[i] = head 0
        self.poke(AVAIL_ADDR + 2, 2, avail_idx + 1); // avail.idx
        self.reg_store(0x050, 0); // QueueNotify
    }
}

#[test]
fn virtio_identifies_itself() {
    let rig = DiskRig::new(4);
    assert_eq!(rig.reg_load(0x000), 0x7472_6976); // "virt", little-endian
    assert_eq!(rig.reg_load(0x004), 2); // modern virtio-mmio, not legacy
    assert_eq!(rig.reg_load(0x008), 2); // 2 = block device
}

#[test]
fn virtio_reports_capacity() {
    let rig = DiskRig::new(4);
    assert_eq!(rig.blk.borrow().dev.sectors(), 4);
    // Config space offset 0 is the capacity in 512-byte sectors.
    assert_eq!(rig.reg_load(0x100), 4);
}

#[test]
fn virtio_read_delivers_sector_data() {
    let rig = DiskRig::new(4);
    rig.setup_queue();
    rig.submit(0, 2, 0);

    // The data descriptor now holds sector 2, whose byte i is 2 + i.
    for i in (0..SECTOR_SIZE).step_by(97) {
        assert_eq!(rig.peek(DATA_ADDR + i, 1), (2 + i) as u8 as u64);
    }
    // Status 0 means the request succeeded.
    assert_eq!(rig.peek(STATUS_ADDR, 1), 0);
    // And the device reported the completion in the used ring.
    assert_eq!(rig.peek(USED_ADDR + 2, 2), 1);
    assert_eq!(rig.peek(USED_ADDR + 4, 4), 0); // used.ring[0].id = chain head
}

#[test]
fn virtio_write_reaches_the_disk() {
    let rig = DiskRig::new(4);
    rig.setup_queue();

    for i in 0..SECTOR_SIZE {
        rig.poke(DATA_ADDR + i, 1, (0xa0 + i) as u8 as u64);
    }
    rig.submit(1, 3, 0);

    assert_eq!(rig.peek(STATUS_ADDR, 1), 0);
    for i in (0..SECTOR_SIZE).step_by(61) {
        assert_eq!(rig.disk((3 * SECTOR_SIZE + i) as usize), (0xa0 + i) as u8);
    }
    // Sector 2 is untouched: a write must not spill into its neighbour.
    assert_eq!(rig.disk((2 * SECTOR_SIZE) as usize), 2);
}

#[test]
fn virtio_raises_an_interrupt_through_the_plic() {
    let rig = DiskRig::new(4);
    let mut csrs = CsrFile::new();
    enable_irq(&mut rig.plic.borrow_mut(), CTX_S, VIRTIO0_IRQ, 1);

    rig.setup_queue();
    rig.submit(0, 0, 0);

    rig.plic.borrow().update(&mut csrs, 0);
    assert_ne!(csrs.read(csr::MIP) & csr::MIP_SEIP, 0);
    assert_eq!(read32(&mut rig.plic.borrow_mut(), ctx_off(CTX_S, PLIC_CLAIM)), VIRTIO0_IRQ);

    // InterruptStatus bit 0 = "the used ring advanced". Acknowledging it drops the device's line.
    assert_ne!(rig.reg_load(0x060) & 1, 0);
    rig.reg_store(0x064, 1);
    assert_eq!(rig.reg_load(0x060) & 1, 0);
}

#[test]
fn virtio_handles_several_requests_in_sequence() {
    let rig = DiskRig::new(4);
    rig.setup_queue();

    // Two requests without the device being reset in between. The second must be noticed, which only
    // works if last_avail advanced past the first.
    rig.submit(0, 1, 0);
    assert_eq!(rig.peek(DATA_ADDR, 1), 1);
    rig.submit(0, 3, 1);
    assert_eq!(rig.peek(DATA_ADDR, 1), 3);
    assert_eq!(rig.peek(USED_ADDR + 2, 2), 2);
}

#[test]
fn virtio_rejects_a_request_past_the_end_of_the_disk() {
    let rig = DiskRig::new(4);
    rig.setup_queue();
    rig.submit(0, 99, 0);
    // Non-zero status is the device saying "I could not do that", which is what a driver checks. It
    // must not fault the machine or read out of bounds.
    assert_ne!(rig.peek(STATUS_ADDR, 1), 0);
}

// --- regressions ------------------------------------------------------------

#[test]
fn plic_registers_beyond_the_bitmaps_read_zero() {
    // The pending and enable bitmaps are 64 bits, so only words 0 and 1 exist. A read of word 2 that
    // shifted a u64 by 64 would alias word 0 on x86 (the shift count is taken modulo 64) and report
    // interrupts that are not pending.
    let mut plic = Plic::new(1);
    plic.store(PLIC_ENABLE, 4, 0xdead_beef).unwrap();
    plic.set_pending(UART0_IRQ, true);

    assert_eq!(read32(&mut plic, PLIC_ENABLE), 0xdead_beef);
    assert_eq!(read32(&mut plic, PLIC_ENABLE + 8), 0); // word 2: nothing there
    assert_eq!(read32(&mut plic, PLIC_PENDING + 8), 0);
}

#[test]
fn virtio_offers_version_1() {
    // A version-2 MMIO device that does not offer VIRTIO_F_VERSION_1 is a contradiction, and Linux
    // refuses it: "device uses modern interface but does not have VIRTIO_F_VERSION_1", and the device
    // is never probed.
    //
    // The bit is number 32, in the upper half of a 64-bit feature space read through 32-bit registers
    // - so it is only reachable if the device honours the select register.
    let rig = DiskRig::new(4);

    rig.reg_store(0x014, 0); // DeviceFeaturesSel = 0
    // The optional features the block device offers: SEG_MAX (bit 2), BLK_SIZE (bit 6) and FLUSH
    // (bit 9). Nothing else - in particular nothing that would make the driver expect a layout this
    // device does not implement.
    assert_eq!(rig.reg_load(0x010), (1 << 2) | (1 << 6) | (1 << 9));

    rig.reg_store(0x014, 1); // DeviceFeaturesSel = 1
    assert_eq!(rig.reg_load(0x010), 1); // bit 32 = VIRTIO_F_VERSION_1
}

#[test]
fn virtio_rejects_a_request_that_would_overflow_the_bounds_check() {
    // The obvious bounds check, `offset + length > size`, overflows: a sector number near 2^55 makes
    // the sum wrap to zero and the check passes, after which the device indexes gigabytes outside its
    // own buffer. Every field here comes from guest-written descriptors, so all of it is hostile input.
    let rig = DiskRig::new(4);
    rig.setup_queue();
    rig.submit(0, 0x7fff_ffff_ffc0_00, 0);

    // Refused with a non-zero status, and - the part that matters - still here to say so.
    assert_ne!(rig.peek(STATUS_ADDR, 1), 0);
}
