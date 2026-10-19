//! The CPU core.
//!
//! The execution loop is the classic fetch / decode / execute cycle, with one
//! important structural detail: **the PC is only advanced on success.** `execute`
//! writes to `next_pc`, and `step_core` commits it only if no trap occurred. On a
//! trap the PC is left pointing at the faulting instruction, which is exactly what
//! a real hart does (it stashes that address in mepc) and exactly what you want to
//! see in a register dump. A bad decode never silently skips an instruction.

mod exec;
mod fp;

use std::cell::Cell;
use std::io::Write;
use std::rc::Rc;

use crate::backends::{alu_reference, AluBackend, AluOp, RegFileBackend};
use crate::bus::Bus;
use crate::clint::{Clint, Clock};
use crate::csr::{csr, CsrFile};
use crate::decoder::{decode16, is_32bit_instruction, mnemonic, DecodedInst, Format};
use crate::device::AccessType;
use crate::mmu::{Mmu, PAGE_SIZE};
use crate::trap::{Exception, Interrupt, Res, Trap};
use crate::types::*;
use crate::uart::Uart;

use std::cell::RefCell;

/// Messages one hart leaves for another. Harts run one at a time on a single host
/// thread, so "sending" is just setting a flag the target consumes before it next
/// runs - the target is idle at that instant, which is the strongest ordering the
/// SBI calls could promise.
pub struct Mailbox {
    /// A hart that is not running: it has not been started yet, or it stopped
    /// itself. Secondary harts come out of reset parked, and SBI's hart_start call
    /// releases them. The scheduler skips parked harts entirely.
    pub parked: Cell<bool>,
    pending: Cell<u32>,
    start_addr: Cell<u64>,
    start_opaque: Cell<u64>,
    /// This hart's LR/SC reservation as a physical 8-byte granule, or `NO_RES`.
    /// Another hart's store to the granule cancels it.
    pub reservation: Cell<u64>,
}

const MB_IPI: u32 = 1;
const MB_FENCE: u32 = 2;
const MB_START: u32 = 4;
const NO_RES: u64 = u64::MAX;

impl Mailbox {
    pub fn new(parked: bool) -> Mailbox {
        Mailbox {
            parked: Cell::new(parked),
            pending: Cell::new(0),
            start_addr: Cell::new(0),
            start_opaque: Cell::new(0),
            reservation: Cell::new(NO_RES),
        }
    }
    /// Raise the supervisor software interrupt on this hart.
    pub fn send_ipi(&self) {
        self.pending.set(self.pending.get() | MB_IPI);
    }
    /// Drop this hart's TLB and decoded-instruction cache.
    pub fn send_fence(&self) {
        self.pending.set(self.pending.get() | MB_FENCE);
    }
    /// Release this hart at `addr`, the way SBI's hart_start does.
    pub fn send_start(&self, addr: u64, opaque: u64) {
        self.start_addr.set(addr);
        self.start_opaque.set(opaque);
        self.pending.set(self.pending.get() | MB_START);
        self.parked.set(false);
    }
}

const DCACHE_SLOTS: usize = 4096; // a power of two

#[derive(Clone, Copy)]
struct DecodeEntry {
    pc: u64,
    priv_: u8,
    inst: DecodedInst,
}

/// The CPU, plus the few machine-level handles it needs.
pub struct Cpu {
    // Architectural state.
    pub regs: [u64; NUM_REGS],
    /// The floating-point register file, separate from the integer one. Each is 64
    /// bits; a single-precision value lives NaN-boxed in the low half.
    pub fregs: [u64; NUM_REGS],
    pub pc: u64,
    pub csrs: CsrFile,
    /// Current privilege level. A hart comes out of reset in machine mode.
    pub priv_: u32,

    pub mmu: Mmu,
    pub bus: Rc<Bus>,

    /// Optional replacements for the integer register file and ALU; see backends.
    pub regfile: Option<Box<dyn RegFileBackend>>,
    pub alu_backend: Option<Box<dyn AluBackend>>,

    next_pc: u64,
    /// LR/SC reservation (virtual address); the physical granule is in `mb`.
    reservation_valid: bool,
    reservation_addr: u64,

    // The decoded-instruction cache. Fetching costs two translated 16-bit reads and
    // a decode; a kernel runs the same few thousand instructions over and over, so
    // the *decoded* result is remembered by PC. An entry is trusted only while the
    // PC and privilege match, DRAM's code epoch is unchanged (nothing has written to
    // a page we cached code from), and nobody has flushed it (SFENCE.VMA, satp,
    // FENCE.I). Only instructions wholly inside one page of DRAM are cached.
    dcache: Vec<DecodeEntry>,
    dcache_epoch: u64,

