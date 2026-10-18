#include "virtio_mmio.hpp"

#include <algorithm>

#include "bus.hpp"

namespace {
// MMIO register offsets, from the virtio specification.
constexpr u64 MAGIC_VALUE         = 0x000;
constexpr u64 VERSION             = 0x004;
constexpr u64 DEVICE_ID           = 0x008;
constexpr u64 VENDOR_ID           = 0x00c;
constexpr u64 DEVICE_FEATURES     = 0x010;
constexpr u64 DEVICE_FEATURES_SEL = 0x014;
constexpr u64 DRIVER_FEATURES     = 0x020;
constexpr u64 DRIVER_FEATURES_SEL = 0x024;
constexpr u64 QUEUE_SEL           = 0x030;
constexpr u64 QUEUE_NUM_MAX       = 0x034;
constexpr u64 QUEUE_NUM           = 0x038;
constexpr u64 QUEUE_READY         = 0x044;
constexpr u64 QUEUE_NOTIFY        = 0x050;
constexpr u64 INTERRUPT_STATUS    = 0x060;
constexpr u64 INTERRUPT_ACK       = 0x064;
constexpr u64 STATUS              = 0x070;
constexpr u64 QUEUE_DESC_LOW      = 0x080;
constexpr u64 QUEUE_DESC_HIGH     = 0x084;
constexpr u64 DRIVER_DESC_LOW     = 0x090;
constexpr u64 DRIVER_DESC_HIGH    = 0x094;
constexpr u64 DEVICE_DESC_LOW     = 0x0a0;
constexpr u64 DEVICE_DESC_HIGH    = 0x0a4;
constexpr u64 CONFIG_GENERATION   = 0x0fc;
constexpr u64 CONFIG              = 0x100;

// "virt" in little-endian ASCII - the magic a driver checks first.
constexpr u32 MAGIC  = 0x74726976;
constexpr u32 VENDOR = 0x554d4551;   // "QEMU"

// Descriptor flags.
constexpr u16 DESC_NEXT  = 1;   // chained to another descriptor
constexpr u16 DESC_WRITE = 2;   // the device writes this buffer

constexpr u16 AVAIL_F_NO_INTERRUPT = 1;

constexpr u32 FEATURES_OK = 8;

// VIRTIO_F_VERSION_1, feature bit 32: "this device speaks the modern protocol
// rather than the legacy one". A version-2 MMIO device that does not offer it
// is a contradiction, and Linux says so - virtio_finalize_features refuses with
// "device uses modern interface but does not have VIRTIO_F_VERSION_1" and never
// probes the device at all.
//
// The feature space is 64 bits wide but the registers are 32, so a driver reads
// it in two halves: it writes 0 or 1 to DeviceFeaturesSel and then reads
// DeviceFeatures. Bit 32 lives in the upper half, which is why ignoring the
// select register makes it unreachable however many bits the device sets.
//
// xv6 never checks this, which is why the phase 7 boot worked without it.
constexpr u64 F_VERSION_1 = 1ull << 32;

// A descriptor chain longer than the queue is a loop, or a hostile driver.
constexpr u32 MAX_CHAIN = VirtioMmio::QUEUE_MAX;
}  // namespace

VirtioMmio::VirtioMmio(u64 base, u32 device_id, u32 num_queues)
    : base_(base), device_id_(device_id), queues_(num_queues) {}

// --- guest memory ---------------------------------------------------------------

bool VirtioMmio::read_guest_bytes(u64 addr, void* dst, u64 len) const {
    if (bus_ == nullptr) return false;
    if (const Dram* ram = bus_->ram(); ram != nullptr && ram->read_bytes(addr, dst, len)) {
        return true;
    }
    // Not (entirely) in DRAM. Fall back to the bus a byte at a time, which is
    // slow but handles a buffer in a device's address range correctly - and
    // fails cleanly for an address nothing owns.
    u8* out = static_cast<u8*>(dst);
    for (u64 i = 0; i < len; ++i) {
        Result<u64> r = bus_->load(addr + i, 1, AccessType::Load);
        if (!r) return false;
        out[i] = static_cast<u8>(r.value);
    }
    return true;
}

bool VirtioMmio::write_guest_bytes(u64 addr, const void* src, u64 len) {
    if (bus_ == nullptr) return false;
    if (Dram* ram = bus_->ram(); ram != nullptr && ram->write_bytes(addr, src, len)) {
        return true;
    }
    const u8* in = static_cast<const u8*>(src);
    for (u64 i = 0; i < len; ++i) {
        if (!bus_->store(addr + i, 1, in[i])) return false;
    }
    return true;
}

