#include "uart.hpp"

#include <ostream>

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

void Uart::feed_input(const std::string& s) {
    for (char c : s) rx_.push_back(static_cast<u8>(c));
}

bool Uart::interrupting() const {
    if ((ier_ & IER_RX_READY) && !rx_.empty()) return true;
    // Our transmitter is never busy - a write completes immediately - so the
    // "transmit holding register empty" condition is always true.
    if (ier_ & IER_TX_EMPTY) return true;
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
        case 2:
            // IIR. Bit 0 clear means "an interrupt is pending"; the encoding is
            // inverted, which is a classic source of confusion.
            return Result<u64>::good(interrupting() ? 0xc0 : 0xc1);
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
            return Status::good();
        case 1:
            if (dlab()) dlm_ = v; else ier_ = v;
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
