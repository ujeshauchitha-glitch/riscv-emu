#include "cpu.hpp"

#include <cmath>
#include <limits>

#include "fpu.hpp"
#include "sbi.hpp"

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>

const char* const REG_ABI_NAMES[NUM_REGS] = {
    "zero", "ra", "sp",  "gp",  "tp", "t0", "t1", "t2",
    "s0",   "s1", "a0",  "a1",  "a2", "a3", "a4", "a5",
    "a6",   "a7", "s2",  "s3",  "s4", "s5", "s6", "s7",
    "s8",   "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

namespace {

// Sign-extend the low 32 bits of a value to 64 bits.
//
// This shows up everywhere in RV64. The "*W" instructions (ADDW, SLLIW, ...)
// operate on 32-bit values but write a 64-bit register, and the spec requires
// the result to be *sign*-extended, not zero-extended. So `addw` producing
// 0xFFFF_FFFF must leave 0xFFFF_FFFF_FFFF_FFFF in the register. Forgetting this
// is one of the most common RV64 emulator bugs, and it stays invisible until a
// kernel does 32-bit pointer arithmetic and lands somewhere absurd.
inline u64 sext32(u32 value) {
    return static_cast<u64>(static_cast<i64>(static_cast<i32>(value)));
}

}  // namespace

Cpu::Cpu(Bus& bus) : mmu(bus), bus_(bus) {
    regs.fill(0);
    pc = DRAM_BASE;
}

u64 Cpu::read_reg(u32 index) const {
    if (index == 0 || index >= NUM_REGS) return 0;
    return regs[index];
}

void Cpu::write_reg(u32 index, u64 value) {
    if (index == 0 || index >= NUM_REGS) return;
    regs[index] = value;
}

u32 Cpu::data_privilege() const {
    if (priv == PRIV_MACHINE && (csrs.mstatus() & csr::MSTATUS_MPRV)) {
        return csrs.mstatus_mpp();
    }
    return priv;
}

Result<u64> Cpu::mem_load(u64 vaddr, unsigned size, AccessType type) {
    const u32 p = (type == AccessType::Instruction) ? priv : data_privilege();
    Result<u64> pa = mmu.translate(vaddr, type, p, csrs);
    if (!pa) return pa;
    return bus_.load(pa.value, size, type);
}

Status Cpu::mem_store(u64 vaddr, unsigned size, u64 value) {
    Result<u64> pa = mmu.translate(vaddr, AccessType::Store, data_privilege(), csrs);
    if (!pa) return Status::bad(pa.trap);
    return bus_.store(pa.value, size, value);
}

// Fetch the instruction at the PC, whatever length it turns out to be.
//
// With the C extension, instruction length is not known until the first
// halfword has been read: bits [1:0] == 11 means 32 bits, anything else means
// 16. So the fetch happens in two steps.
//
// That is not merely a formality. A 32-bit instruction may **straddle a page
// boundary**, with its first halfword on a mapped page and its second on one
// that is not - and then the correct behaviour is an instruction page fault
// reporting the address of the *second* halfword, not of the PC. Reading four
// bytes in one access would report the wrong address, or, worse, succeed
// against a page the program was never allowed to execute from. Two accesses
// give each halfword its own translation, which is what the hardware does.
Result<DecodedInst> Cpu::fetch_inst() {
    // IALIGN is 16 bits when C is implemented, so only bit 0 must be clear.
    // This is a backstop: a misaligned target is normally caught by
    // set_branch_target() on the jump that produced it, where the reported PC
    // is the more useful one.
    if ((pc & 0x1) != 0) {
        return Result<DecodedInst>::bad(Exception::InstructionAddressMisaligned, pc);
    }

    Result<u64> low = mem_load(pc, 2, AccessType::Instruction);
    if (!low) return Result<DecodedInst>::bad(low.trap);

    const u16 low16 = static_cast<u16>(low.value);
    if (!is_32bit_instruction(low16)) {
        return Result<DecodedInst>::good(decode16(low16, 0));
    }

    Result<u64> high = mem_load(pc + 2, 2, AccessType::Instruction);
    if (!high) return Result<DecodedInst>::bad(high.trap);

    return Result<DecodedInst>::good(decode16(low16, static_cast<u16>(high.value)));
}

Result<u32> Cpu::fetch() {
    Result<DecodedInst> inst = fetch_inst();
    if (!inst) return Result<u32>::bad(inst.trap);
    return Result<u32>::good(inst.value.raw);
}

Status Cpu::step() {
    // Devices that evolve with time, not just when addressed. The CLINT's
    // counter advances and may assert a timer interrupt; that has to happen
    // before the interrupt check below or a deadline would be noticed one
    // instruction late.
    if (clint) {
        clint->tick();
        clint->update(csrs);
    }

    // Sample the UART's level-triggered line, then let the PLIC arbitrate
    // between whatever is asserted and drive MEIP/SEIP.
    //
    // Host keystrokes are collected on the same path, but only every 4096th
    // instruction: a read() syscall per emulated instruction would cost more
    // than the instruction itself and cut the machine's speed by an order of
    // magnitude. At ~15M instructions a second that is still a poll every
    // fraction of a millisecond, far below anything a typist notices.
    if (uart && (instret & 0xfff) == 0) uart->poll_host_input();

    if (plic) {
        if (uart) plic->set_pending(uart_irq, uart->interrupting());
        plic->update(csrs);
    }

    // Interrupts are checked before the fetch, not after the instruction.
    // An interrupt is not caused by the instruction at the PC - it is an
    // external event that happens *between* instructions, so the instruction
    // that has not run yet must not run, and mepc must point at it so that MRET
    // resumes exactly there.
    // Note there is no "no handler installed" special case here, unlike for
    // exceptions: an interrupt can only be pending if the guest set mstatus.MIE
    // and an mie bit, which means it is deliberately using interrupts and has
    // had every opportunity to set mtvec.
    Interrupt intr;
    if (next_interrupt(intr)) {
        take_interrupt(intr);
        return Status::good();
    }

    Result<DecodedInst> fetched = fetch_inst();
    if (!fetched) return handle_trap_or_stop(fetched.trap);

    const DecodedInst inst = fetched.value;

    // Default: fall through to the next instruction. Jumps and branches
    // overwrite this inside execute(). The step is the instruction's own
    // length, which is where the C extension reaches the execution loop - and
    // is the only place it does.
    next_pc_ = pc + inst.length;
    counter_written_ = false;

    if (trace) trace_inst(inst);

    Status st = execute(inst);
    if (!st) return handle_trap_or_stop(st.trap);

    pc = next_pc_;
    ++instret;

    // One instruction per cycle. A real machine's cycle count differs from its
    // retired-instruction count, but nothing we can boot depends on that, and
    // guest code does use mcycle for crude delay loops.
    //
    // An instruction that wrote a counter does not also increment it: the spec
    // requires the value read back afterwards to be exactly what was written.
    if (!counter_written_) csrs.tick_counters();
    return Status::good();
}

// Dispatch a trap to the guest's handler, or stop the machine if there is no
// handler installed yet. See `trap_fatal_without_handler` in cpu.hpp.
Status Cpu::handle_trap_or_stop(const Trap& trap) {
    last_trap = trap;

    if (trap_fatal_without_handler && !handler_installed_for(trap)) {
        // Leave pc on the faulting instruction, which is both what a register
        // dump wants to show and what mepc would have received.
        return Status::bad(trap);
    }

    take_trap(trap);
    return Status::good();
}

// Would this trap reach a vector the guest actually wrote?
//
// The check has to follow the same delegation decision enter_trap() will make,
// not just look at mtvec. xv6 is the case that proves it: it delegates every
// exception to supervisor mode (medeleg = 0xffff), installs stvec, and never
// writes mtvec at all - so mtvec == 0 is entirely legitimate there. Testing
// mtvec alone would declare the first delegated ECALL from user mode fatal,
// stopping the emulator on the very trap the kernel is waiting for.
bool Cpu::handler_installed_for(const Trap& trap) const {
    const bool to_supervisor = (priv <= PRIV_SUPERVISOR) &&
                               csrs.delegated_exception(trap.cause_code());
    return csrs.read(to_supervisor ? csr::STVEC : csr::MTVEC) != 0;
}

// ---------------------------------------------------------------------------
// Trap entry.
//
// The sequence below is fixed by the privileged spec, and every step matters:
//
//   mepc    <- the address to resume at
//   mcause  <- why we trapped
//   mtval   <- extra detail (faulting address, or the instruction bits)
//   MPIE    <- MIE          (save whether interrupts were enabled)
//   MIE     <- 0            (disable them, so the handler is not re-entered)
//   MPP     <- current privilege  (remember where to return to)
//   pc      <- mtvec
//
// MPIE and MPP together are what let MRET restore the machine exactly. Without
// them there would be no way back.
// ---------------------------------------------------------------------------
void Cpu::enter_trap(u64 cause_code, u64 tval, u64 epc, bool is_interrupt) {
    // Which mode handles this?
    //
    // A trap taken in supervisor or user mode goes to supervisor mode when the
    // matching medeleg/mideleg bit is set. That delegation is what makes an OS
    // efficient: without it every page fault and every system call would have
    // to bounce through machine-mode firmware before reaching the kernel that
    // actually handles it.
    //
    // Traps taken *in* machine mode are never delegated - there is nothing more
    // privileged to delegate from.
    const u64 raw_cause = cause_code & ~(1ull << 63);
    const bool deleg = is_interrupt ? csrs.delegated_interrupt(raw_cause)
                                    : csrs.delegated_exception(raw_cause);
    const bool to_supervisor = (priv <= PRIV_SUPERVISOR) && deleg;

    u64 status = csrs.mstatus();

    if (to_supervisor) {
        csrs.write(csr::SEPC, epc);
        csrs.write(csr::SCAUSE, cause_code);
        csrs.write(csr::STVAL, tval);

        // Save SIE into SPIE, then clear SIE, and record where we came from.
        // SPP is a single bit because supervisor traps can only come from
        // supervisor or user mode.
        const bool sie = (status & csr::MSTATUS_SIE) != 0;
        status = sie ? (status | csr::MSTATUS_SPIE) : (status & ~csr::MSTATUS_SPIE);
        status &= ~csr::MSTATUS_SIE;
        status = (priv == PRIV_SUPERVISOR) ? (status | csr::MSTATUS_SPP)
                                           : (status & ~csr::MSTATUS_SPP);
        csrs.set_mstatus(status);
        priv = PRIV_SUPERVISOR;

        const u64 tvec = csrs.read(csr::STVEC);
        const u64 base = tvec & ~csr::MTVEC_MODE_MASK;
        const u64 mode = tvec & csr::MTVEC_MODE_MASK;
        pc = (mode == csr::MTVEC_MODE_VECTORED && is_interrupt)
                 ? base + 4 * (cause_code & 0x3f)
                 : base;
    } else {
        csrs.write(csr::MEPC, epc);
        csrs.write(csr::MCAUSE, cause_code);
        csrs.write(csr::MTVAL, tval);

        const bool mie = (status & csr::MSTATUS_MIE) != 0;
        status = mie ? (status | csr::MSTATUS_MPIE) : (status & ~csr::MSTATUS_MPIE);
        status &= ~csr::MSTATUS_MIE;
        status = (status & ~csr::MSTATUS_MPP) |
                 (static_cast<u64>(priv) << csr::MSTATUS_MPP_SHIFT);
        csrs.set_mstatus(status);
        priv = PRIV_MACHINE;

        const u64 tvec = csrs.read(csr::MTVEC);
        const u64 base = tvec & ~csr::MTVEC_MODE_MASK;
        const u64 mode = tvec & csr::MTVEC_MODE_MASK;
        pc = (mode == csr::MTVEC_MODE_VECTORED && is_interrupt)
                 ? base + 4 * (cause_code & 0x3f)
                 : base;
    }

    // A trap may be a context switch. If one thread does LR, is interrupted,
    // and another thread runs, the first thread's SC must fail when it
    // eventually resumes - otherwise both could believe they took the lock.
    clear_reservation();
}

void Cpu::take_trap(const Trap& trap) {
    // An exception resumes at the instruction that caused it. That is right for
    // a page fault (fix the mapping, retry) and for ECALL the handler advances
    // mepc by 4 itself before returning.
    enter_trap(trap.cause_code(), trap.tval, pc, false);
}

void Cpu::take_interrupt(Interrupt intr) {
    // An interrupt resumes at the instruction that has not run yet.
    enter_trap(interrupt_cause_code(intr), 0, pc, true);
}

bool Cpu::next_interrupt(Interrupt& out) const {
    const u64 ready = csrs.pending_enabled();
    if (ready == 0) return false;

    const u64 mideleg = csrs.read(csr::MIDELEG);

    // An interrupt is enabled for a mode when we are *below* that mode - a less
    // privileged context can always be interrupted by a more privileged one -
    // or when we are in it and its global enable is set.
    const bool m_enabled = (priv < PRIV_MACHINE) || csrs.mstatus_mie();
    const bool s_enabled = (priv < PRIV_SUPERVISOR) || csrs.mstatus_sie();

    // Machine-mode interrupts (those not delegated) outrank supervisor ones.
    const u64 m_ready = ready & ~mideleg;
    const u64 s_ready = ready & mideleg;

    const u64 candidates = (m_enabled && m_ready) ? m_ready
                         : (s_enabled && s_ready) ? s_ready
                         : 0;
    if (candidates == 0) return false;

    // Priority is fixed by the spec and is not bit order: external, then
    // software, then timer, machine before supervisor.
    if (candidates & csr::MIP_MEIP) { out = Interrupt::MachineExternal;    return true; }
    if (candidates & csr::MIP_MSIP) { out = Interrupt::MachineSoftware;    return true; }
    if (candidates & csr::MIP_MTIP) { out = Interrupt::MachineTimer;       return true; }
    if (candidates & csr::MIP_SEIP) { out = Interrupt::SupervisorExternal; return true; }
    if (candidates & csr::MIP_SSIP) { out = Interrupt::SupervisorSoftware; return true; }
    if (candidates & csr::MIP_STIP) { out = Interrupt::SupervisorTimer;    return true; }
    return false;
}

Status Cpu::run(u64 max_steps, u64* steps_out) {
    u64 n = 0;
    for (; n < max_steps; ++n) {
        Status st = step();
        if (!st) {
            if (steps_out) *steps_out = n;
            return st;
        }
        // A guest that writes the poweroff word to syscon has asked to stop.
        if (syscon && (syscon->poweroff_requested() || syscon->reboot_requested())) {
            halted = true;
            if (steps_out) *steps_out = n + 1;
            return Status::good();
        }

        // So has a user who pressed Ctrl-A X at the console. The guest is not
        // consulted: this is the human at the keyboard asking to leave, which
        // is the one request a running guest must not be able to refuse.
        if (uart && uart->exit_requested()) {
            halted = true;
            user_quit = true;
            if (steps_out) *steps_out = n + 1;
            return Status::good();
        }

        // The riscv-tests suite stops by writing its result to `tohost` and
        // then spinning, so there is nothing to notice except the write itself.
        if (htif_tohost_addr != 0) {
            Result<u64> r = bus_.load(htif_tohost_addr, 8, AccessType::Load);
            if (r && r.value != 0) {
                htif_tohost_value = r.value;
                halted = true;
                if (steps_out) *steps_out = n + 1;
                return Status::good();
            }
        }
    }
    if (steps_out) *steps_out = n;
    return Status::good();
}

Status Cpu::execute(const DecodedInst& inst) {
    switch (inst.opcode) {
        // LUI - load upper immediate. The immediate is already shifted into the
        // upper 20 bits by the decoder, so this is just a move.
        case opcodes::LUI:
            write_reg(inst.rd, static_cast<u64>(inst.imm));
            return Status::good();

        // AUIPC - add upper immediate to PC. The basis of every PC-relative
        // address in RISC-V: paired with ADDI it reaches anywhere within +/-2GiB
        // of the current instruction, which is how position-independent code and
        // long jumps are built.
        case opcodes::AUIPC:
            write_reg(inst.rd, pc + static_cast<u64>(inst.imm));
            return Status::good();

        case opcodes::JAL:       return execute_jal(inst);
        case opcodes::JALR:      return execute_jalr(inst);
        case opcodes::BRANCH:    return execute_branch(inst);
        case opcodes::LOAD:      return execute_load(inst);
        case opcodes::STORE:     return execute_store(inst);
        case opcodes::OP_IMM:    return execute_op_imm(inst);
        case opcodes::OP_IMM_32: return execute_op_imm_32(inst);
        case opcodes::OP:        return execute_op(inst);
        case opcodes::OP_32:     return execute_op_32(inst);
        case opcodes::AMO:       return execute_amo(inst);
        case opcodes::SYSTEM:    return execute_system(inst);

        // F and D.
        case opcodes::LOAD_FP:   return execute_load_fp(inst);
        case opcodes::STORE_FP:  return execute_store_fp(inst);
        case opcodes::OP_FP:     return execute_op_fp(inst);
        case opcodes::MADD:
        case opcodes::MSUB:
        case opcodes::NMSUB:
        case opcodes::NMADD:     return execute_fused_madd(inst);

        // FENCE orders memory operations for other harts and devices. We are a
        // single-hart emulator that executes strictly in order and completes
        // every access before the next instruction, so the ordering FENCE asks
        // for already holds and it is correctly implemented as a no-op.
        // FENCE.I (funct3 = 1) is the instruction-cache variant; we have no
        // instruction cache, so the same reasoning applies.
        case opcodes::MISC_MEM:
            return Status::good();

        default:
            // Anything not yet implemented traps as an illegal instruction with
            // the offending bits in tval, rather than being skipped.
            return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }
}

Status Cpu::set_branch_target(u64 target) {
    // IALIGN is 16 with the C extension: a 2-byte-aligned target is legal, and
    // compilers emit them constantly once compressed instructions are in play.
    // Only an odd address is a misaligned jump now.
    if ((target & 0x1) != 0) {
        return Status::bad(Exception::InstructionAddressMisaligned, target);
    }
    next_pc_ = target;
    return Status::good();
}

// ---------------------------------------------------------------------------
// Control transfer
// ---------------------------------------------------------------------------

Status Cpu::execute_jal(const DecodedInst& inst) {
    const u64 target = pc + static_cast<u64>(inst.imm);

    // The link address is the instruction *after* this one, which with the C
    // extension is not always four bytes on. C.JALR expands to a JALR, and a
    // compiler expects it to link pc + 2 - link pc + 4 and the return goes one
    // instruction too far, skipping whatever followed the call. riscv-tests
    // rv64uc/rvc check 36 exists precisely to catch this.
    const u64 link = pc + inst.length;

    // Compute the target and check it *before* writing the link register, so a
    // misaligned jump leaves the machine untouched.
    Status st = set_branch_target(target);
    if (!st) return st;

    write_reg(inst.rd, link);
    return Status::good();
}

Status Cpu::execute_jalr(const DecodedInst& inst) {
    if (inst.funct3 != 0x0) {
        return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }

    // The low bit of the computed target is cleared, unconditionally. This is
    // not an alignment check - it is defined behaviour, and it exists so that
    // the C extension can use bit 0 of a function pointer without confusing
    // indirect jumps. Note the ordering: add, *then* clear, so `jalr rd, rs1, 1`
    // is a legal way to reach an even address.
    const u64 target = (read_reg(inst.rs1) + static_cast<u64>(inst.imm)) & ~1ull;
    const u64 link   = pc + inst.length;   // 2 after a C.JALR - see execute_jal

    Status st = set_branch_target(target);
    if (!st) return st;

    // rd is written after the target is computed, which matters because rd and
    // rs1 may be the same register - `jalr ra, ra, 0` is a real idiom.
    write_reg(inst.rd, link);
    return Status::good();
}


// ===========================================================================
// F and D: floating point.
// ===========================================================================

Status Cpu::require_fpu() const {
    // mstatus.FS == Off means the floating-point unit is disabled, and every
    // instruction that touches it is illegal. This is not an optional check: a
    // kernel that does not save the f registers across a context switch turns
    // FS off precisely so that user code cannot use them, and an emulator that
    // ignored it would let two processes silently share a register file.
    if (!csrs.fpu_enabled()) return Status::bad(Exception::IllegalInstruction, 0);
    return Status::good();
}

void Cpu::write_freg(u32 index, u64 value) {
    fregs[index] = value;
    // Writing any f register makes the unit Dirty, which is what tells a
    // context switch it has 32 registers to save.
    csrs.mark_fpu_dirty();
}

template <typename Op>
Status Cpu::with_rounding(const DecodedInst& inst, Op&& op) {
    // The rounding mode comes from the instruction's rm field, or from fcsr
    // when that field says DYN. A reserved mode makes the instruction illegal
    // rather than defaulting to something plausible - software that asks for a
    // mode that does not exist has a bug, and hiding it helps nobody.
    RoundingScope rounding(inst.funct3, csrs.rounding_mode());
    if (!rounding.valid()) {
        return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }

    clear_host_exceptions();
    op();
    csrs.raise_fflags(take_host_exceptions());
    return Status::good();
}

Status Cpu::execute_load_fp(const DecodedInst& inst) {
    Status fp = require_fpu();
    if (!fp) return Status::bad(Exception::IllegalInstruction, inst.encoded);

    const u64 addr = read_reg(inst.rs1) + static_cast<u64>(inst.imm);

    switch (inst.funct3) {
        case 0x2: {  // FLW
            Result<u64> v = mem_load(addr, 4, AccessType::Load);
            if (!v) return Status::bad(v.trap);
            // Boxed on the way in, so that reading this register as a double
            // yields a NaN rather than a plausible-looking wrong number.
            write_freg(inst.rd, nan_box(static_cast<u32>(v.value)));
            return Status::good();
        }
        case 0x3: {  // FLD
            Result<u64> v = mem_load(addr, 8, AccessType::Load);
            if (!v) return Status::bad(v.trap);
            write_freg(inst.rd, v.value);
            return Status::good();
        }
        default:
            return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }
}

Status Cpu::execute_store_fp(const DecodedInst& inst) {
    Status fp = require_fpu();
    if (!fp) return Status::bad(Exception::IllegalInstruction, inst.encoded);

    const u64 addr = read_reg(inst.rs1) + static_cast<u64>(inst.imm);

    switch (inst.funct3) {
        case 0x2:  // FSW - the low half only; the box is not stored
            return mem_store(addr, 4, fregs[inst.rs2] & 0xffffffffull);
        case 0x3:  // FSD
            return mem_store(addr, 8, fregs[inst.rs2]);
        default:
            return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }
}

namespace {

// FMIN/FMAX have semantics that are neither C's fmin/fmax nor a comparison:
//
//   - a signalling NaN in either operand raises Invalid,
//   - if one operand is NaN the result is the *other* operand,
//   - if both are NaN the result is the canonical NaN,
//   - and -0.0 is defined to be less than +0.0, which no comparison reports.
//
// Writing this out is shorter than explaining why std::fmin is wrong.
template <typename T>
T fmin_max(T a, T b, bool want_max, bool a_nan, bool b_nan, bool a_neg_zero,
           bool b_neg_zero) {
    if (a_nan && b_nan) return T{};                 // caller substitutes NaN
    if (a_nan) return b;
    if (b_nan) return a;
    if (a == b && (a_neg_zero || b_neg_zero)) {
        // Both zeros. The sign decides, and only for zeros does a == b while
        // the two values are still distinguishable.
        const bool pick_positive = want_max;
        if (a_neg_zero && b_neg_zero) return a;
        if (pick_positive) return a_neg_zero ? b : a;
        return a_neg_zero ? a : b;
    }
    if (want_max) return a > b ? a : b;
    return a < b ? a : b;
}

// Convert a float to an integer with RISC-V's out-of-range behaviour, which is
// *not* C's. C says an out-of-range conversion is undefined; RISC-V says it
// saturates to the largest or smallest representable value and raises Invalid.
// A NaN converts to the maximum positive value, not to zero.
template <typename Int, typename Float>
Int convert_to_int(Float value, bool& invalid) {
    invalid = false;
    if (std::isnan(value)) {
        invalid = true;
        return std::numeric_limits<Int>::max();
    }
    // Compare against the exact bounds as long doubles so that the comparison
    // itself does not round the limit into range.
    const long double v   = static_cast<long double>(value);
    const long double lo  = static_cast<long double>(std::numeric_limits<Int>::min());
    const long double hi  = static_cast<long double>(std::numeric_limits<Int>::max());
    if (v < lo) { invalid = true; return std::numeric_limits<Int>::min(); }
    // `hi` may round up when converted, so the test has to be >= the next
    // representable value above the maximum rather than > the maximum.
    if (v >= hi + 1.0L) { invalid = true; return std::numeric_limits<Int>::max(); }
    if (v > hi) { invalid = true; return std::numeric_limits<Int>::max(); }
    return static_cast<Int>(v);
}

}  // namespace

Status Cpu::execute_op_fp(const DecodedInst& inst) {
    Status fp = require_fpu();
    if (!fp) return Status::bad(Exception::IllegalInstruction, inst.encoded);

    // funct7's low bit selects the precision: 0 single, 1 double. The rest of
    // funct7 names the operation.
    const u32  op       = inst.funct7 >> 2;
    const bool is_double = (inst.funct7 & 0x3) == 0x1;
    if ((inst.funct7 & 0x3) > 0x1) {
        // fmt 2 and 3 are half and quad precision, which we do not implement.
        return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }

    const u64 a_bits = fregs[inst.rs1];
    const u64 b_bits = fregs[inst.rs2];
    const double a_d = bits_to_f64(a_bits);
    const double b_d = bits_to_f64(b_bits);
    const float  a_f = bits_to_f32(nan_unbox(a_bits));
    const float  b_f = bits_to_f32(nan_unbox(b_bits));

    auto store_f64 = [&](double v) {
        write_freg(inst.rd, std::isnan(v) ? CANONICAL_NAN_F64 : f64_to_bits(v));
    };
    auto store_f32 = [&](float v) {
        write_freg(inst.rd,
                   nan_box(std::isnan(v) ? CANONICAL_NAN_F32 : f32_to_bits(v)));
    };

    switch (op) {
        case 0x00:  // FADD
            return with_rounding(inst, [&] {
                if (is_double) store_f64(a_d + b_d); else store_f32(a_f + b_f);
            });
        case 0x01:  // FSUB
            return with_rounding(inst, [&] {
                if (is_double) store_f64(a_d - b_d); else store_f32(a_f - b_f);
            });
        case 0x02:  // FMUL
            return with_rounding(inst, [&] {
                if (is_double) store_f64(a_d * b_d); else store_f32(a_f * b_f);
            });
        case 0x03:  // FDIV
            return with_rounding(inst, [&] {
                if (is_double) store_f64(a_d / b_d); else store_f32(a_f / b_f);
            });
        case 0x0b:  // FSQRT
            if (inst.rs2 != 0) {
                return Status::bad(Exception::IllegalInstruction, inst.encoded);
            }
            return with_rounding(inst, [&] {
                if (is_double) store_f64(std::sqrt(a_d));
                else           store_f32(std::sqrt(a_f));
            });

        case 0x04: {  // FSGNJ / FSGNJN / FSGNJX - sign injection
            // These are bit manipulation, not arithmetic: they never round,
            // never raise a flag, and never treat a NaN specially. That is what
            // makes them the canonical way to write fabs (FSGNJX rd, rs, rs),
            // fneg (FSGNJN) and a register move (FSGNJ rd, rs, rs).
            const u64 sign_bit = is_double ? (1ull << 63) : (1ull << 31);
            const u64 a = is_double ? a_bits : nan_unbox(a_bits);
            const u64 b = is_double ? b_bits : nan_unbox(b_bits);
            u64 sign;
            switch (inst.funct3) {
                case 0x0: sign = b & sign_bit; break;                  // FSGNJ
                case 0x1: sign = (~b) & sign_bit; break;               // FSGNJN
                case 0x2: sign = (a ^ b) & sign_bit; break;            // FSGNJX
                default:
                    return Status::bad(Exception::IllegalInstruction, inst.encoded);
            }
            const u64 result = (a & ~sign_bit) | sign;
            write_freg(inst.rd, is_double ? result : nan_box(static_cast<u32>(result)));
            return Status::good();
        }

        case 0x05: {  // FMIN / FMAX
            if (inst.funct3 > 0x1) {
                return Status::bad(Exception::IllegalInstruction, inst.encoded);
            }
            const bool want_max = inst.funct3 == 0x1;
            u64 flags = 0;
            if (is_double) {
                if (is_snan_f64(a_bits) || is_snan_f64(b_bits)) flags |= csr::FFLAG_NV;
                const bool an = std::isnan(a_d), bn = std::isnan(b_d);
                if (an && bn) {
                    write_freg(inst.rd, CANONICAL_NAN_F64);
                } else {
                    store_f64(fmin_max(a_d, b_d, want_max, an, bn,
                                       f64_to_bits(a_d) == (1ull << 63),
                                       f64_to_bits(b_d) == (1ull << 63)));
                }
            } else {
                if (is_snan_f32(nan_unbox(a_bits)) || is_snan_f32(nan_unbox(b_bits))) {
                    flags |= csr::FFLAG_NV;
                }
                const bool an = std::isnan(a_f), bn = std::isnan(b_f);
                if (an && bn) {
                    write_freg(inst.rd, nan_box(CANONICAL_NAN_F32));
                } else {
                    store_f32(fmin_max(a_f, b_f, want_max, an, bn,
                                       f32_to_bits(a_f) == (1u << 31),
                                       f32_to_bits(b_f) == (1u << 31)));
                }
            }
            csrs.raise_fflags(flags);
            return Status::good();
        }

        case 0x08:  // FCVT.S.D / FCVT.D.S - between the two precisions
            return with_rounding(inst, [&] {
                if (is_double) store_f64(static_cast<double>(a_f));  // FCVT.D.S
                else           store_f32(static_cast<float>(a_d));   // FCVT.S.D
            });

        case 0x14: {  // FEQ / FLT / FLE - comparisons, result to an x register
            // The quiet comparison (FEQ) raises Invalid only for a signalling
            // NaN; the ordered ones (FLT, FLE) raise it for any NaN. That
            // difference is the whole reason there are two kinds.
            bool result = false;
            u64  flags  = 0;
            const bool a_nan = is_double ? std::isnan(a_d) : std::isnan(a_f);
            const bool b_nan = is_double ? std::isnan(b_d) : std::isnan(b_f);
            const bool any_snan = is_double
                ? (is_snan_f64(a_bits) || is_snan_f64(b_bits))
                : (is_snan_f32(nan_unbox(a_bits)) || is_snan_f32(nan_unbox(b_bits)));

            switch (inst.funct3) {
                case 0x2:  // FEQ
                    if (any_snan) flags |= csr::FFLAG_NV;
                    result = !a_nan && !b_nan && (is_double ? a_d == b_d : a_f == b_f);
                    break;
                case 0x1:  // FLT
                    if (a_nan || b_nan) flags |= csr::FFLAG_NV;
                    result = !a_nan && !b_nan && (is_double ? a_d < b_d : a_f < b_f);
                    break;
                case 0x0:  // FLE
                    if (a_nan || b_nan) flags |= csr::FFLAG_NV;
                    result = !a_nan && !b_nan && (is_double ? a_d <= b_d : a_f <= b_f);
                    break;
                default:
                    return Status::bad(Exception::IllegalInstruction, inst.encoded);
            }
            csrs.raise_fflags(flags);
            write_reg(inst.rd, result ? 1 : 0);
            return Status::good();
        }

        case 0x18: {  // FCVT.W/WU/L/LU.S/D - float to integer
            bool invalid = false;
            i64  result  = 0;
            RoundingScope rounding(inst.funct3, csrs.rounding_mode());
            if (!rounding.valid()) {
                return Status::bad(Exception::IllegalInstruction, inst.encoded);
            }
            clear_host_exceptions();
            const double v = is_double ? a_d : static_cast<double>(a_f);
            // std::rint applies the current rounding mode, so the truncation to
            // an integer type afterwards cannot round again.
            //
            // rint, not nearbyint: they round identically, and differ only in
            // that nearbyint is specified to *suppress* the inexact exception.
            // RISC-V requires fcvt to raise it, so nearbyint gives the right
            // number with the wrong flags - which riscv-tests rv64uf/fcvt_w
            // check 2 catches, converting -1.1 and expecting fflags 0x01.
            const double rounded = std::rint(v);
            switch (inst.rs2) {
                case 0: result = convert_to_int<i32>(rounded, invalid); break;  // .W
                case 1: {                                                        // .WU
                    const u32 u = convert_to_int<u32>(rounded, invalid);
                    // A 32-bit result is sign-extended into the 64-bit
                    // register, even the unsigned one. That surprises people,
                    // and it is what the spec says.
                    result = static_cast<i64>(static_cast<i32>(u));
                    break;
                }
                case 2: result = convert_to_int<i64>(rounded, invalid); break;  // .L
                case 3: result = static_cast<i64>(convert_to_int<u64>(rounded, invalid));
                        break;                                                   // .LU
                default:
                    return Status::bad(Exception::IllegalInstruction, inst.encoded);
            }
            // Only the inexact flag survives from the host; range errors are
            // reported as Invalid, per the rule above.
            u64 flags = take_host_exceptions() & csr::FFLAG_NX;
            if (invalid) flags = csr::FFLAG_NV;   // and Invalid displaces Inexact
            csrs.raise_fflags(flags);
            write_reg(inst.rd, static_cast<u64>(result));
            return Status::good();
        }

        case 0x1a: {  // FCVT.S/D.W/WU/L/LU - integer to float
            const u64 x = read_reg(inst.rs1);
            return with_rounding(inst, [&] {
                double v = 0;
                switch (inst.rs2) {
                    case 0: v = static_cast<double>(static_cast<i32>(x)); break;
                    case 1: v = static_cast<double>(static_cast<u32>(x)); break;
                    case 2: v = static_cast<double>(static_cast<i64>(x)); break;
                    case 3: v = static_cast<double>(x); break;
                    default: break;
                }
                if (is_double) store_f64(v); else store_f32(static_cast<float>(v));
            });
        }

        case 0x1c: {  // FMV.X.W / FMV.X.D, and FCLASS
            if (inst.funct3 == 0x0) {
                // A raw bit move out of the float file. No conversion, no
                // rounding, no flags - which is how software inspects a float's
                // encoding, and how it implements copysign in portable C.
                //
                // Raw means raw: FMV.X.W takes bits 31:0 as they are and
                // sign-extends them. It does *not* apply the NaN-boxing rule,
                // even though almost every other single-precision instruction
                // does. Unboxing here would replace the bits software asked to
                // see with a canonical NaN, which defeats the entire purpose of
                // the instruction. riscv-tests rv64ud/move check 71 builds a
                // deliberately unboxed register and reads its low half back.
                if (inst.rs2 != 0) {
                    return Status::bad(Exception::IllegalInstruction, inst.encoded);
                }
                const u64 v = is_double
                    ? a_bits
                    : static_cast<u64>(static_cast<i64>(static_cast<i32>(
                          static_cast<u32>(a_bits))));
                write_reg(inst.rd, v);
                return Status::good();
            }
            if (inst.funct3 == 0x1) {  // FCLASS
                write_reg(inst.rd, is_double ? fclass_f64(a_bits)
                                             : fclass_f32(nan_unbox(a_bits)));
                return Status::good();
            }
            return Status::bad(Exception::IllegalInstruction, inst.encoded);
        }

        case 0x1e: {  // FMV.W.X / FMV.D.X - raw bit move into the float file
            if (inst.funct3 != 0x0 || inst.rs2 != 0) {
                return Status::bad(Exception::IllegalInstruction, inst.encoded);
            }
            const u64 x = read_reg(inst.rs1);
            write_freg(inst.rd, is_double ? x : nan_box(static_cast<u32>(x)));
            return Status::good();
        }

        default:
            return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }
}

Status Cpu::execute_fused_madd(const DecodedInst& inst) {
    Status fp = require_fpu();
    if (!fp) return Status::bad(Exception::IllegalInstruction, inst.encoded);

    // R4-type: a fourth register in the top five bits of what would be funct7.
    const u32  rs3       = inst.raw >> 27;
    const bool is_double = ((inst.raw >> 25) & 0x3) == 0x1;
    if (((inst.raw >> 25) & 0x3) > 0x1) {
        return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }

    // The point of a fused multiply-add is that the product is *not* rounded
    // before the addition - the whole a*b+c is computed once and rounded once.
    // std::fma is exactly this operation, and using a*b+c instead would round
    // twice and give a different answer in the last bit. That difference is
    // precisely what numerical code uses fma to avoid, so it is not a detail
    // that can be waved away.
    const bool negate_product = (inst.opcode == opcodes::NMSUB ||
                                 inst.opcode == opcodes::NMADD);
    const bool subtract_addend = (inst.opcode == opcodes::MSUB ||
                                  inst.opcode == opcodes::NMADD);

    return with_rounding(inst, [&] {
        if (is_double) {
            double a = bits_to_f64(fregs[inst.rs1]);
            double b = bits_to_f64(fregs[inst.rs2]);
            double c = bits_to_f64(fregs[rs3]);
            if (negate_product)  a = -a;
            if (subtract_addend) c = -c;
            const double r = std::fma(a, b, c);
            write_freg(inst.rd, std::isnan(r) ? CANONICAL_NAN_F64 : f64_to_bits(r));
        } else {
            float a = bits_to_f32(nan_unbox(fregs[inst.rs1]));
            float b = bits_to_f32(nan_unbox(fregs[inst.rs2]));
            float c = bits_to_f32(nan_unbox(fregs[rs3]));
            if (negate_product)  a = -a;
            if (subtract_addend) c = -c;
            const float r = std::fma(a, b, c);
            write_freg(inst.rd,
                       nan_box(std::isnan(r) ? CANONICAL_NAN_F32 : f32_to_bits(r)));
        }
    });
}

Status Cpu::execute_branch(const DecodedInst& inst) {
    const u64 a = read_reg(inst.rs1);
    const u64 b = read_reg(inst.rs2);

    bool taken = false;
    switch (inst.funct3) {
        case 0x0: taken = (a == b); break;                                     // BEQ
        case 0x1: taken = (a != b); break;                                     // BNE
        case 0x4: taken = (static_cast<i64>(a) < static_cast<i64>(b)); break;  // BLT
        case 0x5: taken = (static_cast<i64>(a) >= static_cast<i64>(b)); break; // BGE
        case 0x6: taken = (a < b); break;                                      // BLTU
        case 0x7: taken = (a >= b); break;                                     // BGEU
        default:
            return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }

    if (!taken) return Status::good();  // next_pc_ already points past this one

    return set_branch_target(pc + static_cast<u64>(inst.imm));
}

// ---------------------------------------------------------------------------
// Loads and stores
//
// A note on misaligned accesses. The spec permits an implementation either to
// support them in hardware or to raise a misaligned-address exception. We
// support them, matching QEMU's `virt` machine - which is what the kernels we
// intend to boot are developed against.
// ---------------------------------------------------------------------------

Status Cpu::execute_load(const DecodedInst& inst) {
    const u64 addr = read_reg(inst.rs1) + static_cast<u64>(inst.imm);

    unsigned size   = 0;
    bool     signed_ = false;
    switch (inst.funct3) {
        case 0x0: size = 1; signed_ = true;  break;  // LB
        case 0x1: size = 2; signed_ = true;  break;  // LH
        case 0x2: size = 4; signed_ = true;  break;  // LW
        case 0x3: size = 8; signed_ = false; break;  // LD
        case 0x4: size = 1; signed_ = false; break;  // LBU
        case 0x5: size = 2; signed_ = false; break;  // LHU
        case 0x6: size = 4; signed_ = false; break;  // LWU
        default:
            return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }

    Result<u64> r = mem_load(addr, size, AccessType::Load);
    if (!r) return Status::bad(r.trap);

    // The signed/unsigned distinction is the whole reason LW and LWU are
    // separate instructions on RV64: both read four bytes, but LW sign-extends
    // to 64 bits and LWU zero-extends. (On RV32 there is no LWU, because there
    // is nothing to extend into.)
    const u64 value = signed_ ? static_cast<u64>(sign_extend(r.value, size * 8))
                              : r.value;

    // Only commit to rd once the access has succeeded: a faulting load must
    // leave the destination register untouched.
    write_reg(inst.rd, value);
    return Status::good();
}

Status Cpu::execute_store(const DecodedInst& inst) {
    const u64 addr  = read_reg(inst.rs1) + static_cast<u64>(inst.imm);
    const u64 value = read_reg(inst.rs2);

    unsigned size = 0;
    switch (inst.funct3) {
        case 0x0: size = 1; break;  // SB
        case 0x1: size = 2; break;  // SH
        case 0x2: size = 4; break;  // SW
        case 0x3: size = 8; break;  // SD
        default:
            return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }

    // Stores have no sign/unsigned variants: they write the low `size` bytes of
    // the register and the rest is simply discarded.
    return mem_store(addr, size, value);
}

// ---------------------------------------------------------------------------
// Register-immediate ALU
//
// This is the group the old code got wrong. It matched on opcode 0x13 alone and
// executed ADDI for all of them, so SLTI, SLTIU, XORI, ORI, ANDI, SLLI, SRLI
// and SRAI - eight distinct instructions - all silently performed an addition.
// The funct3 field that distinguishes them was extracted and printed, but never
// used for dispatch.
// ---------------------------------------------------------------------------

Status Cpu::execute_op_imm(const DecodedInst& inst) {
    const u64 rs1 = read_reg(inst.rs1);
    const u64 imm = static_cast<u64>(inst.imm);  // already sign-extended to 64

    switch (inst.funct3) {
        case 0x0:  // ADDI
            // Wrapping addition is the defined behaviour; overflow is ignored
            // and never traps. Unsigned arithmetic gives us that for free
            // (signed overflow would be UB in C++).
            write_reg(inst.rd, rs1 + imm);
            return Status::good();

        case 0x1: {  // SLLI
            // On RV64 the shift amount is 6 bits. The upper 6 bits must be zero
            // for SLLI; any other encoding is reserved.
            if (inst.funct6() != 0x00) {
                return Status::bad(Exception::IllegalInstruction, inst.encoded);
            }
            write_reg(inst.rd, rs1 << inst.shamt6());
            return Status::good();
        }

        case 0x2:  // SLTI - set if less than, signed
            write_reg(inst.rd, (static_cast<i64>(rs1) < inst.imm) ? 1 : 0);
            return Status::good();

        case 0x3:  // SLTIU - set if less than, unsigned
            // Subtle: the immediate is still *sign-extended* to 64 bits first,
            // and only then compared as unsigned. So `sltiu rd, rs1, -1`
            // compares against 0xFFFF_FFFF_FFFF_FFFF, which is the idiom for
            // "is rs1 != max". Zero-extending the 12-bit field instead is a
            // classic emulator bug.
            write_reg(inst.rd, (rs1 < imm) ? 1 : 0);
            return Status::good();

        case 0x4:  // XORI
            write_reg(inst.rd, rs1 ^ imm);
            return Status::good();

        case 0x5: {  // SRLI / SRAI
            const u32 shamt = inst.shamt6();
            if (inst.funct6() == 0x00) {  // SRLI - logical, shifts in zeros
                write_reg(inst.rd, rs1 >> shamt);
                return Status::good();
            }
            if (inst.funct6() == 0x10) {  // SRAI - arithmetic, shifts in sign
                write_reg(inst.rd, static_cast<u64>(static_cast<i64>(rs1) >> shamt));
                return Status::good();
            }
            return Status::bad(Exception::IllegalInstruction, inst.encoded);
        }

        case 0x6:  // ORI
            write_reg(inst.rd, rs1 | imm);
            return Status::good();

        case 0x7:  // ANDI
            write_reg(inst.rd, rs1 & imm);
            return Status::good();

        default:
            return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }
}

// The "*W" immediate forms: compute on 32 bits, sign-extend the result to 64.
// Note that the shift amount here is 5 bits, not 6 - you cannot shift a 32-bit
// value by more than 31 - so these use funct7 as the selector rather than the
// funct6 that the 64-bit shifts need.
Status Cpu::execute_op_imm_32(const DecodedInst& inst) {
    const u32 rs1 = static_cast<u32>(read_reg(inst.rs1));

    switch (inst.funct3) {
        case 0x0:  // ADDIW
            write_reg(inst.rd, sext32(rs1 + static_cast<u32>(inst.imm)));
            return Status::good();

        case 0x1:  // SLLIW
            if (inst.funct7 != 0x00) {
                return Status::bad(Exception::IllegalInstruction, inst.encoded);
            }
            write_reg(inst.rd, sext32(rs1 << inst.shamt5()));
            return Status::good();

        case 0x5: {  // SRLIW / SRAIW
            const u32 shamt = inst.shamt5();
            if (inst.funct7 == 0x00) {  // SRLIW
                write_reg(inst.rd, sext32(rs1 >> shamt));
                return Status::good();
            }
            if (inst.funct7 == 0x20) {  // SRAIW
                write_reg(inst.rd, sext32(static_cast<u32>(static_cast<i32>(rs1) >> shamt)));
                return Status::good();
            }
            return Status::bad(Exception::IllegalInstruction, inst.encoded);
        }

        default:
            return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }
}

// ---------------------------------------------------------------------------
// Register-register ALU
// ---------------------------------------------------------------------------

Status Cpu::execute_op(const DecodedInst& inst) {
    // funct7 == 0x01 selects the M extension, which shares this opcode with the
    // base ALU operations and differs only in funct7. A `mul` decoded as an
    // `add` would be a genuinely nasty bug.
    if (inst.funct7 == 0x01) return execute_mul_div(inst);

    if (inst.funct7 != 0x00 && inst.funct7 != 0x20) {
        return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }

    const u64 a = read_reg(inst.rs1);
    const u64 b = read_reg(inst.rs2);

    // Shift amounts come from the low 6 bits of rs2 on RV64. Masking is
    // required, not optional: shifting a 64-bit value by >= 64 is undefined
    // behaviour in C++, so an unmasked shift by a large register value is a
    // real bug even though the spec says only the low bits are used.
    const u32 shamt = static_cast<u32>(b & 0x3f);

    switch (inst.funct3) {
        case 0x0:  // ADD / SUB
            write_reg(inst.rd, (inst.funct7 == 0x20) ? (a - b) : (a + b));
            return Status::good();

        case 0x1:  // SLL
            if (inst.funct7 != 0x00) return Status::bad(Exception::IllegalInstruction, inst.encoded);
            write_reg(inst.rd, a << shamt);
            return Status::good();

        case 0x2:  // SLT
            if (inst.funct7 != 0x00) return Status::bad(Exception::IllegalInstruction, inst.encoded);
            write_reg(inst.rd, (static_cast<i64>(a) < static_cast<i64>(b)) ? 1 : 0);
            return Status::good();

        case 0x3:  // SLTU
            if (inst.funct7 != 0x00) return Status::bad(Exception::IllegalInstruction, inst.encoded);
            write_reg(inst.rd, (a < b) ? 1 : 0);
            return Status::good();

        case 0x4:  // XOR
            if (inst.funct7 != 0x00) return Status::bad(Exception::IllegalInstruction, inst.encoded);
            write_reg(inst.rd, a ^ b);
            return Status::good();

        case 0x5:  // SRL / SRA
            if (inst.funct7 == 0x20) {
                write_reg(inst.rd, static_cast<u64>(static_cast<i64>(a) >> shamt));
            } else {
                write_reg(inst.rd, a >> shamt);
            }
            return Status::good();

        case 0x6:  // OR
            if (inst.funct7 != 0x00) return Status::bad(Exception::IllegalInstruction, inst.encoded);
            write_reg(inst.rd, a | b);
            return Status::good();

        case 0x7:  // AND
            if (inst.funct7 != 0x00) return Status::bad(Exception::IllegalInstruction, inst.encoded);
            write_reg(inst.rd, a & b);
            return Status::good();

        default:
            return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }
}

Status Cpu::execute_op_32(const DecodedInst& inst) {
    if (inst.funct7 == 0x01) return execute_mul_div_32(inst);

    if (inst.funct7 != 0x00 && inst.funct7 != 0x20) {
        return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }

    const u32 a = static_cast<u32>(read_reg(inst.rs1));
    const u32 b = static_cast<u32>(read_reg(inst.rs2));

    // 5-bit shift amount for the 32-bit forms.
    const u32 shamt = static_cast<u32>(read_reg(inst.rs2) & 0x1f);

    switch (inst.funct3) {
        case 0x0:  // ADDW / SUBW
            write_reg(inst.rd, sext32((inst.funct7 == 0x20) ? (a - b) : (a + b)));
            return Status::good();

        case 0x1:  // SLLW
            if (inst.funct7 != 0x00) return Status::bad(Exception::IllegalInstruction, inst.encoded);
            write_reg(inst.rd, sext32(a << shamt));
            return Status::good();

        case 0x5:  // SRLW / SRAW
            if (inst.funct7 == 0x20) {
                write_reg(inst.rd, sext32(static_cast<u32>(static_cast<i32>(a) >> shamt)));
            } else {
                write_reg(inst.rd, sext32(a >> shamt));
            }
            return Status::good();

        default:
            return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }
}

// ---------------------------------------------------------------------------
// SYSTEM
//
// ECALL and EBREAK are the base RV64I members; the CSR instructions share this
// opcode and are told apart by funct3 != 0.
//
// Both ECALL and EBREAK "fail" by design: they raise a trap, which is how they
// do their job. ECALL is how every system call in every RISC-V OS is made - the
// guest puts arguments in registers, executes ECALL, and the handler at mtvec
// takes over.
// ---------------------------------------------------------------------------

Status Cpu::execute_system(const DecodedInst& inst) {
    if (inst.funct3 != 0x0) return execute_csr(inst);

    switch (inst.imm & 0xfff) {
        case 0x000:  // ECALL
            if (inst.rd != 0 || inst.rs1 != 0) {
                return Status::bad(Exception::IllegalInstruction, inst.encoded);
            }
            // The cause encodes which privilege level made the call, so that a
            // machine-mode handler can tell a kernel's SBI call apart from a
            // user program's system call.
            switch (priv) {
                case PRIV_USER:       return Status::bad(Exception::ECallFromUMode, 0);
                case PRIV_SUPERVISOR:
                    // With SBI enabled, an ecall from supervisor mode is a call
                    // into firmware, not an exception. The emulator answers it
                    // and execution continues at the instruction after the
                    // ecall - which is why this returns success rather than a
                    // trap, and why next_pc_ has already been set past it.
                    if (sbi_enabled && sbi::handle_ecall(*this)) {
                        return Status::good();
                    }
                    return Status::bad(Exception::ECallFromSMode, 0);
                default:              return Status::bad(Exception::ECallFromMMode, 0);
            }

        case 0x001:  // EBREAK
            if (inst.rd != 0 || inst.rs1 != 0) {
                return Status::bad(Exception::IllegalInstruction, inst.encoded);
            }
            return Status::bad(Exception::Breakpoint, pc);

        case 0x102:  // SRET
            return execute_sret(inst);

        case 0x302:  // MRET
            return execute_mret(inst);

        case 0x105:  // WFI - wait for interrupt
            if (inst.rd != 0 || inst.rs1 != 0) {
                return Status::bad(Exception::IllegalInstruction, inst.encoded);
            }
            // The spec explicitly permits implementing WFI as a no-op: it is a
            // hint, and software must treat its completion as advisory and
            // re-check the condition it was waiting on. Running the loop hot
            // costs us nothing but host CPU. Phase 4 can make it genuinely idle
            // once there is a timer to sleep until.
            return Status::good();

        default:
            // SFENCE.VMA shares funct3 == 0 but is an R-type: funct7 == 0x09,
            // with rs1/rs2 naming an address and an ASID to narrow the flush.
            if (inst.funct7 == 0x09) return execute_sfence_vma(inst);
            return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }
}

// ---------------------------------------------------------------------------
// MRET - return from a machine-mode trap handler.
//
// Exactly undoes what enter_trap() did:
//
//   MIE       <- MPIE       (restore the previous interrupt-enable)
//   MPIE      <- 1
//   privilege <- MPP        (return to where the trap came from)
//   MPP       <- least-privileged supported mode
//   pc        <- mepc
// ---------------------------------------------------------------------------
Status Cpu::execute_mret(const DecodedInst& inst) {
    if (inst.rd != 0 || inst.rs1 != 0) {
        return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }
    if (priv < PRIV_MACHINE) {
        // MRET from anything below machine mode is illegal. Unreachable until
        // phase 6, but the check belongs with the instruction.
        return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }

    u64 status = csrs.mstatus();

    const bool mpie = (status & csr::MSTATUS_MPIE) != 0;
    status = mpie ? (status | csr::MSTATUS_MIE) : (status & ~csr::MSTATUS_MIE);

    // MPIE is set, not cleared. The spec is specific about this and it is easy
    // to get backwards.
    status |= csr::MSTATUS_MPIE;

    const u32 return_priv =
        static_cast<u32>((status & csr::MSTATUS_MPP) >> csr::MSTATUS_MPP_SHIFT);

    status = (status & ~csr::MSTATUS_MPP) |
             (static_cast<u64>(PRIV_LEAST_SUPPORTED) << csr::MSTATUS_MPP_SHIFT);

    // Returning below machine mode clears MPRV. Leaving it set would let the
    // supervisor we return into keep performing accesses at MPP's privilege,
    // which would be an escalation.
    if (return_priv != PRIV_MACHINE) status &= ~csr::MSTATUS_MPRV;

    csrs.set_mstatus(status);
    priv = return_priv;

    next_pc_ = csrs.read(csr::MEPC);
    return Status::good();
}

// ---------------------------------------------------------------------------
// SRET - return from a supervisor trap handler. The supervisor counterpart of
// MRET, using SPP/SPIE instead of MPP/MPIE.
// ---------------------------------------------------------------------------
Status Cpu::execute_sret(const DecodedInst& inst) {
    if (inst.rd != 0 || inst.rs1 != 0) {
        return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }
    if (priv < PRIV_SUPERVISOR) {
        return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }
    // mstatus.TSR ("trap SRET") lets machine-mode firmware intercept a
    // supervisor's returns - used by hypervisors to virtualise them.
    if (priv == PRIV_SUPERVISOR && (csrs.mstatus() & csr::MSTATUS_TSR)) {
        return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }

    u64 status = csrs.mstatus();

    const bool spie = (status & csr::MSTATUS_SPIE) != 0;
    status = spie ? (status | csr::MSTATUS_SIE) : (status & ~csr::MSTATUS_SIE);
    status |= csr::MSTATUS_SPIE;

    // SPP is one bit: supervisor or user.
    const u32 return_priv = (status & csr::MSTATUS_SPP) ? PRIV_SUPERVISOR : PRIV_USER;
    status &= ~csr::MSTATUS_SPP;   // reset to user, the least-privileged mode

    if (return_priv != PRIV_MACHINE) status &= ~csr::MSTATUS_MPRV;

    csrs.set_mstatus(status);
    priv = return_priv;

    next_pc_ = csrs.read(csr::SEPC);
    return Status::good();
}

// ---------------------------------------------------------------------------
// SFENCE.VMA - tell the hardware that page tables changed.
//
// Needed because the TLB caches translations, and nothing about writing a page
// table entry in memory tells the hardware to forget what it cached. A kernel
// that unmaps a page and does not fence would find the old mapping still works.
//
// rs1 names a virtual address and rs2 an ASID, allowing a narrower flush; we
// flush everything, which is always correct and is what a small TLB would do
// anyway.
// ---------------------------------------------------------------------------
Status Cpu::execute_sfence_vma(const DecodedInst& inst) {
    if (inst.rd != 0) {
        return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }
    if (priv < PRIV_SUPERVISOR) {
        return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }
    // mstatus.TVM ("trap virtual memory") intercepts a supervisor's page-table
    // management, again for virtualisation.
    if (priv == PRIV_SUPERVISOR && (csrs.mstatus() & csr::MSTATUS_TVM)) {
        return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }

    mmu.flush();
    return Status::good();
}

// ---------------------------------------------------------------------------
// CSR access checks.
//
// Three ways a CSR access can be illegal, all reported as IllegalInstruction:
//
//   1. the CSR is not implemented
//   2. the instruction writes a read-only CSR (address bits [11:10] == 11)
//   3. the current privilege is below the CSR's minimum (bits [9:8])
//
// The first of these is not a limitation but a feature: probing a CSR and
// catching the trap is how software detects optional extensions.
// ---------------------------------------------------------------------------

// mstatus.TVM traps a supervisor's access to satp as well as SFENCE.VMA - both
// are page-table management, and a hypervisor wants to intercept both.
bool Cpu::tvm_blocks(u32 addr) const {
    return addr == csr::SATP && priv == PRIV_SUPERVISOR &&
           (csrs.mstatus() & csr::MSTATUS_TVM) != 0;
}

Result<u64> Cpu::csr_read(u32 addr) const {
    if (!csrs.exists(addr)) {
        return Result<u64>::bad(Exception::IllegalInstruction, 0);
    }
    // The floating-point CSRs are part of the state mstatus.FS protects, so
    // they are unreachable while the unit is off. A kernel that turns the FPU
    // off to avoid saving it must be able to rely on user code not reading the
    // rounding mode a *different* process left behind - fcsr is as much
    // floating-point state as the registers are.
    if (addr == csr::FCSR || addr == csr::FFLAGS || addr == csr::FRM) {
        if (!csrs.fpu_enabled()) {
            return Result<u64>::bad(Exception::IllegalInstruction, 0);
        }
    }


    if (priv < csr::min_privilege(addr)) {
        return Result<u64>::bad(Exception::IllegalInstruction, 0);
    }
    if (tvm_blocks(addr)) {
        return Result<u64>::bad(Exception::IllegalInstruction, 0);
    }

    // The unprivileged counters are readable from a lower privilege level only
    // when the level above has enabled them. That is how a kernel can deny user
    // code a high-resolution clock - a real concern, since precise timing is
    // what side-channel attacks are built on.
    if (addr == csr::CYCLE && !csrs.counter_enabled(0, priv)) {
        return Result<u64>::bad(Exception::IllegalInstruction, 0);
    }
    if (addr == csr::TIME && !csrs.counter_enabled(1, priv)) {
        return Result<u64>::bad(Exception::IllegalInstruction, 0);
    }
    if (addr == csr::INSTRET && !csrs.counter_enabled(2, priv)) {
        return Result<u64>::bad(Exception::IllegalInstruction, 0);
    }

    // `time` is not a counter the hart keeps: it is a window onto the CLINT's
    // mtime, which is the machine-wide clock every hart shares.
    if (addr == csr::TIME && clint) {
        return Result<u64>::good(clint->mtime());
    }

    return Result<u64>::good(csrs.read(addr));
}

Status Cpu::csr_write(u32 addr, u64 value) {
    if (!csrs.exists(addr)) {
        return Status::bad(Exception::IllegalInstruction, 0);
    }

    // See csr_read: the floating-point CSRs are gated by mstatus.FS, and a
    // write to one also makes the unit Dirty - fcsr is state a context switch
    // has to save, exactly like the registers.
    if (addr == csr::FCSR || addr == csr::FFLAGS || addr == csr::FRM) {
        if (!csrs.fpu_enabled()) {
            return Status::bad(Exception::IllegalInstruction, 0);
        }
        csrs.mark_fpu_dirty();
    }
    if (csr::is_read_only(addr)) {
        return Status::bad(Exception::IllegalInstruction, 0);
    }
    if (priv < csr::min_privilege(addr)) {
        return Status::bad(Exception::IllegalInstruction, 0);
    }
    if (tvm_blocks(addr)) {
        return Status::bad(Exception::IllegalInstruction, 0);
    }
    if (addr == csr::MINSTRET || addr == csr::MCYCLE) counter_written_ = true;

    csrs.write(addr, value);

    // Changing satp changes the entire address space, so every cached
    // translation is now potentially wrong.
    if (addr == csr::SATP) mmu.flush();

    return Status::good();
}

// ---------------------------------------------------------------------------
// The six CSR instructions.
//
//   CSRRW  rd, csr, rs1     rd = csr; csr = rs1
//   CSRRS  rd, csr, rs1     rd = csr; csr |= rs1      (set bits)
//   CSRRC  rd, csr, rs1     rd = csr; csr &= ~rs1     (clear bits)
//   CSRRWI/CSRRSI/CSRRCI    same, with a 5-bit zero-extended immediate
//
// Each does an atomic read-modify-write, which is why setting a single bit in
// mstatus does not need a lock even on a real multi-hart machine.
//
// The subtlety is in when the access is *suppressed*:
//
//   * CSRRW/CSRRWI with rd == x0 must not READ the CSR
//   * CSRRS/CSRRC (and the immediate forms) with a zero source must not WRITE
//
// This matters because some CSRs have read or write side effects - reading a
// PLIC claim register acknowledges an interrupt, for instance. Performing a
// suppressed access would silently consume events. It also means
// `csrr rd, csr` (which assembles to CSRRS with rs1 = x0) is a pure read and
// can legally target a read-only CSR.
// ---------------------------------------------------------------------------
Status Cpu::execute_csr(const DecodedInst& inst) {
    const u32 addr = static_cast<u32>(inst.imm & 0xfff);

    // For the immediate forms the rs1 field is a 5-bit unsigned value, not a
    // register number.
    const bool immediate_form = (inst.funct3 & 0x4) != 0;
    const u64  src = immediate_form ? static_cast<u64>(inst.rs1) : read_reg(inst.rs1);

    switch (inst.funct3 & 0x3) {
        case 0x1: {  // CSRRW / CSRRWI - swap
            u64 old = 0;
            if (inst.rd != 0) {
                Result<u64> r = csr_read(addr);
                if (!r) return Status::bad(r.trap);
                old = r.value;
            } else {
                // rd == x0: the read is suppressed, but the write still has to
                // pass its access checks.
                if (!csrs.exists(addr) || priv < csr::min_privilege(addr)) {
                    return Status::bad(Exception::IllegalInstruction, 0);
                }
            }

            Status st = csr_write(addr, src);
            if (!st) return st;

            write_reg(inst.rd, old);
            return Status::good();
        }

        case 0x2:    // CSRRS / CSRRSI - set bits
        case 0x3: {  // CSRRC / CSRRCI - clear bits
            Result<u64> r = csr_read(addr);
            if (!r) return Status::bad(r.trap);

            // A zero source means "read only": no write is attempted at all,
            // so this is legal even on a read-only CSR.
            if (src != 0) {
                const u64 next = ((inst.funct3 & 0x3) == 0x2) ? (r.value | src)
                                                              : (r.value & ~src);
                Status st = csr_write(addr, next);
                if (!st) return st;
            }

            write_reg(inst.rd, r.value);
            return Status::good();
        }

        default:
            return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }
}

// ---------------------------------------------------------------------------
// Debug output
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// M extension: multiply and divide.
//
// The striking thing about RISC-V division is that **it never traps**. Divide
// by zero and signed overflow both produce specific, defined values rather than
// an exception:
//
//   DIV  by zero -> -1        DIVU by zero -> all ones (2^64 - 1)
//   REM  by zero -> dividend  REMU by zero -> dividend
//   DIV(INT64_MIN, -1) -> INT64_MIN     REM(INT64_MIN, -1) -> 0
//
// That last pair is the one C++ cannot express directly: INT64_MIN / -1 is
// mathematically 2^63, which does not fit, and computing it is undefined
// behaviour that crashes on x86 with SIGFPE. It has to be special-cased.
//
// The "no trap" choice keeps the hardware simple: no divider needs a fault
// path, and software that cares checks its divisor beforehand.
// ---------------------------------------------------------------------------

Status Cpu::execute_mul_div(const DecodedInst& inst) {
    const u64 a = read_reg(inst.rs1);
    const u64 b = read_reg(inst.rs2);
    const i64 sa = static_cast<i64>(a);
    const i64 sb = static_cast<i64>(b);

    switch (inst.funct3) {
        case 0x0:  // MUL - low 64 bits; signedness does not affect the low half
            write_reg(inst.rd, a * b);
            return Status::good();

        // The MULH family returns the *upper* 64 bits of the 128-bit product,
        // so a full 64x64->128 multiply is needed. __int128 is a GCC/Clang
        // extension rather than standard C++, but every compiler that can build
        // this project has it, and hand-rolling the four-way split would add
        // bugs without adding portability we would actually use.
        case 0x1: {  // MULH - signed x signed
            const __int128 p = static_cast<__int128>(sa) * static_cast<__int128>(sb);
            write_reg(inst.rd, static_cast<u64>(static_cast<unsigned __int128>(p) >> 64));
            return Status::good();
        }

        case 0x2: {  // MULHSU - signed x unsigned
            // Casting the unsigned operand to __int128 zero-extends it, so it
            // stays non-negative and the multiply is genuinely signed-by-
            // unsigned. This instruction exists to make multi-word arithmetic
            // work: the limbs of a bignum are unsigned, but the top one carries
            // the sign.
            const __int128 p = static_cast<__int128>(sa) * static_cast<__int128>(b);
            write_reg(inst.rd, static_cast<u64>(static_cast<unsigned __int128>(p) >> 64));
            return Status::good();
        }

        case 0x3: {  // MULHU - unsigned x unsigned
            const unsigned __int128 p =
                static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b);
            write_reg(inst.rd, static_cast<u64>(p >> 64));
            return Status::good();
        }

        case 0x4:  // DIV
            if (sb == 0) {
                write_reg(inst.rd, ~0ull);                    // -1
            } else if (sa == INT64_MIN && sb == -1) {
                write_reg(inst.rd, static_cast<u64>(sa));     // overflow: wraps
            } else {
                write_reg(inst.rd, static_cast<u64>(sa / sb));
            }
            return Status::good();

        case 0x5:  // DIVU
            write_reg(inst.rd, (b == 0) ? ~0ull : (a / b));
            return Status::good();

        case 0x6:  // REM
            if (sb == 0) {
                write_reg(inst.rd, a);                        // the dividend
            } else if (sa == INT64_MIN && sb == -1) {
                write_reg(inst.rd, 0);
            } else {
                write_reg(inst.rd, static_cast<u64>(sa % sb));
            }
            return Status::good();

        case 0x7:  // REMU
            write_reg(inst.rd, (b == 0) ? a : (a % b));
            return Status::good();

        default:
            return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }
}

