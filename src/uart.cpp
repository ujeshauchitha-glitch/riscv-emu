#include "uart.hpp"

#include <ostream>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#define UART_HOST_CONSOLE 1
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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
#ifdef _WIN32
namespace {
// What to put back when the emulator exits.
struct WinConsole {
    HANDLE in = INVALID_HANDLE_VALUE;
    HANDLE out = INVALID_HANDLE_VALUE;
    bool   in_is_console = false;
    bool   out_is_console = false;
    DWORD  saved_in_mode = 0;
    DWORD  saved_out_mode = 0;
    UINT   saved_out_cp = 0;
    wchar_t pending_high_surrogate = 0;
};

void append_utf8(std::string& out, unsigned codepoint) {
    if (codepoint < 0x80) {
        out += static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
        out += static_cast<char>(0xc0 | (codepoint >> 6));
        out += static_cast<char>(0x80 | (codepoint & 0x3f));
    } else if (codepoint < 0x10000) {
        out += static_cast<char>(0xe0 | (codepoint >> 12));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (codepoint & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (codepoint >> 18));
        out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (codepoint & 0x3f));
    }
}

// A key the console reports as a virtual-key code rather than a character: the
// arrows and friends. A Linux terminal sends these as escape sequences, so that
// is what the guest has to receive for its shell's line editing to work.
const char* escape_for(WORD vk) {
    switch (vk) {
        case VK_UP:     return "\x1b[A";
        case VK_DOWN:   return "\x1b[B";
        case VK_RIGHT:  return "\x1b[C";
        case VK_LEFT:   return "\x1b[D";
        case VK_HOME:   return "\x1b[H";
        case VK_END:    return "\x1b[F";
        case VK_INSERT: return "\x1b[2~";
        case VK_DELETE: return "\x1b[3~";
        case VK_PRIOR:  return "\x1b[5~";
        case VK_NEXT:   return "\x1b[6~";
        default:        return nullptr;
    }
}
}  // namespace
#endif

