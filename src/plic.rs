//! PLIC - Platform-Level Interrupt Controller.
//!
//! The CLINT handles the two interrupts a hart raises for itself (timer and
//! software). Everything else - a keystroke arriving at the UART, a disk finishing a
//! read - comes through the PLIC.
//!
//! Its job is arbitration. Many devices share one interrupt line into the hart, so
//! something has to decide which of several simultaneously-pending devices the CPU
//! should hear about first, and then let software find out *which* it was. That is
//! the claim/complete handshake:
//!
//!   claim     read the claim register -> the highest-priority pending IRQ, which the
//!             PLIC marks in-service and stops re-raising
//!   complete  write the IRQ back -> the handler is done, the source may fire again
//!
//! **Contexts.** A context is one (hart, privilege level) pair, each with its own
//! enable bits and threshold. Hart h owns contexts 2h (M-mode) and 2h+1 (S-mode).

use std::cell::{Cell, RefCell};

use crate::csr::{csr, CsrFile};
use crate::device::Device;
use crate::trap::{Exception, Res, Trap};
use crate::types::PLIC_BASE;

pub const MAX_IRQS: u32 = 64;

// Register layout, matching the QEMU virt machine (and therefore what xv6 and Linux
// drivers expect).
const PRIORITY_BASE: u64 = 0x000000; // + irq * 4
const PENDING_BASE: u64 = 0x001000; // + (irq / 32) * 4
const ENABLE_BASE: u64 = 0x002000; // + context * 0x80 + (irq / 32) * 4
const CONTEXT_BASE: u64 = 0x200000; // + context * 0x1000
const CONTEXT_STRIDE: u64 = 0x1000;
const THRESHOLD_OFF: u64 = 0x0;
const CLAIM_OFF: u64 = 0x4;

#[derive(Default, Clone, Copy)]
struct Context {
    enabled: u64,
    threshold: u32,
}

pub struct Plic {
    priority: [u32; MAX_IRQS as usize],
    /// Two bitmaps, not one. `line` is what the devices are asserting right now;
    /// `pending` is what the PLIC's gateway has forwarded and not yet had claimed.
    /// They differ exactly between a claim and its completion, and keeping them apart
    /// is what lets a still-asserted source be re-offered when the handler finishes.
    line: u64,
    pending: u64,
    /// In service: claimed but not yet completed.
    claimed: u64,
    contexts: Vec<Context>,
    /// Arbitration is a scan over every source and the machine consults the PLIC
    /// constantly, so the answer is cached and invalidated whenever interrupt state
    /// changes.
    best: RefCell<Vec<u32>>,
    dirty: Cell<bool>,
}

impl Plic {
    pub fn new(harts: u32) -> Plic {
        let n = 2 * harts.max(1) as usize;
        Plic {
            priority: [0; MAX_IRQS as usize],
            line: 0,
            pending: 0,
            claimed: 0,
            contexts: vec![Context::default(); n],
            best: RefCell::new(vec![0; n]),
            dirty: Cell::new(true),
        }
    }

    pub fn num_contexts(&self) -> u32 {
        self.contexts.len() as u32
    }

    /// Devices call this as their line rises and falls. The PLIC is level-triggered:
    /// a source stays pending while its line is asserted.
    pub fn set_pending(&mut self, irq: u32, asserted: bool) {
        if irq == 0 || irq >= MAX_IRQS {
            return;
        }
        let bit = 1u64 << irq;
        let (before_line, before_pending) = (self.line, self.pending);
        if asserted {
            self.line |= bit;
            self.pending |= bit;
        } else {
            self.line &= !bit;
            self.pending &= !bit;
        }
        if self.line != before_line || self.pending != before_pending {
            self.dirty.set(true);
        }
    }

    /// Drive MEIP/SEIP in `hart`'s mip from that hart's two contexts.
    pub fn update(&self, csrs: &mut CsrFile, hart: u32) {
        if self.best_irq(2 * hart) != 0 {
            csrs.raise_interrupt(csr::MIP_MEIP);
        } else {
            csrs.clear_interrupt(csr::MIP_MEIP);
        }
        if self.best_irq(2 * hart + 1) != 0 {
            csrs.raise_interrupt(csr::MIP_SEIP);
        } else {
            csrs.clear_interrupt(csr::MIP_SEIP);
        }
    }

    fn best_irq(&self, context: u32) -> u32 {
        if self.dirty.get() {
            let mut best = self.best.borrow_mut();
            for (ctx, slot) in best.iter_mut().enumerate() {
                *slot = self.scan(ctx);
            }
            self.dirty.set(false);
        }
        self.best.borrow().get(context as usize).copied().unwrap_or(0)
    }

