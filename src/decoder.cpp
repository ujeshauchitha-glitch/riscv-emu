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
        case opcodes::LOAD_FP:
        case opcodes::MISC_MEM:
        case opcodes::OP_IMM:
        case opcodes::OP_IMM_32:
        case opcodes::JALR:
        case opcodes::SYSTEM:
            return Format::I;

        case opcodes::STORE:
        case opcodes::STORE_FP:
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
        // The floating-point operations reuse the R-type field layout. The
        // fused multiply-adds add a fourth register in what would be funct7's
        // top five bits (the spec calls this R4-type), which the CPU reads out
        // of `raw` directly rather than growing the struct for one family.
        case opcodes::OP_FP:
        case opcodes::MADD:
        case opcodes::MSUB:
        case opcodes::NMSUB:
        case opcodes::NMADD:
            return Format::R;

        default:
            return Format::Unknown;
    }
}

DecodedInst decode(u32 raw) {
    DecodedInst inst;
    inst.raw     = raw;
    inst.encoded = raw;
    inst.length  = 4;

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


// ---------------------------------------------------------------------------
// The C extension: expanding 16 bits into 32.
//
// Two things make this fiddly, and both are deliberate design choices in the
// spec rather than accidents.
//
// **The registers are three bits wide.** A compressed instruction has no room
// for two or three 5-bit register fields, so most of them can only name eight
// registers - x8 through x15. Those are the ones a compiler uses most (the
// saved registers s0/s1 and the first argument/temporary registers a0-a5), so
// in practice the restriction costs little.
//
// **The immediates are scattered.** Bits of an offset appear in whatever
// corners of the 16-bit word were free, in an order that looks random. It is
// not: the layout is chosen so that each bit lands in the same *physical wire
// position* as it occupies in the 32-bit form wherever possible, which makes
// the expansion cheap in hardware. It makes it verbose in software, which is
// why every one of the encodings below is written out explicitly with the
// spec's own bit-range notation in a comment. Guessing here produces an
// emulator that runs most programs and corrupts a few.
// ---------------------------------------------------------------------------
namespace {

// The 3-bit register fields name x8..x15.
constexpr u32 creg(u32 three_bits) { return three_bits + 8; }

// Assemble the six 32-bit formats from their parts. Writing these once and
// building every expansion out of them keeps each case below to a line or two
// of immediate arithmetic, which is the part that actually differs.
constexpr u32 enc_r(u32 opcode, u32 rd, u32 funct3, u32 rs1, u32 rs2, u32 funct7) {
    return (funct7 << 25) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) | (rd << 7) | opcode;
}
constexpr u32 enc_i(u32 opcode, u32 rd, u32 funct3, u32 rs1, u32 imm) {
    return ((imm & 0xfff) << 20) | (rs1 << 15) | (funct3 << 12) | (rd << 7) | opcode;
}
constexpr u32 enc_s(u32 opcode, u32 funct3, u32 rs1, u32 rs2, u32 imm) {
    return ((imm & 0xfe0) << 20) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) |
           ((imm & 0x1f) << 7) | opcode;
}
constexpr u32 enc_b(u32 opcode, u32 funct3, u32 rs1, u32 rs2, u32 imm) {
    return ((imm & 0x1000) << 19) | ((imm & 0x7e0) << 20) | (rs2 << 20) | (rs1 << 15) |
           (funct3 << 12) | ((imm & 0x1e) << 7) | ((imm & 0x800) >> 4) | opcode;
}
constexpr u32 enc_u(u32 opcode, u32 rd, u32 imm) {
    return (imm & 0xfffff000) | (rd << 7) | opcode;
}
constexpr u32 enc_j(u32 opcode, u32 rd, u32 imm) {
    return ((imm & 0x100000) << 11) | ((imm & 0x7fe) << 20) | ((imm & 0x800) << 9) |
           (imm & 0xff000) | (rd << 7) | opcode;
}

// Sign-extend the low `bits` of `value`.
constexpr u32 sx(u32 value, unsigned bits) {
    const u32 sign = 1u << (bits - 1);
    return (value ^ sign) - sign;
}

u32 bits(u16 h, unsigned hi, unsigned lo) {
    return (static_cast<u32>(h) >> lo) & ((1u << (hi - lo + 1)) - 1);
}

}  // namespace