// The 32-bit forms. Same defined results, computed on 32 bits, then
// sign-extended to 64 like every other *W instruction. Note there is no MULHW:
// the upper half of a 32x32 product fits in the low 64 bits anyway, so MULW
// alone is enough.
Status Cpu::execute_mul_div_32(const DecodedInst& inst) {
    const u32 a = static_cast<u32>(read_reg(inst.rs1));
    const u32 b = static_cast<u32>(read_reg(inst.rs2));
    const i32 sa = static_cast<i32>(a);
    const i32 sb = static_cast<i32>(b);

    switch (inst.funct3) {
        case 0x0:  // MULW
            write_reg(inst.rd, sext32(a * b));
            return Status::good();

        case 0x4:  // DIVW
            if (sb == 0) {
                write_reg(inst.rd, ~0ull);
            } else if (sa == INT32_MIN && sb == -1) {
                write_reg(inst.rd, sext32(static_cast<u32>(sa)));
            } else {
                write_reg(inst.rd, sext32(static_cast<u32>(sa / sb)));
            }
            return Status::good();

        case 0x5:  // DIVUW
            write_reg(inst.rd, (b == 0) ? ~0ull : sext32(a / b));
            return Status::good();

        case 0x6:  // REMW
            if (sb == 0) {
                write_reg(inst.rd, sext32(a));
            } else if (sa == INT32_MIN && sb == -1) {
                write_reg(inst.rd, 0);
            } else {
                write_reg(inst.rd, sext32(static_cast<u32>(sa % sb)));
            }
            return Status::good();

        case 0x7:  // REMUW
            write_reg(inst.rd, (b == 0) ? sext32(a) : sext32(a % b));
            return Status::good();

        default:
            return Status::bad(Exception::IllegalInstruction, inst.encoded);
    }
}

