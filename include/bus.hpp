#pragma once

#include <memory>
#include <vector>

#include "device.hpp"
#include "dram.hpp"
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

    // The guest RAM, if one is attached. Nearly every access a running guest
    // makes lands here, so load() and store() test it first and call it without
    // a virtual dispatch or a scan over the other devices; the CPU also uses it
    // to register the pages it has cached decoded instructions from.
    Dram* ram() const { return ram_; }

    // How many accesses have gone to a device other than RAM.
    //
    // A guest that touches a device may have changed an interrupt line - written
    // msip, enabled an interrupt in the PLIC, claimed one, sent a byte to the
    // UART. The machine only polls devices between scheduling rounds, so
    // without this a guest would see its own MMIO write take effect up to a
    // whole round late. A hart watches this counter and ends its round early
    // when it moves, so the devices are re-polled straight away.
    u64 mmio_epoch() const { return mmio_epoch_; }

private:
    std::vector<std::unique_ptr<Device>> devices_;
    Dram* ram_ = nullptr;
    mutable u64 mmio_epoch_ = 0;
};

// The access fault cause corresponding to an access type.
Exception access_fault_for(AccessType type);
