#include "csr.hpp"

namespace {

// The MXL field in misa: 1 = RV32, 2 = RV64, in the top two bits.
constexpr u64 MISA_MXL_64 = 2ull << 62;

// One bit per supported extension, bit 0 = 'A' ... bit 25 = 'Z'.
constexpr u64 misa_ext(char c) { return 1ull << (c - 'A'); }

// Every interrupt source that exists, machine and supervisor.
constexpr u64 INTERRUPT_MASK =
    csr::MIP_MSIP | csr::MIP_MTIP | csr::MIP_MEIP |
    csr::MIP_SSIP | csr::MIP_STIP | csr::MIP_SEIP;

// The supervisor pending bits M-mode software may write directly. Unlike the
// machine ones - which the CLINT and PLIC own - SSIP is genuinely software-set:
// it is how M-mode posts a supervisor software interrupt.
constexpr u64 MIP_SOFTWARE_WRITABLE = csr::MIP_SSIP | csr::MIP_STIP | csr::MIP_SEIP;

// mstatus bits that are writable. Everything else is WPRI (reserved) and must
// read back as zero however the guest writes it.
constexpr u64 MSTATUS_MASK =
    csr::MSTATUS_MIE | csr::MSTATUS_MPIE | csr::MSTATUS_MPP |
    csr::MSTATUS_SIE | csr::MSTATUS_SPIE | csr::MSTATUS_SPP |
    csr::MSTATUS_MPRV | csr::MSTATUS_SUM | csr::MSTATUS_MXR |
    csr::MSTATUS_TVM | csr::MSTATUS_TW | csr::MSTATUS_TSR;

}  // namespace

CsrFile::CsrFile() {
    // misa advertises the ISA. C and F/D join in phase 8. Guest code reads this
    // to decide what it may use, so it must not claim more than we deliver.
    // 'S' and 'U' announce that supervisor and user mode exist. A kernel checks
    // misa before trying to drop privilege.
    raw_[csr::MISA] = MISA_MXL_64 | misa_ext('I') | misa_ext('M') | misa_ext('A') |
                      misa_ext('S') | misa_ext('U');

    // A single hart, numbered 0. Every RISC-V system must have a hart 0, and
    // kernels use mhartid to pick which core runs the boot path.
    raw_[csr::MHARTID] = 0;

    // Non-commercial implementation: vendor/arch/impl IDs are zero by
    // convention, which is a legal and meaningful answer.
    raw_[csr::MVENDORID] = 0;
    raw_[csr::MARCHID]   = 0;
    raw_[csr::MIMPID]    = 0;

    // mstatus starts with interrupts disabled and MPP = M. A hart comes out of
    // reset in machine mode with nothing set up yet.
    raw_[csr::MSTATUS] = static_cast<u64>(PRIV_MACHINE) << csr::MSTATUS_MPP_SHIFT;
}

bool CsrFile::exists(u32 addr) const {
    switch (addr) {
        case csr::MVENDORID:
        case csr::MARCHID:
        case csr::MIMPID:
        case csr::MHARTID:
        case csr::MSTATUS:
        case csr::MISA:
        case csr::MEDELEG:
        case csr::MIDELEG:
        case csr::MIE:
        case csr::MTVEC:
        case csr::MCOUNTEREN:
        case csr::MSCRATCH:
        case csr::MEPC:
        case csr::MCAUSE:
        case csr::MTVAL:
        case csr::MIP:
        case csr::MCYCLE:
        case csr::MINSTRET:
        case csr::CYCLE:
        case csr::TIME:
        case csr::INSTRET:
        // Supervisor registers. sstatus/sie/sip are views onto their machine
        // counterparts rather than storage of their own; see read()/write().
        case csr::SSTATUS:
        case csr::SIE:
        case csr::STVEC:
        case csr::SCOUNTEREN:
        case csr::SSCRATCH:
        case csr::SEPC:
        case csr::SCAUSE:
        case csr::STVAL:
        case csr::SIP:
        case csr::SATP:
            return true;
        default:
            // Everything else is unimplemented, and reading or writing it is an
            // illegal instruction. That is how software probes for features.
            return false;
    }
}

u64 CsrFile::read(u32 addr) const {
    // The unprivileged counter shadows alias their machine-mode counterparts
    // rather than being separate storage.
    switch (addr) {
        case csr::CYCLE:   return raw_[csr::MCYCLE];
        case csr::INSTRET: return raw_[csr::MINSTRET];

        // The supervisor views. Reading sstatus reads the *same bits* as
        // mstatus, with the machine-only ones masked out - there is no second
        // copy that could drift out of step with the first.
        case csr::SSTATUS: return raw_[csr::MSTATUS] & csr::SSTATUS_MASK;
        case csr::SIE:     return raw_[csr::MIE] & csr::SIE_SIP_MASK;
        case csr::SIP:     return raw_[csr::MIP] & csr::SIE_SIP_MASK;

        default:           return raw_[addr & 0xfff];
    }
}

