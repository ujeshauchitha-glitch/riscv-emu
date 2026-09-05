#pragma once

#include <array>

#include "types.hpp"

// ---------------------------------------------------------------------------
// Control and Status Registers.
//
// CSRs are a separate register file from x0-x31, addressed by a 12-bit number
// and reachable only through the six CSR instructions. They hold everything the
// integer registers cannot: what privilege mode we are in, where the trap
// handler lives, why the last trap happened, which interrupts are enabled, and
// (from phase 6) where the page tables are.
//
// This is the point where the emulator stops being a calculator and starts
// being a machine an operating system can run on.
//
// Two structural details are encoded in the CSR *address* itself:
//
//   bits [11:10]  00, 01, 10 = read/write;  11 = read-only
//   bits  [9:8]   the lowest privilege level that may access it
//
// So a write to any CSR numbered 0xC00-0xFFF is an illegal instruction, and
// (from phase 6) a read of a machine CSR from user mode is too - without any
// per-register table needing to say so.
// ---------------------------------------------------------------------------

namespace csr {

// --- machine information (read-only) ---
constexpr u32 MVENDORID = 0xf11;
constexpr u32 MARCHID   = 0xf12;
constexpr u32 MIMPID    = 0xf13;
constexpr u32 MHARTID   = 0xf14;

// --- machine trap setup ---
constexpr u32 MSTATUS   = 0x300;
constexpr u32 MISA      = 0x301;
constexpr u32 MEDELEG   = 0x302;
constexpr u32 MIDELEG   = 0x303;
constexpr u32 MIE       = 0x304;
constexpr u32 MTVEC     = 0x305;
constexpr u32 MCOUNTEREN = 0x306;

// --- machine trap handling ---
constexpr u32 MSCRATCH  = 0x340;
constexpr u32 MEPC      = 0x341;
constexpr u32 MCAUSE    = 0x342;
constexpr u32 MTVAL     = 0x343;
constexpr u32 MIP       = 0x344;

// --- supervisor trap setup ---
//
// sstatus, sie and sip are NOT separate registers. They are restricted *views*
// onto mstatus, mie and mip: the same physical bits, with the machine-only ones
// hidden. Modelling them as separate storage is a classic emulator bug - a
// kernel clears sstatus.SIE to disable interrupts, and if that does not
// actually clear mstatus.SIE, interrupts keep arriving inside what the kernel
// believes is a critical section.
// --- floating point (F and D) ---
//
// Three addresses onto one register. fcsr is the whole thing; fflags and frm
// are windows onto its two halves, and software uses them constantly - a
// routine that needs round-toward-zero writes frm alone rather than
// read-modify-writing fcsr.
constexpr u32 FFLAGS    = 0x001;
constexpr u32 FRM       = 0x002;
constexpr u32 FCSR      = 0x003;

constexpr u32 SSTATUS   = 0x100;
constexpr u32 SIE       = 0x104;
constexpr u32 STVEC     = 0x105;
constexpr u32 SCOUNTEREN = 0x106;

// --- supervisor trap handling ---
constexpr u32 SSCRATCH  = 0x140;
constexpr u32 SEPC      = 0x141;
constexpr u32 SCAUSE    = 0x142;
constexpr u32 STVAL     = 0x143;
constexpr u32 SIP       = 0x144;

// --- supervisor address translation ---
constexpr u32 SATP      = 0x180;

// --- Sstc: the supervisor timer ---
//
// Without Sstc, a supervisor asks machine-mode firmware to set the timer for it
// (an SBI call on every tick). Sstc gives supervisor mode its own compare
// register, so a kernel arms its next tick directly. It is gated by
// menvcfg.STCE so that firmware stays in control of whether S-mode may.
constexpr u32 STIMECMP  = 0x14d;

// --- machine environment configuration ---
constexpr u32 MENVCFG   = 0x30a;
constexpr u64 MENVCFG_STCE = 1ull << 63;   // enable Sstc for S-mode
constexpr u64 MENVCFG_ADUE = 1ull << 61;   // hardware updates of PTE A/D bits

// --- physical memory protection ---
//
// PMP restricts what physical addresses each privilege level may touch. It is
// stored here but NOT enforced: see the note in csr.cpp.
constexpr u32 PMPCFG0   = 0x3a0;
constexpr u32 PMPCFG2   = 0x3a2;
constexpr u32 PMPADDR0  = 0x3b0;
constexpr u32 PMPADDR15 = 0x3bf;

// --- counters ---
// The unprivileged shadows (CYCLE, TIME, INSTRET) read the same underlying
// state as their machine-mode counterparts.
constexpr u32 MCYCLE    = 0xb00;
constexpr u32 MINSTRET  = 0xb02;
constexpr u32 CYCLE     = 0xc00;
constexpr u32 TIME      = 0xc01;
constexpr u32 INSTRET   = 0xc02;

// --- mstatus fields (RV64) ---
constexpr u64 MSTATUS_SIE  = 1ull << 1;   // supervisor interrupt enable
constexpr u64 MSTATUS_MIE  = 1ull << 3;   // machine interrupt enable
constexpr u64 MSTATUS_SPIE = 1ull << 5;   // previous SIE
constexpr u64 MSTATUS_MPIE = 1ull << 7;   // previous MIE
constexpr u64 MSTATUS_SPP  = 1ull << 8;   // previous privilege (supervisor)
constexpr u64 MSTATUS_MPP  = 3ull << 11;  // previous privilege (machine), 2 bits
constexpr int MSTATUS_MPP_SHIFT = 11;
// The floating-point unit's state, in two bits: 0 Off, 1 Initial, 2 Clean,
// 3 Dirty. A kernel uses this to avoid saving 32 registers on every context
// switch - it only has to save them if the FPU is Dirty, meaning the process
// actually touched a float since the state was last made Clean.
//
// It is also an enable: while FS is Off, every floating-point instruction and
// every access to fcsr traps as an illegal instruction. That is not a detail -
// it is how a kernel that does not support floating point stops user code from
// silently corrupting a register file nobody is saving.
constexpr u64 MSTATUS_FS       = 3ull << 13;
constexpr int MSTATUS_FS_SHIFT = 13;
constexpr u64 MSTATUS_FS_OFF     = 0ull << MSTATUS_FS_SHIFT;
constexpr u64 MSTATUS_FS_INITIAL = 1ull << MSTATUS_FS_SHIFT;
constexpr u64 MSTATUS_FS_CLEAN   = 2ull << MSTATUS_FS_SHIFT;
constexpr u64 MSTATUS_FS_DIRTY   = 3ull << MSTATUS_FS_SHIFT;

// SD, the topmost bit of mstatus, is a read-only summary: set when FS (or, on
// a machine with vectors, VS) is Dirty. A context switch reads one bit rather
// than picking apart fields.
constexpr u64 MSTATUS_SD   = 1ull << 63;

constexpr u64 MSTATUS_MPRV = 1ull << 17;  // load/store as MPP's privilege
constexpr u64 MSTATUS_SUM  = 1ull << 18;  // supervisor may access user pages
constexpr u64 MSTATUS_MXR  = 1ull << 19;  // make executable pages readable

// Virtualisation trap controls. Each makes an operation that supervisor mode
// would normally perform freely trap to machine mode instead, so firmware or a
// hypervisor can intercept it.
constexpr u64 MSTATUS_TVM  = 1ull << 20;  // trap satp access and SFENCE.VMA
constexpr u64 MSTATUS_TW   = 1ull << 21;  // trap WFI after a timeout
constexpr u64 MSTATUS_TSR  = 1ull << 22;  // trap SRET

// The bits sstatus exposes. Everything else in mstatus is machine-only and
// reads as zero through the supervisor view.
constexpr u64 SSTATUS_MASK = MSTATUS_SIE | MSTATUS_SPIE | MSTATUS_SPP |
                             MSTATUS_SUM | MSTATUS_MXR | MSTATUS_FS |
                             MSTATUS_SD;

// The interrupt bits sie/sip expose.
constexpr u64 SIE_SIP_MASK = (1ull << 1) | (1ull << 5) | (1ull << 9);

// --- fcsr ---
// Bits [4:0] are the accrued exception flags, [7:5] the rounding mode.
constexpr u64 FCSR_FFLAGS_MASK = 0x1f;
constexpr u64 FCSR_FRM_MASK    = 0x7 << 5;
constexpr int FCSR_FRM_SHIFT   = 5;

// The five accrued exception flags, in the order the spec numbers them.
constexpr u64 FFLAG_NX = 1 << 0;   // inexact
constexpr u64 FFLAG_UF = 1 << 1;   // underflow
constexpr u64 FFLAG_OF = 1 << 2;   // overflow
constexpr u64 FFLAG_DZ = 1 << 3;   // divide by zero
constexpr u64 FFLAG_NV = 1 << 4;   // invalid operation

// Rounding modes. RNE is round-to-nearest-even, the IEEE default; DYN in an
// instruction's rm field means "use whatever frm says".
constexpr u32 FRM_RNE = 0;   // to nearest, ties to even
constexpr u32 FRM_RTZ = 1;   // toward zero
constexpr u32 FRM_RDN = 2;   // down (toward -inf)
constexpr u32 FRM_RUP = 3;   // up (toward +inf)
constexpr u32 FRM_RMM = 4;   // to nearest, ties away from zero
constexpr u32 FRM_DYN = 7;   // in an instruction: use frm

// --- satp ---
constexpr int SATP_MODE_SHIFT = 60;
constexpr u64 SATP_MODE_MASK  = 0xfull << SATP_MODE_SHIFT;
constexpr u64 SATP_ASID_MASK  = 0xffffull << 44;
constexpr u64 SATP_PPN_MASK   = (1ull << 44) - 1;

constexpr u64 SATP_MODE_BARE = 0;
constexpr u64 SATP_MODE_SV39 = 8;
constexpr u64 SATP_MODE_SV48 = 9;

// --- mie / mip interrupt bits ---
// The bit position equals the interrupt's cause number, which is why
// Interrupt's enumerators are the numbers they are.
constexpr u64 MIP_SSIP = 1ull << 1;
constexpr u64 MIP_MSIP = 1ull << 3;
constexpr u64 MIP_STIP = 1ull << 5;
constexpr u64 MIP_MTIP = 1ull << 7;
constexpr u64 MIP_SEIP = 1ull << 9;
constexpr u64 MIP_MEIP = 1ull << 11;

// mie uses exactly the same bit positions as mip - bit N in mie enables the
// interrupt whose pending flag is bit N of mip, and N is also its cause number.
// The aliases exist so that code reads as what it means.
constexpr u64 MIE_SSIE = MIP_SSIP;
constexpr u64 MIE_MSIE = MIP_MSIP;
constexpr u64 MIE_STIE = MIP_STIP;
constexpr u64 MIE_MTIE = MIP_MTIP;
constexpr u64 MIE_SEIE = MIP_SEIP;
constexpr u64 MIE_MEIE = MIP_MEIP;

// --- mtvec modes ---
constexpr u64 MTVEC_MODE_MASK   = 0x3;
constexpr u64 MTVEC_MODE_DIRECT = 0;
constexpr u64 MTVEC_MODE_VECTORED = 1;

// True if writing this CSR address is architecturally forbidden.
constexpr bool is_read_only(u32 addr) { return ((addr >> 10) & 0x3) == 0x3; }

// The lowest privilege level permitted to access this CSR (0=U, 1=S, 3=M).
constexpr u32 min_privilege(u32 addr) { return (addr >> 8) & 0x3; }

}  // namespace csr

