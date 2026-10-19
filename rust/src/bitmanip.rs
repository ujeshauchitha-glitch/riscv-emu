//! The arithmetic behind the bit-manipulation (Zba, Zbb, Zbc, Zbs) and
//! conditional-zero (Zicond) extensions.
//!
//! These are pure functions of their operands, so they live apart from the CPU: the
//! CPU decides *which* operation an instruction names, and this file says what the
//! operation computes. That separation also lets the tests check the encodings and
//! the arithmetic independently.

use crate::types::sign_extend;

#[inline(always)]
pub const fn zext32(x: u64) -> u64 {
    x & 0xffff_ffff
}

/// Sign-extend the low `bits` bits of `x` to 64.
#[inline(always)]
pub const fn sext(x: u64, bits: u32) -> u64 {
    sign_extend(x, bits) as u64
}

// Rotates. The amount is taken modulo the width, so a rotate by the full width is a
// rotate by zero. (`rotate_left` already does exactly that, but spelling the
// masking out documents the rule.)
#[inline(always)]
pub fn rol64(x: u64, n: u32) -> u64 {
    x.rotate_left(n & 63)
}
#[inline(always)]
pub fn ror64(x: u64, n: u32) -> u64 {
    x.rotate_right(n & 63)
}
#[inline(always)]
pub fn rol32(x: u32, n: u32) -> u32 {
    x.rotate_left(n & 31)
}
#[inline(always)]
pub fn ror32(x: u32, n: u32) -> u32 {
    x.rotate_right(n & 31)
}

// Counting. The count of an all-zero value is the width, by definition (which is
// what leading_zeros/trailing_zeros return).
#[inline(always)]
pub fn clz64(x: u64) -> u64 {
    x.leading_zeros() as u64
}
#[inline(always)]
pub fn ctz64(x: u64) -> u64 {
    x.trailing_zeros() as u64
}
#[inline(always)]
pub fn cpop64(x: u64) -> u64 {
    x.count_ones() as u64
}
#[inline(always)]
pub fn clz32(x: u64) -> u64 {
    (x as u32).leading_zeros() as u64
}
#[inline(always)]
pub fn ctz32(x: u64) -> u64 {
    (x as u32).trailing_zeros() as u64
}
#[inline(always)]
pub fn cpop32(x: u64) -> u64 {
    (x as u32).count_ones() as u64
}

/// Reverse the order of the bytes.
#[inline(always)]
pub fn rev8(x: u64) -> u64 {
    x.swap_bytes()
}

/// Within each byte: 0xff if any bit was set, 0x00 if none were. Used to find a
/// zero byte in a word (strlen, memchr) without a loop.
pub fn orc_b(x: u64) -> u64 {
    let mut r = 0u64;
    for i in 0..8 {
        if (x >> (8 * i)) & 0xff != 0 {
            r |= 0xffu64 << (8 * i);
        }
    }
    r
}

/// Carry-less multiplication: multiply as polynomials over GF(2), where addition is
/// XOR and there are no carries. The 128-bit product is returned as (lo, hi). This
/// is the heart of CRC and GCM computations.
pub fn clmul128(a: u64, b: u64) -> (u64, u64) {
    let (mut lo, mut hi) = (0u64, 0u64);
    for i in 0..64 {
        if (b >> i) & 1 != 0 {
            lo ^= a << i;
            if i != 0 {
                hi ^= a >> (64 - i);
            }
        }
    }
    (lo, hi)
}

pub fn clmul(a: u64, b: u64) -> u64 {
    clmul128(a, b).0
}
pub fn clmulh(a: u64, b: u64) -> u64 {
    clmul128(a, b).1
}
/// clmulr is bits 126:63 of the product: the "reversed" form used by CRC code that
/// processes bits most-significant first.
pub fn clmulr(a: u64, b: u64) -> u64 {
    let (lo, hi) = clmul128(a, b);
    (hi << 1) | (lo >> 63)
}