    /// The highest-priority IRQ this context should be told about, or 0 for none. IRQ
    /// 0 is reserved precisely so that zero can mean "nothing".
    fn scan(&self, context: usize) -> u32 {
        let Some(c) = self.contexts.get(context) else {
            return 0;
        };
        // A claimed IRQ is masked until completed, which is what stops a
        // level-triggered source re-interrupting its own handler.
        let candidates = self.pending & c.enabled & !self.claimed;
        let (mut best, mut best_priority) = (0u32, 0u32);
        for irq in 1..MAX_IRQS {
            if candidates & (1u64 << irq) == 0 {
                continue;
            }
            let p = self.priority[irq as usize];
            // Priority 0 means "never interrupt", and the threshold is a floor the
            // priority must exceed - not merely meet.
            if p == 0 || p <= c.threshold {
                continue;
            }
            if p > best_priority {
                best_priority = p;
                best = irq;
            }
        }
        best
    }

    fn claim(&mut self, context: u32) -> u32 {
        let irq = self.best_irq(context);
        if irq != 0 {
            self.claimed |= 1u64 << irq;
            // Claiming also clears the pending bit. A level-triggered device that is
            // still asserting will set it again through set_pending().
            self.pending &= !(1u64 << irq);
            self.dirty.set(true);
        }
        irq
    }

    fn complete(&mut self, irq: u32) {
        if irq == 0 || irq >= MAX_IRQS {
            return;
        }
        let bit = 1u64 << irq;
        self.claimed &= !bit;
        // A source whose line is still asserted becomes pending again the moment the
        // handler says it is done. This is the level half of level-triggered:
        // modelling only the clear would drop interrupts under load - rarely, and
        // only when the timing lines up, which is the worst kind of bug to find in a
        // running kernel.
        if self.line & bit != 0 {
            self.pending |= bit;
        }
        self.dirty.set(true);
    }
}

impl Device for Plic {
    fn base(&self) -> u64 {
        PLIC_BASE
    }
    fn size(&self) -> u64 {
        0x400000
    }
    fn name(&self) -> &'static str {
        "plic"
    }

    fn load(&mut self, offset: u64, size: usize) -> Res<u64> {
        // Every PLIC register is 32 bits.
        if size != 4 {
            return Err(Trap::new(Exception::LoadAccessFault, PLIC_BASE + offset));
        }
        if offset < PRIORITY_BASE + MAX_IRQS as u64 * 4 {
            return Ok(self.priority[(offset / 4) as usize] as u64);
        }
        // The bitmaps are 64 bits wide, so only words 0 and 1 exist.
        if (PENDING_BASE..PENDING_BASE + 0x80).contains(&offset) {
            let word = (offset - PENDING_BASE) / 4;
            if word >= 2 {
                return Ok(0);
            }
            return Ok((self.pending >> (word * 32)) as u32 as u64);
        }
        if (ENABLE_BASE..CONTEXT_BASE).contains(&offset) {
            let ctx = ((offset - ENABLE_BASE) / 0x80) as usize;
            let word = ((offset - ENABLE_BASE) % 0x80) / 4;
            if ctx >= self.contexts.len() || word >= 2 {
                return Ok(0);
            }
            return Ok((self.contexts[ctx].enabled >> (word * 32)) as u32 as u64);
        }
        if offset >= CONTEXT_BASE {
            let ctx = ((offset - CONTEXT_BASE) / CONTEXT_STRIDE) as usize;
            let within = (offset - CONTEXT_BASE) % CONTEXT_STRIDE;
            if ctx >= self.contexts.len() {
                return Ok(0);
            }
            if within == THRESHOLD_OFF {
                return Ok(self.contexts[ctx].threshold as u64);
            }
            if within == CLAIM_OFF {
                return Ok(self.claim(ctx as u32) as u64);
            }
        }
        Ok(0)
    }

    fn store(&mut self, offset: u64, size: usize, value: u64) -> Res<()> {
        if size != 4 {
            return Err(Trap::new(Exception::StoreAMOAccessFault, PLIC_BASE + offset));
        }
        let v = value as u32;

        if offset < PRIORITY_BASE + MAX_IRQS as u64 * 4 {
            self.priority[(offset / 4) as usize] = v;
            self.dirty.set(true);
            return Ok(());
        }
        if (ENABLE_BASE..CONTEXT_BASE).contains(&offset) {
            let ctx = ((offset - ENABLE_BASE) / 0x80) as usize;
            let word = ((offset - ENABLE_BASE) % 0x80) / 4;
            if ctx < self.contexts.len() && word < 2 {
                let mask = 0xffff_ffffu64 << (word * 32);
                let c = &mut self.contexts[ctx];
                c.enabled = (c.enabled & !mask) | ((v as u64) << (word * 32));
                self.dirty.set(true);
            }
            return Ok(());
        }
        if offset >= CONTEXT_BASE {
            let ctx = ((offset - CONTEXT_BASE) / CONTEXT_STRIDE) as usize;
            let within = (offset - CONTEXT_BASE) % CONTEXT_STRIDE;
            if ctx < self.contexts.len() {
                if within == THRESHOLD_OFF {
                    self.contexts[ctx].threshold = v;
                    self.dirty.set(true);
                }
                // Writing the claim register is the "complete" half of the
                // handshake: the handler is telling us it is finished with that IRQ.
                if within == CLAIM_OFF {
                    self.complete(v);
                }
            }
        }
        Ok(())
    }
}