    /// Traps are fatal while no handler is installed.
    ///
    /// A hart out of reset has mtvec = 0, so a trap would vector to address 0, fault
    /// on the unmapped fetch, and vector to 0 again - an infinite loop that looks
    /// exactly like a hang. Until a guest installs a handler we stop and report.
    pub trap_fatal_without_handler: bool,
    /// The trap that stopped execution.
    pub last_trap: Option<Trap>,

    // Machine-level handles.
    pub clock: Option<Rc<Clock>>,
    pub clint: Option<Rc<RefCell<Clint>>>,
    pub uart: Option<Rc<RefCell<Uart>>>,

    pub halted: bool,
    /// SBI: firmware services for a supervisor. When on, an ECALL from supervisor
    /// mode is answered by the emulator rather than delivered as a trap - the role
    /// OpenSBI plays on real hardware. Off by default: a kernel that provides its
    /// own machine-mode code (xv6) must see its own ecalls.
    pub sbi_enabled: bool,
    pub sbi_shutdown: bool,
    /// The run stopped because the user pressed Ctrl-A X.
    pub user_quit: bool,
    pub htif_tohost_addr: u64,
    pub htif_tohost_value: u64,

    // Multi-hart support.
    pub hartid: u32,
    pub mb: Rc<Mailbox>,
    /// Every hart's mailbox, indexed by hartid (this one included).
    pub peers: Rc<Vec<Rc<Mailbox>>>,
    /// Make WFI genuinely idle the hart instead of being a no-op.
    pub idle_on_wfi: bool,
    pub waiting_for_interrupt: bool,
    /// Set by run_core when it returned early because the guest touched a device.
    pub device_touched: bool,
    /// Instruction count at the start of the current scheduling round.
    pub round_start_instret: u64,

    // Tracing is opt-in and off by default.
    pub trace: bool,
    pub trace_out: Option<Box<dyn Write>>,
    pub instret: u64,
    counter_written: bool,
}

impl Cpu {
    pub fn new(bus: Rc<Bus>) -> Cpu {
        let mb = Rc::new(Mailbox::new(false));
        let empty = DecodeEntry { pc: !0, priv_: 0, inst: DecodedInst::default() };
        Cpu {
            regs: [0; NUM_REGS],
            fregs: [0; NUM_REGS],
            pc: DRAM_BASE,
            csrs: CsrFile::new(),
            priv_: PRIV_MACHINE,
            mmu: Mmu::new(),
            bus,
            regfile: None,
            alu_backend: None,
            next_pc: 0,
            reservation_valid: false,
            reservation_addr: 0,
            dcache: vec![empty; DCACHE_SLOTS],
            dcache_epoch: 0,
            trap_fatal_without_handler: true,
            last_trap: None,
            clock: None,
            clint: None,
            uart: None,
            halted: false,
            sbi_enabled: false,
            sbi_shutdown: false,
            user_quit: false,
            htif_tohost_addr: 0,
            htif_tohost_value: 0,
            hartid: 0,
            peers: Rc::new(vec![mb.clone()]),
            mb,
            idle_on_wfi: false,
            waiting_for_interrupt: false,
            device_touched: false,
            round_start_instret: 0,
            trace: false,
            trace_out: None,
            instret: 0,
            counter_written: false,
        }
    }

    // --- registers -------------------------------------------------------------

    /// x0 is hardwired to zero: reads always yield 0 and writes are discarded.
    /// Compilers rely on this constantly (`add x0, x0, x0` is the canonical NOP).
    #[inline(always)]
    pub fn read_reg(&mut self, index: usize) -> u64 {
        if index == 0 || index >= NUM_REGS {
            return 0;
        }
        if let Some(rf) = self.regfile.as_mut() {
            return rf.read(index);
        }
        self.regs[index]
    }

    #[inline(always)]
    pub fn write_reg(&mut self, index: usize, value: u64) {
        if index == 0 || index >= NUM_REGS {
            return;
        }
        if let Some(rf) = self.regfile.as_mut() {
            rf.write(index, value);
            return;
        }
        self.regs[index] = value;
    }

