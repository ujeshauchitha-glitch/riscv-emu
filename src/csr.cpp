#include "csr.hpp"

namespace {

// The MXL field in misa: 1 = RV32, 2 = RV64, in the top two bits.
constexpr u64 MISA_MXL_64 = 2ull << 62;

// One bit per supported extension, bit 0 = 'A' ... bit 25 = 'Z'.
constexpr u64 misa_ext(char c) { return 1ull << (c - 'A'); }

// Only the machine-level interrupts exist while M-mode is the only privilege
// level. Phase 6 widens this to include the supervisor bits.
constexpr u64 INTERRUPT_MASK = csr::MIP_MSIP | csr::MIP_MTIP | csr::MIP_MEIP;

// mstatus bits that are writable in this phase. Everything else is WPRI
// (reserved) or belongs to a privilege level that does not exist yet, and must
// read back as zero however the guest writes it.
constexpr u64 MSTATUS_MASK = csr::MSTATUS_MIE | csr::MSTATUS_MPIE | csr::MSTATUS_MPP;

}  // namespace

CsrFile::CsrFile() {
    // misa advertises the ISA. C and F/D join in phase 8. Guest code reads this
    // to decide what it may use, so it must not claim more than we deliver.
    raw_[csr::MISA] = MISA_MXL_64 | misa_ext('I') | misa_ext('M') | misa_ext('A');

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
        case csr::MIP:      return 0;

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
