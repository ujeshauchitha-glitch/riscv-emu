//! The machine: one or more harts, scheduled deterministically on one host thread.
//!
//! Harts take turns in fixed-size quanta, round-robin. Because the interleaving is a
//! pure function of the program, a run is exactly reproducible - the same bug happens
//! at the same instruction every time, which is worth more when debugging a kernel
//! than the speed a parallel host implementation would buy.
//!
//! The platform also owns the things that make a collection of harts a *machine*:
//! the shared clock (advanced once per round by the longest quantum, since the harts
//! notionally ran side by side), device polling, idle fast-forwarding when every hart
//! sleeps, and the conditions that end a run.

use std::cell::RefCell;
use std::rc::Rc;
use std::time::Duration;

use crate::bus::Bus;
use crate::clint::{Clint, Clock};
use crate::cpu::Cpu;
use crate::csr::csr;
use crate::device::AccessType;
use crate::plic::Plic;
use crate::syscon::Syscon;
use crate::trap::Trap;
use crate::types::PRIV_SUPERVISOR;
use crate::uart::Uart;

/// A debugger's hook: called before every instruction; returning true stops the run
/// with that hart paused, instruction not yet executed.
pub type BeforeStep = Box<dyn FnMut(&mut Cpu) -> bool>;

pub struct Platform {
    pub bus: Rc<Bus>,
    pub harts: Vec<Cpu>,
    pub clock: Rc<Clock>,

    clint: Option<Rc<RefCell<Clint>>>,
    plic: Option<Rc<RefCell<Plic>>>,
    uart: Option<Rc<RefCell<Uart>>>,
    syscon: Option<Rc<RefCell<Syscon>>>,
    uart_irq: u32,

    /// Instructions a hart runs before the next one gets a turn.
    pub quantum: u64,

    pub before_step: Option<BeforeStep>,
    hook_stopped: bool,
    hook_hart: u32,

    halted: bool,
    user_quit: bool,
    sbi_shutdown: bool,
    stalled: bool,
    fault_hart: u32,

    /// The riscv-tests HTIF `tohost` address, and the value written to it.
    pub htif_tohost_addr: u64,
    pub htif_tohost_value: u64,

    retired: u64,
    since_host_poll: u64,
}

impl Platform {
    pub fn new(bus: Rc<Bus>, num_harts: u32) -> Platform {
        let n = num_harts.max(1);
        let mut harts: Vec<Cpu> = (0..n)
            .map(|i| {
                let mut cpu = Cpu::new(bus.clone());
                cpu.hartid = i;
                cpu.csrs.set_hartid(i as u64);
                cpu
            })
            .collect();

        // Every hart sees every hart's mailbox. A single hart needs no peer list, and
        // leaving it as just itself keeps the single-hart store path free of the
        // reservation check.
        if harts.len() > 1 {
            let peers = Rc::new(harts.iter().map(|h| h.mb.clone()).collect::<Vec<_>>());
            for h in harts.iter_mut() {
                h.peers = peers.clone();
            }
        }

        // WFI stays a no-op unless asked otherwise (set_idle_on_wfi, which a Linux
        // boot turns on). The architecture lets WFI complete immediately, and
        // software written against that - the official riscv-tests suite executes a
        // WFI with nothing pending and expects to carry on - must keep working.
        Platform {
            bus,
            harts,
            clock: Rc::new(Clock::new()),
            clint: None,
            plic: None,
            uart: None,
            syscon: None,
            uart_irq: 0,
            quantum: 256,
            before_step: None,
            hook_stopped: false,
            hook_hart: 0,
            halted: false,
            user_quit: false,
            sbi_shutdown: false,
            stalled: false,
            fault_hart: 0,
            htif_tohost_addr: 0,
            htif_tohost_value: 0,
            retired: 0,
            since_host_poll: 0,
        }
    }

    pub fn num_harts(&self) -> u32 {
        self.harts.len() as u32
    }
    pub fn boot_hart(&mut self) -> &mut Cpu {
        &mut self.harts[0]
    }

