//! The interface every memory-mapped device implements.
//!
//! The bus owns a list of them and routes each access to whichever one claims the
//! address. `offset` is relative to the device's own base, so a device never has
//! to know where it was mapped.

use crate::trap::Res;

/// Why memory is being touched.
///
/// The *same* underlying failure produces a different trap cause depending on
/// why: reading address 0 during instruction fetch is an InstructionAccessFault,
/// during a load a LoadAccessFault. The MMU uses it to pick page-fault causes and
/// to check execute/read/write permission.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AccessType {
    Instruction,
    Load,
    Store,
}

pub trait Device {
    fn base(&self) -> u64;
    fn size(&self) -> u64;
    fn name(&self) -> &'static str;

    /// `size` is 1, 2, 4 or 8. Devices are free to reject widths they do not
    /// support (many MMIO registers are 32-bit only) by returning an access fault.
    fn load(&mut self, offset: u64, size: usize) -> Res<u64>;
    fn store(&mut self, offset: u64, size: usize, value: u64) -> Res<()>;

    /// Called by the machine once per scheduling round, for devices whose state
    /// changes without the guest touching them - a network card receiving a
    /// packet, an alarm clock going off.
    fn poll(&mut self) {}

    /// True while the device is waiting on something outside the machine - a
    /// network reply, say - that real time must be allowed to deliver. While any
    /// device says so, an idle machine does not skip its clock forward to the next
    /// timer deadline: doing that would let the guest's own timeout expire in zero
    /// real seconds, before the answer it is waiting for could possibly arrive.
    fn io_pending(&self) -> bool {
        false
    }
}

/// Little-endian byte helpers shared by devices.
pub fn read_le(bytes: &[u8]) -> u64 {
    let mut v = 0u64;
    for (i, b) in bytes.iter().enumerate().take(8) {
        v |= (*b as u64) << (8 * i);
    }
    v
}
