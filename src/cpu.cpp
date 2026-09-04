#include "cpu.hpp"

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
    Result<u32> word = fetch();
    if (!word) return Status::bad(word.trap);

    const DecodedInst inst = decode(word.value);

    // Default: fall through to the next instruction. Jumps and branches
    // overwrite this inside execute().
    next_pc_ = pc + 4;

    if (trace) trace_inst(inst);

    Status st = execute(inst);
    if (!st) {
        // Leave pc on the faulting instruction. Once CSRs exist (phase 2) this
        // is the value that goes into mepc.
        return st;
    }

    pc = next_pc_;
    ++instret;
    return Status::good();
}

Status Cpu::run(u64 max_steps, u64* steps_out) {
    u64 n = 0;
    for (; n < max_steps; ++n) {
        Status st = step();
        if (!st) {
            if (steps_out) *steps_out = n;
            return st;
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
    // funct7 == 0x01 selects the M extension (MUL, DIV, REM ...), which lands
    // in phase 3. Until then it correctly traps as illegal rather than being
    // mistaken for one of the base operations.
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
// Only ECALL and EBREAK exist in base RV64I. The CSR instructions share this
// opcode (funct3 != 0) and arrive in phase 2.
//
// Both of these "fail" by design: they raise a trap, which is how they do their
// job. ECALL is how every system call in every RISC-V OS is made - the guest
// puts arguments in registers, executes ECALL, and the trap handler takes over.
// Right now a trap simply stops the emulator, because there is nowhere for it
// to go; phase 2 adds mtvec and the machinery to dispatch it to a handler.
// ---------------------------------------------------------------------------

Status Cpu::execute_system(const DecodedInst& inst) {
    if (inst.funct3 != 0x0) {
        // CSRRW/CSRRS/CSRRC and friends - phase 2.
        return Status::bad(Exception::IllegalInstruction, inst.raw);
    }

    // rd and rs1 must be zero for ECALL/EBREAK.
    if (inst.rd != 0 || inst.rs1 != 0) {
        return Status::bad(Exception::IllegalInstruction, inst.raw);
    }

    switch (inst.imm & 0xfff) {
        case 0x000:
            // With no privilege modes yet we are effectively always in machine
            // mode, so this is the M-mode cause. Phase 6 makes it depend on the
            // current privilege level.
            return Status::bad(Exception::ECallFromMMode, 0);

        case 0x001:
            return Status::bad(Exception::Breakpoint, pc);

        default:
            // MRET (0x302), SRET (0x102), WFI (0x105) etc. - phases 2 and 6.
            return Status::bad(Exception::IllegalInstruction, inst.raw);
    }
}

// ---------------------------------------------------------------------------
// Debug output
// ---------------------------------------------------------------------------

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
            // ECALL/EBREAK have no meaningful operands at all.
            if (inst.opcode == opcodes::SYSTEM && inst.funct3 == 0) break;
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
