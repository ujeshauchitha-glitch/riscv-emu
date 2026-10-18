#include "virtio.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "bus.hpp"

namespace {
constexpr u32 BLOCK_DEVICE = 2;

// Request types.
constexpr u32 BLK_T_IN    = 0;    // read from disk
constexpr u32 BLK_T_OUT   = 1;    // write to disk
constexpr u32 BLK_T_FLUSH = 4;    // make earlier writes durable
constexpr u32 BLK_T_GET_ID = 8;   // return a device identifier string

// Status values written to the last byte of the chain.
constexpr u8 BLK_S_OK     = 0;
constexpr u8 BLK_S_IOERR  = 1;
constexpr u8 BLK_S_UNSUPP = 2;

// Feature bits (the low half of the feature space).
constexpr u64 F_SEG_MAX  = 1ull << 2;    // the config space says how many segments a request may have
constexpr u64 F_BLK_SIZE = 1ull << 6;    // the config space says the logical block size
constexpr u64 F_FLUSH    = 1ull << 9;    // the device understands BLK_T_FLUSH

// The most data segments one request may have. Linux sizes its scatter-gather
// lists from this, and then needs the queue to have room for the header and
// status descriptors as well.
constexpr u32 SEG_MAX = 126;

constexpr u64 HEADER_BYTES = 16;
}  // namespace

VirtioBlk::VirtioBlk(u64 base) : VirtioMmio(base, BLOCK_DEVICE, 1) {}

bool VirtioBlk::load_image(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize len = f.tellg();
    if (len < 0) return false;
    f.seekg(0, std::ios::beg);
    disk_.resize(static_cast<std::size_t>(len));
    if (len > 0 && !f.read(reinterpret_cast<char*>(disk_.data()), len)) return false;
    file_mode_ = false;
    return true;
}

bool VirtioBlk::open_image(const std::string& path, bool snapshot) {
    // Snapshot mode never writes the file, so it can be opened read-only - which
    // also lets an image on a read-only medium be used.
    file_.open(path, snapshot ? (std::ios::in | std::ios::binary)
                              : (std::ios::in | std::ios::out | std::ios::binary));
    if (!file_) return false;
    file_.seekg(0, std::ios::end);
    const std::streamoff len = file_.tellg();
    if (len < 0) return false;
    file_size_ = static_cast<u64>(len);
    file_mode_ = true;
    snapshot_  = snapshot;
    overlay_.clear();
    disk_.clear();
    return true;
}

u64 VirtioBlk::device_features() const {
    return F_SEG_MAX | F_BLK_SIZE | F_FLUSH;
}

// struct virtio_blk_config:
//    0  u64 capacity (in 512-byte sectors)
//    8  u32 size_max
//   12  u32 seg_max
//   16  geometry: u16 cylinders, u8 heads, u8 sectors
//   20  u32 blk_size
u64 VirtioBlk::config_read(u64 offset, unsigned size_bytes) {
    u8 config[24] = {};
    const u64 cap = sectors();
    std::memcpy(config + 0, &cap, 8);
    const u32 seg_max = SEG_MAX;
    std::memcpy(config + 12, &seg_max, 4);
    const u32 blk_size = static_cast<u32>(SECTOR_SIZE);
    std::memcpy(config + 20, &blk_size, 4);

    // Any read that lies wholly inside the structure is served from it; anything
    // past it reads as zero.
    if (offset >= sizeof(config) || size_bytes == 0 || size_bytes > 8) return 0;
    u64 value = 0;
    for (unsigned i = 0; i < size_bytes && offset + i < sizeof(config); ++i) {
        value |= static_cast<u64>(config[offset + i]) << (8 * i);
    }
    return value;
}

// --- the backing store ------------------------------------------------------------

bool VirtioBlk::disk_read(u64 offset, u8* dst, u64 len) {
    if (!file_mode_) {
        std::memcpy(dst, disk_.data() + offset, static_cast<std::size_t>(len));
        return true;
    }

    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset));
    file_.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(len));
    if (!file_) { file_.clear(); return false; }

    // The guest's own writes shadow the file in snapshot mode. Walk the sectors
    // the request touches and patch in any it has overwritten.
    if (snapshot_ && !overlay_.empty()) {
        const u64 first = offset / SECTOR_SIZE;
        const u64 last  = (offset + len - 1) / SECTOR_SIZE;
        for (u64 s = first; s <= last; ++s) {
            auto it = overlay_.find(s);
            if (it == overlay_.end()) continue;
            const u64 sector_start = s * SECTOR_SIZE;
            const u64 from = std::max(sector_start, offset);
            const u64 to   = std::min(sector_start + SECTOR_SIZE, offset + len);
            std::memcpy(dst + (from - offset), it->second.data() + (from - sector_start),
                        static_cast<std::size_t>(to - from));
        }
    }
    return true;
}

