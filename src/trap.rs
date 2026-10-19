//! Traps: exceptions and interrupts.
//!
//! A trap is a *value*, not a Rust panic and not an unwinding mechanism. Every
//! operation that can fault returns `Result<T, Trap>`, so the fault travels up to
//! the one place that knows what to do with it (the CPU's trap entry), and nothing
//! in between can forget to handle it.

/// Synchronous exception causes, numbered as in `mcause`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u64)]
pub enum Exception {
    InstructionAddressMisaligned = 0,
    InstructionAccessFault = 1,
    IllegalInstruction = 2,
    Breakpoint = 3,
    LoadAddressMisaligned = 4,
    LoadAccessFault = 5,
    StoreAMOAddressMisaligned = 6,
    StoreAMOAccessFault = 7,
    ECallFromUMode = 8,
    ECallFromSMode = 9,
    ECallFromMMode = 11,
    InstructionPageFault = 12,
    LoadPageFault = 13,
    StoreAMOPageFault = 15,
}

impl Exception {
    /// The value that belongs in mcause/scause.
    #[inline]
    pub fn code(self) -> u64 {
        self as u64
    }

    pub fn name(self) -> &'static str {
        use Exception::*;
        match self {
            InstructionAddressMisaligned => "instruction address misaligned",
            InstructionAccessFault => "instruction access fault",
            IllegalInstruction => "illegal instruction",
            Breakpoint => "breakpoint",
            LoadAddressMisaligned => "load address misaligned",
            LoadAccessFault => "load access fault",
            StoreAMOAddressMisaligned => "store/AMO address misaligned",
            StoreAMOAccessFault => "store/AMO access fault",
            ECallFromUMode => "ecall from U-mode",
            ECallFromSMode => "ecall from S-mode",
            ECallFromMMode => "ecall from M-mode",
            InstructionPageFault => "instruction page fault",
            LoadPageFault => "load page fault",
            StoreAMOPageFault => "store/AMO page fault",
        }
    }
}

/// Asynchronous interrupt causes. The enumerator's value is both the cause number
/// and the bit position in `mip`/`mie`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u64)]
pub enum Interrupt {
    SupervisorSoftware = 1,
    MachineSoftware = 3,
    SupervisorTimer = 5,
    MachineTimer = 7,
    SupervisorExternal = 9,
    MachineExternal = 11,
}

impl Interrupt {
    /// mcause value: the interrupt flag in bit 63 plus the cause number.
    pub fn cause_code(self) -> u64 {
        (1u64 << 63) | self as u64
    }
}

/// A trap in flight: why, and the extra detail (faulting address, or the
/// instruction bits) that goes in `mtval`/`stval`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Trap {
    pub cause: Exception,
    pub tval: u64,
}

impl Trap {
    #[inline]
    pub fn new(cause: Exception, tval: u64) -> Trap {
        Trap { cause, tval }
    }
    pub fn cause_code(&self) -> u64 {
        self.cause.code()
    }
}

/// Everything that can fail returns this.
pub type Res<T> = Result<T, Trap>;
