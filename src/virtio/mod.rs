//! The virtio-mmio transport: the register file and virtqueues every virtio device
//! shares.
//!
//! virtio is an interface designed for virtual machines, and it is far simpler than
//! emulating real hardware: instead of a disk controller's registers, the guest and
//! the host share *queues in guest memory*. The guest describes a request as a chain
//! of buffers, publishes it on a ring, and pokes a doorbell register; the host works
//! through the chain and publishes the result on a second ring.
//!
//! This module is the half that is the same for every device - the MMIO registers,
//! feature negotiation, and the descriptor-chain walking. A concrete device (block,
//! network, entropy) implements `VirtioDevice` and gets a `Core` to move data with.
//!
//! The device is a bus master: it reads descriptors and writes buffers in guest
//! memory by itself. Every field it reads is therefore hostile input - a driver can
//! write anything into a descriptor - so lengths are never trusted, chains are
//! bounded, and every guest access is checked rather than assumed.

pub mod blk;
pub mod net;
pub mod rng;

use std::cell::RefCell;
use std::rc::Rc;

use crate::device::Device;
use crate::dram::Dram;
use crate::plic::Plic;
use crate::trap::{Exception, Res, Trap};

pub const QUEUE_MAX: u32 = 256;

// MMIO register offsets, from the virtio specification.
const MAGIC_VALUE: u64 = 0x000;
const VERSION: u64 = 0x004;
const DEVICE_ID: u64 = 0x008;
const VENDOR_ID: u64 = 0x00c;
const DEVICE_FEATURES: u64 = 0x010;
const DEVICE_FEATURES_SEL: u64 = 0x014;
const DRIVER_FEATURES: u64 = 0x020;
const DRIVER_FEATURES_SEL: u64 = 0x024;
const QUEUE_SEL: u64 = 0x030;
const QUEUE_NUM_MAX: u64 = 0x034;
const QUEUE_NUM: u64 = 0x038;
const QUEUE_READY: u64 = 0x044;
const QUEUE_NOTIFY: u64 = 0x050;
const INTERRUPT_STATUS: u64 = 0x060;
const INTERRUPT_ACK: u64 = 0x064;
const STATUS: u64 = 0x070;
const QUEUE_DESC_LOW: u64 = 0x080;
const QUEUE_DESC_HIGH: u64 = 0x084;
const DRIVER_DESC_LOW: u64 = 0x090;
const DRIVER_DESC_HIGH: u64 = 0x094;
const DEVICE_DESC_LOW: u64 = 0x0a0;
const DEVICE_DESC_HIGH: u64 = 0x0a4;
const CONFIG_GENERATION: u64 = 0x0fc;
const CONFIG: u64 = 0x100;

/// "virt" in little-endian ASCII - the magic a driver checks first.
const MAGIC: u64 = 0x7472_6976;
const VENDOR: u64 = 0x554d_4551; // "QEMU"

// Descriptor flags.
const DESC_NEXT: u16 = 1; // chained to another descriptor
const DESC_WRITE: u16 = 2; // the device writes this buffer

const AVAIL_F_NO_INTERRUPT: u16 = 1;
const FEATURES_OK: u32 = 8;

/// VIRTIO_F_VERSION_1, feature bit 32: "this device speaks the modern protocol rather
/// than the legacy one". A version-2 MMIO device that does not offer it is a
/// contradiction, and Linux says so - it refuses with "device uses modern interface
/// but does not have VIRTIO_F_VERSION_1" and never probes the device at all.
///
/// The feature space is 64 bits wide but the registers are 32, so a driver reads it
/// in two halves: it writes 0 or 1 to DeviceFeaturesSel and then reads DeviceFeatures.
/// Bit 32 lives in the upper half. (xv6 never checks this, which is why its boot
/// worked without it.)
const F_VERSION_1: u64 = 1 << 32;

/// A descriptor chain longer than the queue is a loop, or a hostile driver.
const MAX_CHAIN: u32 = QUEUE_MAX;

/// One buffer in a descriptor chain.
#[derive(Clone, Copy, Debug)]
pub struct Segment {
    pub addr: u64,
    pub len: u32,
    /// True if the *device* writes this buffer (it is a result), false if the driver
    /// filled it (it is a request).
    pub device_writes: bool,
}

#[derive(Clone, Copy, Default)]
struct Queue {
    num: u32,
    ready: bool,
    desc: u64,  // descriptor table
    avail: u64, // driver ring
    used: u64,  // device ring
    /// The last avail.idx we consumed.
    last_avail: u16,
}

