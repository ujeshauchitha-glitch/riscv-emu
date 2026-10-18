#include "dram.hpp"

#include <bit>
#include <cstring>

Dram::Dram(u64 size_bytes)
    : mem_(static_cast<std::size_t>(size_bytes), 0),
      code_pages_(static_cast<std::size_t>((size_bytes + 4095) >> 12), 0) {}

void Dram::mark_code_page(u64 offset) {
    const std::size_t page = static_cast<std::size_t>(offset >> 12);
    if (page >= code_pages_.size() || code_pages_[page]) return;
    code_pages_[page] = 1;
    marked_.push_back(static_cast<u32>(page));
}

void Dram::invalidate_code() {
    ++code_epoch_;
    for (u32 page : marked_) code_pages_[page] = 0;
    marked_.clear();
}

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
    //
    // Guest memory is stored in guest (little-endian) byte order, so on a
    // little-endian host a single memcpy of `size_bytes` bytes into a zeroed
    // u64 produces exactly the value. This replaces a byte-at-a-time loop on
    // what is the hottest path in the emulator.
    u64 value = 0;
    static_assert(std::endian::native == std::endian::little,
                  "Dram assumes a little-endian host");
    std::memcpy(&value, mem_.data() + static_cast<std::size_t>(offset), size_bytes);
    return Result<u64>::good(value);
}

Status Dram::store(u64 offset, unsigned size_bytes, u64 value) {
    if (!in_range(offset, size_bytes)) {
        return Status::bad(Exception::StoreAMOAccessFault, DRAM_BASE + offset);
    }

    note_write(offset, size_bytes);
    std::memcpy(mem_.data() + static_cast<std::size_t>(offset), &value, size_bytes);
    return Status::good();
}

bool Dram::load_image(u64 guest_addr, const std::vector<u8>& blob) {
    if (guest_addr < DRAM_BASE) return false;
    const u64 offset = guest_addr - DRAM_BASE;
    if (offset > mem_.size() || blob.size() > mem_.size() - offset) return false;

    if (!blob.empty()) {
        invalidate_code();
        std::memcpy(mem_.data() + offset, blob.data(), blob.size());
    }
    return true;
}
