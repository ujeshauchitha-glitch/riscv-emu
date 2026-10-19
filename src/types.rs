//! Basic widths, the physical memory map, and privilege levels.
//!
//! A RISC-V emulator is almost entirely bit manipulation, so the exact width and
//! signedness of every value matters. Everything is spelled `u64`/`i64`/`u32`
//! rather than `usize` wherever the ISA manual gives a width.

/// Number of architectural integer registers (x0..x31).
pub const NUM_REGS: usize = 32;

// ---------------------------------------------------------------------------
// Physical memory map.
//
// These addresses match the QEMU `virt` machine, which is what xv6-riscv and a
// stock Linux kernel are built to expect. Matching it means unmodified guest
// binaries boot.
//
//   0x0010_0000  syscon      (poweroff / reboot)
//   0x0010_1000  RTC         (goldfish real-time clock)
//   0x0200_0000  CLINT       (timer + software interrupts)
//   0x0C00_0000  PLIC        (external interrupts)
//   0x1000_0000  UART0       (NS16550A console)
//   0x1000_1000  virtio-mmio (block device); +0x1000 net, +0x2000 rng
//   0x8000_0000  DRAM        (where the kernel is loaded)
// ---------------------------------------------------------------------------
pub const SYSCON_BASE: u64 = 0x0010_0000;
pub const RTC_BASE: u64 = 0x0010_1000;
pub const CLINT_BASE: u64 = 0x0200_0000;
pub const PLIC_BASE: u64 = 0x0c00_0000;
pub const UART0_BASE: u64 = 0x1000_0000;
pub const VIRTIO_BASE: u64 = 0x1000_1000;
pub const VIRTIO_STRIDE: u64 = 0x1000;
pub const VIRTIO_NET_BASE: u64 = VIRTIO_BASE + VIRTIO_STRIDE;
pub const VIRTIO_RNG_BASE: u64 = VIRTIO_BASE + 2 * VIRTIO_STRIDE;
pub const DRAM_BASE: u64 = 0x8000_0000;

/// Interrupt numbers on the PLIC, matching the QEMU virt machine.
pub const VIRTIO0_IRQ: u32 = 1;
pub const VIRTIO_NET_IRQ: u32 = 2;
pub const VIRTIO_RNG_IRQ: u32 = 3;
pub const UART0_IRQ: u32 = 10;
pub const RTC_IRQ: u32 = 11;

/// Default guest RAM size: 128 MiB.
pub const DRAM_SIZE_DEFAULT: u64 = 128 * 1024 * 1024;

// Privilege levels. The numeric values are fixed by the spec and appear directly
// in mstatus.MPP and mstatus.SPP.
pub const PRIV_USER: u32 = 0;
pub const PRIV_SUPERVISOR: u32 = 1;
pub const PRIV_MACHINE: u32 = 3;

/// After MRET or SRET the "previous privilege" field is reset to the
/// least-privileged mode the machine supports. Leaving it at machine would let a
/// later MRET escalate.
pub const PRIV_LEAST_SUPPORTED: u32 = PRIV_USER;

pub fn privilege_name(p: u32) -> &'static str {
    match p {
        PRIV_USER => "U",
        PRIV_SUPERVISOR => "S",
        PRIV_MACHINE => "M",
        _ => "?",
    }
}

/// Sign-extend the low `bits` bits of `value` to a full 64-bit signed integer.
///
/// Immediates in RISC-V are stored in narrow fields (12, 13, 20, 21 bits) but are
/// always used as signed 64-bit values. Shifting left to put the sign bit in
/// position 63 and then arithmetic-shifting back is the standard trick.
#[inline(always)]
pub const fn sign_extend(value: u64, bits: u32) -> i64 {
    let shift = 64 - bits;
    ((value << shift) as i64) >> shift
}

/// Sign-extend the low 32 bits of a value to 64 bits.
///
/// This shows up everywhere in RV64. The "*W" instructions operate on 32-bit
/// values but write a 64-bit register, and the spec requires the result to be
/// *sign*-extended, not zero-extended.
#[inline(always)]
pub const fn sext32(value: u32) -> u64 {
    value as i32 as i64 as u64
}

/// ABI register names (zero, ra, sp, ...) indexed by register number.
pub const REG_ABI_NAMES: [&str; NUM_REGS] = [
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0", "a1", "a2", "a3", "a4",
    "a5", "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4",
    "t5", "t6",
];