    /// Hand the platform the devices it has to drive: it ticks the timer, updates
    /// every hart's interrupt-pending bits, polls the console, and watches the power
    /// button.
    pub fn wire(
        &mut self,
        clint: Rc<RefCell<Clint>>,
        plic: Rc<RefCell<Plic>>,
        uart: Rc<RefCell<Uart>>,
        uart_irq: u32,
        syscon: Rc<RefCell<Syscon>>,
    ) {
        self.clock = clint.borrow().clock().clone();
        for h in self.harts.iter_mut() {
            h.clock = Some(self.clock.clone());
            h.clint = Some(clint.clone());
            h.uart = Some(uart.clone());
        }
        self.clint = Some(clint);
        self.plic = Some(plic);
        self.uart = Some(uart);
        self.syscon = Some(syscon);
        self.uart_irq = uart_irq;
    }

    pub fn set_idle_on_wfi(&mut self, on: bool) {
        for h in self.harts.iter_mut() {
            h.idle_on_wfi = on;
        }
    }

    pub fn halted(&self) -> bool {
        self.halted
    }
    pub fn user_quit(&self) -> bool {
        self.user_quit
    }
    pub fn sbi_shutdown(&self) -> bool {
        self.sbi_shutdown
    }
    /// True if the run ended because nothing could ever execute again.
    pub fn stalled(&self) -> bool {
        self.stalled
    }
    pub fn fault_hart(&self) -> u32 {
        self.fault_hart
    }
    pub fn hook_stopped(&self) -> bool {
        self.hook_stopped
    }
    pub fn hook_hart(&self) -> u32 {
        self.hook_hart
    }
    pub fn retired(&self) -> u64 {
        self.retired
    }

    /// Put the machine in the state a Linux kernel expects from firmware: boot hart
    /// in S-mode at `entry` with a0 = hartid and a1 = the device tree, SBI answering
    /// the kernel's ecalls, traps delegated, the other harts parked.
    pub fn enter_supervisor(&mut self, entry: u64, dtb_addr: u64) {
        // A kernel sleeps in WFI whenever it has nothing to run. Letting the hart
        // really idle - and, when every hart does, skipping the clock to the next
        // timer deadline - is what makes an idle Linux cost nothing.
        self.set_idle_on_wfi(true);

        for h in self.harts.iter_mut() {
            h.sbi_enabled = true;

            // Delegate every trap to supervisor mode. This is firmware's job, and there
            // is no firmware here to do it. A trap taken in S-mode goes to M-mode unless
            // the matching medeleg bit is set - so without this, the kernel's very first
            // page fault (the fetch immediately after it enables paging, which is
            // *designed* to fault so that execution lands on the virtual-address
            // continuation at stvec) is delivered to mtvec instead. mtvec is zero, so the
            // machine vectors to address 0, faults on that fetch too, and loops forever
            // with nothing on the console. OpenSBI writes exactly this before its own
            // mret into the kernel.
            h.csrs.write(csr::MEDELEG, 0xffff);
            h.csrs.write(csr::MIDELEG, 0xffff);

            // Let the supervisor see its own timer and external interrupts. mie is
            // machine-only, so a kernel cannot enable these for itself.
            h.csrs.write(csr::MIE, csr::MIP_SEIP | csr::MIP_STIP | csr::MIP_SSIP);

            // Let supervisor and user code read the counters. `rdtime` is an ordinary
            // instruction to a kernel - Linux's udelay() is built on it - but reading
            // the time CSR from a lower privilege level is illegal unless the level
            // above has enabled it.
            h.csrs.write(csr::MCOUNTEREN, 0xffff_ffff);
            h.csrs.write(csr::SCOUNTEREN, 0xffff_ffff);

            // The "no handler installed" guard is a debugging aid rather than
            // architectural behaviour, and would fire on the first page fault of the
            // boot. A kernel with real firmware underneath it would simply take the
            // trap.
            h.trap_fatal_without_handler = false;
        }

        // Only the boot hart is released. The others wait in firmware until the kernel
        // starts them with SBI hart_start.
        let boot = &mut self.harts[0];
        boot.priv_ = PRIV_SUPERVISOR;
        boot.pc = entry;
        boot.write_reg(10, 0); // a0 = hartid
        boot.write_reg(11, dtb_addr); // a1 = device tree
        for h in self.harts.iter().skip(1) {
            h.set_parked(true);
        }
    }