u64 CsrFile::write_mask(u32 addr) {
    switch (addr) {
        case csr::MSTATUS:  return MSTATUS_MASK;
        case csr::MIE:      return INTERRUPT_MASK;

        // mip is NOT software-writable for the machine-level bits. MEIP, MTIP
        // and MSIP are driven by hardware - the PLIC and the CLINT - and are
        // read-only in mip. A kernel acknowledges a timer interrupt by moving
        // mtimecmp forward, not by clearing MTIP; letting software clear the
        // bit directly would make the interrupt reappear on the next update and
        // look like a phantom re-entry.
        //
        // Phase 6 opens SSIP, which M-mode software genuinely may write to
        // post a supervisor software interrupt.
        case csr::MIP:      return MIP_SOFTWARE_WRITABLE;

        // misa is WARL and we do not support disabling extensions, so writes
        // are ignored entirely.
        case csr::MISA:     return 0;

        // Delegation registers are stored but have no effect until phase 6
        // introduces supervisor mode. Storing them now means early boot code
        // that writes them (as OpenSBI and Linux both do) reads back what it
        // wrote instead of tripping over a phantom difference.
        case csr::MEDELEG:  return ~0ull;
        case csr::MIDELEG:  return ~0ull;

        default:            return ~0ull;
    }
}

void CsrFile::write(u32 addr, u64 value) {
    const u32 a = addr & 0xfff;

    switch (a) {
        case csr::MTVEC: {
            // mtvec holds a base address and a mode in its low two bits. Only
            // modes 0 (direct) and 1 (vectored) are defined; the field is WARL,
            // so an unsupported mode is coerced rather than stored.
            u64 mode = value & csr::MTVEC_MODE_MASK;
            if (mode > csr::MTVEC_MODE_VECTORED) mode = csr::MTVEC_MODE_DIRECT;
            raw_[a] = (value & ~csr::MTVEC_MODE_MASK) | mode;
            return;
        }

        // Writing a supervisor view writes through to the machine register,
        // touching only the bits that view exposes.
        case csr::SSTATUS:
            raw_[csr::MSTATUS] = (raw_[csr::MSTATUS] & ~csr::SSTATUS_MASK) |
                                 (value & csr::SSTATUS_MASK);
            return;
        case csr::SIE:
            raw_[csr::MIE] = (raw_[csr::MIE] & ~csr::SIE_SIP_MASK) |
                             (value & csr::SIE_SIP_MASK);
            return;
        case csr::SIP:
            // Only SSIP is writable through sip; STIP and SEIP are driven by
            // hardware, as their machine counterparts are.
            raw_[csr::MIP] = (raw_[csr::MIP] & ~csr::MIP_SSIP) |
                             (value & csr::MIP_SSIP);
            return;

        case csr::SATP: {
            // Only Bare and Sv39 are supported. satp's MODE field is WARL, so
            // an unsupported mode is ignored entirely rather than stored - that
            // is how a kernel discovers Sv48 is unavailable: it writes the
            // mode, reads it back, and finds its request did not take.
            const u64 mode = (value & csr::SATP_MODE_MASK) >> csr::SATP_MODE_SHIFT;
            if (mode != csr::SATP_MODE_BARE && mode != csr::SATP_MODE_SV39) return;
            raw_[a] = value;
            return;
        }

        case csr::SEPC:
            raw_[a] = value & ~3ull;   // as mepc: an instruction address
            return;

        case csr::STVEC: {
            u64 mode = value & csr::MTVEC_MODE_MASK;
            if (mode > csr::MTVEC_MODE_VECTORED) mode = csr::MTVEC_MODE_DIRECT;
            raw_[a] = (value & ~csr::MTVEC_MODE_MASK) | mode;
            return;
        }

        case csr::MEPC:
            // mepc always holds an instruction address, so its low bit is
            // hardwired to zero - and because we only support 32-bit-aligned
            // instructions (no C extension yet), bit 1 is too. Phase 8 relaxes
            // this to just bit 0 when compressed instructions arrive.
            raw_[a] = value & ~3ull;
            return;

        case csr::CYCLE:
        case csr::TIME:
        case csr::INSTRET:
            // Read-only shadows; the caller should have rejected the write.
            return;

        default:
            raw_[a] = (raw_[a] & ~write_mask(a)) | (value & write_mask(a));
            return;
    }
}
