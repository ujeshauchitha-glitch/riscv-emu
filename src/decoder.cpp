#include "decoder.hpp"

namespace {

// Each of these pulls the immediate out of a 32-bit instruction word according
// to one format's encoding, then sign-extends it to 64 bits.
//
// The B and J immediates look absurdly scrambled. There is a reason: the
// encoding is chosen so that every immediate bit sits in the *same* physical
// instruction bit position across as many formats as possible. That lets a
// hardware decoder wire the immediate bits straight through with almost no
// multiplexing, which is cheap in silicon. It costs us a few lines of shifting
// here; it saves real gates in a real CPU.
//
// Note also that B and J immediates encode a *multiple of two* — bit 0 is
// always zero and is not stored — because branch and jump targets are always
// at least 2-byte aligned. That is why they cover 13 and 21 bits of range using
// only 12 and 20 stored bits.

i64 imm_i(u32 raw) {
    return sign_extend((raw >> 20) & 0xfff, 12);
}

i64 imm_s(u32 raw) {
    const u64 bits = (static_cast<u64>((raw >> 25) & 0x7f) << 5) |
                     static_cast<u64>((raw >> 7) & 0x1f);
    return sign_extend(bits, 12);
}

i64 imm_b(u32 raw) {
    const u64 bits = (static_cast<u64>((raw >> 31) & 0x1) << 12) |
                     (static_cast<u64>((raw >> 7) & 0x1) << 11) |
                     (static_cast<u64>((raw >> 25) & 0x3f) << 5) |
                     (static_cast<u64>((raw >> 8) & 0xf) << 1);
    return sign_extend(bits, 13);
}

i64 imm_u(u32 raw) {
    // The upper 20 bits, already in place, with the low 12 bits zero. The
    // result is sign-extended from bit 31 — so LUI with a large immediate
    // produces a negative 64-bit value, which is what the spec requires.
    return static_cast<i64>(static_cast<i32>(raw & 0xffff'f000));
}

i64 imm_j(u32 raw) {
    const u64 bits = (static_cast<u64>((raw >> 31) & 0x1) << 20) |
                     (static_cast<u64>((raw >> 12) & 0xff) << 12) |
                     (static_cast<u64>((raw >> 20) & 0x1) << 11) |
                     (static_cast<u64>((raw >> 21) & 0x3ff) << 1);
    return sign_extend(bits, 21);
}

}  // namespace

Format format_for_opcode(u32 opcode) {
    switch (opcode) {
        case opcodes::LOAD:
        case opcodes::MISC_MEM:
        case opcodes::OP_IMM:
        case opcodes::OP_IMM_32:
        case opcodes::JALR:
        case opcodes::SYSTEM:
            return Format::I;

        case opcodes::STORE:
            return Format::S;

        case opcodes::BRANCH:
            return Format::B;

        case opcodes::AUIPC:
        case opcodes::LUI:
            return Format::U;

        case opcodes::JAL:
            return Format::J;

        case opcodes::OP:
        case opcodes::OP_32:
        case opcodes::AMO:
            return Format::R;

        default:
            return Format::Unknown;
    }
}

DecodedInst decode(u32 raw) {
    DecodedInst inst;
    inst.raw = raw;

    // These four fields sit in fixed positions in every format that has them,
    // so they can be extracted unconditionally. (A format that lacks a given
    // field simply ignores the bits we pulled out.)
    inst.opcode = raw & 0x7f;
    inst.rd     = (raw >> 7) & 0x1f;
    inst.funct3 = (raw >> 12) & 0x07;
    inst.rs1    = (raw >> 15) & 0x1f;
    inst.rs2    = (raw >> 20) & 0x1f;
    inst.funct7 = (raw >> 25) & 0x7f;

    inst.fmt = format_for_opcode(inst.opcode);

    // The immediate, in contrast, depends entirely on the format.
    switch (inst.fmt) {
        case Format::I: inst.imm = imm_i(raw); break;
        case Format::S: inst.imm = imm_s(raw); break;
        case Format::B: inst.imm = imm_b(raw); break;
        case Format::U: inst.imm = imm_u(raw); break;
        case Format::J: inst.imm = imm_j(raw); break;
        case Format::R:
        case Format::Unknown:
            inst.imm = 0;
            break;
    }

    return inst;
}

const char* mnemonic(const DecodedInst& inst) {
    switch (inst.opcode) {
        case opcodes::LUI:   return "lui";
        case opcodes::AUIPC: return "auipc";
        case opcodes::JAL:   return "jal";
        case opcodes::JALR:  return "jalr";

        case opcodes::BRANCH:
            switch (inst.funct3) {
                case 0x0: return "beq";
                case 0x1: return "bne";
                case 0x4: return "blt";
                case 0x5: return "bge";
                case 0x6: return "bltu";
                case 0x7: return "bgeu";
                default:  return "unimp";
            }

        case opcodes::LOAD:
            switch (inst.funct3) {
                case 0x0: return "lb";
                case 0x1: return "lh";
                case 0x2: return "lw";
                case 0x3: return "ld";
                case 0x4: return "lbu";
                case 0x5: return "lhu";
                case 0x6: return "lwu";
                default:  return "unimp";
            }

        case opcodes::STORE:
            switch (inst.funct3) {
                case 0x0: return "sb";
                case 0x1: return "sh";
                case 0x2: return "sw";
                case 0x3: return "sd";
                default:  return "unimp";
            }

        case opcodes::OP_IMM:
            switch (inst.funct3) {
                case 0x0: return "addi";
                case 0x1: return "slli";
                case 0x2: return "slti";
                case 0x3: return "sltiu";
                case 0x4: return "xori";
                case 0x5: return (inst.funct6() == 0x10) ? "srai" : "srli";
                case 0x6: return "ori";
                case 0x7: return "andi";
                default:  return "unimp";
            }

        case opcodes::OP_IMM_32:
            switch (inst.funct3) {
                case 0x0: return "addiw";
                case 0x1: return "slliw";
                case 0x5: return (inst.funct7 == 0x20) ? "sraiw" : "srliw";
                default:  return "unimp";
            }

        case opcodes::OP:
            if (inst.funct7 == 0x01) {  // M extension
                switch (inst.funct3) {
                    case 0x0: return "mul";
                    case 0x1: return "mulh";
                    case 0x2: return "mulhsu";
                    case 0x3: return "mulhu";
                    case 0x4: return "div";
                    case 0x5: return "divu";
                    case 0x6: return "rem";
                    case 0x7: return "remu";
                    default:  return "unimp";
                }
            }
            switch (inst.funct3) {
                case 0x0: return (inst.funct7 == 0x20) ? "sub" : "add";
                case 0x1: return "sll";
                case 0x2: return "slt";
                case 0x3: return "sltu";
                case 0x4: return "xor";
                case 0x5: return (inst.funct7 == 0x20) ? "sra" : "srl";
                case 0x6: return "or";
                case 0x7: return "and";
                default:  return "unimp";
            }

        case opcodes::OP_32:
            if (inst.funct7 == 0x01) {  // M extension, 32-bit forms
                switch (inst.funct3) {
                    case 0x0: return "mulw";
                    case 0x4: return "divw";
                    case 0x5: return "divuw";
                    case 0x6: return "remw";
                    case 0x7: return "remuw";
                    default:  return "unimp";
                }
            }
            switch (inst.funct3) {
                case 0x0: return (inst.funct7 == 0x20) ? "subw" : "addw";
                case 0x1: return "sllw";
                case 0x5: return (inst.funct7 == 0x20) ? "sraw" : "srlw";
                default:  return "unimp";
            }

        case opcodes::MISC_MEM:
            return (inst.funct3 == 0x1) ? "fence.i" : "fence";

        case opcodes::AMO: {
            // funct5 (bits 31:27) picks the operation; funct3 picks the width.
            const bool w = (inst.funct3 == 0x2);
            switch ((inst.funct7 >> 2) & 0x1f) {
                case 0x00: return w ? "amoadd.w"  : "amoadd.d";
                case 0x01: return w ? "amoswap.w" : "amoswap.d";
                case 0x02: return w ? "lr.w"      : "lr.d";
                case 0x03: return w ? "sc.w"      : "sc.d";
                case 0x04: return w ? "amoxor.w"  : "amoxor.d";
                case 0x08: return w ? "amoor.w"   : "amoor.d";
                case 0x0c: return w ? "amoand.w"  : "amoand.d";
                case 0x10: return w ? "amomin.w"  : "amomin.d";
                case 0x14: return w ? "amomax.w"  : "amomax.d";
                case 0x18: return w ? "amominu.w" : "amominu.d";
                case 0x1c: return w ? "amomaxu.w" : "amomaxu.d";
                default:   return "unimp";
            }
        }

        case opcodes::SYSTEM:
            switch (inst.funct3) {
                case 0x0:
                    switch (inst.imm & 0xfff) {
                        case 0x000: return "ecall";
                        case 0x001: return "ebreak";
                        case 0x102: return "sret";    // phase 6
                        case 0x105: return "wfi";
                        case 0x302: return "mret";
                        default:
                            return (inst.funct7 == 0x09) ? "sfence.vma" : "unimp";
                    }
                case 0x1: return "csrrw";
                case 0x2: return "csrrs";
                case 0x3: return "csrrc";
                case 0x5: return "csrrwi";
                case 0x6: return "csrrsi";
                case 0x7: return "csrrci";
                default:  return "unimp";
            }

        default:
            return "unimp";
    }
}
