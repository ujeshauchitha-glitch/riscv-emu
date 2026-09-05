#include "uart.hpp"

#include <ostream>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#define UART_HOST_CONSOLE 1
#endif

namespace {
// Line Status Register bits.
constexpr u8 LSR_DATA_READY   = 1 << 0;  // a byte is waiting to be read
constexpr u8 LSR_THR_EMPTY    = 1 << 5;  // ready to accept a byte to send
constexpr u8 LSR_TRANSMIT_IDLE = 1 << 6; // shift register also empty

// Interrupt Enable Register bits.
constexpr u8 IER_RX_READY = 1 << 0;
constexpr u8 IER_TX_EMPTY = 1 << 1;
}  // namespace

Uart::Uart(std::ostream& out) : out_(out) {}

// ---------------------------------------------------------------------------
// The host console as the receive line.
//
// Three things have to be arranged, and leaving out any one of them breaks the
// guest shell in a different way:
//
//   raw mode      - the terminal must stop buffering a line and stop
//                   interpreting keys itself, or the guest sees nothing until
//                   Enter and never sees Ctrl-C at all.
//   non-blocking  - poll_host_input() runs from the CPU's inner loop; a
//                   blocking read with nothing typed would stop the machine.
//   restore       - raw mode is a property of the terminal, not the process,
//                   so it outlives us. Not putting it back leaves the user's
//                   shell without echo.
//
// When stdin is a pipe or a file rather than a terminal there is nothing to put
// into raw mode; the non-blocking read still works, which is what lets
// `printf 'ls\n' | riscv_emu ...` drive the guest from a script.
// ---------------------------------------------------------------------------
void Uart::attach_host_stdin() {
#ifdef UART_HOST_CONSOLE
    const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        saved_stdin_flags_ = flags;
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }

    if (isatty(STDIN_FILENO)) {
        auto* saved = new termios{};
        if (tcgetattr(STDIN_FILENO, saved) == 0) {
            termios raw = *saved;
            cfmakeraw(&raw);
            // VMIN/VTIME zero: read() returns immediately with whatever is
            // there, which pairs with O_NONBLOCK rather than fighting it.
            raw.c_cc[VMIN]  = 0;
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &raw);
            saved_termios_   = saved;
            restore_termios_ = true;
        } else {
            delete saved;
        }
    }
    host_stdin_ = true;
#endif
}

void Uart::poll_host_input() {
#ifdef UART_HOST_CONSOLE
    if (!host_stdin_ || host_stdin_eof_) return;

    // Bounded so that a large paste cannot starve the guest: the receive queue
    // is drained one byte per RBR read, and a driver with no FIFO handles a few
    // dozen bytes between polls comfortably.
    constexpr u8 CTRL_A = 0x01;

    char buf[64];
    const ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
    if (n > 0) {
        for (ssize_t i = 0; i < n; ++i) {
            const u8 c = static_cast<u8>(buf[i]);

            if (escape_armed_) {
                escape_armed_ = false;
                if (c == 'x' || c == 'X') { exit_requested_ = true; return; }
                if (c == CTRL_A) { rx_.push_back(CTRL_A); continue; }
                // Not a sequence we know. Give the guest both bytes rather than
                // swallowing the prefix - a guest program that wanted Ctrl-A
                // followed by something else should still get it.
                rx_.push_back(CTRL_A);
                rx_.push_back(c);
                continue;
            }

            if (c == CTRL_A) { escape_armed_ = true; continue; }
            rx_.push_back(c);
        }
    } else if (n == 0) {
        // End of input. Stop polling: a closed stdin stays readable and would
        // otherwise return 0 forever, once per poll.
        host_stdin_eof_ = true;
    }
#endif
}

Uart::~Uart() {
#ifdef UART_HOST_CONSOLE
    if (restore_termios_) {
        tcsetattr(STDIN_FILENO, TCSANOW, static_cast<termios*>(saved_termios_));
    }
    delete static_cast<termios*>(saved_termios_);

    // The file flags need putting back as much as the terminal settings do, and
    // for the same reason: O_NONBLOCK is a property of the open file
    // description, which the shell that launched us shares. Leaving it set
    // makes that shell's next read of the terminal fail with EAGAIN -
    // "bash: read error: 0: Resource temporarily unavailable" - long after the
    // emulator has exited.
    if (saved_stdin_flags_ >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, saved_stdin_flags_);
    }
#endif
}

void Uart::feed_input(const std::string& s) {
    for (char c : s) rx_.push_back(static_cast<u8>(c));
}

bool Uart::interrupting() const {
    if ((ier_ & IER_RX_READY) && !rx_.empty()) return true;
    if ((ier_ & IER_TX_EMPTY) && tx_irq_) return true;
    return false;
}

Result<u64> Uart::load(u64 offset, unsigned size_bytes) {
    // Every UART register is one byte wide. A driver reads them with lb.
    if (size_bytes != 1) {
        return Result<u64>::bad(Exception::LoadAccessFault, UART0_BASE + offset);
    }

    switch (offset & 0x7) {
        case 0:
            if (dlab()) return Result<u64>::good(dll_);
            // RBR: take the next queued byte, or zero if nothing is waiting.
            if (rx_.empty()) return Result<u64>::good(0);
            {
                const u8 c = rx_.front();
                rx_.pop_front();
                return Result<u64>::good(c);
            }
        case 1:
            return Result<u64>::good(dlab() ? dlm_ : ier_);
        case 2: {
            // IIR. Bit 0 clear means "an interrupt is pending"; the encoding is
            // inverted, which is a classic source of confusion.
            const u8 iir = interrupting() ? 0xc0 : 0xc1;
            // Reading IIR acknowledges a transmit-empty interrupt. This is what
            // makes THRE an edge rather than a level that never falls.
            tx_irq_ = false;
            return Result<u64>::good(iir);
        }
        case 3: return Result<u64>::good(lcr_);
        case 4: return Result<u64>::good(mcr_);
        case 5: {
            // The transmitter is always ready because a write to THR completes
            // immediately - there is no real serial line to wait for.
            u8 lsr = LSR_THR_EMPTY | LSR_TRANSMIT_IDLE;
            if (!rx_.empty()) lsr |= LSR_DATA_READY;
            return Result<u64>::good(lsr);
        }
        case 6: return Result<u64>::good(0);   // modem status: nothing connected
        case 7: return Result<u64>::good(scr_);
    }
    return Result<u64>::good(0);
}

Status Uart::store(u64 offset, unsigned size_bytes, u64 value) {
    if (size_bytes != 1) {
        return Status::bad(Exception::StoreAMOAccessFault, UART0_BASE + offset);
    }

    const u8 v = static_cast<u8>(value);
    switch (offset & 0x7) {
        case 0:
            if (dlab()) { dll_ = v; return Status::good(); }
            // THR: this is the line that makes a kernel's printf visible.
            out_.put(static_cast<char>(v));
            out_.flush();
            // The byte is sent instantly, so the holding register becomes empty
            // again right away - the edge THRE reports.
            tx_irq_ = true;
            return Status::good();
        case 1:
            if (dlab()) {
                dlm_ = v;
            } else {
                // Enabling the transmit interrupt does not by itself assert it:
                // that needs a byte to have been sent.
                ier_ = v;
            }
            return Status::good();
        case 2: return Status::good();   // FCR: we have no FIFOs to configure
        case 3: lcr_ = v; return Status::good();
        case 4: mcr_ = v; return Status::good();
        case 5: return Status::good();   // LSR is read-only
        case 6: return Status::good();
        case 7: scr_ = v; return Status::good();
    }
    return Status::good();
}