u64 VirtioMmio::read_guest(u64 addr, unsigned size) const {
    if (!bus_) return 0;
    Result<u64> r = bus_->load(addr, size, AccessType::Load);
    return r ? r.value : 0;
}

void VirtioMmio::write_guest(u64 addr, unsigned size, u64 value) {
    if (bus_) bus_->store(addr, size, value);
}

u64 VirtioMmio::segments_length(const std::vector<Segment>& segs, bool device_writes) {
    u64 total = 0;
    for (const Segment& s : segs) {
        if (s.device_writes == device_writes) total += s.len;
    }
    return total;
}

u64 VirtioMmio::read_from_segments(const std::vector<Segment>& segs, u64 skip, u8* dst, u64 len) {
    u64 done = 0;
    for (const Segment& s : segs) {
        if (s.device_writes) continue;            // only buffers the driver filled
        if (skip >= s.len) { skip -= s.len; continue; }
        const u64 n = std::min<u64>(len - done, s.len - skip);
        if (!read_guest_bytes(s.addr + skip, dst + done, n)) break;
        done += n;
        skip = 0;
        if (done == len) break;
    }
    return done;
}

u64 VirtioMmio::write_to_segments(const std::vector<Segment>& segs, u64 skip, const u8* src, u64 len) {
    u64 done = 0;
    for (const Segment& s : segs) {
        if (!s.device_writes) continue;           // only buffers the driver left for us
        if (skip >= s.len) { skip -= s.len; continue; }
        const u64 n = std::min<u64>(len - done, s.len - skip);
        if (!write_guest_bytes(s.addr + skip, src + done, n)) break;
        done += n;
        skip = 0;
        if (done == len) break;
    }
    return done;
}

// --- virtqueues ---------------------------------------------------------------------

bool VirtioMmio::pop_chain(u32 queue, u16& head, std::vector<Segment>& segments) {
    segments.clear();
    if (!queue_ready(queue)) return false;
    Queue& q = queues_[queue];

    // The available ring: {u16 flags; u16 idx; u16 ring[];}
    const u16 avail_idx = static_cast<u16>(read_guest(q.avail + 2, 2));
    if (q.last_avail == avail_idx) return false;

    const u64 slot = q.avail + 4 + static_cast<u64>(q.last_avail % q.num) * 2;
    head = static_cast<u16>(read_guest(slot, 2));
    ++q.last_avail;

    // Descriptor layout: {u64 addr; u32 len; u16 flags; u16 next;} = 16 bytes.
    u16 index = head;
    for (u32 steps = 0; steps < MAX_CHAIN; ++steps) {
        if (index >= q.num) break;               // an index outside the table
        const u64 d = q.desc + static_cast<u64>(index) * 16;
        Segment seg;
        seg.addr = read_guest(d + 0, 8);
        seg.len  = static_cast<u32>(read_guest(d + 8, 4));
        const u16 flags = static_cast<u16>(read_guest(d + 12, 2));
        seg.device_writes = (flags & DESC_WRITE) != 0;
        segments.push_back(seg);
        if ((flags & DESC_NEXT) == 0) break;
        index = static_cast<u16>(read_guest(d + 14, 2));
    }
    return true;
}

void VirtioMmio::push_used(u32 queue, u16 head, u32 written) {
    if (!queue_ready(queue)) return;
    Queue& q = queues_[queue];

    // Publish completion in the used ring:
    //   {u16 flags; u16 idx; struct {u32 id; u32 len;} ring[];}
    const u16 used_idx = static_cast<u16>(read_guest(q.used + 2, 2));
    const u64 slot = q.used + 4 + static_cast<u64>(used_idx % q.num) * 8;
    write_guest(slot + 0, 4, head);          // which chain finished
    write_guest(slot + 4, 4, written);       // how many bytes were transferred

    // The index is bumped only after the entry is fully written, so the driver
    // can never observe a half-published completion.
    write_guest(q.used + 2, 2, static_cast<u16>(used_idx + 1));
}

void VirtioMmio::signal_used(u32 queue) {
    if (!queue_ready(queue)) return;
    const Queue& q = queues_[queue];
    const u16 avail_flags = static_cast<u16>(read_guest(q.avail, 2));
    if (avail_flags & AVAIL_F_NO_INTERRUPT) return;
    raise_interrupt();
}

void VirtioMmio::raise_interrupt() {
    // Bit 0 means "the device used a buffer" - the driver reads this to learn
    // why it was interrupted, then acknowledges it.
    interrupt_status_ |= 1;
    if (plic_) plic_->set_pending(irq_, true);
}

