#include "bitmanip.hpp"
#include "cpu.hpp"

// ---------------------------------------------------------------------------
// Zba, Zbb, Zbc, Zbs and Zicond.
//
// None of these adds an opcode. Each is a set of new (funct7, funct3) or
// (funct12, funct3) combinations inside the three ALU opcodes the base ISA
// already uses: OP, OP-32, OP-IMM and OP-IMM-32. So they are handled as a
// pre-check in the four existing dispatchers: if the encoding belongs to one of
// these extensions it is executed here, and otherwise the dispatcher carries on
// exactly as before. That keeps the base-ISA paths - and the "unrecognised
// encodings must trap" rule - untouched.
//
// Each function returns true if it recognised the encoding (and stored the
// outcome in `out`) and false if the caller should treat it as it always did.
//
// What each instruction computes is in bitmanip.hpp; this file is only the map
// from encoding to operation.
// ---------------------------------------------------------------------------

namespace {

// A (funct7, funct3) pair packed into one switch key.
constexpr u32 key(u32 funct7, u32 funct3) { return (funct7 << 3) | funct3; }

constexpr u64 bit(u64 index) { return 1ull << (index & 63); }

}  // namespace

// --- OP: register-register ---------------------------------------------------------

bool Cpu::execute_zb_op(const DecodedInst& inst, Status& out) {
    const u64 a = read_reg(inst.rs1);
    const u64 b = read_reg(inst.rs2);
    u64 r;

    switch (key(inst.funct7, inst.funct3)) {
        // Zba: shift-and-add, the address calculation for an array of 2-, 4- or
        // 8-byte elements in one instruction.
        case key(0x10, 2): r = (a << 1) + b; break;   // SH1ADD
        case key(0x10, 4): r = (a << 2) + b; break;   // SH2ADD
        case key(0x10, 6): r = (a << 3) + b; break;   // SH3ADD

        // Zbb: logic with a negated operand.
        case key(0x20, 7): r = a & ~b; break;         // ANDN
        case key(0x20, 6): r = a | ~b; break;         // ORN
        case key(0x20, 4): r = ~(a ^ b); break;       // XNOR

        // Zbb: minimum and maximum, signed and unsigned.
        case key(0x05, 4): r = static_cast<i64>(a) < static_cast<i64>(b) ? a : b; break;   // MIN
        case key(0x05, 5): r = a < b ? a : b; break;                                       // MINU
        case key(0x05, 6): r = static_cast<i64>(a) > static_cast<i64>(b) ? a : b; break;   // MAX
        case key(0x05, 7): r = a > b ? a : b; break;                                       // MAXU

        // Zbb: rotates. The amount is rs2 modulo 64.
        case key(0x30, 1): r = bitmanip::rol64(a, static_cast<unsigned>(b)); break;        // ROL
        case key(0x30, 5): r = bitmanip::ror64(a, static_cast<unsigned>(b)); break;        // ROR

        // Zbs: single-bit operations. The bit index is rs2 modulo 64.
        case key(0x24, 1): r = a & ~bit(b); break;    // BCLR
        case key(0x24, 5): r = (a >> (b & 63)) & 1; break;   // BEXT
        case key(0x34, 1): r = a ^ bit(b); break;     // BINV
        case key(0x14, 1): r = a | bit(b); break;     // BSET

        // Zbc: carry-less multiply.
        case key(0x05, 1): r = bitmanip::clmul(a, b); break;    // CLMUL
        case key(0x05, 2): r = bitmanip::clmulr(a, b); break;   // CLMULR
        case key(0x05, 3): r = bitmanip::clmulh(a, b); break;   // CLMULH

        // Zicond: conditional zero. These build a branch-free select:
        //   rd = cond ? x : y   is   czero.nez(y, cond) | czero.eqz(x, cond)
        case key(0x07, 5): r = (b == 0) ? 0 : a; break;   // CZERO.EQZ
        case key(0x07, 7): r = (b != 0) ? 0 : a; break;   // CZERO.NEZ

        default:
            return false;
    }

    write_reg(inst.rd, r);
    out = Status::good();
    return true;
}

// --- OP-32: register-register on 32 bits --------------------------------------------