u32 decompress(u16 h) {
    if (h == 0) return 0;   // the all-zero halfword is permanently illegal

    const u32 op     = h & 0x3;
    const u32 funct3 = bits(h, 15, 13);

    // Fields shared by several formats.
    const u32 rd_rs1  = bits(h, 11, 7);    // full 5-bit field
    const u32 rs2     = bits(h, 6, 2);     // full 5-bit field
    const u32 rd_p    = creg(bits(h, 4, 2));    // rd'  / rs2'
    const u32 rs1_p   = creg(bits(h, 9, 7));    // rs1' / rd'

    switch (op) {
    // --- Quadrant 0 ---------------------------------------------------------
    case 0:
        switch (funct3) {
        case 0: {
            // C.ADDI4SPN: addi rd', x2, nzuimm
            //   nzuimm[5:4] = inst[12:11], [9:6] = inst[10:7], [2] = inst[6],
            //   [3] = inst[5].  A zero immediate is the reserved encoding.
            const u32 imm = (bits(h, 10, 7) << 6) | (bits(h, 12, 11) << 4) |
                            (bits(h, 5, 5) << 3) | (bits(h, 6, 6) << 2);
            if (imm == 0) return 0;
            return enc_i(opcodes::OP_IMM, rd_p, 0x0, 2, imm);
        }
        case 2: {
            // C.LW: lw rd', offset(rs1')
            //   offset[5:3] = inst[12:10], [2] = inst[6], [6] = inst[5]
            const u32 imm = (bits(h, 5, 5) << 6) | (bits(h, 12, 10) << 3) |
                            (bits(h, 6, 6) << 2);
            return enc_i(opcodes::LOAD, rd_p, 0x2, rs1_p, imm);
        }
        case 1: {
            // C.FLD: fld rd', offset(rs1').  Same immediate layout as C.LD -
            // both move eight bytes, so both scale their offset the same way.
            const u32 imm = (bits(h, 6, 5) << 6) | (bits(h, 12, 10) << 3);
            return enc_i(opcodes::LOAD_FP, rd_p, 0x3, rs1_p, imm);
        }
        case 3: {
            // C.LD: ld rd', offset(rs1')
            //   offset[5:3] = inst[12:10], [7:6] = inst[6:5]
            const u32 imm = (bits(h, 6, 5) << 6) | (bits(h, 12, 10) << 3);
            return enc_i(opcodes::LOAD, rd_p, 0x3, rs1_p, imm);
        }
        case 5: {
            // C.FSD: fsd rs2', offset(rs1')
            const u32 imm = (bits(h, 6, 5) << 6) | (bits(h, 12, 10) << 3);
            return enc_s(opcodes::STORE_FP, 0x3, rs1_p, rd_p, imm);
        }
        case 6: {
            // C.SW: sw rs2', offset(rs1')
            const u32 imm = (bits(h, 5, 5) << 6) | (bits(h, 12, 10) << 3) |
                            (bits(h, 6, 6) << 2);
            return enc_s(opcodes::STORE, 0x2, rs1_p, rd_p, imm);
        }
        case 7: {
            // C.SD: sd rs2', offset(rs1')
            const u32 imm = (bits(h, 6, 5) << 6) | (bits(h, 12, 10) << 3);
            return enc_s(opcodes::STORE, 0x3, rs1_p, rd_p, imm);
        }
        default:
            // funct3 4 is reserved.
            return 0;
        }

    // --- Quadrant 1 ---------------------------------------------------------
    case 1:
        switch (funct3) {
        case 0: {
            // C.ADDI: addi rd, rd, nzimm.  rd == 0 is C.NOP, which expands to
            // `addi x0, x0, 0` and is a genuine no-op either way.
            const u32 imm = sx((bits(h, 12, 12) << 5) | rs2, 6);
            return enc_i(opcodes::OP_IMM, rd_rs1, 0x0, rd_rs1, imm);
        }
        case 1: {
            // C.ADDIW: addiw rd, rd, imm.  rd == 0 is reserved - unlike C.ADDI
            // there is no harmless reading of it, since ADDIW with rd=x0 would
            // still have to sign-extend something.
            if (rd_rs1 == 0) return 0;
            const u32 imm = sx((bits(h, 12, 12) << 5) | rs2, 6);
            return enc_i(opcodes::OP_IMM_32, rd_rs1, 0x0, rd_rs1, imm);
        }
        case 2: {
            // C.LI: addi rd, x0, imm
            const u32 imm = sx((bits(h, 12, 12) << 5) | rs2, 6);
            return enc_i(opcodes::OP_IMM, rd_rs1, 0x0, 0, imm);
        }
        case 3: {
            if (rd_rs1 == 2) {
                // C.ADDI16SP: addi x2, x2, nzimm
                //   nzimm[9] = inst[12], [4] = inst[6], [6] = inst[5],
                //   [8:7] = inst[4:3], [5] = inst[2]
                const u32 imm = sx((bits(h, 12, 12) << 9) | (bits(h, 4, 3) << 7) |
                                   (bits(h, 5, 5) << 6) | (bits(h, 2, 2) << 5) |
                                   (bits(h, 6, 6) << 4), 10);
                if (imm == 0) return 0;
                return enc_i(opcodes::OP_IMM, 2, 0x0, 2, imm);
            }
            // C.LUI: lui rd, nzimm.  rd must not be x0 or x2, and the immediate
            // must not be zero.
            if (rd_rs1 == 0) return 0;
            const u32 imm = sx((bits(h, 12, 12) << 5) | rs2, 6);
            if (imm == 0) return 0;
            return enc_u(opcodes::LUI, rd_rs1, imm << 12);
        }
        case 4: {
            // MISC-ALU. inst[11:10] selects the group.
            const u32 group = bits(h, 11, 10);
            if (group == 0 || group == 1) {
                // C.SRLI / C.SRAI: shamt[5] = inst[12], shamt[4:0] = inst[6:2].
                // RV64 allows the full 6 bits, so nothing here is reserved.
                const u32 shamt  = (bits(h, 12, 12) << 5) | rs2;
                const u32 funct6 = (group == 0) ? 0x00 : 0x10;
                return (funct6 << 26) | (shamt << 20) | (rs1_p << 15) |
                       (0x5 << 12) | (rs1_p << 7) | opcodes::OP_IMM;
            }
            if (group == 2) {
                // C.ANDI: andi rd', rd', imm
                const u32 imm = sx((bits(h, 12, 12) << 5) | rs2, 6);
                return enc_i(opcodes::OP_IMM, rs1_p, 0x7, rs1_p, imm);
            }
            // group == 3: register-register ALU. inst[12] picks the 64-bit
            // *W forms, inst[6:5] the operation.
            const u32 sel = bits(h, 6, 5);
            if (bits(h, 12, 12) == 0) {
                switch (sel) {
                case 0: return enc_r(opcodes::OP, rs1_p, 0x0, rs1_p, rd_p, 0x20); // C.SUB
                case 1: return enc_r(opcodes::OP, rs1_p, 0x4, rs1_p, rd_p, 0x00); // C.XOR
                case 2: return enc_r(opcodes::OP, rs1_p, 0x6, rs1_p, rd_p, 0x00); // C.OR
                default:return enc_r(opcodes::OP, rs1_p, 0x7, rs1_p, rd_p, 0x00); // C.AND
                }
            }
            switch (sel) {
            case 0: return enc_r(opcodes::OP_32, rs1_p, 0x0, rs1_p, rd_p, 0x20); // C.SUBW
            case 1: return enc_r(opcodes::OP_32, rs1_p, 0x0, rs1_p, rd_p, 0x00); // C.ADDW
            default: return 0;   // reserved
            }
        }
        case 5: {
            // C.J: jal x0, offset
            //   offset[11] = inst[12], [4] = inst[11], [9:8] = inst[10:9],
            //   [10] = inst[8], [6] = inst[7], [7] = inst[6],
            //   [3:1] = inst[5:3], [5] = inst[2]
            const u32 imm = sx((bits(h, 12, 12) << 11) | (bits(h, 8, 8) << 10) |
                               (bits(h, 10, 9) << 8) | (bits(h, 6, 6) << 7) |
                               (bits(h, 7, 7) << 6) | (bits(h, 2, 2) << 5) |
                               (bits(h, 11, 11) << 4) | (bits(h, 5, 3) << 1), 12);
            return enc_j(opcodes::JAL, 0, imm);
        }
        case 6:
        case 7: {
            // C.BEQZ / C.BNEZ: compare rs1' against x0.
            //   offset[8] = inst[12], [4:3] = inst[11:10], [7:6] = inst[6:5],
            //   [2:1] = inst[4:3], [5] = inst[2]
            const u32 imm = sx((bits(h, 12, 12) << 8) | (bits(h, 6, 5) << 6) |
                               (bits(h, 2, 2) << 5) | (bits(h, 11, 10) << 3) |
                               (bits(h, 4, 3) << 1), 9);
            return enc_b(opcodes::BRANCH, (funct3 == 6) ? 0x0 : 0x1, rs1_p, 0, imm);
        }
        default:
            return 0;
        }

    // --- Quadrant 2 ---------------------------------------------------------
    case 2:
        switch (funct3) {
        case 0: {
            // C.SLLI: slli rd, rd, shamt
            if (rd_rs1 == 0) return 0;
            const u32 shamt = (bits(h, 12, 12) << 5) | rs2;
            return (shamt << 20) | (rd_rs1 << 15) | (0x1 << 12) | (rd_rs1 << 7) |
                   opcodes::OP_IMM;
        }
        case 2: {
            // C.LWSP: lw rd, offset(x2)
            //   offset[5] = inst[12], [4:2] = inst[6:4], [7:6] = inst[3:2]
            if (rd_rs1 == 0) return 0;
            const u32 imm = (bits(h, 3, 2) << 6) | (bits(h, 12, 12) << 5) |
                            (bits(h, 6, 4) << 2);
            return enc_i(opcodes::LOAD, rd_rs1, 0x2, 2, imm);
        }
        case 1: {
            // C.FLDSP: fld rd, offset(x2).  Same layout as C.LDSP, but with no
            // reserved encoding: f0 is an ordinary register, so rd == 0 is
            // perfectly legal here where `c.ldsp x0` is not.
            const u32 imm = (bits(h, 4, 2) << 6) | (bits(h, 12, 12) << 5) |
                            (bits(h, 6, 5) << 3);
            return enc_i(opcodes::LOAD_FP, rd_rs1, 0x3, 2, imm);
        }
        case 3: {
            // C.LDSP: ld rd, offset(x2)
            //   offset[5] = inst[12], [4:3] = inst[6:5], [8:6] = inst[4:2]
            if (rd_rs1 == 0) return 0;
            const u32 imm = (bits(h, 4, 2) << 6) | (bits(h, 12, 12) << 5) |
                            (bits(h, 6, 5) << 3);
            return enc_i(opcodes::LOAD, rd_rs1, 0x3, 2, imm);
        }
        case 5: {
            // C.FSDSP: fsd rs2, offset(x2).  Same layout as C.SDSP.
            const u32 imm = (bits(h, 9, 7) << 6) | (bits(h, 12, 10) << 3);
            return enc_s(opcodes::STORE_FP, 0x3, 2, rs2, imm);
        }
        case 4: {
            if (bits(h, 12, 12) == 0) {
                // C.JR: jalr x0, 0(rs1)   -   C.MV: add rd, x0, rs2
                if (rs2 == 0) {
                    if (rd_rs1 == 0) return 0;   // reserved
                    return enc_i(opcodes::JALR, 0, 0x0, rd_rs1, 0);
                }
                return enc_r(opcodes::OP, rd_rs1, 0x0, 0, rs2, 0x00);
            }
            // C.EBREAK / C.JALR / C.ADD
            if (rs2 == 0) {
                if (rd_rs1 == 0) return 0x00100073;              // C.EBREAK
                return enc_i(opcodes::JALR, 1, 0x0, rd_rs1, 0);  // C.JALR
            }
            return enc_r(opcodes::OP, rd_rs1, 0x0, rd_rs1, rs2, 0x00);  // C.ADD
        }
        case 6: {
            // C.SWSP: sw rs2, offset(x2)
            //   offset[5:2] = inst[12:9], [7:6] = inst[8:7]
            const u32 imm = (bits(h, 8, 7) << 6) | (bits(h, 12, 9) << 2);
            return enc_s(opcodes::STORE, 0x2, 2, rs2, imm);
        }
        case 7: {
            // C.SDSP: sd rs2, offset(x2)
            //   offset[5:3] = inst[12:10], [8:6] = inst[9:7]
            const u32 imm = (bits(h, 9, 7) << 6) | (bits(h, 12, 10) << 3);
            return enc_s(opcodes::STORE, 0x3, 2, rs2, imm);
        }
        default:
            return 0;
        }

    default:
        // op == 3 means this was not a compressed instruction at all; the
        // caller is responsible for checking that before calling here.
        return 0;
    }
}

DecodedInst decode16(u16 low, u16 high) {
    if (is_32bit_instruction(low)) {
        const u32 raw = (static_cast<u32>(high) << 16) | low;
        DecodedInst inst = decode(raw);
        inst.encoded = raw;
        inst.length  = 4;
        return inst;
    }

    // A compressed instruction decodes as its expansion, so everything
    // downstream is unchanged - but `encoded` and `length` remember that the
    // program contained sixteen bits, not thirty-two.
    DecodedInst inst = decode(decompress(low));
    inst.encoded = low;
    inst.length  = 2;
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
