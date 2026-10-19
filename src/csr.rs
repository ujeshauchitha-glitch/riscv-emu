//! Control and Status Registers.
//!
//! CSRs are a separate register file from x0-x31, addressed by a 12-bit number and
//! reachable only through the six CSR instructions. They hold everything the
//! integer registers cannot: what privilege mode we are in, where the trap handler
//! lives, why the last trap happened, which interrupts are enabled, and where the
//! page tables are.
//!
//! Two structural details are encoded in the CSR *address* itself:
//!
//! ```text
//!   bits [11:10]  00, 01, 10 = read/write;  11 = read-only
//!   bits  [9:8]   the lowest privilege level that may access it
//! ```
//!
//! So a write to any CSR numbered 0xC00-0xFFF is an illegal instruction, and a read
//! of a machine CSR from user mode is too - without any per-register table.

use crate::types::{PRIV_MACHINE, PRIV_SUPERVISOR, PRIV_USER};

pub mod csr {
    // --- machine information (read-only) ---
    pub const MVENDORID: u32 = 0xf11;
    pub const MARCHID: u32 = 0xf12;
    pub const MIMPID: u32 = 0xf13;
    pub const MHARTID: u32 = 0xf14;

    // --- machine trap setup ---
    pub const MSTATUS: u32 = 0x300;
    pub const MISA: u32 = 0x301;
    pub const MEDELEG: u32 = 0x302;
    pub const MIDELEG: u32 = 0x303;
    pub const MIE: u32 = 0x304;
    pub const MTVEC: u32 = 0x305;
    pub const MCOUNTEREN: u32 = 0x306;

    // --- machine trap handling ---
    pub const MSCRATCH: u32 = 0x340;
    pub const MEPC: u32 = 0x341;
    pub const MCAUSE: u32 = 0x342;
    pub const MTVAL: u32 = 0x343;
    pub const MIP: u32 = 0x344;

    // --- floating point: three addresses onto one register ---
    pub const FFLAGS: u32 = 0x001;
    pub const FRM: u32 = 0x002;
    pub const FCSR: u32 = 0x003;

    // --- supervisor ---
    //
    // sstatus, sie and sip are NOT separate registers. They are restricted *views*
    // onto mstatus, mie and mip: the same physical bits with the machine-only ones
    // hidden. Modelling them as separate storage is a classic emulator bug.
    pub const SSTATUS: u32 = 0x100;
    pub const SIE: u32 = 0x104;
    pub const STVEC: u32 = 0x105;
    pub const SCOUNTEREN: u32 = 0x106;
    pub const SSCRATCH: u32 = 0x140;
    pub const SEPC: u32 = 0x141;
    pub const SCAUSE: u32 = 0x142;
    pub const STVAL: u32 = 0x143;
    pub const SIP: u32 = 0x144;
    pub const SATP: u32 = 0x180;

    /// Sstc: the supervisor timer. Gated by menvcfg.STCE.
    pub const STIMECMP: u32 = 0x14d;
    pub const MENVCFG: u32 = 0x30a;
    pub const MENVCFG_STCE: u64 = 1 << 63;
    pub const MENVCFG_ADUE: u64 = 1 << 61;

    // --- physical memory protection: stored, not enforced ---
    pub const PMPCFG0: u32 = 0x3a0;
    pub const PMPCFG2: u32 = 0x3a2;
    pub const PMPADDR0: u32 = 0x3b0;
    pub const PMPADDR15: u32 = 0x3bf;

    // --- counters ---
    pub const MCYCLE: u32 = 0xb00;
    pub const MINSTRET: u32 = 0xb02;
    pub const CYCLE: u32 = 0xc00;
    pub const TIME: u32 = 0xc01;
    pub const INSTRET: u32 = 0xc02;