void Uart::attach_host_stdin() {
#if defined(_WIN32)
    auto* c = new WinConsole{};
    c->in  = GetStdHandle(STD_INPUT_HANDLE);
    c->out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (c->in == nullptr || c->in == INVALID_HANDLE_VALUE) { delete c; return; }

    // Input. Turn off everything the console does for itself - line editing,
    // echo, Ctrl-C handling - so each keystroke reaches the guest as it is
    // struck, and Ctrl-C reaches the guest's shell rather than killing us. (The
    // Ctrl-A X escape is how you leave.)
    DWORD mode = 0;
    if (GetConsoleMode(c->in, &mode)) {
        c->in_is_console = true;
        c->saved_in_mode = mode;
        SetConsoleMode(c->in, 0);
    }

    // Output. A Linux console speaks ANSI escape sequences - colours, cursor
    // movement, clear-screen - and UTF-8; the Windows console understands both,
    // but only once asked.
    if (c->out != nullptr && c->out != INVALID_HANDLE_VALUE && GetConsoleMode(c->out, &mode)) {
        c->out_is_console = true;
        c->saved_out_mode = mode;
        c->saved_out_cp = GetConsoleOutputCP();
        SetConsoleMode(c->out, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        SetConsoleOutputCP(CP_UTF8);
    }
    win_console_ = c;
    host_stdin_ = true;
#elif defined(UART_HOST_CONSOLE)
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

long Uart::read_host(u8* buf, std::size_t capacity) {
#if defined(_WIN32)
    auto* c = static_cast<WinConsole*>(win_console_);
    if (c == nullptr) return -1;

    if (c->in_is_console) {
        DWORD events = 0;
        if (!GetNumberOfConsoleInputEvents(c->in, &events) || events == 0) return 0;

        INPUT_RECORD records[32];
        DWORD got = 0;
        if (!ReadConsoleInputW(c->in, records, events < 32 ? events : 32, &got)) return -1;

        std::string bytes;
        for (DWORD i = 0; i < got; ++i) {
            const INPUT_RECORD& r = records[i];
            if (r.EventType != KEY_EVENT || !r.Event.KeyEvent.bKeyDown) continue;
            const KEY_EVENT_RECORD& k = r.Event.KeyEvent;
            const unsigned repeat = k.wRepeatCount ? k.wRepeatCount : 1;

            for (unsigned n = 0; n < repeat; ++n) {
                const wchar_t ch = k.uChar.UnicodeChar;
                if (ch == 0) {
                    if (const char* esc = escape_for(k.wVirtualKeyCode)) bytes += esc;
                } else if (ch >= 0xd800 && ch <= 0xdbff) {
                    c->pending_high_surrogate = ch;                     // wait for its partner
                } else if (ch >= 0xdc00 && ch <= 0xdfff && c->pending_high_surrogate) {
                    const unsigned cp = 0x10000 + ((c->pending_high_surrogate - 0xd800) << 10) + (ch - 0xdc00);
                    c->pending_high_surrogate = 0;
                    append_utf8(bytes, cp);
                } else if (ch == 0x08) {
                    bytes += '\x7f';       // Backspace: a Linux tty erases with DEL, not BS
                } else {
                    append_utf8(bytes, ch);
                }
            }
        }
        const std::size_t n = bytes.size() < capacity ? bytes.size() : capacity;
        for (std::size_t i = 0; i < n; ++i) buf[i] = static_cast<u8>(bytes[i]);
        // Bytes beyond `capacity` are dropped; the caller's buffer is generous
        // and a single poll never sees more than a few keystrokes.
        return static_cast<long>(n);
    }

    // Redirected input: a pipe, or a file.
    DWORD available = 0;
    if (PeekNamedPipe(c->in, nullptr, 0, nullptr, &available, nullptr)) {
        if (available == 0) return 0;
        DWORD got = 0;
        const DWORD want = available < capacity ? available : static_cast<DWORD>(capacity);
        if (!ReadFile(c->in, buf, want, &got, nullptr)) return -1;
        return static_cast<long>(got);
    }
    if (GetLastError() == ERROR_BROKEN_PIPE) return -1;
    // Not a pipe (a regular file): reads never block.
    DWORD got = 0;
    if (!ReadFile(c->in, buf, static_cast<DWORD>(capacity), &got, nullptr) || got == 0) return -1;
    return static_cast<long>(got);
#elif defined(UART_HOST_CONSOLE)
    const ssize_t n = ::read(STDIN_FILENO, buf, capacity);
    if (n > 0) return static_cast<long>(n);
    if (n == 0) return -1;      // end of input
    return 0;                   // EAGAIN: nothing typed yet
#else
    (void)buf;
    (void)capacity;
    return -1;
#endif
}

void Uart::poll_host_input() {
#ifdef UART_HOST_CONSOLE
    if (!host_stdin_ || host_stdin_eof_) return;

    // Bounded so that a large paste cannot starve the guest: the receive queue
    // is drained one byte per RBR read, and a driver with no FIFO handles a few
    // dozen bytes between polls comfortably.
    constexpr u8 CTRL_A = 0x01;

    u8 buf[64];
    const long n = read_host(buf, sizeof buf);
    if (n > 0) {
        for (long i = 0; i < n; ++i) {
            const u8 c = buf[i];

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
    } else if (n < 0) {
        // End of input. Stop polling: a closed stdin stays readable and would
        // otherwise return 0 forever, once per poll.
        host_stdin_eof_ = true;
    }
#endif
}

Uart::~Uart() {
#if defined(_WIN32)
    if (auto* c = static_cast<WinConsole*>(win_console_)) {
        if (c->in_is_console) SetConsoleMode(c->in, c->saved_in_mode);
        if (c->out_is_console) {
            SetConsoleMode(c->out, c->saved_out_mode);
            SetConsoleOutputCP(c->saved_out_cp);
        }
        delete c;
    }
#elif defined(UART_HOST_CONSOLE)
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
            // IIR - "why did you interrupt me?"
            //
            // Bit 0 clear means an interrupt is pending; the sense is inverted,
            // which is a classic source of confusion. But bit 0 is not the
            // whole answer: **bits 3:1 say which condition caused it**, and a
            // real driver dispatches on that field rather than going looking.
            //
            //   0b000  modem status change
            //   0b001  transmitter holding register empty
            //   0b010  received data available
            //   0b011  receiver line status (overrun, parity, break)
            //   0b110  character timeout (FIFO has data that has sat too long)
            //
            // This used to return a bare 0xc0 for "something happened", whose
            // ID field is 000 - modem status. Linux's 8250 driver believed it,
            // read the modem status register, found nothing, and returned
            // without ever touching the receive buffer. The console printed
            // perfectly and could not be typed at.
            //
            // xv6 never reads IIR at all - it checks LSR and drains - which is
            // why phase 7 worked with this wrong, and is another instance of a
            // second, stricter OS finding what the first could not.
            //
            // The conditions are prioritised: received data outranks a free
            // transmitter, because an unread byte will be lost if the next one
            // arrives and an idle transmitter will still be idle later.
            u8 cause;
            if ((ier_ & IER_RX_READY) && !rx_.empty()) {
                cause = 0x04;          // received data available
            } else if ((ier_ & IER_TX_EMPTY) && tx_irq_) {
                cause = 0x02;          // transmitter holding register empty
                // Reading IIR acknowledges *this* interrupt, and only when it
                // is the one being reported - which is what makes THRE an edge
                // rather than a level that never falls.
                tx_irq_ = false;
            } else {
                cause = 0x01;          // bit 0 set: nothing pending
            }

            // Bits 7:6 set advertise working FIFOs, which is what makes the
            // driver identify this as a 16550A rather than an earlier part.
            return Result<u64>::good(static_cast<u8>(0xc0 | cause));
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