bool VirtioBlk::disk_write(u64 offset, const u8* src, u64 len) {
    if (!file_mode_) {
        std::memcpy(disk_.data() + offset, src, static_cast<std::size_t>(len));
        return true;
    }

    if (snapshot_) {
        // Copy-on-write at sector granularity: pull in the sector's current
        // contents the first time it is touched, so a partial-sector write
        // keeps the bytes around it.
        u64 pos = 0;
        while (pos < len) {
            const u64 s      = (offset + pos) / SECTOR_SIZE;
            const u64 within = (offset + pos) % SECTOR_SIZE;
            const u64 n      = std::min<u64>(SECTOR_SIZE - within, len - pos);
            auto it = overlay_.find(s);
            if (it == overlay_.end()) {
                std::array<u8, SECTOR_SIZE> sector{};
                if (n != SECTOR_SIZE && !disk_read(s * SECTOR_SIZE, sector.data(), SECTOR_SIZE)) {
                    return false;
                }
                it = overlay_.emplace(s, sector).first;
            }
            std::memcpy(it->second.data() + within, src + pos, static_cast<std::size_t>(n));
            pos += n;
        }
        return true;
    }

    file_.clear();
    file_.seekp(static_cast<std::streamoff>(offset));
    file_.write(reinterpret_cast<const char*>(src), static_cast<std::streamsize>(len));
    if (!file_) { file_.clear(); return false; }
    return true;
}

bool VirtioBlk::disk_flush() {
    if (!file_mode_ || snapshot_) return true;
    file_.flush();
    return static_cast<bool>(file_);
}

// --- requests -----------------------------------------------------------------------

void VirtioBlk::queue_notify(u32 queue) {
    u16 head;
    std::vector<Segment> segs;
    bool any = false;
    while (pop_chain(queue, head, segs)) {
        process_request(queue, head, segs);
        any = true;
    }
    if (any) signal_used(queue);
}

void VirtioBlk::process_request(u32 queue, u16 head, const std::vector<Segment>& segs) {
    // A well-formed chain has a header at the front and a status byte at the
    // back. Without both there is nowhere to report anything, so complete it
    // empty rather than guess.
    if (segs.size() < 2 || segs.front().device_writes || segs.front().len < HEADER_BYTES ||
        !segs.back().device_writes || segs.back().len < 1) {
        push_used(queue, head, 0);
        return;
    }

    u8 header[HEADER_BYTES];
    if (!read_guest_bytes(segs.front().addr, header, HEADER_BYTES)) {
        push_used(queue, head, 0);
        return;
    }
    u32 type;
    u64 sector;
    std::memcpy(&type, header + 0, 4);
    std::memcpy(&sector, header + 8, 8);

    const std::vector<Segment> data(segs.begin() + 1, segs.end() - 1);
    u8  status  = BLK_S_OK;
    u32 written = 0;     // bytes written into device-writable buffers (data only)

    if (type == BLK_T_IN || type == BLK_T_OUT) {
        // Total bytes the request covers, and whether every data buffer points
        // the right way: the device writes them for a read, the driver filled
        // them for a write. A driver that got this backwards would be asking us
        // to write into a read-only buffer.
        const bool is_read = (type == BLK_T_IN);
        u64 total = 0;
        bool direction_ok = true;
        for (const Segment& s : data) {
            total += s.len;
            if (s.device_writes != is_read) direction_ok = false;
        }

        // Bounds-check by comparison before arithmetic.
        //
        // The obvious `sector * 512 + total > size` overflows: a guest that asks
        // for sector 0x7fff_ffff_ffc0_00 makes the product wrap, the check
        // passes, and the loops below index far outside the disk. That is an
        // out-of-bounds host read for BLK_T_IN and an out-of-bounds host *write*
        // for BLK_T_OUT - the device is a bus master reading guest-controlled
        // descriptors, so every field here is hostile input. Testing the sector
        // number against the capacity first means the multiply cannot overflow,
        // and `total` is compared by subtraction.
        const u64 size = size_bytes();
        if (!direction_ok || sector > size / SECTOR_SIZE || total > size ||
            sector * SECTOR_SIZE > size - total) {
            status = BLK_S_IOERR;
        } else if (total != 0) {
            // Move the data one buffer at a time, bounded by a modest staging
            // buffer so a 64 MiB request does not allocate 64 MiB.
            std::vector<u8> chunk(64 * 1024);
            u64 disk_offset = sector * SECTOR_SIZE;
            for (const Segment& s : data) {
                u64 done = 0;
                while (done < s.len && status == BLK_S_OK) {
                    const u64 n = std::min<u64>(chunk.size(), s.len - done);
                    if (is_read) {
                        if (!disk_read(disk_offset, chunk.data(), n) ||
                            !write_guest_bytes(s.addr + done, chunk.data(), n)) {
                            status = BLK_S_IOERR;
                        }
                    } else {
                        if (!read_guest_bytes(s.addr + done, chunk.data(), n) ||
                            !disk_write(disk_offset, chunk.data(), n)) {
                            status = BLK_S_IOERR;
                        }
                    }
                    done += n;
                    disk_offset += n;
                }
                if (status != BLK_S_OK) break;
                if (is_read) written += s.len;
            }
        }
    } else if (type == BLK_T_FLUSH) {
        if (!disk_flush()) status = BLK_S_IOERR;
    } else if (type == BLK_T_GET_ID) {
        static const char id[20] = "riscv-emu-virtio";
        if (!data.empty() && data.front().device_writes) {
            const u32 n = std::min<u32>(data.front().len, sizeof(id));
            if (write_guest_bytes(data.front().addr, id, n)) written = n;
            else status = BLK_S_IOERR;
        } else {
            status = BLK_S_IOERR;
        }
    } else {
        status = BLK_S_UNSUPP;
    }

    // [last] the status byte, which the device always writes.
    write_guest_bytes(segs.back().addr, &status, 1);
    push_used(queue, head, written + 1);
}
