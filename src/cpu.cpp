#include "cpu.hpp"

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

Cpu::Cpu(Bus& bus) : bus_(bus) {
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

Result<u32> Cpu::fetch() const {
    // Without the C (compressed) extension every instruction is 4 bytes and
    // must be 4-byte aligned. Phase 8 relaxes this to 2 bytes when C lands.
    //
    // In practice a misaligned PC is caught by set_branch_target() on the jump
    // that produced it, so this is a backstop for a PC set some other way.
    if ((pc & 0x3) != 0) {
        return Result<u32>::bad(Exception::InstructionAddressMisaligned, pc);
    }

    Result<u64> word = bus_.load(pc, 4, AccessType::Instruction);
    if (!word) return Result<u32>::bad(word.trap);

    return Result<u32>::good(static_cast<u32>(word.value));
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

    Result<u32> word = fetch();
    if (!word) return handle_trap_or_stop(word.trap);

    const DecodedInst inst = decode(word.value);

    // Default: fall through to the next instruction. Jumps and branches
    // overwrite this inside execute().
    next_pc_ = pc + 4;
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

    if (trap_fatal_without_handler && csrs.read(csr::MTVEC) == 0) {
        // Leave pc on the faulting instruction, which is both what a register
        // dump wants to show and what mepc would have received.
        return Status::bad(trap);
    }

    take_trap(trap);
    return Status::good();
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
    csrs.write(csr::MEPC, epc);
    csrs.write(csr::MCAUSE, cause_code);
    csrs.write(csr::MTVAL, tval);

    u64 status = csrs.mstatus();

    // Save the current interrupt-enable into MPIE, then clear MIE. A handler
    // therefore starts with interrupts off and cannot be interrupted by the
    // same source before it has had a chance to quiet it.
    const bool mie = (status & csr::MSTATUS_MIE) != 0;
    status = mie ? (status | csr::MSTATUS_MPIE) : (status & ~csr::MSTATUS_MPIE);
    status &= ~csr::MSTATUS_MIE;

    // Record the privilege we came from, so MRET knows where to return.
    status = (status & ~csr::MSTATUS_MPP) |
             (static_cast<u64>(priv) << csr::MSTATUS_MPP_SHIFT);

    csrs.set_mstatus(status);
    priv = PRIV_MACHINE;  // traps always land in machine mode until phase 6

    // A trap may be a context switch. If one thread does LR, is interrupted,
    // and another thread runs, the first thread's SC must fail when it
    // eventually resumes - otherwise both could believe they took the lock.
    clear_reservation();

    const u64 tvec = csrs.read(csr::MTVEC);
    const u64 base = tvec & ~csr::MTVEC_MODE_MASK;
    const u64 mode = tvec & csr::MTVEC_MODE_MASK;

    // Vectored mode spreads *interrupts* across a table of handlers, one entry
    // per cause, four bytes apart. Exceptions always go to the base address
    // even in vectored mode - a detail that is easy to miss and produces wild
    // jumps when got wrong.
    if (mode == csr::MTVEC_MODE_VECTORED && is_interrupt) {
        pc = base + 4 * (cause_code & 0x3f);
    } else {
        pc = base;
    }
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
    // Interrupts are globally gated by mstatus.MIE while in machine mode.
    if (!csrs.mstatus_mie()) return false;

    const u64 ready = csrs.pending_enabled();
    if (ready == 0) return false;

    // Priority order is fixed by the spec: external, then software, then timer,
    // machine before supervisor. It is not the bit order.
    if (ready & csr::MIP_MEIP) { out = Interrupt::MachineExternal; return true; }
    if (ready & csr::MIP_MSIP) { out = Interrupt::MachineSoftware; return true; }
    if (ready & csr::MIP_MTIP) { out = Interrupt::MachineTimer;    return true; }
    if (ready & csr::MIP_SEIP) { out = Interrupt::SupervisorExternal; return true; }
    if (ready & csr::MIP_SSIP) { out = Interrupt::SupervisorSoftware; return true; }
    if (ready & csr::MIP_STIP) { out = Interrupt::SupervisorTimer;    return true; }
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
            return Status::bad(Exception::IllegalInstruction, inst.raw);
    }
}

