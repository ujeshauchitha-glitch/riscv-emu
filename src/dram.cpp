#include "dram.hpp"

#include <cstring>

Dram::Dram(u64 size_bytes) : mem_(static_cast<std::size_t>(size_bytes), 0) {}

bool Dram::in_range(u64 offset, unsigned size_bytes) const {
    const u64 len = mem_.size();
    // Guard against an access that starts in range but whose width runs off the
    // end. Comparing `offset > len - size_bytes` (rather than
    // `offset + size_bytes > len`) cannot overflow.
    if (offset >= len) return false;
    return size_bytes <= len - offset;
}

Result<u64> Dram::load(u64 offset, unsigned size_bytes) {
    if (!in_range(offset, size_bytes)) {
        // Report the absolute guest address in tval — that is what a trap
        // handler wants to see, not an offset into our internal array.
        return Result<u64>::bad(Exception::LoadAccessFault, DRAM_BASE + offset);
    }

    // RISC-V is little-endian: the byte at the lowest address is the least
    // significant byte of the value.
    u64 value = 0;
    for (unsigned i = 0; i < size_bytes; ++i) {
        value |= static_cast<u64>(mem_[static_cast<std::size_t>(offset) + i]) << (8 * i);
    }
    return Result<u64>::good(value);
}

Status Dram::store(u64 offset, unsigned size_bytes, u64 value) {
    if (!in_range(offset, size_bytes)) {
        return Status::bad(Exception::StoreAMOAccessFault, DRAM_BASE + offset);
    }

    for (unsigned i = 0; i < size_bytes; ++i) {
        mem_[static_cast<std::size_t>(offset) + i] = static_cast<u8>((value >> (8 * i)) & 0xff);
    }
    return Status::good();
}

bool Dram::load_image(u64 guest_addr, const std::vector<u8>& blob) {
    if (guest_addr < DRAM_BASE) return false;
    const u64 offset = guest_addr - DRAM_BASE;
    if (offset > mem_.size() || blob.size() > mem_.size() - offset) return false;

    if (!blob.empty()) {
        std::memcpy(mem_.data() + offset, blob.data(), blob.size());
    }
    return true;
}
