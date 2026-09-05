#pragma once

#include "csr.hpp"
#include "device.hpp"
#include "types.hpp"

// ---------------------------------------------------------------------------
// CLINT - Core Local Interruptor.
//
// Provides the two interrupt sources every RISC-V system needs before it can
// run a scheduler:
//
//   msip      software interrupts - one hart poking another (and itself)
//   mtime     a free-running counter
//   mtimecmp  when mtime reaches it, a timer interrupt fires
//
// The timer is what makes preemptive multitasking possible. A kernel sets
// mtimecmp to "now + one tick", and when the interrupt arrives it runs the
// scheduler and sets the next deadline. Without it a runaway user process would
// never give the CPU back.
//
// Layout (matching the QEMU `virt` machine):
//
//   0x0000 + hart*4   msip
//   0x4000 + hart*8   mtimecmp
//   0xBFF8            mtime
//
// mtime advances with retired instructions rather than wall-clock time. That
// makes runs reproducible - the same program takes the same interrupt at the
// same instruction every time - which matters enormously when debugging why a
// kernel wedged on its third context switch.
// ---------------------------------------------------------------------------
class Clint : public Device {
public:
    u64         base() const override { return CLINT_BASE; }
    u64         size() const override { return 0x10000; }
    const char* name() const override { return "clint"; }

    Result<u64> load(u64 offset, unsigned size_bytes) override;
    Status      store(u64 offset, unsigned size_bytes, u64 value) override;

    // Advance the timer. Called once per retired instruction.
    void tick() { mtime_ += ticks_per_instruction; }

    // Drive MTIP and MSIP in mip to match the device's state. Interrupt pending
    // bits are owned by the device, not by software: a guest cannot clear MTIP
    // by writing mip, only by moving mtimecmp forward.
    void update(CsrFile& csrs) const;

    u64 mtime() const { return mtime_; }

    // Set the timer deadline on behalf of a supervisor, and clear the pending
    // timer interrupt.
    //
    // mtimecmp is a machine-mode register, so a kernel running in S-mode cannot
    // write it - which is the entire reason SBI has a set_timer call. Clearing
    // MTIP here is required by the SBI specification and is easy to miss: the
    // interrupt that just fired is still pending, and if it is not cleared when
    // the new deadline is set the kernel re-enters its timer handler
    // immediately and never makes progress.
    void set_timer(u64 deadline, CsrFile& csrs) {
        mtimecmp_ = deadline;
        csrs.clear_interrupt(csr::MIP_MTIP);
    }

    // How fast the clock runs relative to instruction retirement. Larger values
    // make each timer tick cover more instructions.
    u64 ticks_per_instruction = 1;

private:
    u64 mtime_    = 0;
    // Reset value 0 means the timer is already "expired", so MTIP asserts
    // immediately - which is exactly what real hardware does, and why every
    // kernel sets mtimecmp before enabling the timer interrupt.
    u64 mtimecmp_ = 0;
    u32 msip_     = 0;
};
