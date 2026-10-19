//! Bus and DRAM tests.
//!
//! The headline case is memory safety: every out-of-range access must come back as a
//! trap rather than touching host memory outside the array.

use riscv_emu::bus::Bus;
use riscv_emu::device::{AccessType, Device};
use riscv_emu::dram::Dram;
use riscv_emu::trap::{Exception, Res};
use riscv_emu::types::{DRAM_BASE, UART0_BASE};

const TEST_DRAM_SIZE: u64 = 1024 * 1024; // 1 MiB, plenty for these tests

fn bus() -> Bus {
    Bus::new(TEST_DRAM_SIZE)
}

#[test]
fn dram_roundtrip() {
    let bus = bus();

    // Every access width, written and read back.
    bus.store(DRAM_BASE + 0x100, 1, 0xab).unwrap();
    bus.store(DRAM_BASE + 0x200, 2, 0xbeef).unwrap();
    bus.store(DRAM_BASE + 0x300, 4, 0xdead_beef).unwrap();
    bus.store(DRAM_BASE + 0x400, 8, 0x0123_4567_89ab_cdef).unwrap();

    assert_eq!(bus.load(DRAM_BASE + 0x100, 1, AccessType::Load).unwrap(), 0xab);
    assert_eq!(bus.load(DRAM_BASE + 0x200, 2, AccessType::Load).unwrap(), 0xbeef);
    assert_eq!(bus.load(DRAM_BASE + 0x300, 4, AccessType::Load).unwrap(), 0xdead_beef);
    assert_eq!(bus.load(DRAM_BASE + 0x400, 8, AccessType::Load).unwrap(), 0x0123_4567_89ab_cdef);
}

#[test]
fn little_endian() {
    let bus = bus();
    // RISC-V is little-endian: the least significant byte lives at the lowest address.
    bus.store(DRAM_BASE + 0x10, 4, 0x1122_3344).unwrap();
    let byte = |o| bus.load(DRAM_BASE + 0x10 + o, 1, AccessType::Load).unwrap();
    assert_eq!([byte(0), byte(1), byte(2), byte(3)], [0x44, 0x33, 0x22, 0x11]);
}

#[test]
fn dram_is_based_at_0x80000000() {
    let bus = bus();

    // Address 0 is *not* RAM. On a real machine the low addresses are reserved for MMIO,
    // and every kernel we care about is linked for RAM at 0x8000_0000.
    let low = bus.load(0, 4, AccessType::Load).unwrap_err();
    assert_eq!(low.cause, Exception::LoadAccessFault);

    // The first byte of DRAM is at 0x8000_0000 and maps to array element 0.
    bus.store(DRAM_BASE, 1, 0x5a).unwrap();
    assert_eq!(bus.ram.peek8(0), 0x5a);
}

#[test]
fn out_of_range_traps_instead_of_corrupting_memory() {
    let bus = bus();
    let last_byte = DRAM_BASE + TEST_DRAM_SIZE - 1;

    // A single byte at the very last address is fine.
    bus.store(last_byte, 1, 0x77).unwrap();
    assert_eq!(bus.load(last_byte, 1, AccessType::Load).unwrap(), 0x77);

    // A 4-byte access *starting* at the last byte begins in range but runs three bytes past
    // the end.
    let over = bus.load(last_byte, 4, AccessType::Load).unwrap_err();
    assert_eq!(over.cause, Exception::LoadAccessFault);

    let over_store = bus.store(last_byte, 8, u64::MAX).unwrap_err();
    assert_eq!(over_store.cause, Exception::StoreAMOAccessFault);

    // Comfortably past the end.
    assert!(bus.load(DRAM_BASE + TEST_DRAM_SIZE + 0x1000, 4, AccessType::Load).is_err());
}

#[test]
fn unmapped_addresses_report_the_right_cause() {
    let bus = bus();

    // 0x1000_0000 is where the UART lives, but nothing is attached there. The same unmapped
    // address yields a different trap cause depending on why we touched it.
    let as_fetch = bus.load(UART0_BASE, 4, AccessType::Instruction).unwrap_err();
    assert_eq!(as_fetch.cause, Exception::InstructionAccessFault);
    assert_eq!(as_fetch.tval, UART0_BASE);

    let as_load = bus.load(UART0_BASE, 4, AccessType::Load).unwrap_err();
    assert_eq!(as_load.cause, Exception::LoadAccessFault);

    let as_store = bus.store(UART0_BASE, 4, 0).unwrap_err();
    assert_eq!(as_store.cause, Exception::StoreAMOAccessFault);
}

struct Window(u64, u64);

impl Device for Window {
    fn base(&self) -> u64 {
        self.0
    }
    fn size(&self) -> u64 {
        self.1
    }
    fn name(&self) -> &'static str {
        "window"
    }
    fn load(&mut self, _: u64, _: usize) -> Res<u64> {
        Ok(0)
    }
    fn store(&mut self, _: u64, _: usize, _: u64) -> Res<()> {
        Ok(())
    }
}

#[test]
fn overlapping_devices_are_rejected() {
    let mut bus = bus();

    // A device over RAM would silently shadow it. Catching it at attach time is far cheaper
    // than debugging why a UART write lands in RAM and nothing prints.
    assert!(!bus.attach(Box::new(Window(DRAM_BASE + 0x1000, 0x1000))));

    assert!(bus.attach(Box::new(Window(UART0_BASE, 0x100))));
    assert!(!bus.attach(Box::new(Window(UART0_BASE + 0x80, 0x100)))); // overlaps the first
    assert_eq!(bus.device_names().len(), 1);
}

#[test]
fn load_image() {
    let dram = Dram::new(TEST_DRAM_SIZE);

    let blob = [0xde, 0xad, 0xbe, 0xef];
    assert!(dram.load_image(DRAM_BASE, &blob));
    assert_eq!(dram.load(0, 4), Some(0xefbe_adde));

    // An image that does not fit must be refused, not truncated or written past the end.
    assert!(!dram.load_image(DRAM_BASE, &vec![0u8; TEST_DRAM_SIZE as usize + 1]));

    // An address below DRAM_BASE is not a valid load target.
    assert!(!dram.load_image(0, &blob));
}