    // --- mstatus fields (RV64) ---
    pub const MSTATUS_SIE: u64 = 1 << 1;
    pub const MSTATUS_MIE: u64 = 1 << 3;
    pub const MSTATUS_SPIE: u64 = 1 << 5;
    pub const MSTATUS_MPIE: u64 = 1 << 7;
    pub const MSTATUS_SPP: u64 = 1 << 8;
    pub const MSTATUS_MPP: u64 = 3 << 11;
    pub const MSTATUS_MPP_SHIFT: u32 = 11;
    /// The floating-point unit's state: 0 Off, 1 Initial, 2 Clean, 3 Dirty. It is
    /// also an enable: while FS is Off every floating-point instruction traps.
    pub const MSTATUS_FS: u64 = 3 << 13;
    pub const MSTATUS_FS_SHIFT: u32 = 13;
    pub const MSTATUS_FS_OFF: u64 = 0;
    pub const MSTATUS_FS_INITIAL: u64 = 1 << 13;
    pub const MSTATUS_FS_CLEAN: u64 = 2 << 13;
    pub const MSTATUS_FS_DIRTY: u64 = 3 << 13;
    /// SD, the topmost bit, is a read-only summary: set when FS is Dirty.
    pub const MSTATUS_SD: u64 = 1 << 63;
    pub const MSTATUS_MPRV: u64 = 1 << 17;
    pub const MSTATUS_SUM: u64 = 1 << 18;
    pub const MSTATUS_MXR: u64 = 1 << 19;
    pub const MSTATUS_TVM: u64 = 1 << 20;
    pub const MSTATUS_TW: u64 = 1 << 21;
    pub const MSTATUS_TSR: u64 = 1 << 22;

    /// UXL and SXL: the register width of user and supervisor mode (1 = 32 bits,
    /// 2 = 64 bits). On an RV64-only machine they are read-only and read as 2.
    pub const MSTATUS_UXL: u64 = 3 << 32;
    pub const MSTATUS_SXL: u64 = 3 << 34;
    pub const MSTATUS_XL_RV64: u64 = 2;

    /// The bits sstatus exposes.
    pub const SSTATUS_MASK: u64 = MSTATUS_SIE
        | MSTATUS_SPIE
        | MSTATUS_SPP
        | MSTATUS_SUM
        | MSTATUS_MXR
        | MSTATUS_FS
        | MSTATUS_UXL
        | MSTATUS_SD;
    /// ...of which UXL is visible but not writable.
    pub const SSTATUS_WRITE_MASK: u64 = SSTATUS_MASK & !MSTATUS_UXL;

    /// The interrupt bits sie/sip expose.
    pub const SIE_SIP_MASK: u64 = (1 << 1) | (1 << 5) | (1 << 9);

    // --- fcsr ---
    pub const FCSR_FFLAGS_MASK: u64 = 0x1f;
    pub const FCSR_FRM_MASK: u64 = 0x7 << 5;
    pub const FCSR_FRM_SHIFT: u32 = 5;

    // The five accrued exception flags, in the order the spec numbers them.
    pub const FFLAG_NX: u64 = 1 << 0;
    pub const FFLAG_UF: u64 = 1 << 1;
    pub const FFLAG_OF: u64 = 1 << 2;
    pub const FFLAG_DZ: u64 = 1 << 3;
    pub const FFLAG_NV: u64 = 1 << 4;

    // Rounding modes. DYN in an instruction's rm field means "use frm".
    pub const FRM_RNE: u32 = 0;
    pub const FRM_RTZ: u32 = 1;
    pub const FRM_RDN: u32 = 2;
    pub const FRM_RUP: u32 = 3;
    pub const FRM_RMM: u32 = 4;
    pub const FRM_DYN: u32 = 7;

    // --- satp ---
    pub const SATP_MODE_SHIFT: u32 = 60;
    pub const SATP_MODE_MASK: u64 = 0xf << 60;
    pub const SATP_ASID_MASK: u64 = 0xffff << 44;
    pub const SATP_PPN_MASK: u64 = (1 << 44) - 1;
    pub const SATP_MODE_BARE: u64 = 0;
    pub const SATP_MODE_SV39: u64 = 8;
    pub const SATP_MODE_SV48: u64 = 9;