    /// Bring everything that changes without the guest touching it up to date: the
    /// console's input, devices with their own clocks, and each hart's view of the
    /// timer and interrupt controller.
    pub fn poll_devices(&mut self) {
        if let Some(uart) = &self.uart {
            if self.since_host_poll >= 4096 {
                self.since_host_poll = 0;
                uart.borrow_mut().poll_host_input();
            }
        }

        // Devices with their own clocks or external inputs (a network card, say).
        self.bus.poll_devices();

        if let (Some(plic), Some(uart)) = (&self.plic, &self.uart) {
            let level = uart.borrow().interrupting();
            plic.borrow_mut().set_pending(self.uart_irq, level);
        }

        for h in self.harts.iter_mut() {
            let id = h.hartid;
            if let Some(c) = &self.clint {
                c.borrow().update(&mut h.csrs, id);
            }
            if let Some(p) = &self.plic {
                p.borrow().update(&mut h.csrs, id);
            }
        }
    }

    fn check_stop(&mut self) -> bool {
        for h in self.harts.iter() {
            if h.halted {
                self.halted = true;
                if h.sbi_shutdown {
                    self.sbi_shutdown = true;
                }
            }
        }

        // A guest that writes the poweroff word to syscon has asked to stop.
        if let Some(s) = &self.syscon {
            let s = s.borrow();
            if s.poweroff_requested() || s.reboot_requested() {
                self.halted = true;
            }
        }

        // So has a user who pressed Ctrl-A X at the console. The guest is not
        // consulted: this is the human at the keyboard asking to leave, which is the
        // one request a running guest must not be able to refuse.
        if let Some(u) = &self.uart {
            if u.borrow().exit_requested() {
                self.halted = true;
                self.user_quit = true;
            }
        }

        // The riscv-tests suite stops by writing its result to `tohost` and then
        // spinning, so there is nothing to notice except the write itself.
        if self.htif_tohost_addr != 0 {
            if let Ok(v) = self.bus.load(self.htif_tohost_addr, 8, AccessType::Load) {
                if v != 0 {
                    self.htif_tohost_value = v;
                    self.halted = true;
                }
            }
        }

        // Mirror the verdict onto the boot hart, which is where single-hart code (and
        // the reporting in main) has always looked for it.
        if self.halted {
            let (q, s, t) = (self.user_quit, self.sbi_shutdown, self.htif_tohost_value);
            let b = &mut self.harts[0];
            b.halted = true;
            b.user_quit = q;
            b.sbi_shutdown = s;
            b.htif_tohost_value = t;
        }
        self.halted
    }

    fn io_pending(&self) -> bool {
        self.bus.io_pending()
    }

    /// Every hart is asleep: jump the clock to the earliest deadline anyone is waiting
    /// for. Returns false if there is no deadline to jump to.
    fn fast_forward_idle(&mut self) -> bool {
        let Some(clint) = &self.clint else {
            return false;
        };
        let clint = clint.borrow();
        let now = clint.mtime();
        // Deadlines further away than this are "disarmed" for practical purposes: a
        // kernel with nothing scheduled writes all-ones, and jumping the clock there
        // would wrap it.
        const HORIZON: u64 = 1 << 36;
        let mut best = u64::MAX;
        let mut consider = |deadline: u64| {
            if deadline > now && deadline - now <= HORIZON {
                best = best.min(deadline);
            }
        };
        for h in self.harts.iter() {
            if h.parked() {
                continue;
            }
            consider(clint.mtimecmp(h.hartid));
            if h.csrs.sstc_enabled() {
                consider(h.csrs.read(csr::STIMECMP));
            }
        }
        if best == u64::MAX {
            return false;
        }
        clint.clock().advance_to(best);
        true
    }

