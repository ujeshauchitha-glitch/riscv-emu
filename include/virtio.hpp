#pragma once

#include <string>
#include <vector>

#include "device.hpp"
#include "plic.hpp"
#include "types.hpp"

class Bus;

// ---------------------------------------------------------------------------
// virtio-mmio block device.
//
// The disk. This is what lets xv6 have a filesystem rather than just a kernel.
//
// virtio is a standard for paravirtualised devices: rather than emulating real
// hardware register-for-register, the guest and the device agree on a shared
// memory protocol. That protocol is the **virtqueue**, and it is worth
// understanding because it is the same shape used by virtio network cards,
// consoles and everything else.
//
// A virtqueue is three arrays in guest memory:
//
//   descriptors  a pool of {address, length, flags, next} entries. A request is
//                a *chain* of them linked by `next`.
//   available    the driver's ring: "here are chain heads I want processed"
//   used         the device's ring: "here are the ones I have finished"
//
// The driver fills descriptors, appends the head index to the available ring,
// bumps `avail.idx`, and writes QueueNotify. The device walks the chains,
// does the work, appends to the used ring, bumps `used.idx`, and raises an
// interrupt. Neither side ever blocks on the other.
//
// A block request chain is always three descriptors:
//
//   [0] the header  {type, reserved, sector}   device reads
//   [1] the data    512 bytes per sector       device reads (write) or writes (read)
//   [2] the status  one byte                   device writes: 0 means OK
//
// The device therefore has to reach into guest memory itself, which is why it
// holds a Bus pointer - it is a bus master, not just a target.
// ---------------------------------------------------------------------------
class VirtioBlk : public Device {
public:
    static constexpr u64 SECTOR_SIZE = 512;
    static constexpr u32 QUEUE_MAX   = 8;

    u64         base() const override { return VIRTIO_BASE; }
    u64         size() const override { return 0x1000; }
    const char* name() const override { return "virtio-blk"; }

    Result<u64> load(u64 offset, unsigned size_bytes) override;
    Status      store(u64 offset, unsigned size_bytes, u64 value) override;

    // The device reads and writes guest memory directly, so it needs the bus.
    // Set after construction because the bus owns the device.
    void attach(Bus* bus, Plic* plic, u32 irq) { bus_ = bus; plic_ = plic; irq_ = irq; }

    // Back the disk with a file's contents. Returns false if unreadable.
    bool load_image(const std::string& path);

    // Capacity in 512-byte sectors, which is what the config space reports.
    u64 sectors() const { return disk_.size() / SECTOR_SIZE; }

    const std::vector<u8>& data() const { return disk_; }
    std::vector<u8>&       data() { return disk_; }

private:
    Bus*  bus_  = nullptr;
    Plic* plic_ = nullptr;
    u32   irq_  = 0;

    std::vector<u8> disk_;

    // Device registers.
    u32 status_          = 0;
    // The feature space is 64 bits, read and written 32 at a time through a
    // pair of select registers.
    u64 driver_features_     = 0;
    u32 device_features_sel_ = 0;
    u32 driver_features_sel_ = 0;
    u32 queue_sel_       = 0;
    u32 queue_num_       = 0;
    u32 queue_ready_     = 0;
    u32 interrupt_status_ = 0;

    u64 desc_addr_  = 0;   // descriptor table
    u64 avail_addr_ = 0;   // driver ring
    u64 used_addr_  = 0;   // device ring

    // The last avail.idx we processed. The difference between this and the
    // driver's current idx is the work waiting for us.
    u16 last_avail_ = 0;

    void process_queue();
    void process_chain(u16 head);
    void raise_interrupt();

    // Helpers for reaching into guest memory.
    u64  read_guest(u64 addr, unsigned size) const;
    void write_guest(u64 addr, unsigned size, u64 value);
};