Status Cpu::set_branch_target(u64 target) {
    if ((target & 0x3) != 0) {
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
    const u64 link   = pc + 4;

    // Compute the target and check it *before* writing the link register, so a
    // misaligned jump leaves the machine untouched.
    Status st = set_branch_target(target);
    if (!st) return st;

    write_reg(inst.rd, link);
    return Status::good();
}

Status Cpu::execute_jalr(const DecodedInst& inst) {
    if (inst.funct3 != 0x0) {
        return Status::bad(Exception::IllegalInstruction, inst.raw);
    }

    // The low bit of the computed target is cleared, unconditionally. This is
    // not an alignment check - it is defined behaviour, and it exists so that
    // the C extension can use bit 0 of a function pointer without confusing
    // indirect jumps. Note the ordering: add, *then* clear, so `jalr rd, rs1, 1`
    // is a legal way to reach an even address.
    const u64 target = (read_reg(inst.rs1) + static_cast<u64>(inst.imm)) & ~1ull;
    const u64 link   = pc + 4;

    Status st = set_branch_target(target);
    if (!st) return st;

    // rd is written after the target is computed, which matters because rd and
    // rs1 may be the same register - `jalr ra, ra, 0` is a real idiom.
    write_reg(inst.rd, link);
    return Status::good();
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
            return Status::bad(Exception::IllegalInstruction, inst.raw);
    }

    if (!taken) return Status::good();  // next_pc_ already points at pc + 4

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
            return Status::bad(Exception::IllegalInstruction, inst.raw);
    }

    Result<u64> r = bus_.load(addr, size, AccessType::Load);
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
            return Status::bad(Exception::IllegalInstruction, inst.raw);
    }

    // Stores have no sign/unsigned variants: they write the low `size` bytes of
    // the register and the rest is simply discarded.
    return bus_.store(addr, size, value);
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
                return Status::bad(Exception::IllegalInstruction, inst.raw);
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
            return Status::bad(Exception::IllegalInstruction, inst.raw);
        }

        case 0x6:  // ORI
            write_reg(inst.rd, rs1 | imm);
            return Status::good();

        case 0x7:  // ANDI
            write_reg(inst.rd, rs1 & imm);
            return Status::good();

        default:
            return Status::bad(Exception::IllegalInstruction, inst.raw);
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
                return Status::bad(Exception::IllegalInstruction, inst.raw);
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
            return Status::bad(Exception::IllegalInstruction, inst.raw);
        }

        default:
            return Status::bad(Exception::IllegalInstruction, inst.raw);
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
        return Status::bad(Exception::IllegalInstruction, inst.raw);
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
            if (inst.funct7 != 0x00) return Status::bad(Exception::IllegalInstruction, inst.raw);
            write_reg(inst.rd, a << shamt);
            return Status::good();

        case 0x2:  // SLT
            if (inst.funct7 != 0x00) return Status::bad(Exception::IllegalInstruction, inst.raw);
            write_reg(inst.rd, (static_cast<i64>(a) < static_cast<i64>(b)) ? 1 : 0);
            return Status::good();

        case 0x3:  // SLTU
            if (inst.funct7 != 0x00) return Status::bad(Exception::IllegalInstruction, inst.raw);
            write_reg(inst.rd, (a < b) ? 1 : 0);
            return Status::good();

        case 0x4:  // XOR
            if (inst.funct7 != 0x00) return Status::bad(Exception::IllegalInstruction, inst.raw);
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
            if (inst.funct7 != 0x00) return Status::bad(Exception::IllegalInstruction, inst.raw);
            write_reg(inst.rd, a | b);
            return Status::good();

        case 0x7:  // AND
            if (inst.funct7 != 0x00) return Status::bad(Exception::IllegalInstruction, inst.raw);
            write_reg(inst.rd, a & b);
            return Status::good();

        default:
            return Status::bad(Exception::IllegalInstruction, inst.raw);
    }
}

