#pragma once

#include <deque>
#include <iosfwd>
#include <string>

#include "device.hpp"
#include "types.hpp"

// ---------------------------------------------------------------------------
// NS16550A UART - the console.
//
// This is the first device that makes the emulator observable from the outside.
// Until now a guest could compute but not communicate; a byte written to
// address 0x1000_0000 now appears on the host's stdout. That is the whole
// mechanism behind a kernel's boot messages.
//
// The NS16550A is a 1980s serial chip, and it is what QEMU's `virt` machine
// exposes, so xv6 and Linux already have drivers for it. Eight byte-wide
// registers:
//
//   0  RBR/THR   receive buffer (read) / transmit holding (write)
//   1  IER       interrupt enable
//   2  IIR/FCR   interrupt identification (read) / FIFO control (write)
//   3  LCR       line control - bit 7 is DLAB
//   4  MCR       modem control
//   5  LSR       line status - bit 0 "data ready", bit 5 "transmit ready"
//   6  MSR       modem status
//   7  SCR       scratch
//
// The DLAB bit in LCR is a bank-switch: when set, registers 0 and 1 become the
// low and high halves of the baud-rate divisor instead. A driver sets DLAB,
// writes the divisor, then clears it. Emulating that matters because a real
// driver's initialisation sequence does exactly this, and a UART that ignored
// DLAB would take the divisor bytes as characters to print.
// ---------------------------------------------------------------------------
class Uart : public Device {
public:
    // Output goes to `out`; tests point this at a stringstream.
    explicit Uart(std::ostream& out);

    u64         base() const override { return UART0_BASE; }
    u64         size() const override { return 0x100; }
    const char* name() const override { return "uart0"; }

    Result<u64> load(u64 offset, unsigned size_bytes) override;
    Status      store(u64 offset, unsigned size_bytes, u64 value) override;

    // Queue bytes as if they had been typed. main() feeds this from stdin;
    // tests call it directly.
    void feed_input(const std::string& s);

    // True when the guest should see an interrupt. Wired to the PLIC in phase 7;
    // exposed now so the logic lives with the device.
    bool interrupting() const;

private:
    std::ostream&   out_;
    std::deque<u8>  rx_;

    u8 ier_ = 0;   // interrupt enable
    u8 lcr_ = 0;   // line control (bit 7 = DLAB)
    u8 mcr_ = 0;   // modem control
    u8 scr_ = 0;   // scratch
    u8 dll_ = 0;   // divisor latch, low
    u8 dlm_ = 0;   // divisor latch, high

    bool dlab() const { return (lcr_ & 0x80) != 0; }
};
