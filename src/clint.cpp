#include "clint.hpp"

namespace {
constexpr u64 MSIP_OFFSET     = 0x0000;
constexpr u64 MTIMECMP_OFFSET = 0x4000;
constexpr u64 MTIME_OFFSET    = 0xbff8;
}  // namespace

void Clint::update(CsrFile& csrs) const {
    if (mtime_ >= mtimecmp_) csrs.raise_interrupt(csr::MIP_MTIP);
    else                     csrs.clear_interrupt(csr::MIP_MTIP);

    if (msip_ & 1) csrs.raise_interrupt(csr::MIP_MSIP);
    else           csrs.clear_interrupt(csr::MIP_MSIP);
}

Result<u64> Clint::load(u64 offset, unsigned size_bytes) {
    if (offset == MSIP_OFFSET && size_bytes == 4) {
        return Result<u64>::good(msip_);
    }
    if (offset == MTIMECMP_OFFSET) {
        if (size_bytes == 8) return Result<u64>::good(mtimecmp_);
        if (size_bytes == 4) return Result<u64>::good(mtimecmp_ & 0xffff'ffff);
    }
    if (offset == MTIMECMP_OFFSET + 4 && size_bytes == 4) {
        return Result<u64>::good(mtimecmp_ >> 32);
    }
    if (offset == MTIME_OFFSET) {
        if (size_bytes == 8) return Result<u64>::good(mtime_);
        if (size_bytes == 4) return Result<u64>::good(mtime_ & 0xffff'ffff);
    }
    if (offset == MTIME_OFFSET + 4 && size_bytes == 4) {
        return Result<u64>::good(mtime_ >> 32);
    }
    return Result<u64>::bad(Exception::LoadAccessFault, CLINT_BASE + offset);
}

Status Clint::store(u64 offset, unsigned size_bytes, u64 value) {
    if (offset == MSIP_OFFSET && size_bytes == 4) {
        // Only bit 0 is implemented.
        msip_ = static_cast<u32>(value) & 1;
        return Status::good();
    }
    if (offset == MTIMECMP_OFFSET) {
        // RV32 kernels write mtimecmp as two 32-bit halves, so both widths have
        // to work.
        if (size_bytes == 8) { mtimecmp_ = value; return Status::good(); }
        if (size_bytes == 4) {
            mtimecmp_ = (mtimecmp_ & ~0xffff'ffffull) | (value & 0xffff'ffff);
            return Status::good();
        }
    }
    if (offset == MTIMECMP_OFFSET + 4 && size_bytes == 4) {
        mtimecmp_ = (mtimecmp_ & 0xffff'ffffull) | (value << 32);
        return Status::good();
    }
    if (offset == MTIME_OFFSET && size_bytes == 8) {
        mtime_ = value;
        return Status::good();
    }
    return Status::bad(Exception::StoreAMOAccessFault, CLINT_BASE + offset);
}
