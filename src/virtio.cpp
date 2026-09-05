#include "virtio.hpp"

#include <cstring>
#include <fstream>

#include "bus.hpp"

namespace {
// MMIO register offsets, from the virtio specification.
constexpr u64 MAGIC_VALUE      = 0x000;
constexpr u64 VERSION          = 0x004;
constexpr u64 DEVICE_ID        = 0x008;
constexpr u64 VENDOR_ID        = 0x00c;
constexpr u64 DEVICE_FEATURES  = 0x010;
constexpr u64 DRIVER_FEATURES  = 0x020;
constexpr u64 QUEUE_SEL        = 0x030;
constexpr u64 QUEUE_NUM_MAX    = 0x034;
constexpr u64 QUEUE_NUM        = 0x038;
constexpr u64 QUEUE_READY      = 0x044;
constexpr u64 QUEUE_NOTIFY     = 0x050;
constexpr u64 INTERRUPT_STATUS = 0x060;
constexpr u64 INTERRUPT_ACK    = 0x064;
constexpr u64 STATUS           = 0x070;
constexpr u64 QUEUE_DESC_LOW   = 0x080;
constexpr u64 QUEUE_DESC_HIGH  = 0x084;
constexpr u64 DRIVER_DESC_LOW  = 0x090;
constexpr u64 DRIVER_DESC_HIGH = 0x094;
constexpr u64 DEVICE_DESC_LOW  = 0x0a0;
constexpr u64 DEVICE_DESC_HIGH = 0x0a4;
constexpr u64 CONFIG           = 0x100;

// "virt" in little-endian ASCII - the magic a driver checks first.
constexpr u32 MAGIC = 0x74726976;
constexpr u32 VENDOR = 0x554d4551;   // "QEMU"
constexpr u32 BLOCK_DEVICE = 2;

// Descriptor flags.
constexpr u16 DESC_NEXT  = 1;   // chained to another descriptor
constexpr u16 DESC_WRITE = 2;   // the device writes this buffer

// Request types.
constexpr u32 BLK_T_IN  = 0;    // read from disk
constexpr u32 BLK_T_OUT = 1;    // write to disk

constexpr u32 FEATURES_OK = 8;
}  // namespace

bool VirtioBlk::load_image(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize len = f.tellg();
    if (len < 0) return false;
    f.seekg(0, std::ios::beg);
    disk_.resize(static_cast<std::size_t>(len));
    if (len > 0 && !f.read(reinterpret_cast<char*>(disk_.data()), len)) return false;
    return true;
}

u64 VirtioBlk::read_guest(u64 addr, unsigned size) const {
    if (!bus_) return 0;
    Result<u64> r = bus_->load(addr, size, AccessType::Load);
    return r ? r.value : 0;
}

void VirtioBlk::write_guest(u64 addr, unsigned size, u64 value) {
    if (bus_) bus_->store(addr, size, value);
}

void VirtioBlk::raise_interrupt() {
    // Bit 0 means "the device used a buffer" - the driver reads this to learn
    // why it was interrupted, then acknowledges it.
    interrupt_status_ |= 1;
    if (plic_) plic_->set_pending(irq_, true);
}

// Walk one descriptor chain and carry out the request it describes.
void VirtioBlk::process_chain(u16 head) {
    // Descriptor layout: {u64 addr; u32 len; u16 flags; u16 next;} = 16 bytes.
    auto desc_addr = [&](u16 i) { return desc_addr_ + static_cast<u64>(i) * 16; };

    // [0] the header. The driver only ever reads from here, so we do too.
    const u16 d0 = head;
    const u64 hdr = read_guest(desc_addr(d0) + 0, 8);
    const u32 type   = static_cast<u32>(read_guest(hdr + 0, 4));
    const u64 sector = read_guest(hdr + 8, 8);

    const u16 d1 = static_cast<u16>(read_guest(desc_addr(d0) + 14, 2));

    // [1] the data buffer.
    const u64 buf     = read_guest(desc_addr(d1) + 0, 8);
    const u32 buf_len = static_cast<u32>(read_guest(desc_addr(d1) + 8, 4));
    const u16 d1_flags = static_cast<u16>(read_guest(desc_addr(d1) + 12, 2));
    const u16 d2 = static_cast<u16>(read_guest(desc_addr(d1) + 14, 2));

    u8 status = 0;   // 0 = OK
    const u64 offset = sector * SECTOR_SIZE;

    if (offset + buf_len > disk_.size()) {
        status = 1;   // IOERR: the request runs off the end of the disk
    } else if (type == BLK_T_IN) {
        // Read: disk -> guest memory. The descriptor must be device-writable;
        // a driver that got this backwards would be asking us to write into a
        // read-only buffer.
        if ((d1_flags & DESC_WRITE) == 0) {
            status = 1;
        } else {
            for (u32 i = 0; i < buf_len; ++i) {
                write_guest(buf + i, 1, disk_[offset + i]);
            }
        }
    } else if (type == BLK_T_OUT) {
        // Write: guest memory -> disk.
        for (u32 i = 0; i < buf_len; ++i) {
            disk_[offset + i] = static_cast<u8>(read_guest(buf + i, 1));
        }
    } else {
        status = 2;   // unsupported request type
    }

    // [2] the status byte, which the device always writes.
    const u64 status_buf = read_guest(desc_addr(d2) + 0, 8);
    write_guest(status_buf, 1, status);

    // Publish completion in the used ring:
    //   {u16 flags; u16 idx; struct {u32 id; u32 len;} ring[];}
    const u16 used_idx = static_cast<u16>(read_guest(used_addr_ + 2, 2));
    const u64 slot = used_addr_ + 4 + static_cast<u64>(used_idx % queue_num_) * 8;
    write_guest(slot + 0, 4, head);          // which chain finished
    write_guest(slot + 4, 4, buf_len);       // how many bytes were transferred

    // The index is bumped only after the entry is fully written, so the driver
    // can never observe a half-published completion.
    write_guest(used_addr_ + 2, 2, static_cast<u16>(used_idx + 1));
}