bool Cpu::execute_zb_op_32(const DecodedInst& inst, Status& out) {
    const u64 a = read_reg(inst.rs1);
    const u64 b = read_reg(inst.rs2);
    u64 r;

    switch (key(inst.funct7, inst.funct3)) {
        // Zba: the .uw forms zero-extend the low 32 bits of rs1 first, so an index
        // held in a 32-bit unsigned variable can be scaled and added in one go.
        case key(0x04, 0): r = bitmanip::zext32(a) + b; break;          // ADD.UW
        case key(0x10, 2): r = (bitmanip::zext32(a) << 1) + b; break;   // SH1ADD.UW
        case key(0x10, 4): r = (bitmanip::zext32(a) << 2) + b; break;   // SH2ADD.UW
        case key(0x10, 6): r = (bitmanip::zext32(a) << 3) + b; break;   // SH3ADD.UW

        // Zbb: ZEXT.H lives in OP-32 on RV64 and is a special case (rs2 = 0) of
        // an encoding that, with other rs2 values, belongs to Zbkb's PACK - not
        // implemented, so those are illegal.
        case key(0x04, 4):
            if (inst.rs2 != 0) { out = Status::bad(Exception::IllegalInstruction, inst.encoded); return true; }
            r = a & 0xffff;
            break;

        // Zbb: 32-bit rotates, result sign-extended like every *W instruction.
        case key(0x30, 1): r = bitmanip::sext(bitmanip::rol32(static_cast<u32>(a), static_cast<unsigned>(b)), 32); break;   // ROLW
        case key(0x30, 5): r = bitmanip::sext(bitmanip::ror32(static_cast<u32>(a), static_cast<unsigned>(b)), 32); break;   // RORW

        default:
            return false;
    }

    write_reg(inst.rd, r);
    out = Status::good();
    return true;
}

// --- OP-IMM: register-immediate ---------------------------------------------------------

bool Cpu::execute_zb_op_imm(const DecodedInst& inst, Status& out) {
    const u64 a = read_reg(inst.rs1);
    const u32 imm12 = (inst.raw >> 20) & 0xfff;     // the whole field, for the unary operations
    const u32 f6 = inst.funct6();                    // bits 31:26, for the shift-like forms
    const u32 shamt = inst.shamt6();
    u64 r;

    if (inst.funct3 == 1) {
        switch (imm12) {
            case 0x600: r = bitmanip::clz64(a); break;                     // CLZ
            case 0x601: r = bitmanip::ctz64(a); break;                     // CTZ
            case 0x602: r = bitmanip::cpop64(a); break;                    // CPOP
            case 0x604: r = bitmanip::sext(a, 8); break;                   // SEXT.B
            case 0x605: r = bitmanip::sext(a, 16); break;                  // SEXT.H
            default:
                switch (f6) {
                    case 0x0a: r = a | bit(shamt); break;                  // BSETI
                    case 0x12: r = a & ~bit(shamt); break;                 // BCLRI
                    case 0x1a: r = a ^ bit(shamt); break;                  // BINVI
                    default: return false;
                }
        }
    } else if (inst.funct3 == 5) {
        switch (imm12) {
            case 0x6b8: r = bitmanip::rev8(a); break;                      // REV8
            case 0x287: r = bitmanip::orc_b(a); break;                     // ORC.B
            default:
                switch (f6) {
                    case 0x18: r = bitmanip::ror64(a, shamt); break;       // RORI
                    case 0x12: r = (a >> shamt) & 1; break;                // BEXTI
                    default: return false;
                }
        }
    } else {
        return false;
    }

    write_reg(inst.rd, r);
    out = Status::good();
    return true;
}

// --- OP-IMM-32 ------------------------------------------------------------------------------

bool Cpu::execute_zb_op_imm_32(const DecodedInst& inst, Status& out) {
    const u64 a = read_reg(inst.rs1);
    const u32 imm12 = (inst.raw >> 20) & 0xfff;
    u64 r;

    if (inst.funct3 == 1) {
        switch (imm12) {
            case 0x600: r = bitmanip::clz32(a); break;                     // CLZW
            case 0x601: r = bitmanip::ctz32(a); break;                     // CTZW
            case 0x602: r = bitmanip::cpop32(a); break;                    // CPOPW
            default:
                // SLLI.UW: zero-extend the low 32 bits, then shift by a *6-bit*
                // amount - unlike SLLIW, whose amount is 5 bits, because the result
                // is a full 64-bit value that can usefully be shifted that far.
                if (inst.funct6() == 0x02) { r = bitmanip::zext32(a) << inst.shamt6(); break; }
                return false;
        }
    } else if (inst.funct3 == 5 && inst.funct7 == 0x30) {
        r = bitmanip::sext(bitmanip::ror32(static_cast<u32>(a), inst.shamt5()), 32);   // RORIW
    } else {
        return false;
    }

    write_reg(inst.rd, r);
    out = Status::good();
    return true;
}
