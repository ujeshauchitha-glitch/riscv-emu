#pragma once

#include <chrono>

#include "device.hpp"
#include "plic.hpp"
#include "types.hpp"

// ---------------------------------------------------------------------------
// A real-time clock: the machine's idea of the wall-clock date.
//
// This is the "goldfish" RTC that QEMU's virt machine uses and that Linux has a
// driver for (CONFIG_RTC_DRV_GOLDFISH), so a stock kernel finds it from the
// device tree and sets the system time from it. Without a clock the guest boots
// into 1970, and anything that checks a date - apt refusing a repository whose
// Release file is "from the future", TLS refusing a certificate that is "not
// yet valid" - fails in a way that looks nothing like a clock problem.
//
// Registers (all 32-bit):
//
//   0x00 TIME_LOW         nanoseconds since the Unix epoch, low half. Reading it
//                         latches the high half so the pair is consistent.
//   0x04 TIME_HIGH        the latched high half
//   0x08 ALARM_LOW        alarm time, low half (writing arms the alarm)
//   0x0c ALARM_HIGH       alarm time, high half (write first)
//   0x10 IRQ_ENABLED      1 to raise an interrupt when the alarm expires
//   0x14 CLEAR_ALARM      any write disarms the alarm
//   0x18 ALARM_STATUS     1 while the alarm is armed
//   0x1c CLEAR_INTERRUPT  any write lowers the interrupt line
// ---------------------------------------------------------------------------
class Rtc : public Device {
public:
    u64         base() const override { return RTC_BASE; }
    u64         size() const override { return 0x1000; }
    const char* name() const override { return "rtc"; }

    Result<u64> load(u64 offset, unsigned size_bytes) override;
    Status      store(u64 offset, unsigned size_bytes, u64 value) override;
    void        poll() override;

    void attach(Plic* plic, u32 irq) { plic_ = plic; irq_ = irq; }

    // Pin the clock to a fixed instant (nanoseconds since the epoch) instead of
    // following the host. It then advances only when `advance` is called. A
    // reproducible date is what a deterministic test or a recorded run wants.
    void set_fixed_time(u64 ns) { fixed_ = true; fixed_ns_ = ns; }
    void advance(u64 ns) { fixed_ns_ += ns; }

    u64 now_ns() const;

private:
    Plic* plic_ = nullptr;
    u32   irq_  = 0;

    bool fixed_    = false;
    u64  fixed_ns_ = 0;

    u64  latched_high_ = 0;
    u64  alarm_ns_     = 0;
    bool alarm_armed_  = false;
    bool irq_enabled_  = false;
    bool irq_raised_   = false;
};
