#pragma once

#include "types.hpp"

// ---------------------------------------------------------------------------
// Traps: exceptions and interrupts.
//
// In RISC-V both are delivered through the same mechanism and both write a
// cause code into a CSR (mcause/scause). They are distinguished by the most
// significant bit of that cause value: 1 for an interrupt, 0 for an exception.
//
// The numeric values below are fixed by the privileged spec — they are not our
// choice, and guest software depends on them exactly.
// ---------------------------------------------------------------------------

enum class Exception : u64 {
    InstructionAddressMisaligned = 0,
    InstructionAccessFault       = 1,
    IllegalInstruction           = 2,
    Breakpoint                   = 3,
    LoadAddressMisaligned        = 4,
    LoadAccessFault              = 5,
    StoreAMOAddressMisaligned    = 6,
    StoreAMOAccessFault          = 7,
    ECallFromUMode               = 8,
    ECallFromSMode               = 9,
    ECallFromMMode               = 11,
    InstructionPageFault         = 12,
    LoadPageFault                = 13,
    StoreAMOPageFault            = 15,
};

enum class Interrupt : u64 {
    SupervisorSoftware = 1,
    MachineSoftware    = 3,
    SupervisorTimer    = 5,
    MachineTimer       = 7,
    SupervisorExternal = 9,
    MachineExternal    = 11,
};

// A trap carries a cause and a "trap value" (tval). What tval holds depends on
// the cause: the faulting address for access/page faults and misaligned
// accesses, the raw instruction bits for an illegal instruction, and zero for
// most others. Guest trap handlers read it, so getting it right matters.
struct Trap {
    Exception cause = Exception::IllegalInstruction;
    u64       tval  = 0;

    // The value that belongs in mcause/scause for this trap.
    u64 cause_code() const { return static_cast<u64>(cause); }
};

// Build the mcause value for an interrupt: cause number with the MSB set.
constexpr u64 interrupt_cause_code(Interrupt i) {
    return (1ull << 63) | static_cast<u64>(i);
}

// Human-readable name, used by the tracer and by error reporting in main().
const char* exception_name(Exception e);
