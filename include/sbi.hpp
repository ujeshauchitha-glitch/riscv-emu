#pragma once

#include "types.hpp"

class Cpu;

// ---------------------------------------------------------------------------
// SBI - the Supervisor Binary Interface.
//
// A Linux kernel runs in supervisor mode, and there are things supervisor mode
// cannot do: set the timer (mtimecmp is a machine-mode register), send an
// interrupt to another hart, shut the machine down. On real hardware a firmware
// layer - OpenSBI, almost always - sits in machine mode and provides them, and
// the kernel reaches it with `ecall`.
//
// So SBI is a syscall interface, with the kernel in the position userspace
// normally occupies. The convention mirrors the Linux one:
//
//   a7   extension ID (EID)
//   a6   function ID (FID) - unused by the older "legacy" calls
//   a0.. arguments
//   a0   error code on return, a1 the value
//
// **This emulator implements SBI directly rather than loading OpenSBI.** Real
// firmware would be more faithful, but it is a second binary to build, a second
// thing to debug, and it hides the boot behind 100 KB of code that is not the
// subject of this project. Implementing the interface here means the kernel
// takes exactly the same path it would take on real hardware, and the thing it
// calls into is fifty lines you can read.
// ---------------------------------------------------------------------------

namespace sbi {

// Extension IDs. The first four are the "legacy" v0.1 calls, which are single
// functions with no FID; the rest are v0.2 extensions identified by name.
constexpr u64 EXT_SET_TIMER          = 0x00;
constexpr u64 EXT_CONSOLE_PUTCHAR    = 0x01;
constexpr u64 EXT_CONSOLE_GETCHAR    = 0x02;
constexpr u64 EXT_SHUTDOWN           = 0x08;

constexpr u64 EXT_BASE   = 0x10;        // "discover what is here"
constexpr u64 EXT_TIME   = 0x54494d45;  // "TIME"
constexpr u64 EXT_IPI    = 0x735049;    // "sPI"
constexpr u64 EXT_RFENCE = 0x52464e43;  // "RFNC"
constexpr u64 EXT_HSM    = 0x48534d;    // "HSM"
constexpr u64 EXT_SRST   = 0x53525354;  // "SRST"

// Return codes. Zero is success; the errors are negative, which is why a0 is
// read as a signed value.
constexpr i64 SUCCESS          = 0;
constexpr i64 ERR_FAILED       = -1;
constexpr i64 ERR_NOT_SUPPORTED = -2;
constexpr i64 ERR_INVALID_PARAM = -3;

// Handle an ECALL from supervisor mode as an SBI call. Returns true if it was
// handled - in which case the CPU must resume after the ecall rather than
// taking a trap - and false if the call should be delivered as an ordinary
// exception.
bool handle_ecall(Cpu& cpu);

}  // namespace sbi