// ---------------------------------------------------------------------------
// A extension: atomics.
//
// Two families sharing one opcode, selected by funct5 (instruction bits 31:27):
//
//   LR / SC   load-reserved and store-conditional, the pair that builds
//             lock-free algorithms and, in xv6's case, spinlocks
//   AMO*      read-modify-write in one indivisible step
//
// **LR/SC.** LR loads a word and registers a reservation on its address. SC
// stores only if that reservation still holds, and reports whether it did:
// 0 in rd for success, non-zero for failure. Software retries on failure.
//
// The reason this pair exists rather than a plain compare-and-swap is the ABA
// problem: CAS cannot tell "unchanged" from "changed and changed back". A
// reservation is broken by *any* intervening write, so LR/SC notices.
//
// On a single-hart emulator nothing can write memory behind our back, so a
// reservation could never be broken and every SC would succeed. That would be
// wrong in one important way: a trap between the LR and the SC is a context
// switch, and the thread that resumes must not inherit the other's
// reservation. So enter_trap() clears it.
//
// **AMOs** load, apply an operation with rs2, store the result, and return the
// *original* value in rd. On real hardware the whole sequence is indivisible;
// here we execute one instruction at a time and nothing else touches memory, so
// it already is.
//
// Unlike ordinary loads and stores, the spec *requires* natural alignment for
// atomics - a misaligned one raises StoreAMOAddressMisaligned rather than being
// emulated.
// ---------------------------------------------------------------------------
Status Cpu::execute_amo(const DecodedInst& inst) {
    unsigned size;
    if (inst.funct3 == 0x2)      size = 4;   // .W
    else if (inst.funct3 == 0x3) size = 8;   // .D
    else return Status::bad(Exception::IllegalInstruction, inst.encoded);

    // Bits 31:27 select the operation; bits 26 and 25 are the aq/rl ordering
    // hints, which a single-hart in-order machine can ignore for the same
    // reason FENCE is a no-op here.
    const u32 funct5 = (inst.funct7 >> 2) & 0x1f;
    const u64 addr   = read_reg(inst.rs1);

    if (addr % size != 0) {
        return Status::bad(Exception::StoreAMOAddressMisaligned, addr);
    }

    if (funct5 == 0x02) {  // LR
        if (inst.rs2 != 0) return Status::bad(Exception::IllegalInstruction, inst.encoded);
        Result<u64> r = mem_load(addr, size, AccessType::Load);
        if (!r) return Status::bad(r.trap);

        reservation_valid_ = true;
        reservation_addr_  = addr;
        write_reg(inst.rd, (size == 4) ? sext32(static_cast<u32>(r.value)) : r.value);
        return Status::good();
    }

    if (funct5 == 0x03) {  // SC
        const bool holds = reservation_valid_ && reservation_addr_ == addr;

        // The reservation is consumed either way. Leaving it set after a failed
        // SC would let a later SC succeed against a stale reservation.
        reservation_valid_ = false;

        if (holds) {
            Status st = mem_store(addr, size, read_reg(inst.rs2));
            if (!st) return st;
            write_reg(inst.rd, 0);   // 0 means the store happened
        } else {
            write_reg(inst.rd, 1);   // any non-zero means it did not
        }
        return Status::good();
    }

    // --- the read-modify-write AMOs ---
    Result<u64> r = mem_load(addr, size, AccessType::Load);
    if (!r) return Status::bad(r.trap);

    const u64 orig = r.value;
    // Read rs2 before writing rd: they may be the same register.
    const u64 src = read_reg(inst.rs2);
    u64 result;

    if (size == 4) {
        const u32 ua = static_cast<u32>(orig), ub = static_cast<u32>(src);
        const i32 sa = static_cast<i32>(ua),   sb = static_cast<i32>(ub);
        switch (funct5) {
            case 0x00: result = ua + ub; break;                        // AMOADD
            case 0x01: result = ub; break;                             // AMOSWAP
            case 0x04: result = ua ^ ub; break;                        // AMOXOR
            case 0x08: result = ua | ub; break;                        // AMOOR
            case 0x0c: result = ua & ub; break;                        // AMOAND
            case 0x10: result = static_cast<u32>(sa < sb ? sa : sb); break;  // AMOMIN
            case 0x14: result = static_cast<u32>(sa > sb ? sa : sb); break;  // AMOMAX
            case 0x18: result = (ua < ub) ? ua : ub; break;            // AMOMINU
            case 0x1c: result = (ua > ub) ? ua : ub; break;            // AMOMAXU
            default: return Status::bad(Exception::IllegalInstruction, inst.encoded);
        }
        Status st = mem_store(addr, 4, result);
        if (!st) return st;
        write_reg(inst.rd, sext32(static_cast<u32>(orig)));
    } else {
        const i64 sa = static_cast<i64>(orig), sb = static_cast<i64>(src);
        switch (funct5) {
            case 0x00: result = orig + src; break;
            case 0x01: result = src; break;
            case 0x04: result = orig ^ src; break;
            case 0x08: result = orig | src; break;
            case 0x0c: result = orig & src; break;
            case 0x10: result = static_cast<u64>(sa < sb ? sa : sb); break;
            case 0x14: result = static_cast<u64>(sa > sb ? sa : sb); break;
            case 0x18: result = (orig < src) ? orig : src; break;
            case 0x1c: result = (orig > src) ? orig : src; break;
            default: return Status::bad(Exception::IllegalInstruction, inst.encoded);
        }
        Status st = mem_store(addr, 8, result);
        if (!st) return st;
        write_reg(inst.rd, orig);
    }
    return Status::good();
}

