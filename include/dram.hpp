#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "device.hpp"
#include "types.hpp"

// ---------------------------------------------------------------------------
// Guest DRAM.
//
// A flat byte array mapped at DRAM_BASE (0x8000_0000). Two things differ from
// the naive version this replaces:
//
//  1. It is *based*. Guest address 0x8000_0000 is element 0, not guest address
//     0. Real RISC-V systems reserve the low addresses for MMIO, and every
//     kernel we want to boot is linked expecting RAM to start at 0x8000_0000.
//
//  2. It is *bounds checked*. Every access is validated against the array
//     length, including the case where the access starts in range but a 4- or
//     8-byte width runs off the end. An out-of-range access returns an access
//     fault to the guest instead of reading host memory it does not own.
//
// The storage is a heap-allocated std::vector rather than a std::array member,
// because 128 MiB is far too large to sit on the stack.
// ---------------------------------------------------------------------------
class Dram final : public Device {
public:
    explicit Dram(u64 size_bytes = DRAM_SIZE_DEFAULT);

    u64         base() const override { return DRAM_BASE; }
    u64         size() const override { return mem_.size(); }
    const char* name() const override { return "dram"; }

    Result<u64> load(u64 offset, unsigned size_bytes) override;
    Status      store(u64 offset, unsigned size_bytes, u64 value) override;

    // Copy a blob of bytes into DRAM at an absolute guest address. Used by the
    // image loaders. Returns false if the blob would not fit.
    bool load_image(u64 guest_addr, const std::vector<u8>& bytes);

    // Direct access for tests and for the ELF loader.
    //
    // The mutable overload counts as a write: whoever holds the reference can
    // change code behind the decode cache's back, so it conservatively
    // invalidates everything the CPUs have cached.
    std::vector<u8>&       bytes() { invalidate_code(); return mem_; }
    const std::vector<u8>& bytes() const { return mem_; }

    // --- decoded-instruction cache support -----------------------------------
    //
    // A CPU that caches decoded instructions must learn when the bytes behind
    // them change. The guest is *supposed* to execute FENCE.I after modifying
    // code, but the cache cannot rely on that: a device DMAing a page of
    // executable code into RAM (virtio-blk reading a binary off disk) never
    // executes FENCE.I, and a missed invalidation there runs stale instructions
    // with no error anywhere.
    //
    // So the CPU tells DRAM which pages it has cached code from, and any store
    // that lands on one of them - from the guest or from a device - bumps an
    // epoch that every cache compares against before trusting an entry.
    // Invalidation is all-or-nothing, which is fine: code pages are written
    // rarely (a program being loaded, a kernel patching itself) compared with
    // how often they are executed.
    u64  code_epoch() const { return code_epoch_; }
    void mark_code_page(u64 offset);
    bool contains_range(u64 offset, unsigned size_bytes) const { return in_range(offset, size_bytes); }

    // Bulk access for devices that read and write guest memory themselves
    // (DMA). `guest_addr` is an absolute physical address; both return false,
    // and touch nothing, if any part of the range lies outside DRAM.
    //
    // This is the fast path for virtio: copying a 4 KiB disk block through the
    // bus a byte at a time is thousands of dispatches, and it would not notice
    // that it was overwriting a page the CPU had cached decoded code from.
    // write_bytes() does the invalidation that Dram::store() does.
    bool read_bytes(u64 guest_addr, void* dst, u64 len) const;
    bool write_bytes(u64 guest_addr, const void* src, u64 len);

private:
    std::vector<u8> mem_;

    u64                  code_epoch_ = 1;
    std::vector<u8>      code_pages_;       // one flag per 4 KiB page
    std::vector<u32>     marked_;           // the pages currently flagged
    void invalidate_code();
    void note_write(u64 offset, u64 size_bytes) {
        if (marked_.empty() || size_bytes == 0) return;
        const u64 first = offset >> 12;
        const u64 last  = (offset + size_bytes - 1) >> 12;
        for (u64 page = first; page <= last; ++page) {
            if (code_pages_[page]) { invalidate_code(); return; }
        }
    }

    // True if [offset, offset + size_bytes) lies entirely inside the array.
    bool in_range(u64 offset, unsigned size_bytes) const;
};
