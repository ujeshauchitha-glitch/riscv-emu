#include "clint.hpp"

namespace {
constexpr u64 MSIP_OFFSET     = 0x0000;   // + hart * 4
constexpr u64 MTIMECMP_OFFSET = 0x4000;   // + hart * 8
constexpr u64 MTIME_OFFSET    = 0xbff8;

constexpr u32 clamp_harts(u32 harts) {
    return harts == 0 ? 1 : (harts > Clint::MAX_HARTS ? Clint::MAX_HARTS : harts);
}
}  // namespace

Clint::Clint(u32 harts)
    : harts_(clamp_harts(harts)),
      sbi_timer_(harts_, 0),
      mtimecmp_(harts_, 0),
      msip_(harts_, 0) {}

void Clint::update(CsrFile& csrs, u32 hart) const {
    if (hart >= harts_) return;

    // The machine timer, compared against this hart's own mtimecmp.
    if (mtime_ >= mtimecmp_[hart]) csrs.raise_interrupt(csr::MIP_MTIP);
    else                           csrs.clear_interrupt(csr::MIP_MTIP);

    if (msip_[hart] & 1) csrs.raise_interrupt(csr::MIP_MSIP);
    else                 csrs.clear_interrupt(csr::MIP_MSIP);

    // The supervisor timer (Sstc). When menvcfg.STCE lets S-mode have its own
    // compare register, STIP is driven by it directly - so a kernel arms its
    // next tick with one CSR write instead of calling into firmware. While STCE
    // is clear, STIP belongs to software (M-mode posts it), so leave it alone.
    if (csrs.sstc_enabled()) {
        if (mtime_ >= csrs.read(csr::STIMECMP)) csrs.raise_interrupt(csr::MIP_STIP);
        else                                    csrs.clear_interrupt(csr::MIP_STIP);
    } else if (sbi_timer_[hart]) {
        // No Sstc, but a supervisor armed this deadline through SBI - so the
        // machine-timer expiry is forwarded as a *supervisor* timer interrupt,
        // which is exactly what M-mode firmware does on real hardware. Without
        // this the deadline expires into a bit no supervisor can enable, and
        // the kernel's clock never advances.
        if (mtime_ >= mtimecmp_[hart]) csrs.raise_interrupt(csr::MIP_STIP);
        else                           csrs.clear_interrupt(csr::MIP_STIP);
    }
}

Result<u64> Clint::load(u64 offset, unsigned size_bytes) {
    if (offset >= MSIP_OFFSET && offset < MSIP_OFFSET + 4ull * MAX_HARTS && size_bytes == 4) {
        const u64 hart = (offset - MSIP_OFFSET) / 4;
        // msip registers for harts that do not exist read as zero.
        return Result<u64>::good(hart < harts_ ? msip_[hart] : 0);
    }
    if (offset >= MTIMECMP_OFFSET && offset < MTIMECMP_OFFSET + 8ull * MAX_HARTS) {
        const u64 hart = (offset - MTIMECMP_OFFSET) / 8;
        const u64 word = (offset - MTIMECMP_OFFSET) % 8;
        const u64 v = hart < harts_ ? mtimecmp_[hart] : ~0ull;
        if (word == 0 && size_bytes == 8) return Result<u64>::good(v);
        if (word == 0 && size_bytes == 4) return Result<u64>::good(v & 0xffff'ffff);
        if (word == 4 && size_bytes == 4) return Result<u64>::good(v >> 32);
    }
    if (offset == MTIME_OFFSET) {
        if (size_bytes == 8) return Result<u64>::good(mtime_now());
        if (size_bytes == 4) return Result<u64>::good(mtime_now() & 0xffff'ffff);
    }
    if (offset == MTIME_OFFSET + 4 && size_bytes == 4) {
        return Result<u64>::good(mtime_now() >> 32);
    }
    return Result<u64>::bad(Exception::LoadAccessFault, CLINT_BASE + offset);
}

Status Clint::store(u64 offset, unsigned size_bytes, u64 value) {
    if (offset >= MSIP_OFFSET && offset < MSIP_OFFSET + 4ull * MAX_HARTS && size_bytes == 4) {
        const u64 hart = (offset - MSIP_OFFSET) / 4;
        // Only bit 0 is implemented. Writes to harts that do not exist are
        // ignored rather than faulted, as on real hardware.
        if (hart < harts_) msip_[hart] = static_cast<u32>(value) & 1;
        return Status::good();
    }
    if (offset >= MTIMECMP_OFFSET && offset < MTIMECMP_OFFSET + 8ull * MAX_HARTS) {
        const u64 hart = (offset - MTIMECMP_OFFSET) / 8;
        const u64 word = (offset - MTIMECMP_OFFSET) % 8;
        const bool ok = (word == 0 && (size_bytes == 8 || size_bytes == 4)) ||
                        (word == 4 && size_bytes == 4);
        if (!ok) return Status::bad(Exception::StoreAMOAccessFault, CLINT_BASE + offset);
        if (hart >= harts_) return Status::good();
        // RV32 kernels write mtimecmp as two 32-bit halves, so both widths have
        // to work.
        if (word == 0 && size_bytes == 8) {
            mtimecmp_[hart] = value;
        } else if (word == 0) {
            mtimecmp_[hart] = (mtimecmp_[hart] & ~0xffff'ffffull) | (value & 0xffff'ffff);
        } else {
            mtimecmp_[hart] = (mtimecmp_[hart] & 0xffff'ffffull) | (value << 32);
        }
        return Status::good();
    }
    if (offset == MTIME_OFFSET && size_bytes == 8) {
        mtime_ = value;
        return Status::good();
    }
    return Status::bad(Exception::StoreAMOAccessFault, CLINT_BASE + offset);
}
