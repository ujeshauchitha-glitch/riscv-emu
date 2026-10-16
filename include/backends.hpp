#pragma once

#include "types.hpp"

// ---------------------------------------------------------------------------
// Optional execution backends.
//
// Both are null by default, and with them null the CPU behaves exactly as it
// always did - the plain host arithmetic and the plain `regs` array. They exist
// so that a different implementation of the integer register file or of the
// integer ALU (a sliced, fault-injected one, say) can be slotted in underneath
// the unchanged decoder, CSRs, privilege modes and MMU, and compared against
// this core as the reference.
//
// Neither backend owns anything the CPU needs to stay correct. They are
// pointers the CPU borrows; the caller keeps them alive.
// ---------------------------------------------------------------------------

// The integer register file, x1..x31. x0 is hardwired to zero by the CPU
// itself, so a backend is never asked for it and never sees a write to it.
//
// While a backend is installed `Cpu::regs` is not updated and must not be
// relied on; use Cpu::read_reg / write_reg.
class RegFileBackend {
public:
    virtual ~RegFileBackend() = default;
    virtual u64  read(u32 index)                 = 0;
    virtual void write(u32 index, u64 value)     = 0;
};

// Integer ALU operations. Every one takes two 64-bit operands and returns a
// 64-bit result, so one entry point covers the register-register forms, the
// immediate forms (the immediate arrives as `b`, already sign-extended), the
// comparisons that branches use, and data-address generation (Add).
//
// *W operations use the low 32 bits of each operand and return the result
// sign-extended from bit 31. Shift amounts are taken from the low bits of `b`:
// 6 bits for the 64-bit shifts, 5 for the W shifts.
enum class AluOp : u8 {
    Add, Sub, And, Or, Xor,
    Sll, Srl, Sra,
    Slt, Sltu,
    AddW, SubW, SllW, SrlW, SraW,
};

class AluBackend {
public:
    virtual ~AluBackend() = default;
    virtual u64 exec(AluOp op, u64 a, u64 b) = 0;
};

// The plain ALU. This is what the CPU runs when no backend is installed, and it
// is the reference any other AluBackend is checked against.
inline u64 alu_reference(AluOp op, u64 a, u64 b) {
    const auto sx32 = [](u32 v) {
        return static_cast<u64>(static_cast<i64>(static_cast<i32>(v)));
    };
    const u32 a32 = static_cast<u32>(a);
    const u32 b32 = static_cast<u32>(b);
    switch (op) {
        case AluOp::Add:  return a + b;
        case AluOp::Sub:  return a - b;
        case AluOp::And:  return a & b;
        case AluOp::Or:   return a | b;
        case AluOp::Xor:  return a ^ b;
        case AluOp::Sll:  return a << (b & 0x3f);
        case AluOp::Srl:  return a >> (b & 0x3f);
        case AluOp::Sra:  return static_cast<u64>(static_cast<i64>(a) >> (b & 0x3f));
        case AluOp::Slt:  return static_cast<i64>(a) < static_cast<i64>(b) ? 1 : 0;
        case AluOp::Sltu: return a < b ? 1 : 0;
        case AluOp::AddW: return sx32(a32 + b32);
        case AluOp::SubW: return sx32(a32 - b32);
        case AluOp::SllW: return sx32(a32 << (b & 0x1f));
        case AluOp::SrlW: return sx32(a32 >> (b & 0x1f));
        case AluOp::SraW: return sx32(static_cast<u32>(static_cast<i32>(a32) >> (b & 0x1f)));
    }
    return 0;
}