    // --- mie / mip interrupt bits: bit position == cause number ---
    pub const MIP_SSIP: u64 = 1 << 1;
    pub const MIP_MSIP: u64 = 1 << 3;
    pub const MIP_STIP: u64 = 1 << 5;
    pub const MIP_MTIP: u64 = 1 << 7;
    pub const MIP_SEIP: u64 = 1 << 9;
    pub const MIP_MEIP: u64 = 1 << 11;

    // --- mtvec modes ---
    pub const MTVEC_MODE_MASK: u64 = 0x3;
    pub const MTVEC_MODE_DIRECT: u64 = 0;
    pub const MTVEC_MODE_VECTORED: u64 = 1;

    /// True if writing this CSR address is architecturally forbidden.
    pub const fn is_read_only(addr: u32) -> bool {
        ((addr >> 10) & 0x3) == 0x3
    }

    /// The lowest privilege level permitted to access this CSR (0=U, 1=S, 3=M).
    pub const fn min_privilege(addr: u32) -> u32 {
        (addr >> 8) & 0x3
    }
}

use csr::*;

/// The MXL field in misa: 2 = RV64, in the top two bits.
const MISA_MXL_64: u64 = 2 << 62;

const fn misa_ext(c: u8) -> u64 {
    1 << (c - b'A')
}

/// Every interrupt source that exists, machine and supervisor.
const INTERRUPT_MASK: u64 = MIP_MSIP | MIP_MTIP | MIP_MEIP | MIP_SSIP | MIP_STIP | MIP_SEIP;

/// The supervisor pending bits M-mode software may write directly. Unlike the
/// machine ones - which the CLINT and PLIC own - SSIP is genuinely software-set.
const MIP_SOFTWARE_WRITABLE: u64 = MIP_SSIP | MIP_STIP | MIP_SEIP;

/// mstatus bits that are writable. Everything else is WPRI (reserved) or
/// read-only and must read back unchanged however the guest writes it.
const MSTATUS_MASK: u64 = MSTATUS_MIE
    | MSTATUS_MPIE
    | MSTATUS_MPP
    | MSTATUS_SIE
    | MSTATUS_SPIE
    | MSTATUS_SPP
    | MSTATUS_MPRV
    | MSTATUS_SUM
    | MSTATUS_MXR
    | MSTATUS_TVM
    | MSTATUS_TW
    | MSTATUS_TSR
    | MSTATUS_FS;

pub struct CsrFile {
    raw: Box<[u64; 4096]>,
}

impl Default for CsrFile {
    fn default() -> Self {
        Self::new()
    }
}

impl CsrFile {
    pub fn new() -> CsrFile {
        let mut raw = Box::new([0u64; 4096]);

        // misa advertises the ISA. Guest code reads it to decide what it may use,
        // so it must not claim more than we deliver. 'S' and 'U' announce that
        // supervisor and user mode exist.
        raw[MISA as usize] = MISA_MXL_64
            | misa_ext(b'I')
            | misa_ext(b'M')
            | misa_ext(b'A')
            | misa_ext(b'B')
            | misa_ext(b'C')
            | misa_ext(b'F')
            | misa_ext(b'D')
            | misa_ext(b'S')
            | misa_ext(b'U');

        // mstatus starts with interrupts disabled and MPP = M: a hart comes out of
        // reset in machine mode with nothing set up.
        raw[MSTATUS as usize] = (PRIV_MACHINE as u64) << MSTATUS_MPP_SHIFT;
        // RV64-only machine: user and supervisor mode are both 64-bit. The fields
        // are outside MSTATUS_MASK, which is what makes them read-only.
        raw[MSTATUS as usize] |= (MSTATUS_XL_RV64 << 32) | (MSTATUS_XL_RV64 << 34);

        CsrFile { raw }
    }

