#pragma once

#include <bit>

#include "types.hpp"

// ---------------------------------------------------------------------------
// The arithmetic behind the bit-manipulation (Zba, Zbb, Zbc, Zbs) and
// conditional-zero (Zicond) extensions.
//
// These are pure functions of their operands, so they are separate from the CPU:
// the CPU decides *which* operation an instruction names, and this file says what
// the operation computes. Keeping them apart is also what lets the tests check
// the encodings and the arithmetic independently.
//
// Why bother with these at all? A kernel or a C library run on a core that has
// them uses them constantly - rotates and bit counts in hashing, `sh3add` to
// index arrays of 8-byte elements, `clz` in allocators - and a distribution
// built for such a core will not run on one that lacks them.
// ---------------------------------------------------------------------------

namespace bitmanip {

constexpr u64 zext32(u64 x) { return x & 0xffff'ffffull; }

// Sign-extend the low `bits` bits of x to 64.
constexpr u64 sext(u64 x, unsigned bits) {
    return static_cast<u64>(sign_extend(x, bits));
}

// Rotates. The amount is taken modulo the width, so a rotate by the full width is
// a rotate by zero; the explicit test avoids `x >> 64`, which is undefined.
constexpr u64 rol64(u64 x, unsigned n) {
    n &= 63;
    return n == 0 ? x : (x << n) | (x >> (64 - n));
}
constexpr u64 ror64(u64 x, unsigned n) {
    n &= 63;
    return n == 0 ? x : (x >> n) | (x << (64 - n));
}
constexpr u32 rol32(u32 x, unsigned n) {
    n &= 31;
    return n == 0 ? x : (x << n) | (x >> (32 - n));
}
constexpr u32 ror32(u32 x, unsigned n) {
    n &= 31;
    return n == 0 ? x : (x >> n) | (x << (32 - n));
}

// Counting. The count of an all-zero value is the width, by definition.
constexpr u64 clz64(u64 x)  { return static_cast<u64>(std::countl_zero(x)); }
constexpr u64 ctz64(u64 x)  { return static_cast<u64>(std::countr_zero(x)); }
constexpr u64 cpop64(u64 x) { return static_cast<u64>(std::popcount(x)); }
constexpr u64 clz32(u64 x)  { return static_cast<u64>(std::countl_zero(static_cast<u32>(x))); }
constexpr u64 ctz32(u64 x)  { return static_cast<u64>(std::countr_zero(static_cast<u32>(x))); }
constexpr u64 cpop32(u64 x) { return static_cast<u64>(std::popcount(static_cast<u32>(x))); }

// Reverse the order of the bytes.
constexpr u64 rev8(u64 x) {
    u64 r = 0;
    for (int i = 0; i < 8; ++i) r |= ((x >> (8 * i)) & 0xff) << (8 * (7 - i));
    return r;
}

// Within each byte: 0xff if any bit was set, 0x00 if none were. Used to find a
// zero byte in a word (strlen, memchr) without a loop.
constexpr u64 orc_b(u64 x) {
    u64 r = 0;
    for (int i = 0; i < 8; ++i) {
        if ((x >> (8 * i)) & 0xff) r |= 0xffull << (8 * i);
    }
    return r;
}

// Carry-less multiplication: multiply as polynomials over GF(2), where addition is
// XOR and there are no carries. The 128-bit product is returned as two halves.
// This is the heart of CRC and GCM computations.
struct Clmul128 { u64 lo, hi; };

constexpr Clmul128 clmul128(u64 a, u64 b) {
    Clmul128 p{0, 0};
    for (unsigned i = 0; i < 64; ++i) {
        if ((b >> i) & 1) {
            p.lo ^= a << i;
            if (i != 0) p.hi ^= a >> (64 - i);
        }
    }
    return p;
}

constexpr u64 clmul(u64 a, u64 b)  { return clmul128(a, b).lo; }
constexpr u64 clmulh(u64 a, u64 b) { return clmul128(a, b).hi; }
// clmulr is bits 126:63 of the product: the "reversed" form used by CRC code that
// processes bits most-significant first.
constexpr u64 clmulr(u64 a, u64 b) {
    const Clmul128 p = clmul128(a, b);
    return (p.hi << 1) | (p.lo >> 63);
}

}  // namespace bitmanip
