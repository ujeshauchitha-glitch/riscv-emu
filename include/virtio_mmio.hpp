#pragma once

#include <vector>

#include "device.hpp"
#include "plic.hpp"
#include "types.hpp"

class Bus;

// ---------------------------------------------------------------------------
// The virtio-mmio transport.
//
// virtio is a standard for paravirtualised devices: rather than emulating real
// hardware register-for-register, the guest and the device agree on a shared
// memory protocol. Everything that is not specific to one kind of device - the
// register file, feature negotiation, the queues, interrupts - is the same for
// a disk, a network card and a random-number source, so it lives here once and
// each device supplies only what makes it itself.
//
// **The virtqueue.** Three arrays in guest memory:
//
//   descriptors  a pool of {address, length, flags, next} entries. A request is
//                a *chain* of them linked by `next`.
//   available    the driver's ring: "here are chain heads I want processed"
//   used         the device's ring: "here are the ones I have finished"
//
// The driver fills descriptors, appends the head index to the available ring,
// bumps `avail.idx`, and writes QueueNotify. The device walks the chains, does
// the work, appends to the used ring, bumps `used.idx`, and raises an interrupt.
// Neither side ever blocks on the other.
//
// A device is a bus master: it reads and writes guest memory itself, so it
// holds a Bus pointer. **Every field it reads from a descriptor is hostile
// input** - a guest can write anything there - so addresses and lengths are
// bounds-checked against the memory they refer to, chains are length-limited so
// a loop cannot hang the host, and nothing is ever indexed with an unchecked
// guest value.
// ---------------------------------------------------------------------------
class VirtioMmio : public Device {
public:
    static constexpr u32 QUEUE_MAX = 256;

    VirtioMmio(u64 base, u32 device_id, u32 num_queues);

    u64         base() const override { return base_; }
    u64         size() const override { return 0x1000; }

    Result<u64> load(u64 offset, unsigned size_bytes) override;
    Status      store(u64 offset, unsigned size_bytes, u64 value) override;

    // The device reads and writes guest memory directly, so it needs the bus.
    // Set after construction because the bus owns the device.
    void attach(Bus* bus, Plic* plic, u32 irq) { bus_ = bus; plic_ = plic; irq_ = irq; }

    u32 interrupt_status() const { return interrupt_status_; }

protected:
    // One piece of a descriptor chain: a buffer in guest memory, and whether the
    // *device* is the one that writes it (the driver wrote it otherwise).
    struct Segment {
        u64  addr;
        u32  len;
        bool device_writes;
    };

    // Subclass hooks ---------------------------------------------------------

    // Feature bits specific to this device (VIRTIO_F_VERSION_1 is added here).
    virtual u64 device_features() const { return 0; }

    // The device-specific configuration space, at offset 0x100.
    virtual u64  config_read(u64 /*offset*/, unsigned /*size_bytes*/) { return 0; }
    virtual void config_write(u64 /*offset*/, unsigned /*size_bytes*/, u64 /*value*/) {}

    // The driver wrote QueueNotify for `queue`: it has put work on it.
    virtual void queue_notify(u32 queue) = 0;

    // The driver reset the device (wrote 0 to Status).
    virtual void device_reset() {}

    // Helpers for subclasses ---------------------------------------------------

    // Take the next chain the driver has made available on `queue`, flattened
    // into segments. Returns false if there is nothing waiting. A chain that is
    // malformed (loops, points outside guest memory) yields whatever was
    // gathered up to the fault and is still consumed, so a hostile driver
    // cannot wedge the queue by posting one.
    bool pop_chain(u32 queue, u16& head, std::vector<Segment>& segments);

    // Return a finished chain to the driver. `written` is how many bytes the
    // device wrote into the chain's device-writable buffers.
    void push_used(u32 queue, u16 head, u32 written);

    // Tell the driver the used ring advanced - unless it asked not to be told
    // (VRING_AVAIL_F_NO_INTERRUPT, which a driver sets while it is polling).
    void signal_used(u32 queue);

    bool queue_ready(u32 queue) const {
        return queue < queues_.size() && queues_[queue].ready && queues_[queue].num != 0;
    }

    // Copy between a host buffer and guest memory, which may be spread over
    // several segments. Both return the number of bytes actually moved; short
    // means the segments ran out or pointed at memory that is not there.
    static u64 segments_length(const std::vector<Segment>& segs, bool device_writes);
    u64 read_from_segments(const std::vector<Segment>& segs, u64 skip, u8* dst, u64 len);
    u64 write_to_segments(const std::vector<Segment>& segs, u64 skip, const u8* src, u64 len);

    bool read_guest_bytes(u64 addr, void* dst, u64 len) const;
    bool write_guest_bytes(u64 addr, const void* src, u64 len);
    u64  read_guest(u64 addr, unsigned size) const;
    void write_guest(u64 addr, unsigned size, u64 value);

    Bus* bus_ = nullptr;

private:
    struct Queue {
        u32  num   = 0;
        bool ready = false;
        u64  desc  = 0;     // descriptor table
        u64  avail = 0;     // driver ring
        u64  used  = 0;     // device ring
        u16  last_avail = 0;   // the last avail.idx we consumed
    };

    u64  base_;
    u32  device_id_;
    std::vector<Queue> queues_;

    Plic* plic_ = nullptr;
    u32   irq_  = 0;

    u32 status_ = 0;
    // The feature space is 64 bits, read and written 32 at a time through a pair
    // of select registers.
    u64 driver_features_     = 0;
    u32 device_features_sel_ = 0;
    u32 driver_features_sel_ = 0;
    u32 queue_sel_           = 0;
    u32 interrupt_status_    = 0;

    void reset();
    void raise_interrupt();
    Queue* selected() { return queue_sel_ < queues_.size() ? &queues_[queue_sel_] : nullptr; }
};