    /// mhartid is read-only to software, so the machine numbers its harts here.
    pub fn set_hartid(&mut self, id: u64) {
        self.raw[MHARTID as usize] = id;
    }

    /// Does this CSR exist? Accessing an unimplemented one raises an
    /// illegal-instruction exception, which is the mechanism software uses to probe
    /// for optional features: try the CSR, catch the trap, conclude it is absent.
    pub fn exists(&self, addr: u32) -> bool {
        match addr {
            MVENDORID | MARCHID | MIMPID | MHARTID | MSTATUS | MISA | MEDELEG | MIDELEG | MIE
            | MTVEC | MCOUNTEREN | MSCRATCH | MEPC | MCAUSE | MTVAL | MIP | MCYCLE | MINSTRET
            | CYCLE | TIME | INSTRET | SSTATUS | SIE | STVEC | SCOUNTEREN | SSCRATCH | SEPC
            | SCAUSE | STVAL | SIP | SATP | STIMECMP | MENVCFG | PMPCFG0 | PMPCFG2 | FFLAGS
            | FRM | FCSR => true,
            // PMP address registers are stored but NOT enforced: a permissive
            // configuration (all a kernel like xv6 writes) behaves identically
            // either way, and a restrictive one would be silently ignored - a real
            // limitation, recorded as such.
            a => (PMPADDR0..=PMPADDR15).contains(&a),
        }
    }

    pub fn read(&self, addr: u32) -> u64 {
        match addr {
            // The unprivileged counter shadows alias their machine counterparts.
            CYCLE => self.raw[MCYCLE as usize],
            INSTRET => self.raw[MINSTRET as usize],

            // The supervisor views: the *same bits* as mstatus, with the
            // machine-only ones masked out.
            SSTATUS => self.read(MSTATUS) & SSTATUS_MASK,

            // mstatus.SD is derived, so it can never disagree with FS.
            MSTATUS => {
                let status = self.raw[MSTATUS as usize];
                if status & MSTATUS_FS == MSTATUS_FS_DIRTY {
                    status | MSTATUS_SD
                } else {
                    status & !MSTATUS_SD
                }
            }

            // fcsr is the whole register; fflags and frm are windows onto its two
            // halves, not copies.
            FCSR => self.raw[FCSR as usize],
            FFLAGS => self.raw[FCSR as usize] & FCSR_FFLAGS_MASK,
            FRM => (self.raw[FCSR as usize] & FCSR_FRM_MASK) >> FCSR_FRM_SHIFT,
            SIE => self.raw[MIE as usize] & SIE_SIP_MASK,
            SIP => self.raw[MIP as usize] & SIE_SIP_MASK,

            _ => self.raw[(addr & 0xfff) as usize],
        }
    }

    fn write_mask(addr: u32) -> u64 {
        match addr {
            MSTATUS => MSTATUS_MASK,
            MIE => INTERRUPT_MASK,
            // mip is NOT software-writable for the machine-level bits: MEIP, MTIP
            // and MSIP are driven by hardware. A kernel acknowledges a timer
            // interrupt by moving mtimecmp, not by clearing MTIP.
            MIP => MIP_SOFTWARE_WRITABLE,
            // misa is WARL and we do not support disabling extensions.
            MISA => 0,
            _ => !0,
        }
    }

