#pragma once

#include <memory>
#include <vector>

#include "device.hpp"
#include "result.hpp"
#include "types.hpp"

// ---------------------------------------------------------------------------
// The system bus.
//
// This is the layer the previous `Memory` class was missing entirely, and it is
// the reason it had to be replaced rather than extended.
//
// A CPU does not talk to RAM. It puts an address on a bus, and *something*
// answers — RAM for most addresses, but a UART for 0x1000_0000, a timer for
// 0x0200_0000, a disk controller for 0x1000_1000. That is the whole idea behind
// memory-mapped I/O: devices are addressed exactly like memory, and the
// address decoder decides who responds.
//
// Without this indirection there is nowhere to attach a console, so there is no
// way for a guest OS to print anything, so there is no way to tell whether it
// booted. Every device phase from here on hangs off this class.
//
// Addresses claimed by no device return an access fault rather than silently
// reading zero. A kernel that jumps into the weeds should stop immediately with
// a clear cause, not wander through unmapped space.
// ---------------------------------------------------------------------------
class Bus {
public:
    // Register a device. The bus takes ownership. Device ranges must not
    // overlap; overlapping registrations are rejected (returns false).
    bool attach(std::unique_ptr<Device> dev);

    // `type` selects which trap cause is reported on failure — see AccessType
    // in device.hpp for why the caller has to tell us.
    Result<u64> load(u64 addr, unsigned size_bytes, AccessType type) const;
    Status      store(u64 addr, unsigned size_bytes, u64 value);

    // Find the device claiming `addr`, or nullptr.
    Device*       find(u64 addr);
    const Device* find(u64 addr) const;

    const std::vector<std::unique_ptr<Device>>& devices() const { return devices_; }

private:
    std::vector<std::unique_ptr<Device>> devices_;
};

// The access fault cause corresponding to an access type.
Exception access_fault_for(AccessType type);
