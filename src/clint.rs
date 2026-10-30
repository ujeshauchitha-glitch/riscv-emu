//! CLINT - Core Local Interruptor.
//!
//! Provides the two interrupt sources every RISC-V system needs before it can run a
//! scheduler:
//!
//!   msip      software interrupts - one hart poking another (and itself)
//!   mtime     a free-running counter
//!   mtimecmp  when mtime reaches it, a timer interrupt fires
//!
//! Layout (matching the QEMU `virt` machine):
//!
//!   0x0000 + hart*4   msip
//!   0x4000 + hart*8   mtimecmp
//!   0xBFF8            mtime
//!
//! **One mtime, one mtimecmp and one msip per hart.** mtime is the machine-wide clock
//! all harts share; the other two are per-hart, which is how a single timer block
//! gives each core its own deadline and lets one core interrupt another (an IPI is a
//! write of 1 to the target's msip).
//!
//! mtime advances with retired instructions rather than wall-clock time. That makes
//! runs reproducible - the same program takes the same interrupt at the same
//! instruction every time - which matters enormously when debugging why a kernel
//! wedged on its third context switch.

use std::cell::Cell;
use std::rc::Rc;

use crate::csr::{csr, CsrFile};
use crate::device::Device;
use crate::trap::{Exception, Res, Trap};
use crate::types::CLINT_BASE;

pub const MAX_HARTS: u32 = 32;

const MSIP_OFFSET: u64 = 0x0000; // + hart * 4
const MTIMECMP_OFFSET: u64 = 0x4000; // + hart * 8
const MTIME_OFFSET: u64 = 0xbff8;

/// The machine-wide clock, shared between the CLINT (which exposes it to the guest
/// as memory) and every hart (which exposes it as the `time` CSR).
///
/// A machine with several harts advances the shared clock once per scheduling round,
/// not once per instruction. A guest reading the time between two such updates would
/// see it frozen - a delay loop spinning on rdtime would not finish until the round
/// ended, and two back-to-back reads would return the same value, which no real
/// clock does. So each hart publishes how many instructions it has retired since the
/// round began in `live`, and a read adds that.
pub struct Clock {
    mtime: Cell<u64>,
    /// Instructions the running hart has retired since this round began.
    pub live: Cell<u64>,
    /// How many retired instructions make one mtime tick.
    ipt: Cell<u64>,
    /// Instructions since the last mtime tick.
    subtick: Cell<u64>,
}

impl Clock {
    pub fn new() -> Clock {
        Clock { mtime: Cell::new(0), live: Cell::new(0), ipt: Cell::new(1), subtick: Cell::new(0) }
    }

    /// The clock as of the last scheduling round. This is what the timer comparison
    /// uses.
    pub fn mtime(&self) -> u64 {
        self.mtime.get()
    }

    /// The clock as the *running hart* sees it: the round's base plus the
    /// instructions that hart has retired since the round began.
    pub fn now(&self) -> u64 {
        self.mtime.get() + self.live.get() / self.ipt.get().max(1)
    }

    pub fn set_mtime(&self, t: u64) {
        self.mtime.set(t);
    }

    pub fn instructions_per_tick(&self) -> u64 {
        self.ipt.get()
    }

    /// One mtime tick every `n` instructions. (This used to add `n` ticks per
    /// instruction, which is the inverse of what the option documents: asking for a
    /// slower clock produced an interrupt storm.)
    pub fn set_instructions_per_tick(&self, n: u64) {
        self.ipt.set(n.max(1));
    }

    /// Advance by `instructions` retired instructions.
    pub fn tick(&self, instructions: u64) {
        let ipt = self.ipt.get().max(1);
        if ipt <= 1 {
            self.mtime.set(self.mtime.get().wrapping_add(instructions));
            return;
        }
        let sub = self.subtick.get() + instructions;
        self.mtime.set(self.mtime.get().wrapping_add(sub / ipt));
        self.subtick.set(sub % ipt);
    }

    /// Jump the clock forward (never back). Used when every hart is idle in WFI:
    /// there is nothing to execute until the next deadline, so rather than burning
    /// host time retiring nothing the clock skips straight to it.
    pub fn advance_to(&self, t: u64) {
        if t > self.mtime.get() {
            self.mtime.set(t);
            self.subtick.set(0);
        }
    }
}