    pub fn write(&mut self, addr: u32, value: u64) {
        let a = addr & 0xfff;
        match a {
            MTVEC | STVEC => {
                // Base address plus a mode in the low two bits. Only 0 (direct) and
                // 1 (vectored) are defined; the field is WARL, so an unsupported
                // mode is coerced rather than stored.
                let mut mode = value & MTVEC_MODE_MASK;
                if mode > MTVEC_MODE_VECTORED {
                    mode = MTVEC_MODE_DIRECT;
                }
                self.raw[a as usize] = (value & !MTVEC_MODE_MASK) | mode;
            }

            FCSR => self.raw[FCSR as usize] = value & (FCSR_FFLAGS_MASK | FCSR_FRM_MASK),
            FFLAGS => {
                self.raw[FCSR as usize] =
                    (self.raw[FCSR as usize] & !FCSR_FFLAGS_MASK) | (value & FCSR_FFLAGS_MASK)
            }
            FRM => {
                self.raw[FCSR as usize] = (self.raw[FCSR as usize] & !FCSR_FRM_MASK)
                    | ((value << FCSR_FRM_SHIFT) & FCSR_FRM_MASK)
            }

            // Writing a supervisor view writes through to the machine register,
            // touching only the bits that view exposes.
            SSTATUS => {
                self.raw[MSTATUS as usize] = (self.raw[MSTATUS as usize] & !SSTATUS_WRITE_MASK)
                    | (value & SSTATUS_WRITE_MASK)
            }
            SIE => {
                self.raw[MIE as usize] =
                    (self.raw[MIE as usize] & !SIE_SIP_MASK) | (value & SIE_SIP_MASK)
            }
            // Only SSIP is writable through sip; STIP and SEIP are hardware's.
            SIP => {
                self.raw[MIP as usize] =
                    (self.raw[MIP as usize] & !MIP_SSIP) | (value & MIP_SSIP)
            }

            SATP => {
                // Only Bare and Sv39 are supported. satp's MODE field is WARL, so an
                // unsupported mode is ignored entirely rather than stored - that is
                // how a kernel discovers Sv48 is unavailable.
                let mode = (value & SATP_MODE_MASK) >> SATP_MODE_SHIFT;
                if mode != SATP_MODE_BARE && mode != SATP_MODE_SV39 {
                    return;
                }
                self.raw[a as usize] = value;
            }

            // mepc/sepc hold an instruction address, so bit 0 is hardwired to zero.
            // Bit 1 is *not*: with the C extension IALIGN is 16 and instructions
            // legitimately live at 2-byte-aligned addresses. (Masking it too silently
            // rounds a return address down by two bytes - xv6 compiled for rv64gc
            // is what exposed it.)
            MEPC | SEPC => self.raw[a as usize] = value & !1,

            // Read-only shadows; the caller should have rejected the write.
            CYCLE | TIME | INSTRET => {}

            _ => {
                let m = Self::write_mask(a);
                self.raw[a as usize] = (self.raw[a as usize] & !m) | (value & m);
            }
        }
    }

    // --- convenience accessors for the fields the trap path touches constantly ---

    #[inline(always)]
    pub fn mstatus(&self) -> u64 {
        self.read(MSTATUS)
    }
    pub fn set_mstatus(&mut self, v: u64) {
        self.write(MSTATUS, v)
    }
    #[inline(always)]
    pub fn mstatus_mie(&self) -> bool {
        self.mstatus() & MSTATUS_MIE != 0
    }
    #[inline(always)]
    pub fn mstatus_sie(&self) -> bool {
        self.mstatus() & MSTATUS_SIE != 0
    }
    pub fn mstatus_sum(&self) -> bool {
        self.raw[MSTATUS as usize] & MSTATUS_SUM != 0
    }
    pub fn mstatus_mxr(&self) -> bool {
        self.raw[MSTATUS as usize] & MSTATUS_MXR != 0
    }
    pub fn mstatus_mpp(&self) -> u32 {
        ((self.raw[MSTATUS as usize] & MSTATUS_MPP) >> MSTATUS_MPP_SHIFT) as u32
    }

