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
        case opcodes::OP_IMM:
            return execute_op_imm(inst);

        default:
            // Everything not yet implemented traps as an illegal instruction
            // with the offending bits in tval, rather than being skipped.
            return Status::bad(Exception::IllegalInstruction, inst.raw);
    }
}

// ---------------------------------------------------------------------------
// OP-IMM: register-immediate ALU operations.
//
// This is the group the old code got wrong. It matched on opcode 0x13 alone and
// executed ADDI for all of them, so SLTI, SLTIU, XORI, ORI, ANDI, SLLI, SRLI
// and SRAI — eight distinct instructions — all silently performed an addition.
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

    const bool is_shift =
        inst.opcode == opcodes::OP_IMM && (inst.funct3 == 0x1 || inst.funct3 == 0x5);

    switch (inst.fmt) {
        case Format::R:
            reg("rd", inst.rd);
            reg("rs1", inst.rs1);
            reg("rs2", inst.rs2);
            break;
        case Format::I:
            reg("rd", inst.rd);
            reg("rs1", inst.rs1);
            // For shifts the useful field is the shift amount, not the raw
            // 12-bit immediate (which also encodes funct6).
            if (is_shift) {
                os << " shamt=" << inst.shamt6();
            } else {
                os << " imm=" << inst.imm;
            }
            break;
        case Format::S:
        case Format::B:
            reg("rs1", inst.rs1);
            reg("rs2", inst.rs2);
            os << " imm=" << inst.imm;
            break;
        case Format::U:
        case Format::J:
            reg("rd", inst.rd);
            os << " imm=" << inst.imm;
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