void Cpu::trace_inst(const DecodedInst& inst) const {
    std::ostream& os = trace_stream ? *trace_stream : std::cerr;

    // One compact line per instruction, on stderr so it does not get mixed into
    // the guest's own console output on stdout.
    std::ios_base::fmtflags saved = os.flags();

    os << std::hex << std::setfill('0')
       << "0x" << std::setw(16) << pc
       << "  " << std::setw(8) << inst.raw << "  "
       << std::setfill(' ') << std::left << std::setw(6) << mnemonic(inst) << std::right
       << std::dec;

    // Print only the operands the instruction's format actually uses. Dumping
    // every field unconditionally is worse than useless: for an I-type
    // instruction the rs2 bits are part of the immediate, so showing "rs2=28"
    // next to a shift invites you to chase a register that is not involved.
    auto reg = [&](const char* label, u32 n) {
        os << " " << label << "=x" << n << "(" << REG_ABI_NAMES[n] << ")";
    };

    const bool is_shift_imm =
        (inst.opcode == opcodes::OP_IMM && (inst.funct3 == 0x1 || inst.funct3 == 0x5)) ||
        (inst.opcode == opcodes::OP_IMM_32 && (inst.funct3 == 0x1 || inst.funct3 == 0x5));

    switch (inst.fmt) {
        case Format::R:
            reg("rd", inst.rd);
            reg("rs1", inst.rs1);
            reg("rs2", inst.rs2);
            break;
        case Format::I:
            // ECALL/EBREAK/MRET/WFI have no meaningful operands at all.
            if (inst.opcode == opcodes::SYSTEM && inst.funct3 == 0) break;
            // For a CSR instruction the immediate is a CSR number, and the rs1
            // field may be a 5-bit immediate rather than a register.
            if (inst.opcode == opcodes::SYSTEM) {
                reg("rd", inst.rd);
                os << " csr=0x" << std::hex << (inst.imm & 0xfff) << std::dec;
                if (inst.funct3 & 0x4) {
                    os << " uimm=" << inst.rs1;
                } else {
                    reg("rs1", inst.rs1);
                }
                break;
            }
            reg("rd", inst.rd);
            reg("rs1", inst.rs1);
            // For shifts the useful field is the shift amount, not the raw
            // 12-bit immediate (which also encodes funct6/funct7).
            if (is_shift_imm) {
                os << " shamt=" << (inst.opcode == opcodes::OP_IMM ? inst.shamt6() : inst.shamt5());
            } else {
                os << " imm=" << inst.imm;
            }
            break;
        case Format::S:
        case Format::B:
            reg("rs1", inst.rs1);
            reg("rs2", inst.rs2);
            os << " imm=" << inst.imm;
            // For a branch, the absolute target is far more useful than the
            // offset when you are following control flow in a trace.
            if (inst.fmt == Format::B) {
                os << " -> 0x" << std::hex << (pc + static_cast<u64>(inst.imm)) << std::dec;
            }
            break;
        case Format::U:
            reg("rd", inst.rd);
            os << " imm=0x" << std::hex << static_cast<u64>(inst.imm) << std::dec;
            break;
        case Format::J:
            reg("rd", inst.rd);
            os << " -> 0x" << std::hex << (pc + static_cast<u64>(inst.imm)) << std::dec;
            break;
        case Format::Unknown:
            break;
    }

    os << "\n";
    os.flags(saved);
}

