#pragma once

#include "csr.hpp"
#include "types.hpp"

// ---------------------------------------------------------------------------
// The F and D extensions: single- and double-precision floating point.
//
// Thirty-two registers, separate from the integer file, each 64 bits wide.
// Doubles live in them directly; singles are **NaN-boxed** - stored in the low
// 32 bits with all of the upper 32 set to one.
//
// NaN boxing is worth understanding, because it is not an optimisation but a
// correctness rule. The two precisions share one register file, so something
// has to happen when a program stores a float and reads it as a double. The
// spec's answer: a 32-bit value in a 64-bit register is only valid if the upper
// half is all ones, which happens to be the encoding of a quiet NaN. Read that
// register as a double and you get a NaN - a value that says "this is not a
// double" and propagates through arithmetic instead of silently producing a
// plausible-looking wrong answer.
//
// So every instruction that writes a single must box it, and every instruction
// that reads one must check the box and substitute a canonical NaN if it is not
// there. Skipping either turns a type error into a wrong number.
//
// **Arithmetic is done in host doubles and floats**, which are IEEE-754 on
// every platform this will run on - the same standard RISC-V specifies. What
// the host does not give us for free is the accrued exception flags and the
// rounding mode, so those are handled explicitly here.
// ---------------------------------------------------------------------------

// The canonical quiet NaNs the spec requires an instruction to produce when its
// result is NaN. Note that RISC-V has exactly one canonical NaN per precision,
// and never propagates the payload of an input NaN - unlike most hardware.
constexpr u32 CANONICAL_NAN_F32 = 0x7fc00000u;
constexpr u64 CANONICAL_NAN_F64 = 0x7ff8000000000000ull;

// Wrap a 32-bit single for storage in a 64-bit register.
constexpr u64 nan_box(u32 single) { return 0xffffffff00000000ull | single; }

// Unwrap a register as a single. A value that is not properly boxed is not a
// float at all, and the spec says it must read as the canonical NaN.
constexpr u32 nan_unbox(u64 reg) {
    return (reg >> 32) == 0xffffffffull ? static_cast<u32>(reg)
                                        : CANONICAL_NAN_F32;
}

// Reinterpret between bit patterns and host floating-point types. These are
// bit_cast, spelled out because the project targets C++20 without assuming
// <bit> everywhere.
float  bits_to_f32(u32 bits);
u32    f32_to_bits(float value);
double bits_to_f64(u64 bits);
u64    f64_to_bits(double value);

// The five classes fclass reports, one bit each. Software uses this instead of
// comparisons because it distinguishes -0 from +0 and signalling from quiet
// NaNs, which no comparison can.
constexpr u64 FCLASS_NEG_INF     = 1 << 0;
constexpr u64 FCLASS_NEG_NORMAL  = 1 << 1;
constexpr u64 FCLASS_NEG_SUBNORM = 1 << 2;
constexpr u64 FCLASS_NEG_ZERO    = 1 << 3;
constexpr u64 FCLASS_POS_ZERO    = 1 << 4;
constexpr u64 FCLASS_POS_SUBNORM = 1 << 5;
constexpr u64 FCLASS_POS_NORMAL  = 1 << 6;
constexpr u64 FCLASS_POS_INF     = 1 << 7;
constexpr u64 FCLASS_SIGNALLING  = 1 << 8;
constexpr u64 FCLASS_QUIET_NAN   = 1 << 9;

u64 fclass_f32(u32 bits);
u64 fclass_f64(u64 bits);

// Is this a signalling NaN? Signalling NaNs raise the invalid-operation flag
// wherever they appear; quiet ones mostly propagate silently.
bool is_snan_f32(u32 bits);
bool is_snan_f64(u64 bits);

// Set the host's rounding mode for the duration of one operation.
//
// RISC-V carries a rounding mode in two places: each instruction has an `rm`
// field, and the fcsr has a default. An rm of 7 (DYN) means "use the default".
// An rm of 5 or 6 is reserved and makes the instruction illegal, which is why
// this returns a failure rather than picking something.
class RoundingScope {
public:
    // `inst_rm` is the instruction's rm field; `frm` the fcsr default.
    RoundingScope(u32 inst_rm, u32 frm);
    ~RoundingScope();

    // False if the resolved mode is reserved - the caller must raise an
    // illegal-instruction trap and must not use the result.
    bool valid() const { return valid_; }

private:
    bool valid_ = true;
    int  saved_ = 0;
    bool changed_ = false;
};

// Read the host's IEEE exception flags raised since they were last cleared, as
// RISC-V fflags bits, and clear them. Called after each arithmetic operation.
u64 take_host_exceptions();
void clear_host_exceptions();