Status Cpu::execute_op_32(const DecodedInst& inst) {
    if (inst.funct7 == 0x01) return execute_mul_div_32(inst);

    if (inst.funct7 != 0x00 && inst.funct7 != 0x20) {
        return Status::bad(Exception::IllegalInstruction, inst.raw);
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
            if (inst.funct7 != 0x00) return Status::bad(Exception::IllegalInstruction, inst.raw);
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
            return Status::bad(Exception::IllegalInstruction, inst.raw);
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
                return Status::bad(Exception::IllegalInstruction, inst.raw);
            }
            // The cause encodes which privilege level made the call, so that a
            // machine-mode handler can tell a kernel's SBI call apart from a
            // user program's system call.
            switch (priv) {
                case PRIV_USER:       return Status::bad(Exception::ECallFromUMode, 0);
                case PRIV_SUPERVISOR: return Status::bad(Exception::ECallFromSMode, 0);
                default:              return Status::bad(Exception::ECallFromMMode, 0);
            }

        case 0x001:  // EBREAK
            if (inst.rd != 0 || inst.rs1 != 0) {
                return Status::bad(Exception::IllegalInstruction, inst.raw);
            }
            return Status::bad(Exception::Breakpoint, pc);

        case 0x302:  // MRET
            return execute_mret(inst);

        case 0x105:  // WFI - wait for interrupt
            if (inst.rd != 0 || inst.rs1 != 0) {
                return Status::bad(Exception::IllegalInstruction, inst.raw);
            }
            // The spec explicitly permits implementing WFI as a no-op: it is a
            // hint, and software must treat its completion as advisory and
            // re-check the condition it was waiting on. Running the loop hot
            // costs us nothing but host CPU. Phase 4 can make it genuinely idle
            // once there is a timer to sleep until.
            return Status::good();

        default:
            // SRET (0x102) and SFENCE.VMA arrive with supervisor mode in
            // phase 6.
            return Status::bad(Exception::IllegalInstruction, inst.raw);
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
        return Status::bad(Exception::IllegalInstruction, inst.raw);
    }
    if (priv < PRIV_MACHINE) {
        // MRET from anything below machine mode is illegal. Unreachable until
        // phase 6, but the check belongs with the instruction.
        return Status::bad(Exception::IllegalInstruction, inst.raw);
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

    csrs.set_mstatus(status);
    priv = return_priv;

    next_pc_ = csrs.read(csr::MEPC);
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

Result<u64> Cpu::csr_read(u32 addr) const {
    if (!csrs.exists(addr)) {
        return Result<u64>::bad(Exception::IllegalInstruction, 0);
    }
    if (priv < csr::min_privilege(addr)) {
        return Result<u64>::bad(Exception::IllegalInstruction, 0);
    }
    return Result<u64>::good(csrs.read(addr));
}

Status Cpu::csr_write(u32 addr, u64 value) {
    if (!csrs.exists(addr)) {
        return Status::bad(Exception::IllegalInstruction, 0);
    }
    if (csr::is_read_only(addr)) {
        return Status::bad(Exception::IllegalInstruction, 0);
    }
    if (priv < csr::min_privilege(addr)) {
        return Status::bad(Exception::IllegalInstruction, 0);
    }
    if (addr == csr::MINSTRET || addr == csr::MCYCLE) counter_written_ = true;

    csrs.write(addr, value);
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
            return Status::bad(Exception::IllegalInstruction, inst.raw);
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
            return Status::bad(Exception::IllegalInstruction, inst.raw);
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
            return Status::bad(Exception::IllegalInstruction, inst.raw);
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
    else return Status::bad(Exception::IllegalInstruction, inst.raw);

    // Bits 31:27 select the operation; bits 26 and 25 are the aq/rl ordering
    // hints, which a single-hart in-order machine can ignore for the same
    // reason FENCE is a no-op here.
    const u32 funct5 = (inst.funct7 >> 2) & 0x1f;
    const u64 addr   = read_reg(inst.rs1);

    if (addr % size != 0) {
        return Status::bad(Exception::StoreAMOAddressMisaligned, addr);
    }

    if (funct5 == 0x02) {  // LR
        if (inst.rs2 != 0) return Status::bad(Exception::IllegalInstruction, inst.raw);
        Result<u64> r = bus_.load(addr, size, AccessType::Load);
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
            Status st = bus_.store(addr, size, read_reg(inst.rs2));
            if (!st) return st;
            write_reg(inst.rd, 0);   // 0 means the store happened
        } else {
            write_reg(inst.rd, 1);   // any non-zero means it did not
        }
        return Status::good();
    }

    // --- the read-modify-write AMOs ---
    Result<u64> r = bus_.load(addr, size, AccessType::Load);
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
            default: return Status::bad(Exception::IllegalInstruction, inst.raw);
        }
        Status st = bus_.store(addr, 4, result);
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
            default: return Status::bad(Exception::IllegalInstruction, inst.raw);
        }
        Status st = bus_.store(addr, 8, result);
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
    os << "=====================\n";

    os.flags(saved);
}