void Cpu::dump_registers(std::ostream& os) const {
    std::ios_base::fmtflags saved = os.flags();

    os << "=== register dump ===\n";
    os << "pc  : 0x" << std::hex << std::setfill('0') << std::setw(16) << pc << "\n";
    for (int i = 0; i < NUM_REGS; ++i) {
        // Build the label first so it can be padded to a fixed width; tabs give
        // ragged columns because the names vary from 2 to 4 characters.
        std::string label = "x" + std::to_string(i) + " (" + REG_ABI_NAMES[i] + ")";
        os << std::left << std::setfill(' ') << std::setw(12) << label << std::right
           << ": 0x" << std::hex << std::setfill('0') << std::setw(16) << regs[i]
           << std::dec << "\n";
    }
    os << std::setfill(' ') << "instret: " << instret << "\n";

    // Privilege and the CSRs that explain where a kernel has got to. Without
    // these a dump from a booting OS says almost nothing: the same PC means
    // very different things in machine mode with paging off and in supervisor
    // mode with a page table installed.
    os << "priv   : " << privilege_name(priv) << "\n";
    auto csr_line = [&](const char* label, u32 addr) {
        os << std::left << std::setfill(' ') << std::setw(12) << label << std::right
           << ": 0x" << std::hex << std::setfill('0') << std::setw(16)
           << csrs.read(addr) << std::dec << "\n";
    };
    csr_line("mstatus", csr::MSTATUS);
    csr_line("mcause", csr::MCAUSE);
    csr_line("mepc", csr::MEPC);
    csr_line("mtvec", csr::MTVEC);
    csr_line("scause", csr::SCAUSE);
    csr_line("sepc", csr::SEPC);
    csr_line("stval", csr::STVAL);
    csr_line("stvec", csr::STVEC);
    csr_line("satp", csr::SATP);
    csr_line("mip", csr::MIP);
    csr_line("mie", csr::MIE);
    os << std::setfill(' ') << "=====================\n";

    os.flags(saved);
}
