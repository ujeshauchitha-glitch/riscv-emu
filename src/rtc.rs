//! Goldfish real-time clock: wall-clock time and a one-shot alarm.
//!
//! Linux's `rtc-goldfish` driver reads the time as nanoseconds since the epoch from
//! two 32-bit registers. That is what lets a booted system print the right date
//! instead of 1970.

use std::cell::RefCell;
use std::rc::Rc;
use std::time::{SystemTime, UNIX_EPOCH};

use crate::device::Device;
use crate::plic::Plic;
use crate::trap::{Exception, Res, Trap};
use crate::types::RTC_BASE;

const TIME_LOW: u64 = 0x00;
const TIME_HIGH: u64 = 0x04;
const ALARM_LOW: u64 = 0x08;
const ALARM_HIGH: u64 = 0x0c;
const IRQ_ENABLED: u64 = 0x10;
const CLEAR_ALARM: u64 = 0x14;
const ALARM_STATUS: u64 = 0x18;
const CLEAR_INTERRUPT: u64 = 0x1c;

#[derive(Default)]
pub struct Rtc {
    plic: Option<Rc<RefCell<Plic>>>,
    irq: u32,
    fixed: bool,
    fixed_ns: u64,
    latched_high: u64,
    alarm_ns: u64,
    alarm_armed: bool,
    irq_enabled: bool,
    irq_raised: bool,
}

impl Rtc {
    pub fn new() -> Rtc {
        Rtc::default()
    }

    pub fn attach(&mut self, plic: Rc<RefCell<Plic>>, irq: u32) {
        self.plic = Some(plic);
        self.irq = irq;
    }

    /// Freeze the clock at a fixed time (for reproducible runs and tests).
    pub fn set_fixed_time(&mut self, ns: u64) {
        self.fixed = true;
        self.fixed_ns = ns;
    }

    pub fn advance(&mut self, ns: u64) {
        self.fixed_ns += ns;
    }

    pub fn now_ns(&self) -> u64 {
        if self.fixed {
            return self.fixed_ns;
        }
        SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| d.as_nanos() as u64)
    }

    fn set_line(&self, on: bool) {
        if let Some(p) = &self.plic {
            p.borrow_mut().set_pending(self.irq, on);
        }
    }
}

impl Device for Rtc {
    fn base(&self) -> u64 {
        RTC_BASE
    }
    fn size(&self) -> u64 {
        0x1000
    }
    fn name(&self) -> &'static str {
        "rtc"
    }

    fn load(&mut self, offset: u64, size: usize) -> Res<u64> {
        if size != 4 {
            return Err(Trap::new(Exception::LoadAccessFault, RTC_BASE + offset));
        }
        Ok(match offset {
            TIME_LOW => {
                // Latch the whole 64-bit time so that reading the two halves one
                // after the other cannot straddle a carry from low into high.
                let t = self.now_ns();
                self.latched_high = t >> 32;
                t & 0xffff_ffff
            }
            TIME_HIGH => self.latched_high,
            ALARM_LOW => self.alarm_ns & 0xffff_ffff,
            ALARM_HIGH => self.alarm_ns >> 32,
            IRQ_ENABLED => self.irq_enabled as u64,
            ALARM_STATUS => self.alarm_armed as u64,
            _ => 0,
        })
    }

    fn store(&mut self, offset: u64, size: usize, value: u64) -> Res<()> {
        if size != 4 {
            return Err(Trap::new(Exception::StoreAMOAccessFault, RTC_BASE + offset));
        }
        let v = value as u32 as u64;
        match offset {
            ALARM_HIGH => self.alarm_ns = (self.alarm_ns & 0xffff_ffff) | (v << 32),
            ALARM_LOW => {
                // The low half is written last, which is what arms the alarm.
                self.alarm_ns = (self.alarm_ns & !0xffff_ffff) | v;
                self.alarm_armed = true;
            }
            IRQ_ENABLED => self.irq_enabled = v & 1 != 0,
            CLEAR_ALARM => self.alarm_armed = false,
            CLEAR_INTERRUPT => {
                self.irq_raised = false;
                self.set_line(false);
            }
            _ => {}
        }
        Ok(())
    }

    fn poll(&mut self) {
        if self.alarm_armed && self.irq_enabled && !self.irq_raised && self.now_ns() >= self.alarm_ns {
            self.alarm_armed = false;
            self.irq_raised = true;
            self.set_line(true);
        }
    }
}