    /// One integer ALU operation: the installed backend, or the plain ALU.
    #[inline(always)]
    pub(crate) fn alu(&mut self, op: AluOp, a: u64, b: u64) -> u64 {
        if let Some(be) = self.alu_backend.as_mut() {
            return be.exec(op, a, b);
        }
        alu_reference(op, a, b)
    }

    // --- parked / idle -----------------------------------------------------------

    pub fn parked(&self) -> bool {
        self.mb.parked.get()
    }
    pub fn set_parked(&self, p: bool) {
        self.mb.parked.set(p);
    }

    /// Apply anything other harts left in this hart's mailbox.
    fn apply_mailbox(&mut self) {
        let flags = self.mb.pending.get();
        if flags == 0 {
            return;
        }
        self.mb.pending.set(0);
        if flags & MB_IPI != 0 {
            self.csrs.raise_interrupt(csr::MIP_SSIP);
        }
        if flags & MB_FENCE != 0 {
            self.remote_fence();
        }
        if flags & MB_START != 0 {
            let (addr, opaque) = (self.mb.start_addr.get(), self.mb.start_opaque.get());
            self.start_at(addr, opaque);
        }
    }

    /// Can this hart execute an instruction right now? False if parked, or if it is
    /// waiting in WFI and nothing it has enabled is pending. Also clears the wait
    /// when an interrupt has arrived.
    pub fn runnable(&mut self) -> bool {
        self.apply_mailbox();
        if self.parked() {
            return false;
        }
        if self.waiting_for_interrupt {
            if self.csrs.pending_enabled() == 0 {
                return false;
            }
            self.waiting_for_interrupt = false; // woken - even if SIE is clear
        }
        true
    }

    /// Release this hart at `addr`, as SBI's hart_start does: supervisor mode,
    /// translation off, interrupts off, a0 = its hart id, a1 = the caller's opaque
    /// value.
    pub fn start_at(&mut self, addr: u64, opaque: u64) {
        self.mb.parked.set(false);
        self.waiting_for_interrupt = false;
        self.priv_ = PRIV_SUPERVISOR;
        self.pc = addr;
        self.csrs.write(csr::SATP, 0);
        let st = self.csrs.mstatus() & !(csr::MSTATUS_SIE | csr::MSTATUS_SPIE);
        self.csrs.set_mstatus(st);
        self.write_reg(10, self.hartid as u64);
        self.write_reg(11, opaque);
        self.clear_reservation();
        self.remote_fence();
    }

    /// Drop this hart's TLB and decoded-instruction cache (SBI remote fence).
    pub fn remote_fence(&mut self) {
        self.mmu.flush();
        self.flush_decode_cache();
    }

    /// Throw away every cached decoded instruction. Called when the mapping from
    /// virtual address to instruction bytes may have changed: SFENCE.VMA, a write to
    /// satp, and FENCE.I. (A write to the *bytes* is caught separately, by DRAM.)
    pub fn flush_decode_cache(&mut self) {
        for e in self.dcache.iter_mut() {
            e.pc = !0;
        }
    }

    /// Clear any outstanding LR/SC reservation. Called on trap entry: a context
    /// switch between an LR and its SC must make the SC fail, or two threads could
    /// both believe they won the same lock.
    pub fn clear_reservation(&mut self) {
        self.reservation_valid = false;
        self.mb.reservation.set(NO_RES);
    }

    // --- memory ------------------------------------------------------------------

    /// The privilege a load or store runs at. Normally the current mode, but
    /// mstatus.MPRV makes machine-mode data accesses use MPP's privilege instead -
    /// how M-mode firmware reaches a supervisor's address space. It deliberately
    /// does not affect instruction fetch.
    #[inline(always)]
    fn data_privilege(&self) -> u32 {
        if self.priv_ == PRIV_MACHINE && self.csrs.mstatus() & csr::MSTATUS_MPRV != 0 {
            self.csrs.mstatus_mpp()
        } else {
            self.priv_
        }
    }

    /// Does this access span two pages, and so need two translations? RISC-V
    /// permits misaligned accesses, so ordinary guest code reaches this.
    #[inline(always)]
    pub fn crosses_page(vaddr: u64, size: usize) -> bool {
        (vaddr & (PAGE_SIZE - 1)) + size as u64 > PAGE_SIZE
    }

