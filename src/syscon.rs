//! The power button: writing a magic value to this register ends the run.
//!
//! This is how test programs report a result and how a guest powers the machine off.
//! The values match the QEMU `virt` machine's sifive_test device, which the Linux
//! `syscon-poweroff` driver already knows.

use crate::device::Device;
use crate::trap::Res;
use crate::types::SYSCON_BASE;

pub const POWEROFF: u64 = 0x5555;
pub const REBOOT: u64 = 0x7777;

#[derive(Default)]
pub struct Syscon {
    poweroff: bool,
    reboot: bool,
    exit_code: u64,
}

impl Syscon {
    pub fn new() -> Syscon {
        Syscon::default()
    }
    pub fn poweroff_requested(&self) -> bool {
        self.poweroff
    }
    pub fn reboot_requested(&self) -> bool {
        self.reboot
    }
    pub fn exit_code(&self) -> u64 {
        self.exit_code
    }
}

impl Device for Syscon {
    fn base(&self) -> u64 {
        SYSCON_BASE
    }
    fn size(&self) -> u64 {
        0x1000
    }
    fn name(&self) -> &'static str {
        "syscon"
    }

    fn load(&mut self, _offset: u64, _size: usize) -> Res<u64> {
        Ok(0)
    }

    fn store(&mut self, offset: u64, _size: usize, value: u64) -> Res<()> {
        if offset != 0 {
            return Ok(());
        }
        match value & 0xffff {
            POWEROFF => {
                self.poweroff = true;
                // riscv-tests packs its result into the upper bits, so a non-zero exit
                // code identifies which test failed.
                self.exit_code = value >> 16;
            }
            REBOOT => self.reboot = true,
            _ => {}
        }
        Ok(())
    }
}
