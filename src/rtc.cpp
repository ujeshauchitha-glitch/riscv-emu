#include "rtc.hpp"

namespace {
constexpr u64 TIME_LOW        = 0x00;
constexpr u64 TIME_HIGH       = 0x04;
constexpr u64 ALARM_LOW       = 0x08;
constexpr u64 ALARM_HIGH      = 0x0c;
constexpr u64 IRQ_ENABLED     = 0x10;
constexpr u64 CLEAR_ALARM     = 0x14;
constexpr u64 ALARM_STATUS    = 0x18;
constexpr u64 CLEAR_INTERRUPT = 0x1c;
}  // namespace

u64 Rtc::now_ns() const {
    if (fixed_) return fixed_ns_;
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count());
}

Result<u64> Rtc::load(u64 offset, unsigned size_bytes) {
    if (size_bytes != 4) return Result<u64>::bad(Exception::LoadAccessFault, RTC_BASE + offset);

    switch (offset) {
        case TIME_LOW: {
            // Latch the whole 64-bit time so that reading the two halves one after
            // the other cannot straddle a carry from low into high.
            const u64 t = now_ns();
            latched_high_ = t >> 32;
            return Result<u64>::good(t & 0xffff'ffff);
        }
        case TIME_HIGH:    return Result<u64>::good(latched_high_);
        case ALARM_LOW:    return Result<u64>::good(alarm_ns_ & 0xffff'ffff);
        case ALARM_HIGH:   return Result<u64>::good(alarm_ns_ >> 32);
        case IRQ_ENABLED:  return Result<u64>::good(irq_enabled_ ? 1 : 0);
        case ALARM_STATUS: return Result<u64>::good(alarm_armed_ ? 1 : 0);
        default:           return Result<u64>::good(0);
    }
}

Status Rtc::store(u64 offset, unsigned size_bytes, u64 value) {
    if (size_bytes != 4) return Status::bad(Exception::StoreAMOAccessFault, RTC_BASE + offset);
    const u32 v = static_cast<u32>(value);

    switch (offset) {
        case ALARM_HIGH:
            alarm_ns_ = (alarm_ns_ & 0xffff'ffffull) | (static_cast<u64>(v) << 32);
            break;
        case ALARM_LOW:
            // The low half is written last, which is what arms the alarm.
            alarm_ns_ = (alarm_ns_ & ~0xffff'ffffull) | v;
            alarm_armed_ = true;
            break;
        case IRQ_ENABLED:
            irq_enabled_ = (v & 1) != 0;
            break;
        case CLEAR_ALARM:
            alarm_armed_ = false;
            break;
        case CLEAR_INTERRUPT:
            irq_raised_ = false;
            if (plic_) plic_->set_pending(irq_, false);
            break;
        default: break;
    }
    return Status::good();
}

void Rtc::poll() {
    if (alarm_armed_ && irq_enabled_ && !irq_raised_ && now_ns() >= alarm_ns_) {
        alarm_armed_ = false;
        irq_raised_  = true;
        if (plic_) plic_->set_pending(irq_, true);
    }
}