impl Default for Clock {
    fn default() -> Self {
        Clock::new()
    }
}

pub struct Clint {
    clock: Rc<Clock>,
    harts: u32,
    /// Has a supervisor armed this hart's timer through SBI?
    ///
    /// mtimecmp is a machine-mode register, so its expiry naturally raises MTIP - a
    /// *machine* timer interrupt. A supervisor cannot enable that: mie is
    /// machine-only, and under --linux there is no M-mode software to enable it on
    /// the kernel's behalf. So without this the deadline expires into nothing,
    /// jiffies never advance, and every sleep in the kernel hangs forever - a
    /// failure that looks exactly like the emulator being slow.
    ///
    /// Real firmware handles the machine timer interrupt and posts a supervisor one
    /// in its place. That is what this flag models.
    sbi_timer: Vec<bool>,
    /// Reset value 0 means the timer is already "expired", so MTIP asserts
    /// immediately - which is exactly what real hardware does, and why every kernel
    /// sets mtimecmp before enabling the timer interrupt.
    mtimecmp: Vec<u64>,
    msip: Vec<u32>,
}

impl Clint {
    pub fn new(harts: u32, clock: Rc<Clock>) -> Clint {
        let harts = harts.clamp(1, MAX_HARTS);
        let n = harts as usize;
        Clint { clock, harts, sbi_timer: vec![false; n], mtimecmp: vec![0; n], msip: vec![0; n] }
    }

    pub fn harts(&self) -> u32 {
        self.harts
    }

    pub fn clock(&self) -> &Rc<Clock> {
        &self.clock
    }

    pub fn mtime(&self) -> u64 {
        self.clock.mtime()
    }

    pub fn mtimecmp(&self, hart: u32) -> u64 {
        self.mtimecmp.get(hart as usize).copied().unwrap_or(u64::MAX)
    }

    /// Drive MTIP and MSIP in `hart`'s mip to match the device's state. Interrupt
    /// pending bits are owned by the device, not by software: a guest cannot clear
    /// MTIP by writing mip, only by moving mtimecmp forward.
    pub fn update(&self, csrs: &mut CsrFile, hart: u32) {
        let h = hart as usize;
        if h >= self.harts as usize {
            return;
        }
        let mtime = self.clock.mtime();

        if mtime >= self.mtimecmp[h] {
            csrs.raise_interrupt(csr::MIP_MTIP);
        } else {
            csrs.clear_interrupt(csr::MIP_MTIP);
        }
        if self.msip[h] & 1 != 0 {
            csrs.raise_interrupt(csr::MIP_MSIP);
        } else {
            csrs.clear_interrupt(csr::MIP_MSIP);
        }

        // The supervisor timer (Sstc). When menvcfg.STCE lets S-mode have its own
        // compare register, STIP is driven by it directly - so a kernel arms its next
        // tick with one CSR write instead of calling into firmware. While STCE is
        // clear, STIP belongs to software (M-mode posts it), so leave it alone.
        if csrs.sstc_enabled() {
            if mtime >= csrs.read(csr::STIMECMP) {
                csrs.raise_interrupt(csr::MIP_STIP);
            } else {
                csrs.clear_interrupt(csr::MIP_STIP);
            }
        } else if self.sbi_timer[h] {
            // No Sstc, but a supervisor armed this deadline through SBI - so the
            // machine-timer expiry is forwarded as a *supervisor* timer interrupt,
            // which is exactly what M-mode firmware does on real hardware.
            if mtime >= self.mtimecmp[h] {
                csrs.raise_interrupt(csr::MIP_STIP);
            } else {
                csrs.clear_interrupt(csr::MIP_STIP);
            }
        }
    }

    /// Set the timer deadline on behalf of a supervisor, and clear the pending timer
    /// interrupt.
    ///
    /// mtimecmp is a machine-mode register, so a kernel running in S-mode cannot
    /// write it - which is the entire reason SBI has a set_timer call. Clearing MTIP
    /// here is required by the SBI specification and is easy to miss: the interrupt
    /// that just fired is still pending, and if it is not cleared when the new
    /// deadline is set the kernel re-enters its timer handler immediately and never
    /// makes progress.
    pub fn set_timer(&mut self, deadline: u64, csrs: &mut CsrFile, hart: u32) {
        let h = hart as usize;
        if h >= self.harts as usize {
            return;
        }
        self.mtimecmp[h] = deadline;
        // Both pending bits, because from here on this deadline belongs to the
        // supervisor - see `sbi_timer` above.
        csrs.clear_interrupt(csr::MIP_MTIP);
        csrs.clear_interrupt(csr::MIP_STIP);
        self.sbi_timer[h] = true;
    }