    pub fn mem_load(&mut self, vaddr: u64, size: usize, ty: AccessType) -> Res<u64> {
        if Self::crosses_page(vaddr, size) {
            return self.load_across_pages(vaddr, size, ty);
        }
        let p = if ty == AccessType::Instruction { self.priv_ } else { self.data_privilege() };
        let pa = self.mmu.translate(&self.bus, &self.csrs, vaddr, ty, p)?;
        self.bus.load(pa, size, ty)
    }

    pub fn mem_store(&mut self, vaddr: u64, size: usize, value: u64) -> Res<()> {
        if Self::crosses_page(vaddr, size) {
            return self.store_across_pages(vaddr, size, value);
        }
        let p = self.data_privilege();
        let pa = self.mmu.translate(&self.bus, &self.csrs, vaddr, AccessType::Store, p)?;
        self.bus.store(pa, size, value)?;
        if self.peers.len() > 1 {
            self.note_store(pa, size);
        }
        Ok(())
    }

    /// A store to `paddr` just completed on this hart. Any other hart holding a
    /// reservation on that location loses it, so its SC fails. Reservations are
    /// tracked at 8-byte granularity; the architecture allows a larger granule, which
    /// only ever makes an SC fail more often, never less - and a spurious failure is
    /// always legal.
    fn note_store(&self, paddr: u64, size: usize) {
        let first = paddr >> 3;
        let last = (paddr + size as u64 - 1) >> 3;
        for (i, mb) in self.peers.iter().enumerate() {
            if i as u32 == self.hartid {
                continue;
            }
            let g = mb.reservation.get();
            if g != NO_RES && g >= first && g <= last {
                mb.reservation.set(NO_RES);
            }
        }
    }

    // Accesses that straddle a page boundary. Translation is per page, so an access
    // whose bytes fall in two pages needs two translations - and the second page may
    // be unmapped, read-only, or owned by somebody else. Translating only the
    // starting address and reading contiguous *physical* bytes would walk off the end
    // of the first page's frame with no fault and no permission check. The split path
    // is byte at a time, which is slow and does not matter: only the rare access that
    // actually crosses pays for it.
    fn load_across_pages(&mut self, vaddr: u64, size: usize, ty: AccessType) -> Res<u64> {
        let p = self.data_privilege();
        let mut value = 0u64;
        for i in 0..size {
            let pa = self.mmu.translate(&self.bus, &self.csrs, vaddr.wrapping_add(i as u64), ty, p)?;
            let b = self.bus.load(pa, 1, ty)?;
            value |= (b & 0xff) << (8 * i);
        }
        Ok(value)
    }

    fn store_across_pages(&mut self, vaddr: u64, size: usize, value: u64) -> Res<()> {
        let p = self.data_privilege();
        // Both halves are translated before either is written. A store that faulted
        // halfway would leave the first page modified by an instruction the guest is
        // about to re-execute after fixing the mapping.
        for i in 0..size {
            self.mmu.translate(&self.bus, &self.csrs, vaddr.wrapping_add(i as u64), AccessType::Store, p)?;
        }
        for i in 0..size {
            let pa = self.mmu.translate(&self.bus, &self.csrs, vaddr.wrapping_add(i as u64), AccessType::Store, p)?;
            self.bus.store(pa, 1, (value >> (8 * i)) & 0xff)?;
            if self.peers.len() > 1 {
                self.note_store(pa, 1);
            }
        }
        Ok(())
    }

    // --- fetch -------------------------------------------------------------------

    /// Fetch the instruction at the PC, whatever length it turns out to be.
    ///
    /// With the C extension, length is not known until the first halfword has been
    /// read: bits [1:0] == 11 means 32 bits, anything else 16. That is not merely a
    /// formality: a 32-bit instruction may **straddle a page boundary**, with its
    /// first halfword on a mapped page and its second on one that is not - and then
    /// the correct behaviour is an instruction page fault reporting the address of the
    /// *second* halfword, not of the PC. Two accesses give each halfword its own
    /// translation, which is what the hardware does.
    pub fn fetch_inst(&mut self) -> Res<DecodedInst> {
        // IALIGN is 16 bits when C is implemented, so only bit 0 must be clear.
        if self.pc & 1 != 0 {
            return Err(Trap::new(Exception::InstructionAddressMisaligned, self.pc));
        }
        let low = self.mem_load(self.pc, 2, AccessType::Instruction)? as u16;
        if !is_32bit_instruction(low) {
            return Ok(decode16(low, 0));
        }
        let high = self.mem_load(self.pc + 2, 2, AccessType::Instruction)? as u16;
        Ok(decode16(low, high))
    }