// ---------------------------------------------------------------------------
// The CSR file.
//
// Only implemented registers exist. Accessing an unimplemented CSR raises an
// illegal-instruction exception, which is not an inconvenience but the actual
// mechanism software uses to probe for optional features: try the CSR, catch
// the trap, conclude it is absent.
//
// Writes are masked. Most CSRs have WARL fields ("write any values, reads
// legal values") or WPRI fields ("reserved, writes ignored"), so a raw store of
// whatever the guest supplied would let it set bits the hardware does not
// actually implement, and later read them back - which real hardware would
// never do.
// ---------------------------------------------------------------------------
class CsrFile {
public:
    CsrFile();

    bool exists(u32 addr) const;

    // Raw access, with no privilege or read-only checking - the CPU does those,
    // because only it knows the current privilege mode and whether the
    // instruction intends to write.
    u64  read(u32 addr) const;
    void write(u32 addr, u64 value);

    // Convenience accessors for the fields the trap path touches constantly.
    u64  mstatus() const { return read(csr::MSTATUS); }
    void set_mstatus(u64 v) { write(csr::MSTATUS, v); }

    bool mstatus_mie() const { return (mstatus() & csr::MSTATUS_MIE) != 0; }
    bool mstatus_sie() const { return (mstatus() & csr::MSTATUS_SIE) != 0; }
    bool mstatus_sum() const { return (mstatus() & csr::MSTATUS_SUM) != 0; }
    bool mstatus_mxr() const { return (mstatus() & csr::MSTATUS_MXR) != 0; }

