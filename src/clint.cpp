#include "clint.hpp"

namespace {
constexpr u64 MSIP_OFFSET     = 0x0000;
constexpr u64 MTIMECMP_OFFSET = 0x4000;
constexpr u64 MTIME_OFFSET    = 0xbff8;
}  // namespace

void Clint::update(CsrFile& csrs) const {
    // The machine timer, compared against the CLINT's own mtimecmp.
    if (mtime_ >= mtimecmp_) csrs.raise_interrupt(csr::MIP_MTIP);
    else                     csrs.clear_interrupt(csr::MIP_MTIP);

    if (msip_ & 1) csrs.raise_interrupt(csr::MIP_MSIP);
    else           csrs.clear_interrupt(csr::MIP_MSIP);

    // The supervisor timer (Sstc). When menvcfg.STCE lets S-mode have its own
    // compare register, STIP is driven by it directly - so a kernel arms its
    // next tick with one CSR write instead of calling into firmware. While STCE
    // is clear, STIP belongs to software (M-mode posts it), so leave it alone.
    if (csrs.sstc_enabled()) {
        if (mtime_ >= csrs.read(csr::STIMECMP)) csrs.raise_interrupt(csr::MIP_STIP);
        else                                    csrs.clear_interrupt(csr::MIP_STIP);
    } else if (sbi_timer_) {
        // No Sstc, but a supervisor armed this deadline through SBI - so the
        // machine-timer expiry is forwarded as a *supervisor* timer interrupt,
        // which is exactly what M-mode firmware does on real hardware. Without
        // this the deadline expires into a bit no supervisor can enable, and
        // the kernel's clock never advances.
        if (mtime_ >= mtimecmp_) csrs.raise_interrupt(csr::MIP_STIP);
        else                     csrs.clear_interrupt(csr::MIP_STIP);
    }
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