    /// The raw 32-bit instruction word at the current PC - the *expansion*, if the
    /// instruction there is compressed. For tests.
    pub fn fetch(&mut self) -> Res<u32> {
        Ok(self.fetch_inst()?.raw)
    }

    // Remember the instruction just fetched at `pc`, if it is safe to.
    fn cache_fill(&mut self, inst: &DecodedInst) {
        // A 32-bit instruction whose second halfword is on the next page was
        // fetched with two translations, and the second page can be remapped
        // independently.
        if inst.length == 4 && (self.pc & (PAGE_SIZE - 1)) == PAGE_SIZE - 2 {
            return;
        }
        let Ok(pa) = self.mmu.translate(&self.bus, &self.csrs, self.pc, AccessType::Instruction, self.priv_) else {
            return;
        };
        if pa < DRAM_BASE {
            return;
        }
        let offset = pa - DRAM_BASE;
        let ram = &self.bus.ram;
        if !ram.in_range(offset, inst.length as usize) {
            return; // MMIO or off the end
        }
        ram.mark_code_page(offset);
        let idx = ((self.pc >> 1) as usize) & (DCACHE_SLOTS - 1);
        self.dcache[idx] = DecodeEntry { pc: self.pc, priv_: self.priv_ as u8, inst: *inst };
    }

    // --- the step loop -------------------------------------------------------------

    /// Run one instruction: check for a pending interrupt, then fetch, decode and
    /// execute. `Err` is a trap that stopped the machine (no handler installed).
    pub fn step(&mut self) -> Result<(), Trap> {
        self.step_core()
    }

    /// One instruction with no device servicing. Everything that does not depend on
    /// another part of the machine happens here.
    #[inline]
    pub fn step_core(&mut self) -> Result<(), Trap> {
        // Interrupts are checked before the fetch, not after the instruction. An
        // interrupt is not caused by the instruction at the PC - it is an external
        // event that happens *between* instructions, so the instruction that has not
        // run yet must not run, and mepc must point at it so that MRET resumes
        // exactly there.
        if let Some(intr) = self.next_interrupt() {
            self.take_interrupt(intr);
            return Ok(());
        }

        // Look in the decode cache before paying for a fetch.
        let epoch = self.bus.ram.code_epoch();
        if epoch != self.dcache_epoch {
            // Something wrote to a page we had cached code from.
            self.flush_decode_cache();
            self.dcache_epoch = epoch;
        }
        let idx = ((self.pc >> 1) as usize) & (DCACHE_SLOTS - 1);
        let e = &self.dcache[idx];
        let inst = if e.pc == self.pc && e.priv_ == self.priv_ as u8 {
            e.inst
        } else {
            match self.fetch_inst() {
                Ok(i) => {
                    self.cache_fill(&i);
                    i
                }
                Err(t) => return self.handle_trap_or_stop(t),
            }
        };

        // Default: fall through to the next instruction. Jumps and branches
        // overwrite this inside execute(). The step is the instruction's own length,
        // which is where the C extension reaches the execution loop.
        self.next_pc = self.pc + inst.length;
        self.counter_written = false;

        if self.trace {
            self.trace_inst(&inst);
        }

        if let Err(t) = self.execute(&inst) {
            return self.handle_trap_or_stop(t);
        }

        self.pc = self.next_pc;
        self.instret += 1;

        // One instruction per cycle. An instruction that wrote a counter does not
        // also increment it: the spec requires the value read back afterwards to be
        // exactly what was written.
        if !self.counter_written {
            self.csrs.tick_counters();
        }
        Ok(())
    }

    /// Run until a trap stops execution or `max_steps` instructions have retired.
    /// Returns the instructions retired and the stopping trap, if any.
    pub fn run(&mut self, max_steps: u64) -> (u64, Option<Trap>) {
        let mut n = 0;
        while n < max_steps {
            if let Err(t) = self.step_core() {
                return (n, Some(t));
            }
            if self.halted {
                return (n + 1, None);
            }
            n += 1;
        }
        (n, None)
    }

