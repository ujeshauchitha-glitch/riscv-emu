#pragma once

#include <cstdint>
#include <cstddef>

// ---------------------------------------------------------------------------
// Basic integer aliases.
//
// A RISC-V emulator is almost entirely bit manipulation, so the exact width and
// signedness of every value matters. Spelling them out as u8/i32/u64 keeps the
// code closer to how the ISA manual describes things than `unsigned long long`
// ever would.
// ---------------------------------------------------------------------------
using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

// We are building RV64: registers and the PC are 64 bits wide.
constexpr int XLEN = 64;

// Number of architectural integer registers (x0..x31).
constexpr int NUM_REGS = 32;

// ---------------------------------------------------------------------------
// Physical memory map.
//
// These addresses are not arbitrary: they match the QEMU `virt` machine, which
// is what xv6-riscv and a stock Linux kernel are built to expect. Matching it
// means we can boot unmodified guest binaries later on.
//
//   0x0010_0000  syscon      (poweroff / reboot)
//   0x0200_0000  CLINT       (timer + software interrupts)
//   0x0C00_0000  PLIC        (external interrupts)         [not yet built]
//   0x1000_0000  UART0       (NS16550A console)
//   0x1000_1000  virtio-mmio (block device)                [not yet built]
//   0x8000_0000  DRAM        (where the kernel is loaded)
//
// Addresses for devices that do not exist yet are reserved here so the map
// lives in one place, and so nothing else is mapped over them by accident.
// ---------------------------------------------------------------------------
constexpr u64 SYSCON_BASE = 0x0010'0000;
constexpr u64 CLINT_BASE  = 0x0200'0000;
constexpr u64 PLIC_BASE   = 0x0c00'0000;
constexpr u64 UART0_BASE  = 0x1000'0000;
constexpr u64 VIRTIO_BASE = 0x1000'1000;
constexpr u64 DRAM_BASE   = 0x8000'0000;

// Default guest RAM size. 128 MiB is comfortably more than xv6 needs and enough
// for a minimal Linux + initramfs later.
constexpr u64 DRAM_SIZE_DEFAULT = 128ull * 1024 * 1024;

// ---------------------------------------------------------------------------
// Privilege levels.
//
// RISC-V defines three: machine (the most privileged, always present),
// supervisor (where a kernel runs), and user. The numeric values are fixed by
// the spec and appear directly in mstatus.MPP and mstatus.SPP.
//
// Only machine mode exists until phase 6; these are defined now because the
// trap path already has to record which mode it came from.
// ---------------------------------------------------------------------------
constexpr u32 PRIV_USER       = 0;
constexpr u32 PRIV_SUPERVISOR = 1;
constexpr u32 PRIV_MACHINE    = 3;

// The mode an MRET returns to when it has nowhere less privileged to go. The
// spec says to set mstatus.MPP to the least-privileged supported mode; with
// only machine mode implemented, that is machine mode. Phase 6 changes this to
// PRIV_USER once user mode exists.
constexpr u32 PRIV_LEAST_SUPPORTED = PRIV_MACHINE;

const char* privilege_name(u32 priv);

// ---------------------------------------------------------------------------
// Sign-extend the low `bits` bits of `value` to a full 64-bit signed integer.
//
// Immediates in RISC-V are stored in narrow fields (12, 13, 20, 21 bits) but
// are always used as signed 64-bit values. Shifting left to put the sign bit in
// position 63 and then arithmetic-shifting back is the standard trick.
// ---------------------------------------------------------------------------
constexpr i64 sign_extend(u64 value, unsigned bits) {
    const unsigned shift = 64 - bits;
    return static_cast<i64>(value << shift) >> shift;
}