    // Counter-enable bits. Bit 0 is CY (cycle), 1 is TM (time), 2 is IR
    // (instret). A less privileged mode may read a counter only if the next
    // mode up has enabled it, so a kernel can deny user code a clock.
    bool counter_enabled(u32 bit, u32 priv) const {
        if (priv >= PRIV_MACHINE) return true;
        if (((read(csr::MCOUNTEREN) >> bit) & 1) == 0) return false;
        if (priv == PRIV_SUPERVISOR) return true;
        return ((read(csr::SCOUNTEREN) >> bit) & 1) != 0;
    }

    bool sstc_enabled() const { return (read(csr::MENVCFG) & csr::MENVCFG_STCE) != 0; }

    // Is the floating-point unit usable?
    //
    // While mstatus.FS is Off, every floating-point instruction and every
    // access to fcsr raises an illegal-instruction trap. That is how a kernel
    // with no FPU support stops user code from quietly corrupting a register
    // file nobody is saving across context switches.
    bool fpu_enabled() const {
        return (read(csr::MSTATUS) & csr::MSTATUS_FS) != csr::MSTATUS_FS_OFF;
    }

    // Mark the floating-point registers as modified. Called by every
    // instruction that writes an f register or the fcsr, because a kernel uses
    // exactly this to decide whether a context switch has to save all 32 of
    // them - and one missed transition is a process silently inheriting
    // another's floating-point state.
    void mark_fpu_dirty() {
        raw_[csr::MSTATUS] = (raw_[csr::MSTATUS] & ~csr::MSTATUS_FS) |
                             csr::MSTATUS_FS_DIRTY;
    }

