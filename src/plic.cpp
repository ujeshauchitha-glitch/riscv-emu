#include "plic.hpp"

namespace {
// Register layout, matching the QEMU virt machine (and therefore what xv6 and
// Linux drivers expect).
constexpr u64 PRIORITY_BASE  = 0x000000;   // + irq * 4
constexpr u64 PENDING_BASE   = 0x001000;   // + (irq / 32) * 4
constexpr u64 ENABLE_BASE    = 0x002000;   // + context * 0x80 + (irq / 32) * 4
constexpr u64 CONTEXT_BASE   = 0x200000;   // + context * 0x1000
constexpr u64 CONTEXT_STRIDE = 0x1000;
constexpr u64 THRESHOLD_OFF  = 0x0;        // within a context block
constexpr u64 CLAIM_OFF      = 0x4;
}  // namespace

void Plic::set_pending(u32 irq, bool asserted) {
    if (irq == 0 || irq >= MAX_IRQS) return;
    const u64 bit = 1ull << irq;
    const u64 before_line = line_, before_pending = pending_;

    if (asserted) { line_ |= bit;  pending_ |= bit; }
    else          { line_ &= ~bit; pending_ &= ~bit; }

    if (line_ != before_line || pending_ != before_pending) invalidate();
}

void Plic::recompute() const {
    for (u32 ctx = 0; ctx < NUM_CONTEXTS; ++ctx) best_[ctx] = scan(ctx);
    dirty_ = false;
}

u32 Plic::best_irq(u32 context) const {
    if (dirty_) recompute();
    return context < NUM_CONTEXTS ? best_[context] : 0;
}

u32 Plic::scan(u32 context) const {
    if (context >= NUM_CONTEXTS) return 0;
    const Context& c = contexts_[context];

    // A claimed IRQ is masked until completed, which is what stops a
    // level-triggered source re-interrupting its own handler.
    const u64 candidates = pending_ & c.enabled & ~claimed_;

    u32 best = 0, best_priority = 0;
    for (u32 irq = 1; irq < MAX_IRQS; ++irq) {
        if ((candidates & (1ull << irq)) == 0) continue;
        const u32 p = priority_[irq];
        // Priority 0 means "never interrupt", and the threshold is a floor the
        // priority must exceed - not merely meet.
        if (p == 0 || p <= c.threshold) continue;
        if (p > best_priority) { best_priority = p; best = irq; }
    }
    return best;
}

void Plic::update(CsrFile& csrs) const {
    // Context 0 drives the machine-mode external interrupt, context 1 the
    // supervisor one.
    if (best_irq(0) != 0) csrs.raise_interrupt(csr::MIP_MEIP);
    else                  csrs.clear_interrupt(csr::MIP_MEIP);

    if (best_irq(1) != 0) csrs.raise_interrupt(csr::MIP_SEIP);
    else                  csrs.clear_interrupt(csr::MIP_SEIP);
}

u32 Plic::claim(u32 context) {
    const u32 irq = best_irq(context);
    if (irq != 0) {
        claimed_ |= (1ull << irq);
        // Claiming also clears the pending bit. A level-triggered device that
        // is still asserting will set it again through set_pending().
        pending_ &= ~(1ull << irq);
        invalidate();
    }
    return irq;
}

void Plic::complete(u32 context, u32 irq) {
    (void)context;
    if (irq == 0 || irq >= MAX_IRQS) return;
    const u64 bit = 1ull << irq;
    claimed_ &= ~bit;

    // A source whose line is still asserted becomes pending again the moment
    // the handler says it is done. This is the level half of level-triggered:
    // claiming clears the pending bit, but the gateway re-raises it if the
    // device has not been quieted, so a second keystroke that arrived while the
    // first was being handled is not lost. Modelling only the clear would drop
    // interrupts under load - rarely, and only when the timing lines up, which
    // is the worst kind of bug to find in a running kernel.
    if (line_ & bit) pending_ |= bit;

    invalidate();
}

Result<u64> Plic::load(u64 offset, unsigned size_bytes) {
    // Every PLIC register is 32 bits.
    if (size_bytes != 4) {
        return Result<u64>::bad(Exception::LoadAccessFault, PLIC_BASE + offset);
    }

    if (offset < PRIORITY_BASE + MAX_IRQS * 4) {
        return Result<u64>::good(priority_[offset / 4]);
    }
    if (offset >= PENDING_BASE && offset < PENDING_BASE + 0x80) {
        const u64 word = (offset - PENDING_BASE) / 4;
        return Result<u64>::good(static_cast<u32>(pending_ >> (word * 32)));
    }
    if (offset >= ENABLE_BASE && offset < CONTEXT_BASE) {
        const u64 ctx = (offset - ENABLE_BASE) / 0x80;
        const u64 word = ((offset - ENABLE_BASE) % 0x80) / 4;
        if (ctx >= NUM_CONTEXTS) return Result<u64>::good(0);
        return Result<u64>::good(static_cast<u32>(contexts_[ctx].enabled >> (word * 32)));
    }
    if (offset >= CONTEXT_BASE) {
        const u64 ctx = (offset - CONTEXT_BASE) / CONTEXT_STRIDE;
        const u64 within = (offset - CONTEXT_BASE) % CONTEXT_STRIDE;
        if (ctx >= NUM_CONTEXTS) return Result<u64>::good(0);
        if (within == THRESHOLD_OFF) return Result<u64>::good(contexts_[ctx].threshold);
        if (within == CLAIM_OFF)     return Result<u64>::good(claim(static_cast<u32>(ctx)));
    }
    return Result<u64>::good(0);
}

Status Plic::store(u64 offset, unsigned size_bytes, u64 value) {
    if (size_bytes != 4) {
        return Status::bad(Exception::StoreAMOAccessFault, PLIC_BASE + offset);
    }
    const u32 v = static_cast<u32>(value);

    if (offset < PRIORITY_BASE + MAX_IRQS * 4) {
        priority_[offset / 4] = v;
        invalidate();
        return Status::good();
    }
    if (offset >= ENABLE_BASE && offset < CONTEXT_BASE) {
        const u64 ctx = (offset - ENABLE_BASE) / 0x80;
        const u64 word = ((offset - ENABLE_BASE) % 0x80) / 4;
        if (ctx < NUM_CONTEXTS && word < 2) {
            const u64 mask = 0xffff'ffffull << (word * 32);
            contexts_[ctx].enabled =
                (contexts_[ctx].enabled & ~mask) | (static_cast<u64>(v) << (word * 32));
            invalidate();
        }
        return Status::good();
    }
    if (offset >= CONTEXT_BASE) {
        const u64 ctx = (offset - CONTEXT_BASE) / CONTEXT_STRIDE;
        const u64 within = (offset - CONTEXT_BASE) % CONTEXT_STRIDE;
        if (ctx < NUM_CONTEXTS) {
            if (within == THRESHOLD_OFF) { contexts_[ctx].threshold = v; invalidate(); }
            // Writing the claim register is the "complete" half of the
            // handshake: the handler is telling us it is finished with that IRQ.
            if (within == CLAIM_OFF) complete(static_cast<u32>(ctx), v);
        }
        return Status::good();
    }
    return Status::good();
}