    /// Run up to `max_steps` instructions, stopping early on a fatal trap, a halt
    /// request, a hart stop, a WFI with nothing pending (when idling), or a guest
    /// access to a device (so the machine can poll its devices at once). Returns the
    /// instructions retired and the stopping trap, if any.
    pub fn run_core(&mut self, max_steps: u64) -> (u64, Option<Trap>) {
        let mut n = 0u64;
        self.device_touched = false;
        let mmio_before = self.bus.mmio_epoch();
        let clock = self.clock.clone();
        while n < max_steps {
            // A WFI parks the hart until something it has enabled becomes pending.
            // Checking here means the wait also ends on an interrupt that arrives
            // between scheduling rounds.
            if self.waiting_for_interrupt && !self.runnable() {
                break;
            }
            if let Err(t) = self.step_core() {
                return (n, Some(t));
            }
            n += 1;
            if let Some(c) = &clock {
                c.live.set(self.instret - self.round_start_instret);
            }
            if self.halted || self.parked() {
                break;
            }
            if self.bus.mmio_epoch() != mmio_before {
                self.device_touched = true;
                break;
            }
        }
        (n, None)
    }

    // --- traps ---------------------------------------------------------------------

    /// Dispatch a trap to the guest's handler, or stop the machine if there is no
    /// handler installed yet.
    fn handle_trap_or_stop(&mut self, trap: Trap) -> Result<(), Trap> {
        self.last_trap = Some(trap);
        if self.trap_fatal_without_handler && !self.handler_installed_for(&trap) {
            // Leave pc on the faulting instruction, which is both what a register
            // dump wants to show and what mepc would have received.
            return Err(trap);
        }
        self.take_trap(trap);
        Ok(())
    }

    /// Would this trap reach a vector the guest actually wrote? The check follows the
    /// same delegation decision enter_trap() will make, not just mtvec: xv6 delegates
    /// every exception to supervisor mode, installs stvec, and never writes mtvec at
    /// all.
    fn handler_installed_for(&self, trap: &Trap) -> bool {
        let to_supervisor =
            self.priv_ <= PRIV_SUPERVISOR && self.csrs.delegated_exception(trap.cause_code());
        self.csrs.read(if to_supervisor { csr::STVEC } else { csr::MTVEC }) != 0
    }

    /// The trap-entry sequence, fixed by the privileged spec:
    ///
    /// ```text
    ///   mepc    <- the address to resume at
    ///   mcause  <- why we trapped
    ///   mtval   <- extra detail (faulting address, or the instruction bits)
    ///   MPIE    <- MIE          (save whether interrupts were enabled)
    ///   MIE     <- 0            (so the handler is not re-entered)
    ///   MPP     <- current privilege  (remember where to return to)
    ///   pc      <- mtvec
    /// ```
    fn enter_trap(&mut self, cause_code: u64, tval: u64, epc: u64, is_interrupt: bool) {
        // A trap taken in supervisor or user mode goes to supervisor mode when the
        // matching medeleg/mideleg bit is set - which is what makes an OS efficient:
        // without it every page fault would bounce through machine-mode firmware.
        // Traps taken *in* machine mode are never delegated.
        let raw_cause = cause_code & !(1u64 << 63);
        let deleg = if is_interrupt {
            self.csrs.delegated_interrupt(raw_cause)
        } else {
            self.csrs.delegated_exception(raw_cause)
        };
        let to_supervisor = self.priv_ <= PRIV_SUPERVISOR && deleg;

        let mut status = self.csrs.mstatus();

        let target = if to_supervisor {
            self.csrs.write(csr::SEPC, epc);
            self.csrs.write(csr::SCAUSE, cause_code);
            self.csrs.write(csr::STVAL, tval);

            // Save SIE into SPIE, clear SIE, and record where we came from. SPP is a
            // single bit because supervisor traps only come from S or U mode.
            let sie = status & csr::MSTATUS_SIE != 0;
            status = if sie { status | csr::MSTATUS_SPIE } else { status & !csr::MSTATUS_SPIE };
            status &= !csr::MSTATUS_SIE;
            status = if self.priv_ == PRIV_SUPERVISOR { status | csr::MSTATUS_SPP } else { status & !csr::MSTATUS_SPP };
            self.csrs.set_mstatus(status);
            self.priv_ = PRIV_SUPERVISOR;
            self.csrs.read(csr::STVEC)
        } else {
            self.csrs.write(csr::MEPC, epc);
            self.csrs.write(csr::MCAUSE, cause_code);
            self.csrs.write(csr::MTVAL, tval);

            let mie = status & csr::MSTATUS_MIE != 0;
            status = if mie { status | csr::MSTATUS_MPIE } else { status & !csr::MSTATUS_MPIE };
            status &= !csr::MSTATUS_MIE;
            status = (status & !csr::MSTATUS_MPP) | ((self.priv_ as u64) << csr::MSTATUS_MPP_SHIFT);
            self.csrs.set_mstatus(status);
            self.priv_ = PRIV_MACHINE;
            self.csrs.read(csr::MTVEC)
        };

        let base = target & !csr::MTVEC_MODE_MASK;
        let mode = target & csr::MTVEC_MODE_MASK;
        self.pc = if mode == csr::MTVEC_MODE_VECTORED && is_interrupt {
            base + 4 * (cause_code & 0x3f)
        } else {
            base
        };

        // A trap may be a context switch. If one thread does LR, is interrupted, and
        // another thread runs, the first thread's SC must fail when it resumes.
        self.clear_reservation();
    }

