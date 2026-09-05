#pragma once

#include "types.hpp"

// ---------------------------------------------------------------------------
// Instruction decoding.
//
// RISC-V has six instruction formats. Every 32-bit instruction is one of them,
// and the format determines which fields are present and — critically — how the
// immediate is assembled:
//
//   R  funct7 | rs2 | rs1 | funct3 | rd     | opcode    no immediate
//   I  imm[11:0]    | rs1 | funct3 | rd     | opcode    12-bit signed
//   S  imm[11:5]|rs2| rs1 | funct3 |imm[4:0]| opcode    12-bit signed, split
//   B  imm[12|10:5] | ... |imm[4:1|11]      | opcode    13-bit signed, scrambled
//   U  imm[31:12]                  | rd     | opcode    upper 20 bits
//   J  imm[20|10:1|11|19:12]       | rd     | opcode    21-bit signed, scrambled
//
// The previous implementation extracted an I-type immediate unconditionally,
// before it knew the format, and passed it to every instruction. That is only
// correct for I-type; for S/B/U/J it produces garbage. Selecting the immediate
// by format is the whole job of this file.
//
// The other structural fix: the decoded result is a struct carrying *all* the
// fields. The old execute() took (opcode, rd, rs1, imm) — no rs2, no funct3, no
// funct7 — which made it impossible to express even a single R-type or store
// instruction. Nothing beyond ADDI could be added until this changed.
// ---------------------------------------------------------------------------

enum class Format {
    R,
    I,
    S,
    B,
    U,
    J,
    Unknown,  // opcode we do not recognise; the CPU raises IllegalInstruction
};

// RISC-V major opcodes (bits [6:0]). Named after the spec's own opcode-map
// mnemonics so they can be looked up directly in the manual.
namespace opcodes {
constexpr u32 LOAD      = 0x03;
constexpr u32 LOAD_FP   = 0x07;  // F/D: FLW, FLD
constexpr u32 MISC_MEM  = 0x0f;  // FENCE, FENCE.I
constexpr u32 OP_IMM    = 0x13;  // ADDI, SLTI, XORI, ORI, ANDI, SLLI, SRLI, SRAI
constexpr u32 AUIPC     = 0x17;
constexpr u32 OP_IMM_32 = 0x1b;  // ADDIW, SLLIW, SRLIW, SRAIW
constexpr u32 STORE     = 0x23;
constexpr u32 STORE_FP  = 0x27;  // F/D: FSW, FSD
constexpr u32 MADD      = 0x43;  // F/D: fused multiply-add, four variants
constexpr u32 MSUB      = 0x47;
constexpr u32 NMSUB     = 0x4b;
constexpr u32 NMADD     = 0x4f;
constexpr u32 OP_FP     = 0x53;  // F/D: all other floating-point operations
constexpr u32 AMO       = 0x2f;  // A extension
constexpr u32 OP        = 0x33;  // register-register ALU, plus M extension
constexpr u32 LUI       = 0x37;
constexpr u32 OP_32     = 0x3b;  // ADDW, SUBW, SLLW, SRLW, SRAW, and M *W ops
constexpr u32 BRANCH    = 0x63;
constexpr u32 JALR      = 0x67;
constexpr u32 JAL       = 0x6f;
constexpr u32 SYSTEM    = 0x73;  // ECALL, EBREAK, CSR ops, MRET/SRET/WFI
}  // namespace opcodes

struct DecodedInst {
    // The 32-bit instruction that is executed. For a compressed instruction
    // this is its *expansion* - see decompress() below - so every execute path
    // downstream of the decoder works on one uniform 32-bit encoding and knows
    // nothing about the C extension.
    u32    raw    = 0;

    // What was actually in memory: the same value as `raw` for a normal
    // instruction, or the 16-bit halfword for a compressed one. This is what
    // belongs in mtval on an illegal-instruction trap, and what the tracer
    // should show - reporting the expansion would name an instruction the
    // program does not contain.
    u32    encoded = 0;

    // 2 for a compressed instruction, 4 otherwise. The PC advances by this.
    u32    length = 4;

    u32    opcode = 0;
    u32    rd     = 0;
    u32    rs1    = 0;
    u32    rs2    = 0;
    u32    funct3 = 0;
    u32    funct7 = 0;
    i64    imm    = 0;
    Format fmt    = Format::Unknown;

    // RV64 shift instructions use a 6-bit shift amount, which steals a bit from
    // what would be funct7 on RV32. The remaining 6 bits (inst[31:26]) select
    // between the logical and arithmetic forms, so shifts must be dispatched on
    // funct6, not funct7. Getting this wrong makes SRAI decode as SRLI.
    u32 funct6() const { return (raw >> 26) & 0x3f; }
    u32 shamt6() const { return (raw >> 20) & 0x3f; }  // RV64 shifts: 6 bits
    u32 shamt5() const { return (raw >> 20) & 0x1f; }  // *W shifts:   5 bits
};

// Decode a 32-bit instruction word. Never fails: an unrecognised opcode yields
// Format::Unknown and the CPU turns that into an IllegalInstruction trap.
DecodedInst decode(u32 raw);

// Decode an instruction that may be compressed, given the halfword at the PC
// and (for a 32-bit instruction) the one after it. `length` on the result says
// how many bytes were consumed, so the caller knows whether it needed `high`.
DecodedInst decode16(u16 low, u16 high);

// True if the halfword at a PC starts a 32-bit instruction rather than a
// compressed one. RISC-V marks instruction length in the low bits: a value with
// bits [1:0] == 11 is 32 bits or longer, anything else is a 16-bit compressed
// instruction. That is why C.* encodings can never begin with those two bits
// set, and why the two lengths can be mixed freely without any mode bit.
constexpr bool is_32bit_instruction(u16 half) { return (half & 0x3) == 0x3; }

// ---------------------------------------------------------------------------
// The C (compressed) extension.
//
// Every compressed instruction is *defined by the spec* as being equivalent to
// exactly one 32-bit base instruction - C.ADDI4SPN is an ADDI, C.JR is a JALR,
// C.MV is an ADD with x0. So the whole extension can be implemented as a
// translation from 16 bits to 32, with the existing decoder and every existing
// execute path left completely untouched.
//
// That is worth doing for more than brevity. A parallel 16-bit execute path
// would be a second implementation of instructions that already work, and every
// bug fixed in one would have to be found again in the other. Here there is
// only ever one implementation of ADDI.
//
// Returns 0 for an encoding that is not a valid compressed instruction. Zero is
// permanently illegal in RISC-V - deliberately, so that a jump into zeroed
// memory faults immediately - so the caller needs no separate error channel.
u32 decompress(u16 half);

// Which format does this major opcode use?
Format format_for_opcode(u32 opcode);

// A short human-readable rendering of a decoded instruction, for the tracer.
// Only the instructions this phase implements are named; the rest print as
// "unimp" with their raw bits.
const char* mnemonic(const DecodedInst& inst);