/// What a concrete virtio device provides.
pub trait VirtioDevice {
    fn name(&self) -> &'static str;
    /// virtio device type: 1 net, 2 block, 4 entropy.
    fn device_id(&self) -> u32;
    fn num_queues(&self) -> u32;
    /// Feature bits offered, not counting VIRTIO_F_VERSION_1 (the transport adds it).
    fn features(&self) -> u64 {
        0
    }
    /// A read of the device-specific configuration space.
    fn config_read(&self, _offset: u64, _size: usize) -> u64 {
        0
    }
    fn config_write(&mut self, _offset: u64, _size: usize, _value: u64) {}
    /// The driver rang the doorbell for `queue`.
    fn notify(&mut self, core: &mut Core, queue: u32);
    fn reset(&mut self) {}
    /// Once-per-round poll, for devices fed by the outside world.
    fn poll(&mut self, _core: &mut Core) {}
    fn io_pending(&self) -> bool {
        false
    }
}

/// The transport state, and the means to read and write guest memory and queues.
pub struct Core {
    base: u64,
    ram: Rc<Dram>,
    plic: Option<Rc<RefCell<Plic>>>,
    irq: u32,
    device_id: u32,
    queues: Vec<Queue>,
    status: u32,
    driver_features: u64,
    device_features_sel: u32,
    driver_features_sel: u32,
    queue_sel: u32,
    interrupt_status: u32,
}

impl Core {
    fn new(base: u64, ram: Rc<Dram>, device_id: u32, num_queues: u32) -> Core {
        Core {
            base,
            ram,
            plic: None,
            irq: 0,
            device_id,
            queues: vec![Queue::default(); num_queues as usize],
            status: 0,
            driver_features: 0,
            device_features_sel: 0,
            driver_features_sel: 0,
            queue_sel: 0,
            interrupt_status: 0,
        }
    }

    pub fn interrupt_status(&self) -> u32 {
        self.interrupt_status
    }

    pub fn queue_ready(&self, queue: u32) -> bool {
        self.queues.get(queue as usize).map_or(false, |q| q.ready && q.num != 0)
    }

    // --- guest memory ---------------------------------------------------------

    pub fn read_guest_bytes(&self, addr: u64, dst: &mut [u8]) -> bool {
        self.ram.read_bytes(addr, dst)
    }

    pub fn write_guest_bytes(&self, addr: u64, src: &[u8]) -> bool {
        self.ram.write_bytes(addr, src)
    }

    /// A little-endian integer of 1, 2, 4 or 8 bytes, or 0 if the address is not RAM.
    fn read_guest(&self, addr: u64, size: usize) -> u64 {
        let mut b = [0u8; 8];
        if self.ram.read_bytes(addr, &mut b[..size]) {
            u64::from_le_bytes(b)
        } else {
            0
        }
    }

    fn write_guest(&self, addr: u64, size: usize, value: u64) {
        let b = value.to_le_bytes();
        self.ram.write_bytes(addr, &b[..size]);
    }

    pub fn segments_length(segs: &[Segment], device_writes: bool) -> u64 {
        segs.iter().filter(|s| s.device_writes == device_writes).map(|s| s.len as u64).sum()
    }

    /// Copy `dst.len()` bytes out of the driver-filled buffers, starting `skip` bytes
    /// in. Returns how many were copied.
    pub fn read_from_segments(&self, segs: &[Segment], mut skip: u64, dst: &mut [u8]) -> u64 {
        let len = dst.len() as u64;
        let mut done = 0u64;
        for s in segs {
            if s.device_writes {
                continue; // only buffers the driver filled
            }
            if skip >= s.len as u64 {
                skip -= s.len as u64;
                continue;
            }
            let n = (len - done).min(s.len as u64 - skip);
            if !self.read_guest_bytes(s.addr.wrapping_add(skip), &mut dst[done as usize..(done + n) as usize]) {
                break;
            }
            done += n;
            skip = 0;
            if done == len {
                break;
            }
        }
        done
    }

    /// Copy `src` into the device-writable buffers, starting `skip` bytes in.
    pub fn write_to_segments(&self, segs: &[Segment], mut skip: u64, src: &[u8]) -> u64 {
        let len = src.len() as u64;
        let mut done = 0u64;
        for s in segs {
            if !s.device_writes {
                continue; // only buffers the driver left for us
            }
            if skip >= s.len as u64 {
                skip -= s.len as u64;
                continue;
            }
            let n = (len - done).min(s.len as u64 - skip);
            if !self.write_guest_bytes(s.addr.wrapping_add(skip), &src[done as usize..(done + n) as usize]) {
                break;
            }
            done += n;
            skip = 0;
            if done == len {
                break;
            }
        }
        done
    }

    // --- virtqueues -----------------------------------------------------------