    // Accumulate IEEE exception flags into fcsr. They are sticky: nothing but
    // an explicit write ever clears them, which is what lets a program run a
    // long calculation and ask afterwards whether anything went wrong.
    void raise_fflags(u64 flags) {
        raw_[csr::FCSR] |= (flags & csr::FCSR_FFLAGS_MASK);
    }

    u32 rounding_mode() const {
        return static_cast<u32>((raw_[csr::FCSR] & csr::FCSR_FRM_MASK) >>
                                csr::FCSR_FRM_SHIFT);
    }

    u64 satp() const { return read(csr::SATP); }
    u64 satp_mode() const { return (satp() & csr::SATP_MODE_MASK) >> csr::SATP_MODE_SHIFT; }
    u64 satp_ppn() const  { return satp() & csr::SATP_PPN_MASK; }
    u64 satp_asid() const { return (satp() & csr::SATP_ASID_MASK) >> 44; }

    // True if the trap with this cause is delegated to supervisor mode.
    bool delegated_exception(u64 cause) const {
        return cause < 64 && ((read(csr::MEDELEG) >> cause) & 1) != 0;
    }
    bool delegated_interrupt(u64 cause) const {
        return cause < 64 && ((read(csr::MIDELEG) >> cause) & 1) != 0;
    }
    u32  mstatus_mpp() const {
        return static_cast<u32>((mstatus() & csr::MSTATUS_MPP) >> csr::MSTATUS_MPP_SHIFT);
    }

    // Interrupts that are both pending and enabled.
    u64 pending_enabled() const { return read(csr::MIP) & read(csr::MIE); }

    // Direct, unmasked access for devices (the CLINT drives MTIP and MSIP, the
    // PLIC drives MEIP) and for tests.
    void raise_interrupt(u64 mip_bit)  { raw_[csr::MIP] |= mip_bit; }
    void clear_interrupt(u64 mip_bit)  { raw_[csr::MIP] &= ~mip_bit; }

    // Advance the retired-instruction and cycle counters by one.
    //
    // These have to *increment*, not be assigned from the emulator's own step
    // count: minstret is writable, and software that writes it expects counting
    // to continue from the value it wrote. Assigning would silently discard the
    // write on the very next instruction.
    void tick_counters() { ++raw_[csr::MINSTRET]; ++raw_[csr::MCYCLE]; }

private:
    std::array<u64, 4096> raw_{};

    static u64 write_mask(u32 addr);
};
