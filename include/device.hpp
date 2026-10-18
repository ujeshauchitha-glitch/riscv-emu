#pragma once

#include "result.hpp"
#include "types.hpp"

// ---------------------------------------------------------------------------
// The access type of a memory operation.
//
// This matters because the *same* underlying failure produces a different trap
// cause depending on why we were touching memory. Reading address 0 during
// instruction fetch is an InstructionAccessFault; reading it for a load is a
// LoadAccessFault. Later, when the MMU lands, this same enum selects between
// InstructionPageFault / LoadPageFault / StoreAMOPageFault and drives the
// execute/read/write permission check on the page table entry.
// ---------------------------------------------------------------------------
enum class AccessType {
    Instruction,
    Load,
    Store,
};

// ---------------------------------------------------------------------------
// A memory-mapped device.
//
// Everything the CPU can address — RAM, the UART, the timer, the interrupt
// controller, the disk — implements this interface. The Bus owns a list of them
// and routes each access to whichever one claims the address.
//
// `offset` is relative to the device's own base address, so a device never has
// to know where it was mapped.
//
// `size_bytes` is 1, 2, 4 or 8. Devices are free to reject widths they do not
// support (many MMIO registers are 32-bit only) by returning an access fault.
// ---------------------------------------------------------------------------
class Device {
public:
    virtual ~Device() = default;

    virtual u64         base() const = 0;
    virtual u64         size() const = 0;
    virtual const char* name() const = 0;

    virtual Result<u64> load(u64 offset, unsigned size_bytes) = 0;
    virtual Status      store(u64 offset, unsigned size_bytes, u64 value) = 0;

    // Called by the machine once per scheduling round, for devices whose state
    // changes without the guest touching them - a network card receiving a
    // packet, an alarm clock going off. Most devices have nothing to do.
    virtual void poll() {}

    // True while the device is waiting on something outside the machine - a
    // network reply, say - that real time must be allowed to deliver. While any
    // device says so, an idle machine does not skip its clock forward to the next
    // timer deadline: doing that would let the guest's own timeout expire in
    // zero real seconds, before the answer it is waiting for could possibly
    // arrive.
    virtual bool io_pending() const { return false; }

    // Does this device claim `addr`?
    //
    // Note the subtraction rather than `addr < base() + size()`: computing the
    // end address could overflow for a device mapped near the top of the
    // address space, and this form cannot.
    bool contains(u64 addr) const {
        return addr >= base() && (addr - base()) < size();
    }
};
