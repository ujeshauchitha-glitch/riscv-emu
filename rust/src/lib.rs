//! An RV64GC emulator that boots xv6, Linux and Debian.
//!
//! The pieces, bottom to top: `types`/`trap` (the vocabulary), `decoder`, `csr`,
//! `mmu`, `softfloat` and `bitmanip` (the arithmetic), `cpu` (one hart), `bus` and the
//! devices, and `platform` (harts plus devices plus a scheduler - a machine).

#[cfg(not(target_endian = "little"))]
compile_error!("the emulator reads guest memory as host-endian words and needs a little-endian host");

pub mod backends;
pub mod bitmanip;
pub mod bus;
pub mod clint;
pub mod cpu;
pub mod csr;
pub mod decoder;
pub mod device;
pub mod dram;
pub mod elf;
pub mod mmu;
pub mod plic;
pub mod platform;
pub mod rtc;
pub mod sbi;
pub mod softfloat;
pub mod syscon;
pub mod trap;
pub mod types;
pub mod uart;