    /// Raise or lower another hart's machine software interrupt. This is the
    /// mechanism behind an IPI.
    pub fn set_msip(&mut self, hart: u32, on: bool) {
        if let Some(m) = self.msip.get_mut(hart as usize) {
            *m = on as u32;
        }
    }

    pub fn msip(&self, hart: u32) -> bool {
        self.msip.get(hart as usize).map_or(false, |m| *m != 0)
    }
}

impl Device for Clint {
    fn base(&self) -> u64 {
        CLINT_BASE
    }
    fn size(&self) -> u64 {
        0x10000
    }
    fn name(&self) -> &'static str {
        "clint"
    }

    fn load(&mut self, offset: u64, size: usize) -> Res<u64> {
        if offset < MSIP_OFFSET + 4 * MAX_HARTS as u64 && size == 4 {
            let hart = ((offset - MSIP_OFFSET) / 4) as usize;
            // msip registers for harts that do not exist read as zero.
            return Ok(self.msip.get(hart).copied().unwrap_or(0) as u64);
        }
        if (MTIMECMP_OFFSET..MTIMECMP_OFFSET + 8 * MAX_HARTS as u64).contains(&offset) {
            let hart = ((offset - MTIMECMP_OFFSET) / 8) as usize;
            let word = (offset - MTIMECMP_OFFSET) % 8;
            let v = self.mtimecmp.get(hart).copied().unwrap_or(u64::MAX);
            match (word, size) {
                (0, 8) => return Ok(v),
                (0, 4) => return Ok(v & 0xffff_ffff),
                (4, 4) => return Ok(v >> 32),
                _ => {}
            }
        }
        if offset == MTIME_OFFSET {
            if size == 8 {
                return Ok(self.clock.now());
            }
            if size == 4 {
                return Ok(self.clock.now() & 0xffff_ffff);
            }
        }
        if offset == MTIME_OFFSET + 4 && size == 4 {
            return Ok(self.clock.now() >> 32);
        }
        Err(Trap::new(Exception::LoadAccessFault, CLINT_BASE + offset))
    }

    fn store(&mut self, offset: u64, size: usize, value: u64) -> Res<()> {
        if offset < MSIP_OFFSET + 4 * MAX_HARTS as u64 && size == 4 {
            let hart = ((offset - MSIP_OFFSET) / 4) as usize;
            // Only bit 0 is implemented. Writes to harts that do not exist are
            // ignored rather than faulted, as on real hardware.
            if let Some(m) = self.msip.get_mut(hart) {
                *m = value as u32 & 1;
            }
            return Ok(());
        }
        if (MTIMECMP_OFFSET..MTIMECMP_OFFSET + 8 * MAX_HARTS as u64).contains(&offset) {
            let hart = ((offset - MTIMECMP_OFFSET) / 8) as usize;
            let word = (offset - MTIMECMP_OFFSET) % 8;
            let ok = (word == 0 && (size == 8 || size == 4)) || (word == 4 && size == 4);
            if !ok {
                return Err(Trap::new(Exception::StoreAMOAccessFault, CLINT_BASE + offset));
            }
            let Some(slot) = self.mtimecmp.get_mut(hart) else {
                return Ok(());
            };
            // RV32 kernels write mtimecmp as two 32-bit halves, so both widths have
            // to work.
            if word == 0 && size == 8 {
                *slot = value;
            } else if word == 0 {
                *slot = (*slot & !0xffff_ffff) | (value & 0xffff_ffff);
            } else {
                *slot = (*slot & 0xffff_ffff) | (value << 32);
            }
            return Ok(());
        }
        if offset == MTIME_OFFSET && size == 8 {
            self.clock.set_mtime(value);
            return Ok(());
        }
        Err(Trap::new(Exception::StoreAMOAccessFault, CLINT_BASE + offset))
    }
}
