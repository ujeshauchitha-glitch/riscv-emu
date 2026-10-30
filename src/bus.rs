//! The system bus.
//!
//! A CPU does not talk to RAM. It puts an address on a bus, and *something*
//! answers - RAM for most addresses, but a UART for 0x1000_0000, a timer for
//! 0x0200_0000. That is the whole idea behind memory-mapped I/O: devices are
//! addressed exactly like memory, and the address decoder decides who responds.
//!
//! Addresses claimed by no device return an access fault rather than silently
//! reading zero, so a kernel that jumps into the weeds stops immediately with a
//! clear cause.
//!
//! RAM is built in (and tested first): nearly every access a running guest makes
//! lands there, so it is served without a scan over the device list.

use std::cell::{Cell, RefCell};
use std::rc::Rc;

use crate::device::{AccessType, Device};
use crate::dram::Dram;
use crate::trap::{Exception, Res, Trap};
use crate::types::DRAM_BASE;

/// The access-fault cause matching an access type.
pub fn access_fault_for(ty: AccessType) -> Exception {
    match ty {
        AccessType::Instruction => Exception::InstructionAccessFault,
        AccessType::Load => Exception::LoadAccessFault,
        AccessType::Store => Exception::StoreAMOAccessFault,
    }
}

pub struct Bus {
    pub ram: Rc<Dram>,
    ranges: Vec<(u64, u64)>, // (base, end) per device, parallel to `devices`
    devices: RefCell<Vec<Box<dyn Device>>>,
    mmio_epoch: Cell<u64>,
}

impl Bus {
    pub fn new(ram_bytes: u64) -> Bus {
        Bus::try_new(ram_bytes).expect("cannot allocate guest RAM")
    }

    /// Like `new`, but reports an impossible RAM size instead of aborting.
    pub fn try_new(ram_bytes: u64) -> Option<Bus> {
        Some(Bus {
            ram: Rc::new(Dram::try_new(ram_bytes)?),
            ranges: Vec::new(),
            devices: RefCell::new(Vec::new()),
            mmio_epoch: Cell::new(0),
        })
    }

    /// Register a device. Ranges must not overlap (including with RAM); an
    /// overlapping registration is rejected, because a silently shadowed device is
    /// a miserable bug to find later, when a UART write lands in RAM and nothing
    /// prints.
    pub fn attach(&mut self, dev: Box<dyn Device>) -> bool {
        let lo = dev.base();
        let hi = lo.saturating_add(dev.size());
        if lo < DRAM_BASE + self.ram.size() && DRAM_BASE < hi {
            return false;
        }
        for &(b_lo, b_hi) in &self.ranges {
            if lo < b_hi && b_lo < hi {
                return false;
            }
        }
        self.ranges.push((lo, hi));
        self.devices.get_mut().push(dev);
        true
    }

    /// How many accesses have gone to a device other than RAM.
    ///
    /// A guest that touches a device may have changed an interrupt line - written
    /// msip, enabled an interrupt in the PLIC, claimed one, sent a byte to the
    /// UART. The machine only polls devices between scheduling rounds, so without
    /// this a guest would see its own MMIO write take effect up to a whole round
    /// late. A hart watches this counter and ends its round early when it moves.
    #[inline(always)]
    pub fn mmio_epoch(&self) -> u64 {
        self.mmio_epoch.get()
    }

    #[inline]
    fn find(&self, addr: u64) -> Option<usize> {
        self.ranges.iter().position(|&(lo, hi)| addr >= lo && addr < hi)
    }

    /// Is there a device (not RAM) at this address?
    pub fn has_device_at(&self, addr: u64) -> bool {
        self.find(addr).is_some()
    }

