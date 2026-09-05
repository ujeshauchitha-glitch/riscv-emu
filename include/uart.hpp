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

    // Queue bytes as if they had been typed. Tests call this directly.
    void feed_input(const std::string& s);

    // Make the host's standard input the receive line.
    //
    // Without this the console is half a console: a guest can print but never
    // be typed at, so xv6 boots to a shell prompt that cannot be used. When
    // stdin is a terminal it is switched to raw mode, because a shell wants
    // each keystroke as it is struck rather than a line at a time, and it wants
    // Ctrl-C delivered to the *guest* rather than killing the emulator. The
    // previous terminal settings are restored by the destructor.
    void attach_host_stdin();

    // Drain whatever the host has typed into the receive queue. Non-blocking:
    // an empty stdin costs one failed read, which is why the CPU can afford to
    // call it periodically. No-op unless attach_host_stdin() succeeded.
    void poll_host_input();

    ~Uart() override;

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

    // Whether the "transmit holding register empty" interrupt is asserted.
    //
    // This must be a latched event, not a standing condition. Our transmitter
    // completes a write instantly, so "the register is empty" is true forever -
    // and reporting that as an interrupt whenever the driver enables THRE means
    // the line never drops. A kernel that enables it then spends every
    // instruction re-entering its handler and makes no progress at all.
    //
    // Real hardware asserts THRE when the register *becomes* empty, and clears
    // it when the driver reads IIR (or writes another byte). That is an edge,
    // and this flag models it.
    bool tx_irq_ = false;

    bool dlab() const { return (lcr_ & 0x80) != 0; }

    // Host console state. `saved_termios_` holds the settings to put back, and
    // is only meaningful when `restore_termios_` is true (stdin was a tty).
    bool  host_stdin_    = false;
    bool  host_stdin_eof_ = false;
    bool  restore_termios_ = false;
    void* saved_termios_ = nullptr;  // struct termios, hidden to keep the header clean
};