void VirtioBlk::process_queue() {
    if (!bus_ || queue_num_ == 0) return;

    // The available ring: {u16 flags; u16 idx; u16 ring[];}
    const u16 avail_idx = static_cast<u16>(read_guest(avail_addr_ + 2, 2));

    // Everything between what we last handled and where the driver has got to.
    while (last_avail_ != avail_idx) {
        const u64 slot = avail_addr_ + 4 + static_cast<u64>(last_avail_ % queue_num_) * 2;
        const u16 head = static_cast<u16>(read_guest(slot, 2));
        process_chain(head);
        ++last_avail_;
    }

    raise_interrupt();
}

Result<u64> VirtioBlk::load(u64 offset, unsigned size_bytes) {
    // The config space reports capacity as a 64-bit sector count, which drivers
    // may read in halves.
    if (offset >= CONFIG) {
        const u64 cap = sectors();
        const u64 within = offset - CONFIG;
        if (within == 0 && size_bytes == 8) return Result<u64>::good(cap);
        if (within == 0 && size_bytes == 4) return Result<u64>::good(cap & 0xffff'ffff);
        if (within == 4 && size_bytes == 4) return Result<u64>::good(cap >> 32);
        return Result<u64>::good(0);
    }

    if (size_bytes != 4) {
        return Result<u64>::bad(Exception::LoadAccessFault, VIRTIO_BASE + offset);
    }

    switch (offset) {
        case MAGIC_VALUE:      return Result<u64>::good(MAGIC);
        case VERSION:          return Result<u64>::good(2);   // modern MMIO
        case DEVICE_ID:        return Result<u64>::good(BLOCK_DEVICE);
        case VENDOR_ID:        return Result<u64>::good(VENDOR);

        // We advertise no optional features. Everything xv6 masks off is
        // already absent, and a bare block device needs none of them.
        case DEVICE_FEATURES:  return Result<u64>::good(0);

        case QUEUE_NUM_MAX:    return Result<u64>::good(QUEUE_MAX);
        case QUEUE_READY:      return Result<u64>::good(queue_ready_);
        case INTERRUPT_STATUS: return Result<u64>::good(interrupt_status_);
        case STATUS:           return Result<u64>::good(status_);
        default:               return Result<u64>::good(0);
    }
}

Status VirtioBlk::store(u64 offset, unsigned size_bytes, u64 value) {
    if (size_bytes != 4) {
        return Status::bad(Exception::StoreAMOAccessFault, VIRTIO_BASE + offset);
    }
    const u32 v = static_cast<u32>(value);

    switch (offset) {
        case DRIVER_FEATURES: driver_features_ = v; break;
        case QUEUE_SEL:       queue_sel_ = v; break;
        case QUEUE_NUM:       queue_num_ = v; break;
        case QUEUE_READY:     queue_ready_ = v; break;

        case QUEUE_DESC_LOW:   desc_addr_  = (desc_addr_  & ~0xffff'ffffull) | v; break;
        case QUEUE_DESC_HIGH:  desc_addr_  = (desc_addr_  & 0xffff'ffffull) | (u64(v) << 32); break;
        case DRIVER_DESC_LOW:  avail_addr_ = (avail_addr_ & ~0xffff'ffffull) | v; break;
        case DRIVER_DESC_HIGH: avail_addr_ = (avail_addr_ & 0xffff'ffffull) | (u64(v) << 32); break;
        case DEVICE_DESC_LOW:  used_addr_  = (used_addr_  & ~0xffff'ffffull) | v; break;
        case DEVICE_DESC_HIGH: used_addr_  = (used_addr_  & 0xffff'ffffull) | (u64(v) << 32); break;

        case QUEUE_NOTIFY:
            // The driver has queued work. This is the only register whose write
            // makes the device *do* something.
            process_queue();
            break;

        case INTERRUPT_ACK:
            interrupt_status_ &= ~v;
            // Once acknowledged the line drops, so the PLIC stops reporting it.
            if (interrupt_status_ == 0 && plic_) plic_->set_pending(irq_, false);
            break;

        case STATUS:
            status_ = v;
            // A driver writing 0 is resetting the device.
            if (v == 0) {
                queue_num_ = 0;
                queue_ready_ = 0;
                last_avail_ = 0;
                interrupt_status_ = 0;
                desc_addr_ = avail_addr_ = used_addr_ = 0;
            } else if (v & FEATURES_OK) {
                // Acknowledge the feature negotiation. The driver reads STATUS
                // back and panics if FEATURES_OK did not stick.
                status_ |= FEATURES_OK;
            }
            break;

        default: break;
    }
    return Status::good();
}