    /// An exception resumes at the instruction that caused it. That is right for a
    /// page fault (fix the mapping, retry); for ECALL the handler advances mepc by 4
    /// itself before returning.
    pub fn take_trap(&mut self, trap: Trap) {
        self.enter_trap(trap.cause_code(), trap.tval, self.pc, false);
    }

    /// An interrupt resumes at the instruction that has not run yet.
    pub fn take_interrupt(&mut self, intr: Interrupt) {
        self.enter_trap(intr.cause_code(), 0, self.pc, true);
    }

    /// The highest-priority interrupt that is pending, enabled, and permitted by the
    /// current privilege and global enables - if any.
    #[inline(always)]
    pub fn next_interrupt(&self) -> Option<Interrupt> {
        let ready = self.csrs.pending_enabled();
        if ready == 0 {
            return None;
        }
        let mideleg = self.csrs.read(csr::MIDELEG);

        // An interrupt is enabled for a mode when we are *below* that mode, or when
        // we are *in* that exact mode and its global enable is set. The "exact mode"
        // half matters for the supervisor case: trap entry into machine mode clears
        // MIE but leaves SIE alone, so a hart that trapped out of supervisor mode sits
        // in M-mode with SIE still set. Testing sstatus.SIE without also checking that
        // we are in supervisor mode would let a delegated interrupt fire in machine
        // mode, and enter_trap would deliver a supervisor-cause interrupt to mtvec.
        let m_enabled = self.priv_ < PRIV_MACHINE || self.csrs.mstatus_mie();
        let s_enabled = self.priv_ < PRIV_SUPERVISOR || (self.priv_ == PRIV_SUPERVISOR && self.csrs.mstatus_sie());

        // Machine-mode interrupts (those not delegated) outrank supervisor ones.
        let m_ready = ready & !mideleg;
        let s_ready = ready & mideleg;
        let candidates = if m_enabled && m_ready != 0 {
            m_ready
        } else if s_enabled && s_ready != 0 {
            s_ready
        } else {
            0
        };
        if candidates == 0 {
            return None;
        }

        // Priority is fixed by the spec and is not bit order: external, then software,
        // then timer, machine before supervisor.
        if candidates & csr::MIP_MEIP != 0 {
            return Some(Interrupt::MachineExternal);
        }
        if candidates & csr::MIP_MSIP != 0 {
            return Some(Interrupt::MachineSoftware);
        }
        if candidates & csr::MIP_MTIP != 0 {
            return Some(Interrupt::MachineTimer);
        }
        if candidates & csr::MIP_SEIP != 0 {
            return Some(Interrupt::SupervisorExternal);
        }
        if candidates & csr::MIP_SSIP != 0 {
            return Some(Interrupt::SupervisorSoftware);
        }
        if candidates & csr::MIP_STIP != 0 {
            return Some(Interrupt::SupervisorTimer);
        }
        None
    }

    /// Redirect control flow to `target`, or raise InstructionAddressMisaligned.
    ///
    /// The spec is specific about where this trap is reported: on the *jump or
    /// branch* instruction, not on the target. So the check happens here, before
    /// next_pc is committed, so the PC left behind (and later mepc) points at the
    /// instruction that did the jumping. IALIGN is 16 with the C extension: only an
    /// odd address is a misaligned jump.
    #[inline(always)]
    pub(crate) fn set_branch_target(&mut self, target: u64) -> Res<()> {
        if target & 1 != 0 {
            return Err(Trap::new(Exception::InstructionAddressMisaligned, target));
        }
        self.next_pc = target;
        Ok(())
    }

    // --- debug output ---------------------------------------------------------------