    /// Counter-enable bits. Bit 0 is CY, 1 is TM, 2 is IR. A less privileged mode
    /// may read a counter only if the next mode up has enabled it.
    pub fn counter_enabled(&self, bit: u32, priv_: u32) -> bool {
        if priv_ >= PRIV_MACHINE {
            return true;
        }
        if (self.read(MCOUNTEREN) >> bit) & 1 == 0 {
            return false;
        }
        if priv_ == PRIV_SUPERVISOR {
            return true;
        }
        (self.read(SCOUNTEREN) >> bit) & 1 != 0
    }

    pub fn sstc_enabled(&self) -> bool {
        self.raw[MENVCFG as usize] & MENVCFG_STCE != 0
    }

    /// Is the floating-point unit usable? While mstatus.FS is Off, every
    /// floating-point instruction and every access to fcsr traps.
    #[inline(always)]
    pub fn fpu_enabled(&self) -> bool {
        self.raw[MSTATUS as usize] & MSTATUS_FS != MSTATUS_FS_OFF
    }

    /// Mark the floating-point registers as modified. A kernel uses this to decide
    /// whether a context switch has to save all 32 of them.
    pub fn mark_fpu_dirty(&mut self) {
        self.raw[MSTATUS as usize] =
            (self.raw[MSTATUS as usize] & !MSTATUS_FS) | MSTATUS_FS_DIRTY;
    }

    /// Accumulate IEEE exception flags into fcsr. They are sticky: nothing but an
    /// explicit write ever clears them.
    pub fn raise_fflags(&mut self, flags: u64) {
        self.raw[FCSR as usize] |= flags & FCSR_FFLAGS_MASK;
    }

    pub fn rounding_mode(&self) -> u32 {
        ((self.raw[FCSR as usize] & FCSR_FRM_MASK) >> FCSR_FRM_SHIFT) as u32
    }

    #[inline(always)]
    pub fn satp(&self) -> u64 {
        self.raw[SATP as usize]
    }
    #[inline(always)]
    pub fn satp_mode(&self) -> u64 {
        (self.satp() & SATP_MODE_MASK) >> SATP_MODE_SHIFT
    }
    pub fn satp_ppn(&self) -> u64 {
        self.satp() & SATP_PPN_MASK
    }
    pub fn satp_asid(&self) -> u64 {
        (self.satp() & SATP_ASID_MASK) >> 44
    }

    /// True if the trap with this cause is delegated to supervisor mode.
    pub fn delegated_exception(&self, cause: u64) -> bool {
        cause < 64 && (self.raw[MEDELEG as usize] >> cause) & 1 != 0
    }
    pub fn delegated_interrupt(&self, cause: u64) -> bool {
        cause < 64 && (self.raw[MIDELEG as usize] >> cause) & 1 != 0
    }

    /// Interrupts that are both pending and enabled.
    #[inline(always)]
    pub fn pending_enabled(&self) -> u64 {
        self.raw[MIP as usize] & self.raw[MIE as usize]
    }

    /// Direct, unmasked access for devices (the CLINT drives MTIP and MSIP, the
    /// PLIC drives MEIP) and for tests.
    pub fn raise_interrupt(&mut self, mip_bit: u64) {
        self.raw[MIP as usize] |= mip_bit;
    }
    pub fn clear_interrupt(&mut self, mip_bit: u64) {
        self.raw[MIP as usize] &= !mip_bit;
    }

    /// Advance the retired-instruction and cycle counters by one.
    ///
    /// These have to *increment*, not be assigned from the emulator's own step
    /// count: minstret is writable, and software that writes it expects counting to
    /// continue from the value it wrote.
    #[inline(always)]
    pub fn tick_counters(&mut self) {
        self.raw[MINSTRET as usize] = self.raw[MINSTRET as usize].wrapping_add(1);
        self.raw[MCYCLE as usize] = self.raw[MCYCLE as usize].wrapping_add(1);
    }
}

/// Privilege level name check helper used by tests.
pub fn is_valid_priv(p: u32) -> bool {
    p == PRIV_USER || p == PRIV_SUPERVISOR || p == PRIV_MACHINE
}
