#pragma once

#include "device.hpp"
#include "types.hpp"

// ---------------------------------------------------------------------------
// syscon - the power controller.
//
// A single register. Writing 0x5555 powers the machine off; writing 0x7777
// reboots it. This is the "test finished" device, and it is how the official
// riscv-tests and a Linux `poweroff` both stop the machine.
//
// Before this, the only way an emulated program could stop was to trap with no
// handler installed, which is a debugging affordance rather than something a
// real guest can rely on. Now a guest has a proper way to exit.
// ---------------------------------------------------------------------------
class Syscon : public Device {
public:
    static constexpr u64 POWEROFF = 0x5555;
    static constexpr u64 REBOOT   = 0x7777;

    u64         base() const override { return SYSCON_BASE; }
    u64         size() const override { return 0x1000; }
    const char* name() const override { return "syscon"; }

    Result<u64> load(u64 offset, unsigned size_bytes) override;
    Status      store(u64 offset, unsigned size_bytes, u64 value) override;

    bool poweroff_requested() const { return poweroff_; }
    bool reboot_requested() const { return reboot_; }

    // The value the guest wrote, shifted down: riscv-tests encodes a test's
    // pass/fail result in the upper bits of the poweroff word.
    u64 exit_code() const { return exit_code_; }

private:
    bool poweroff_  = false;
    bool reboot_    = false;
    u64  exit_code_ = 0;
};