    /// Run until `max_steps` instructions have retired in total, a stop condition is
    /// reached, a hart takes a fatal trap, or a debugger hook asks to stop. A fatal
    /// trap comes back as `Err`; `fault_hart()` says which hart took it.
    pub fn run(&mut self, max_steps: u64) -> Result<(), Trap> {
        let mut consecutive_idle = 0u32;
        // Idle rounds retire nothing, so they would never use up `max_steps` by
        // themselves. This bounds them anyway: a clock that failed to move, or devices
        // that keep waking a hart which then immediately sleeps again, must end the
        // run rather than spin the host forever.
        let mut idle_rounds = 0u64;
        self.hook_stopped = false;

        while self.retired < max_steps {
            let mut round_max = 0u64;
            let mut any_ran = false;

            for i in 0..self.harts.len() {
                if !self.harts[i].runnable() {
                    continue;
                }
                any_ran = true;
                {
                    let h = &mut self.harts[i];
                    h.round_start_instret = h.instret;
                }
                self.clock.live.set(0);

                // Run this hart's quantum. If the guest touches a device partway
                // through, bring the devices up to date and carry on with the rest of
                // the quantum, so that a write to a timer or interrupt register takes
                // effect at the next instruction rather than a round later.
                let mut budget = self.quantum.min(max_steps - self.retired);
                let mut used = 0u64;
                while budget > 0 {
                    // A debugger gets to look at the hart before every instruction.
                    if let Some(hook) = self.before_step.as_mut() {
                        if hook(&mut self.harts[i]) {
                            self.hook_stopped = true;
                            self.hook_hart = self.harts[i].hartid;
                            return Ok(());
                        }
                    }
                    let stepping = self.before_step.is_some();
                    let (done, fault) = self.harts[i].run_core(if stepping { 1 } else { budget });
                    self.retired += done;
                    used += done;
                    budget -= budget.min(done);
                    self.since_host_poll += done;

                    if let Some(t) = fault {
                        self.fault_hart = self.harts[i].hartid;
                        return Err(t);
                    }
                    let h = &self.harts[i];
                    if h.halted || h.parked() || !h.device_touched {
                        break;
                    }
                    self.poll_devices();
                }
                round_max = round_max.max(used);
                if self.retired >= max_steps {
                    break;
                }
            }

            if any_ran {
                consecutive_idle = 0;
                // The harts ran side by side, so the clock moves by the longest of
                // them, not the sum.
                self.clock.live.set(0);
                self.clock.tick(round_max);
            } else {
                // Nothing can execute. Skip to the next deadline if there is one;
                // otherwise the only thing that can still wake the machine is the
                // outside world typing at the console.
                idle_rounds += 1;
                if idle_rounds > max_steps {
                    break;
                }
                // An idle round executes nothing, so it would never accumulate toward
                // the next host-keyboard poll on its own.
                self.since_host_poll += 64;

                if self.io_pending() {
                    // Something outside the machine is still expected to answer. Let
                    // real time pass - a couple of hundred microseconds a round, the
                    // guest clock keeping pace at its nominal 10 MHz - rather than
                    // skipping to the next deadline and timing the request out before
                    // the reply could possibly arrive.
                    std::thread::sleep(Duration::from_micros(200));
                    self.clock.tick(2000);
                    consecutive_idle = 0;
                } else if !self.fast_forward_idle() {
                    let host_input = self.uart.as_ref().map_or(false, |u| u.borrow().host_input_active());
                    if host_input {
                        std::thread::sleep(Duration::from_millis(1));
                        self.since_host_poll = 4096;
                    } else {
                        consecutive_idle += 1;
                        if consecutive_idle > 4 {
                            self.stalled = true;
                            return Ok(());
                        }
                    }
                }
            }

            self.poll_devices();
            if self.check_stop() {
                return Ok(());
            }
        }
        Ok(())
    }

    /// Execute exactly one instruction on one hart (for a debugger's single-step).
    pub fn step_hart(&mut self, hart: u32) -> Result<(), Trap> {
        let Some(h) = self.harts.get_mut(hart as usize) else {
            return Ok(());
        };
        if !h.runnable() {
            return Ok(());
        }
        h.round_start_instret = h.instret;
        self.clock.live.set(0);
        let (done, fault) = h.run_core(1);
        self.retired += done;
        if let Some(t) = fault {
            self.fault_hart = hart;
            return Err(t);
        }
        self.clock.tick(done);
        self.clock.live.set(0);
        self.poll_devices();
        self.check_stop();
        Ok(())
    }

    /// Run the stop checks and device polling without executing anything (used by a
    /// debugger after it changes state itself).
    pub fn refresh(&mut self) {
        self.poll_devices();
        self.check_stop();
    }
}