    /// Take the next available descriptor chain off the queue: its head index and its
    /// buffers. Returns None if the queue is not ready or has nothing waiting.
    pub fn pop_chain(&mut self, queue: u32) -> Option<(u16, Vec<Segment>)> {
        if !self.queue_ready(queue) {
            return None;
        }
        let q = self.queues[queue as usize];

        // The available ring: {u16 flags; u16 idx; u16 ring[];}
        let avail_idx = self.read_guest(q.avail.wrapping_add(2), 2) as u16;
        if q.last_avail == avail_idx {
            return None;
        }
        let slot = q.avail.wrapping_add(4).wrapping_add((q.last_avail as u64 % q.num as u64) * 2);
        let head = self.read_guest(slot, 2) as u16;
        self.queues[queue as usize].last_avail = q.last_avail.wrapping_add(1);

        // Descriptor layout: {u64 addr; u32 len; u16 flags; u16 next;} = 16 bytes.
        let mut segments = Vec::new();
        let mut index = head;
        for _ in 0..MAX_CHAIN {
            if index as u32 >= q.num {
                break; // an index outside the table
            }
            let d = q.desc.wrapping_add(index as u64 * 16);
            let flags = self.read_guest(d.wrapping_add(12), 2) as u16;
            segments.push(Segment {
                addr: self.read_guest(d, 8),
                len: self.read_guest(d.wrapping_add(8), 4) as u32,
                device_writes: flags & DESC_WRITE != 0,
            });
            if flags & DESC_NEXT == 0 {
                break;
            }
            index = self.read_guest(d.wrapping_add(14), 2) as u16;
        }
        Some((head, segments))
    }

    /// Publish a finished chain in the used ring.
    pub fn push_used(&mut self, queue: u32, head: u16, written: u32) {
        if !self.queue_ready(queue) {
            return;
        }
        let q = self.queues[queue as usize];
        // The used ring: {u16 flags; u16 idx; struct {u32 id; u32 len;} ring[];}
        let used_idx = self.read_guest(q.used.wrapping_add(2), 2) as u16;
        let slot = q.used.wrapping_add(4).wrapping_add((used_idx as u64 % q.num as u64) * 8);
        self.write_guest(slot, 4, head as u64); // which chain finished
        self.write_guest(slot.wrapping_add(4), 4, written as u64); // how many bytes were transferred
        // The index is bumped only after the entry is fully written, so the driver can
        // never observe a half-published completion.
        self.write_guest(q.used.wrapping_add(2), 2, used_idx.wrapping_add(1) as u64);
    }

    /// Interrupt the driver about used buffers, unless it asked not to be.
    pub fn signal_used(&mut self, queue: u32) {
        if !self.queue_ready(queue) {
            return;
        }
        let avail_flags = self.read_guest(self.queues[queue as usize].avail, 2) as u16;
        if avail_flags & AVAIL_F_NO_INTERRUPT != 0 {
            return;
        }
        self.raise_interrupt();
    }

    fn raise_interrupt(&mut self) {
        // Bit 0 means "the device used a buffer" - the driver reads this to learn why
        // it was interrupted, then acknowledges it.
        self.interrupt_status |= 1;
        if let Some(p) = &self.plic {
            p.borrow_mut().set_pending(self.irq, true);
        }
    }

    fn reset(&mut self) {
        self.status = 0;
        self.driver_features = 0;
        self.interrupt_status = 0;
        for q in self.queues.iter_mut() {
            *q = Queue::default();
        }
        if let Some(p) = &self.plic {
            p.borrow_mut().set_pending(self.irq, false);
        }
    }
}

/// A virtio device on an MMIO slot: the transport plus the device behind it.
pub struct VirtioMmio<D: VirtioDevice> {
    pub core: Core,
    pub dev: D,
}

impl<D: VirtioDevice> VirtioMmio<D> {
    pub fn new(base: u64, ram: Rc<Dram>, dev: D) -> VirtioMmio<D> {
        let core = Core::new(base, ram, dev.device_id(), dev.num_queues());
        VirtioMmio { core, dev }
    }

    /// Route this device's interrupt through the PLIC.
    pub fn attach(&mut self, plic: Rc<RefCell<Plic>>, irq: u32) {
        self.core.plic = Some(plic);
        self.core.irq = irq;
    }

    fn selected(&mut self) -> Option<&mut Queue> {
        let i = self.core.queue_sel as usize;
        self.core.queues.get_mut(i)
    }
}

