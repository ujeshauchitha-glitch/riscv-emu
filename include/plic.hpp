#pragma once

#include <array>

#include "csr.hpp"
#include "device.hpp"
#include "types.hpp"

// ---------------------------------------------------------------------------
// PLIC - Platform-Level Interrupt Controller.
//
// The CLINT handles the two interrupts a hart raises for itself (timer and
// software). Everything else - a keystroke arriving at the UART, a disk finishing
// a read - comes through the PLIC.
//
// Its job is arbitration. Many devices share one interrupt line into the hart,
// so something has to decide which of several simultaneously-pending devices
// the CPU should hear about first, and then let software find out *which* it
// was. That is the claim/complete handshake:
//
//   claim     read the claim register -> the highest-priority pending IRQ,
//             which the PLIC marks in-service and stops re-raising
//   complete  write the IRQ back -> the handler is done, the source may fire
//             again
//
// Without the in-service step, a level-triggered device that has not yet been
// quieted would re-interrupt immediately and the handler would never make
// progress.
//
// **Contexts.** A context is one (hart, privilege level) pair, each with its own
// enable bits and threshold, so machine-mode firmware and a supervisor kernel
// can be interested in different devices. For hart 0: context 0 is M-mode,
// context 1 is S-mode. xv6 uses context 1.
// ---------------------------------------------------------------------------
class Plic : public Device {
public:
    static constexpr u32 MAX_IRQS = 64;      // more than enough for this machine
    static constexpr u32 NUM_CONTEXTS = 2;   // hart 0 M-mode and S-mode

    u64         base() const override { return PLIC_BASE; }
    u64         size() const override { return 0x400000; }
    const char* name() const override { return "plic"; }

    Result<u64> load(u64 offset, unsigned size_bytes) override;
    Status      store(u64 offset, unsigned size_bytes, u64 value) override;

    // Devices call this as their line rises and falls. The PLIC is
    // level-triggered: a source stays pending while its line is asserted.
    void set_pending(u32 irq, bool asserted);

    // Drive MEIP/SEIP in mip from each context's state.
    void update(CsrFile& csrs) const;

private:
    std::array<u32, MAX_IRQS> priority_{};
    // Two bitmaps, not one. `line_` is what the devices are asserting right
    // now; `pending_` is what the PLIC's gateway has forwarded and not yet had
    // claimed. They differ exactly between a claim and its completion, and
    // keeping them apart is what lets a still-asserted source be re-offered
    // when the handler finishes - see complete().
    u64 line_    = 0;
    u64 pending_ = 0;
    u64 claimed_ = 0;      // in service: claimed but not yet completed

    struct Context {
        u64 enabled = 0;   // bitmap
        u32 threshold = 0;
    };
    std::array<Context, NUM_CONTEXTS> contexts_{};

    // The highest-priority IRQ this context should be told about, or 0 for
    // none. IRQ 0 is reserved precisely so that zero can mean "nothing".
    //
    // Arbitration is a scan over every source, and the CPU consults the PLIC
    // once per instruction - so recomputing it every time costs more than the
    // instruction being emulated. The answer only changes when something about
    // the interrupt state changes, so it is cached and invalidated on write.
    u32 best_irq(u32 context) const;
    u32 scan(u32 context) const;
    void recompute() const;

    mutable std::array<u32, NUM_CONTEXTS> best_{};
    mutable bool dirty_ = true;
    void invalidate() { dirty_ = true; }

    u32  claim(u32 context);
    void complete(u32 context, u32 irq);
};
