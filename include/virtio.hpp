#pragma once

#include <array>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "plic.hpp"
#include "types.hpp"
#include "virtio_mmio.hpp"

class Bus;

// ---------------------------------------------------------------------------
// virtio-mmio block device.
//
// The disk. This is what lets xv6 have a filesystem, and what holds a Debian
// root filesystem.
//
// A block request chain is a header, any number of data buffers, and a status
// byte:
//
//   [0]    the header  {type, reserved, sector}   device reads
//   [1..n] the data    512-byte multiples          device reads (write) or writes (read)
//   [n+1]  the status  one byte                    device writes: 0 means OK
//
// xv6 sends exactly three descriptors. Linux sends one data buffer per
// physically contiguous run of pages, so a large request has several - the
// device must walk the whole chain rather than assume a fixed shape.
//
// **Where the bytes live.** Two modes:
//
//   in memory  load_image() reads the whole file into a vector. Simple, and what
//              the tests and xv6's small filesystem use.
//   on disk    open_image() keeps the file open and reads and writes it in
//              place, so a multi-gigabyte root filesystem costs no RAM and
//              guest writes persist. With `snapshot` set, writes are held in a
//              sector overlay and discarded on exit - the image on the host is
//              never modified, which is what you want for experimenting.
// ---------------------------------------------------------------------------
class VirtioBlk : public VirtioMmio {
public:
    static constexpr u64 SECTOR_SIZE = 512;

    explicit VirtioBlk(u64 base = VIRTIO_BASE);

    const char* name() const override { return "virtio-blk"; }

    // Back the disk with a file's contents, loaded into memory. Returns false if
    // unreadable.
    bool load_image(const std::string& path);

    // Back the disk with a file kept open on the host. See the class comment.
    bool open_image(const std::string& path, bool snapshot);

    // Capacity in 512-byte sectors, which is what the config space reports.
    u64 sectors() const { return size_bytes() / SECTOR_SIZE; }

    // The in-memory disk. Only meaningful in memory mode.
    const std::vector<u8>& data() const { return disk_; }
    std::vector<u8>&       data() { return disk_; }

protected:
    u64  device_features() const override;
    u64  config_read(u64 offset, unsigned size_bytes) override;
    void queue_notify(u32 queue) override;

private:
    std::vector<u8> disk_;

    bool          file_mode_ = false;
    bool          snapshot_  = false;
    std::fstream  file_;
    u64           file_size_ = 0;
    // Snapshot mode: sectors the guest has written, kept instead of the file's.
    std::unordered_map<u64, std::array<u8, SECTOR_SIZE>> overlay_;

    u64  size_bytes() const { return file_mode_ ? file_size_ : disk_.size(); }
    bool disk_read(u64 offset, u8* dst, u64 len);
    bool disk_write(u64 offset, const u8* src, u64 len);
    bool disk_flush();

    void process_request(u32 queue, u16 head, const std::vector<Segment>& segs);
};