impl<D: VirtioDevice> Device for VirtioMmio<D> {
    fn base(&self) -> u64 {
        self.core.base
    }
    fn size(&self) -> u64 {
        0x1000
    }
    fn name(&self) -> &'static str {
        self.dev.name()
    }

    fn load(&mut self, offset: u64, size: usize) -> Res<u64> {
        // The device-specific configuration space may be read at any width.
        if offset >= CONFIG {
            return Ok(self.dev.config_read(offset - CONFIG, size));
        }
        if size != 4 {
            return Err(Trap::new(Exception::LoadAccessFault, self.core.base + offset));
        }
        let c = &self.core;
        Ok(match offset {
            MAGIC_VALUE => MAGIC,
            VERSION => 2, // modern MMIO
            DEVICE_ID => c.device_id as u64,
            VENDOR_ID => VENDOR,
            // The half of the feature space the driver most recently selected.
            DEVICE_FEATURES => {
                let features = self.dev.features() | F_VERSION_1;
                (features >> (c.device_features_sel * 32)) as u32 as u64
            }
            QUEUE_NUM_MAX => {
                if (c.queue_sel as usize) < c.queues.len() {
                    QUEUE_MAX as u64
                } else {
                    0
                }
            }
            QUEUE_READY => c.queues.get(c.queue_sel as usize).map_or(0, |q| q.ready as u64),
            INTERRUPT_STATUS => c.interrupt_status as u64,
            STATUS => c.status as u64,
            CONFIG_GENERATION => 0,
            _ => 0,
        })
    }

    fn store(&mut self, offset: u64, size: usize, value: u64) -> Res<()> {
        if offset >= CONFIG {
            self.dev.config_write(offset - CONFIG, size, value);
            return Ok(());
        }
        if size != 4 {
            return Err(Trap::new(Exception::StoreAMOAccessFault, self.core.base + offset));
        }
        let v = value as u32;
        let hi = (v as u64) << 32;
        let lo_mask = 0xffff_ffffu64;

        match offset {
            DEVICE_FEATURES_SEL => self.core.device_features_sel = (v != 0) as u32,
            DRIVER_FEATURES_SEL => self.core.driver_features_sel = (v != 0) as u32,
            DRIVER_FEATURES => {
                // The driver acknowledges features the same way it read them, one
                // 32-bit half at a time.
                let sh = self.core.driver_features_sel * 32;
                self.core.driver_features &= !(lo_mask << sh);
                self.core.driver_features |= (v as u64) << sh;
            }
            QUEUE_SEL => self.core.queue_sel = v,
            QUEUE_NUM => {
                // The ring size must be at most QUEUE_NUM_MAX; a larger value is a
                // driver bug, and indexing a ring with it would run off the end.
                if let Some(q) = self.selected() {
                    q.num = v.min(QUEUE_MAX);
                }
            }
            QUEUE_READY => {
                if let Some(q) = self.selected() {
                    q.ready = v & 1 != 0;
                }
            }
            QUEUE_DESC_LOW => {
                if let Some(q) = self.selected() {
                    q.desc = (q.desc & !lo_mask) | v as u64;
                }
            }
            QUEUE_DESC_HIGH => {
                if let Some(q) = self.selected() {
                    q.desc = (q.desc & lo_mask) | hi;
                }
            }
            DRIVER_DESC_LOW => {
                if let Some(q) = self.selected() {
                    q.avail = (q.avail & !lo_mask) | v as u64;
                }
            }
            DRIVER_DESC_HIGH => {
                if let Some(q) = self.selected() {
                    q.avail = (q.avail & lo_mask) | hi;
                }
            }
            DEVICE_DESC_LOW => {
                if let Some(q) = self.selected() {
                    q.used = (q.used & !lo_mask) | v as u64;
                }
            }
            DEVICE_DESC_HIGH => {
                if let Some(q) = self.selected() {
                    q.used = (q.used & lo_mask) | hi;
                }
            }
            QUEUE_NOTIFY => {
                // The driver has queued work. For most devices this is the only
                // register whose write makes the device *do* something.
                if (v as usize) < self.core.queues.len() {
                    self.dev.notify(&mut self.core, v);
                }
            }
            INTERRUPT_ACK => {
                self.core.interrupt_status &= !v;
                // Once acknowledged the line drops, so the PLIC stops reporting it.
                if self.core.interrupt_status == 0 {
                    if let Some(p) = &self.core.plic {
                        p.borrow_mut().set_pending(self.core.irq, false);
                    }
                }
            }
            STATUS => {
                if v == 0 {
                    // A driver writing 0 is resetting the device.
                    self.core.reset();
                    self.dev.reset();
                } else {
                    self.core.status = v;
                    if v & FEATURES_OK != 0 {
                        // Acknowledge the feature negotiation. The driver reads STATUS
                        // back and gives up if FEATURES_OK did not stick.
                        self.core.status |= FEATURES_OK;
                    }
                }
            }
            _ => {}
        }
        Ok(())
    }

    fn poll(&mut self) {
        self.dev.poll(&mut self.core);
    }

    fn io_pending(&self) -> bool {
        self.dev.io_pending()
    }
}
