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
class Dram : public Device {
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
    std::vector<u8>&       bytes() { return mem_; }
    const std::vector<u8>& bytes() const { return mem_; }

private:
    std::vector<u8> mem_;

    // True if [offset, offset + size_bytes) lies entirely inside the array.
    bool in_range(u64 offset, unsigned size_bytes) const;
};