void VirtioMmio::reset() {
    status_ = 0;
    driver_features_ = 0;
    interrupt_status_ = 0;
    for (Queue& q : queues_) q = Queue{};
    if (plic_) plic_->set_pending(irq_, false);
    device_reset();
}

// --- the register file ----------------------------------------------------------------

Result<u64> VirtioMmio::load(u64 offset, unsigned size_bytes) {
    // The device-specific configuration space may be read at any width.
    if (offset >= CONFIG) return Result<u64>::good(config_read(offset - CONFIG, size_bytes));

    if (size_bytes != 4) {
        return Result<u64>::bad(Exception::LoadAccessFault, base_ + offset);
    }

    switch (offset) {
        case MAGIC_VALUE: return Result<u64>::good(MAGIC);
        case VERSION:     return Result<u64>::good(2);   // modern MMIO
        case DEVICE_ID:   return Result<u64>::good(device_id_);
        case VENDOR_ID:   return Result<u64>::good(VENDOR);

        // The half of the feature space the driver most recently selected.
        case DEVICE_FEATURES: {
            const u64 features = device_features() | F_VERSION_1;
            return Result<u64>::good(static_cast<u32>(features >> (device_features_sel_ * 32)));
        }

        case QUEUE_NUM_MAX:
            return Result<u64>::good(selected() ? QUEUE_MAX : 0);
        case QUEUE_READY:
            return Result<u64>::good(selected() && selected()->ready ? 1 : 0);
        case INTERRUPT_STATUS: return Result<u64>::good(interrupt_status_);
        case STATUS:           return Result<u64>::good(status_);
        case CONFIG_GENERATION: return Result<u64>::good(0);
        default:               return Result<u64>::good(0);
    }
}

Status VirtioMmio::store(u64 offset, unsigned size_bytes, u64 value) {
    if (offset >= CONFIG) {
        config_write(offset - CONFIG, size_bytes, value);
        return Status::good();
    }
    if (size_bytes != 4) {
        return Status::bad(Exception::StoreAMOAccessFault, base_ + offset);
    }
    const u32 v = static_cast<u32>(value);
    Queue* q = selected();

    switch (offset) {
        case DEVICE_FEATURES_SEL: device_features_sel_ = (v != 0) ? 1 : 0; break;
        case DRIVER_FEATURES_SEL: driver_features_sel_ = (v != 0) ? 1 : 0; break;

        case DRIVER_FEATURES:
            // The driver acknowledges features the same way it read them, one
            // 32-bit half at a time.
            driver_features_ &= ~(0xffffffffull << (driver_features_sel_ * 32));
            driver_features_ |= (v & 0xffffffffull) << (driver_features_sel_ * 32);
            break;

        case QUEUE_SEL: queue_sel_ = v; break;
        case QUEUE_NUM:
            // The ring size must be at most QUEUE_NUM_MAX; a larger value is a
            // driver bug, and indexing a ring with it would run off the end.
            if (q) q->num = std::min<u32>(v, QUEUE_MAX);
            break;
        case QUEUE_READY:
            if (q) q->ready = (v & 1) != 0;
            break;

        case QUEUE_DESC_LOW:   if (q) q->desc  = (q->desc  & ~0xffff'ffffull) | v; break;
        case QUEUE_DESC_HIGH:  if (q) q->desc  = (q->desc  & 0xffff'ffffull) | (u64(v) << 32); break;
        case DRIVER_DESC_LOW:  if (q) q->avail = (q->avail & ~0xffff'ffffull) | v; break;
        case DRIVER_DESC_HIGH: if (q) q->avail = (q->avail & 0xffff'ffffull) | (u64(v) << 32); break;
        case DEVICE_DESC_LOW:  if (q) q->used  = (q->used  & ~0xffff'ffffull) | v; break;
        case DEVICE_DESC_HIGH: if (q) q->used  = (q->used  & 0xffff'ffffull) | (u64(v) << 32); break;

        case QUEUE_NOTIFY:
            // The driver has queued work. For most devices this is the only
            // register whose write makes the device *do* something.
            if (v < queues_.size()) queue_notify(v);
            break;

        case INTERRUPT_ACK:
            interrupt_status_ &= ~v;
            // Once acknowledged the line drops, so the PLIC stops reporting it.
            if (interrupt_status_ == 0 && plic_) plic_->set_pending(irq_, false);
            break;

        case STATUS:
            if (v == 0) {
                // A driver writing 0 is resetting the device.
                reset();
            } else {
                status_ = v;
                if (v & FEATURES_OK) {
                    // Acknowledge the feature negotiation. The driver reads
                    // STATUS back and gives up if FEATURES_OK did not stick.
                    status_ |= FEATURES_OK;
                }
            }
            break;

        default: break;
    }
    return Status::good();
}