    #[inline]
    pub fn load(&self, addr: u64, size: usize, ty: AccessType) -> Res<u64> {
        // RAM first. The subtraction form of the range test cannot overflow.
        if addr >= DRAM_BASE {
            if let Some(v) = self.ram.load(addr - DRAM_BASE, size) {
                return Ok(v);
            }
            if addr - DRAM_BASE < self.ram.size() {
                // Starts in range but the width runs off the end.
                return Err(Trap::new(access_fault_for(ty), addr));
            }
        }
        self.load_device(addr, size, ty)
    }

    fn load_device(&self, addr: u64, size: usize, ty: AccessType) -> Res<u64> {
        let Some(i) = self.find(addr) else {
            return Err(Trap::new(access_fault_for(ty), addr));
        };
        self.mmio_epoch.set(self.mmio_epoch.get() + 1);
        let base = self.ranges[i].0;
        let mut devs = self.devices.borrow_mut();
        devs[i].load(addr - base, size).map_err(|mut t| {
            // A device reports faults as load faults; for an instruction fetch the
            // architecturally correct cause is the instruction one.
            if ty == AccessType::Instruction && t.cause == Exception::LoadAccessFault {
                t.cause = Exception::InstructionAccessFault;
            }
            t
        })
    }

    #[inline]
    pub fn store(&self, addr: u64, size: usize, value: u64) -> Res<()> {
        if addr >= DRAM_BASE {
            if self.ram.store(addr - DRAM_BASE, size, value) {
                return Ok(());
            }
            if addr - DRAM_BASE < self.ram.size() {
                return Err(Trap::new(Exception::StoreAMOAccessFault, addr));
            }
        }
        self.store_device(addr, size, value)
    }

    fn store_device(&self, addr: u64, size: usize, value: u64) -> Res<()> {
        let Some(i) = self.find(addr) else {
            return Err(Trap::new(Exception::StoreAMOAccessFault, addr));
        };
        self.mmio_epoch.set(self.mmio_epoch.get() + 1);
        let base = self.ranges[i].0;
        self.devices.borrow_mut()[i].store(addr - base, size, value)
    }

    /// Give every device its once-per-round poll.
    pub fn poll_devices(&self) {
        for d in self.devices.borrow_mut().iter_mut() {
            d.poll();
        }
    }

    /// Is any device waiting on the outside world?
    pub fn io_pending(&self) -> bool {
        self.devices.borrow().iter().any(|d| d.io_pending())
    }

    /// Names of attached devices (diagnostics).
    pub fn device_names(&self) -> Vec<&'static str> {
        self.devices.borrow().iter().map(|d| d.name()).collect()
    }
}

/// Let a device live both on the bus and in the machine's own hands.
///
/// The bus owns its devices as `Box<dyn Device>`, but the platform also needs to
/// reach some of them directly (to tick the timer, poll the UART's host input,
/// read the poweroff flag). Wrapping the device in `Rc<RefCell<_>>` and putting a
/// clone of this adapter on the bus gives both sides a handle to the same device.
pub struct Shared<T: Device>(pub Rc<RefCell<T>>);

impl<T: Device> Shared<T> {
    pub fn new(dev: T) -> (Shared<T>, Rc<RefCell<T>>) {
        let rc = Rc::new(RefCell::new(dev));
        (Shared(rc.clone()), rc)
    }
}

impl<T: Device> Device for Shared<T> {
    fn base(&self) -> u64 {
        self.0.borrow().base()
    }
    fn size(&self) -> u64 {
        self.0.borrow().size()
    }
    fn name(&self) -> &'static str {
        self.0.borrow().name()
    }
    fn load(&mut self, offset: u64, size: usize) -> Res<u64> {
        self.0.borrow_mut().load(offset, size)
    }
    fn store(&mut self, offset: u64, size: usize, value: u64) -> Res<()> {
        self.0.borrow_mut().store(offset, size, value)
    }
    fn poll(&mut self) {
        self.0.borrow_mut().poll()
    }
    fn io_pending(&self) -> bool {
        self.0.borrow().io_pending()
    }
}