    fn trace_inst(&mut self, inst: &DecodedInst) {
        // One compact line per instruction, on stderr so it does not get mixed into
        // the guest's own console output on stdout.
        let mut s = format!("0x{:016x}  {:08x}  {:<6}", self.pc, inst.raw, mnemonic(inst));
        let reg = |label: &str, n: usize| format!(" {}=x{}({})", label, n, REG_ABI_NAMES[n]);

        // Print only the operands the instruction's format actually uses. For an
        // I-type instruction the rs2 bits are part of the immediate, so showing
        // "rs2=28" next to a shift invites you to chase a register that is not
        // involved.
        let is_shift_imm = (inst.opcode == crate::decoder::opcodes::OP_IMM
            || inst.opcode == crate::decoder::opcodes::OP_IMM_32)
            && (inst.funct3 == 0x1 || inst.funct3 == 0x5);

        match inst.fmt {
            Format::R => {
                s += &reg("rd", inst.rd);
                s += &reg("rs1", inst.rs1);
                s += &reg("rs2", inst.rs2);
            }
            Format::I => {
                if inst.opcode == crate::decoder::opcodes::SYSTEM && inst.funct3 == 0 {
                    // ECALL/EBREAK/MRET/WFI have no meaningful operands at all.
                } else if inst.opcode == crate::decoder::opcodes::SYSTEM {
                    s += &reg("rd", inst.rd);
                    s += &format!(" csr=0x{:x}", inst.imm & 0xfff);
                    if inst.funct3 & 0x4 != 0 {
                        s += &format!(" uimm={}", inst.rs1);
                    } else {
                        s += &reg("rs1", inst.rs1);
                    }
                } else {
                    s += &reg("rd", inst.rd);
                    s += &reg("rs1", inst.rs1);
                    if is_shift_imm {
                        let sh = if inst.opcode == crate::decoder::opcodes::OP_IMM { inst.shamt6() } else { inst.shamt5() };
                        s += &format!(" shamt={}", sh);
                    } else {
                        s += &format!(" imm={}", inst.imm);
                    }
                }
            }
            Format::S | Format::B => {
                s += &reg("rs1", inst.rs1);
                s += &reg("rs2", inst.rs2);
                s += &format!(" imm={}", inst.imm);
                if inst.fmt == Format::B {
                    s += &format!(" -> 0x{:x}", self.pc.wrapping_add(inst.imm as u64));
                }
            }
            Format::U => {
                s += &reg("rd", inst.rd);
                s += &format!(" imm=0x{:x}", inst.imm as u64);
            }
            Format::J => {
                s += &reg("rd", inst.rd);
                s += &format!(" -> 0x{:x}", self.pc.wrapping_add(inst.imm as u64));
            }
            Format::Unknown => {}
        }
        s.push('\n');
        match self.trace_out.as_mut() {
            Some(w) => {
                let _ = w.write_all(s.as_bytes());
            }
            None => eprint!("{}", s),
        }
    }

    pub fn dump_registers(&mut self, os: &mut dyn Write) {
        let _ = writeln!(os, "=== register dump ===");
        let _ = writeln!(os, "pc  : 0x{:016x}", self.pc);
        for i in 0..NUM_REGS {
            // Build the label first so it can be padded to a fixed width; tabs give
            // ragged columns because the names vary from 2 to 4 characters.
            let label = format!("x{} ({})", i, REG_ABI_NAMES[i]);
            let v = self.read_reg(i);
            let _ = writeln!(os, "{:<12}: 0x{:016x}", label, v);
        }
        let _ = writeln!(os, "instret: {}", self.instret);

        // Privilege and the CSRs that explain where a kernel has got to. Without
        // these a dump from a booting OS says almost nothing: the same PC means very
        // different things in machine mode with paging off and in supervisor mode with
        // a page table installed.
        let _ = writeln!(os, "priv   : {}", privilege_name(self.priv_));
        for (label, addr) in [
            ("mstatus", csr::MSTATUS),
            ("mcause", csr::MCAUSE),
            ("mepc", csr::MEPC),
            ("mtvec", csr::MTVEC),
            ("scause", csr::SCAUSE),
            ("sepc", csr::SEPC),
            ("stval", csr::STVAL),
            ("stvec", csr::STVEC),
            ("satp", csr::SATP),
            ("mip", csr::MIP),
            ("mie", csr::MIE),
        ] {
            let _ = writeln!(os, "{:<12}: 0x{:016x}", label, self.csrs.read(addr));
        }
        let _ = writeln!(os, "=====================");
    }
}
