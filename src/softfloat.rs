//! IEEE-754 binary32 and binary64 arithmetic in software.
//!
//! The previous implementation did the arithmetic on the host's floating-point unit
//! and read its rounding mode and exception flags back through `<cfenv>`. That works
//! for four of RISC-V's five rounding modes; the fifth, round-to-nearest-ties-away
//! (RMM), has no host equivalent, so it quietly behaved as round-to-nearest-even. It
//! also depends on the compiler not reordering floating-point operations around the
//! calls that change the rounding mode, which neither C++ nor Rust promises.
//!
//! This does it exactly: every operation computes the *infinitely precise* result as
//! an integer significand and exponent, then rounds it once, in one place
//! (`round_pack`), under any of the five modes, raising the five exception flags as
//! IEEE specifies. Everything works on raw bit patterns, so there is no host
//! floating-point involved at all and the results are identical on every platform.
//!
//! **RISC-V specifics.** Tininess is detected *after* rounding. A NaN result is
//! always the canonical NaN (no payload propagation). A signalling NaN input raises
//! Invalid.

use crate::csr::csr::{FFLAG_DZ, FFLAG_NV, FFLAG_NX, FFLAG_OF, FFLAG_UF};

/// The five rounding modes, numbered as in the `rm` field and `frm`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Rm {
    Rne = 0, // to nearest, ties to even
    Rtz = 1, // toward zero
    Rdn = 2, // down (toward -inf)
    Rup = 3, // up (toward +inf)
    Rmm = 4, // to nearest, ties away from zero
}

impl Rm {
    pub fn from_bits(b: u32) -> Option<Rm> {
        match b {
            0 => Some(Rm::Rne),
            1 => Some(Rm::Rtz),
            2 => Some(Rm::Rdn),
            3 => Some(Rm::Rup),
            4 => Some(Rm::Rmm),
            _ => None,
        }
    }
}

/// A binary floating-point format.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Fmt {
    pub exp_bits: u32,
    pub frac_bits: u32,
}

pub const F32: Fmt = Fmt { exp_bits: 8, frac_bits: 23 };
pub const F64: Fmt = Fmt { exp_bits: 11, frac_bits: 52 };

impl Fmt {
    fn bias(self) -> i32 {
        (1 << (self.exp_bits - 1)) - 1
    }
    fn exp_max(self) -> u64 {
        (1u64 << self.exp_bits) - 1
    }
    fn frac_mask(self) -> u64 {
        (1u64 << self.frac_bits) - 1
    }
    fn sign_bit(self) -> u64 {
        1u64 << (self.exp_bits + self.frac_bits)
    }
    /// Precision in bits, counting the implicit leading one.
    fn p(self) -> u32 {
        self.frac_bits + 1
    }
    pub fn canonical_nan(self) -> u64 {
        // exponent all ones, top fraction bit set, nothing else
        (self.exp_max() << self.frac_bits) | (1u64 << (self.frac_bits - 1))
    }
    pub fn inf(self, sign: bool) -> u64 {
        (if sign { self.sign_bit() } else { 0 }) | (self.exp_max() << self.frac_bits)
    }
    pub fn zero(self, sign: bool) -> u64 {
        if sign {
            self.sign_bit()
        } else {
            0
        }
    }
    fn max_finite(self, sign: bool) -> u64 {
        (if sign { self.sign_bit() } else { 0 }) | (((self.exp_max() - 1) << self.frac_bits) | self.frac_mask())
    }
}

/// An unpacked value.
#[derive(Clone, Copy, Debug)]
pub enum Class {
    Zero,
    Inf,
    Nan { signalling: bool },
    /// A finite non-zero value: `sig * 2^exp`, with `sig` an exact integer.
    Finite { sig: u64, exp: i32 },
}

#[derive(Clone, Copy, Debug)]
pub struct Unpacked {
    pub sign: bool,
    pub class: Class,
}

pub fn unpack(bits: u64, f: Fmt) -> Unpacked {
    let sign = bits & f.sign_bit() != 0;
    let e = (bits >> f.frac_bits) & f.exp_max();
    let frac = bits & f.frac_mask();
    let class = if e == f.exp_max() {
        if frac == 0 {
            Class::Inf
        } else {
            // The top fraction bit distinguishes quiet from signalling: set means quiet.
            Class::Nan { signalling: frac & (1u64 << (f.frac_bits - 1)) == 0 }
        }
    } else if e == 0 {
        if frac == 0 {
            Class::Zero
        } else {
            Class::Finite { sig: frac, exp: 1 - f.bias() - f.frac_bits as i32 }
        }
    } else {
        Class::Finite { sig: frac | (1u64 << f.frac_bits), exp: e as i32 - f.bias() - f.frac_bits as i32 }
    };
    Unpacked { sign, class }
}

pub fn is_nan(bits: u64, f: Fmt) -> bool {
    matches!(unpack(bits, f).class, Class::Nan { .. })
}
pub fn is_snan(bits: u64, f: Fmt) -> bool {
    matches!(unpack(bits, f).class, Class::Nan { signalling: true })
}

/// The position the significand's top bit is normalised to inside `round_pack`. High
/// enough that 53 bits of precision plus guard, round and sticky all fit with room
/// to spare in a u128.
const T: u32 = 120;

/// Round and pack the exact value `(sig + epsilon) * 2^exp`, where `epsilon` is a
/// positive fraction of one unit of `sig` if `sticky` is set. Returns the bit pattern
/// and the exception flags (NX, UF, OF).
///
/// This is the only place rounding happens. It handles normals, subnormals, overflow
/// to infinity or to the largest finite value (depending on the mode and sign), and
/// the tininess-after-rounding rule.
pub fn round_pack(sign: bool, exp: i32, sig: u128, sticky: bool, f: Fmt, rm: Rm) -> (u64, u64) {
    debug_assert!(sig != 0 || sticky);
    if sig == 0 {
        // Only sticky bits remain: a value strictly between zero and the smallest
        // representable. Rounding it is the same as rounding a one-unit value at a
        // vanishingly small exponent.
        return round_pack(sign, -100_000, 1, false, f, rm);
    }

    // Normalise so the top bit is at position T, collecting anything shifted out.
    let msb = 127 - sig.leading_zeros();
    let mut sticky = sticky;
    let sig_n = if msb > T {
        let k = msb - T;
        if sig & ((1u128 << k) - 1) != 0 {
            sticky = true;
        }
        sig >> k
    } else {
        sig << (T - msb)
    };
    // value = sig_n * 2^e2, sig_n in [2^T, 2^(T+1)): leading bit has unbiased
    // exponent `e_lead`.
    let e2 = exp + msb as i32 - T as i32;
    let e_lead = e2 + T as i32;

    let bias = f.bias();
    let emin = 1 - bias;
    let p = f.p();
    let biased = e_lead + bias;

    // How many low bits of sig_n fall below the last place we keep.
    let base_shift = T + 1 - p;
    let shift: u32 = if biased >= 1 {
        base_shift
    } else {
        // Subnormal range: the last place is fixed at 2^(emin - frac_bits), so every
        // step below emin drops one more bit.
        let extra = (1 - biased) as u32;
        base_shift.saturating_add(extra)
    };

    let round_up = |kept: u128, half: bool, rest: bool| -> u128 {
        let inexact = half || rest;
        let lsb = kept & 1 != 0;
        let inc = match rm {
            Rm::Rne => half && (rest || lsb),
            Rm::Rtz => false,
            Rm::Rdn => sign && inexact,
            Rm::Rup => !sign && inexact,
            Rm::Rmm => half,
        };
        kept + inc as u128
    };

    let split = |shift: u32| -> (u128, bool, bool) {
        if shift >= T + 2 {
            // Everything (including the half bit) is below the kept range.
            (0, false, true)
        } else {
            let kept = sig_n >> shift;
            let dropped = sig_n & ((1u128 << shift) - 1);
            let half = 1u128 << (shift - 1);
            (kept, dropped & half != 0, (dropped & (half - 1)) != 0 || sticky)
        }
    };

    let (kept0, half, rest) = split(shift);
    let inexact = half || rest;
    let kept = round_up(kept0, half, rest);
    let mut flags = if inexact { FFLAG_NX } else { 0 };

    if biased >= 1 {
        let mut be = biased;
        let mut k = kept;
        if k == 1u128 << p {
            // Rounding carried into a new binade.
            k >>= 1;
            be += 1;
        }
        if be as u64 >= f.exp_max() {
            // Overflow. The result depends on the mode: nearest rounds to infinity, but
            // the directed modes toward zero stop at the largest finite value.
            let to_inf = match rm {
                Rm::Rne | Rm::Rmm => true,
                Rm::Rtz => false,
                Rm::Rdn => sign,
                Rm::Rup => !sign,
            };
            let bits = if to_inf { f.inf(sign) } else { f.max_finite(sign) };
            return (bits, flags | FFLAG_OF | FFLAG_NX);
        }
        let bits = (if sign { f.sign_bit() } else { 0 }) | ((be as u64) << f.frac_bits) | (k as u64 & f.frac_mask());
        (bits, flags)
    } else {
        // Subnormal range. Tininess is detected *after* rounding: the result is tiny
        // unless rounding to full precision (with an unbounded exponent) would carry
        // it up to the smallest normal number.
        let (k_p, half_p, rest_p) = split(base_shift);
        let rounded_p = round_up(k_p, half_p, rest_p);
        let reaches_normal = e_lead == emin - 1 && rounded_p == 1u128 << p;
        if inexact && !reaches_normal {
            flags |= FFLAG_UF;
        }
        // `kept` is the raw fraction field. If rounding carried it to 2^frac_bits that
        // is exactly the encoding of the smallest normal number, which is right.
        let bits = (if sign { f.sign_bit() } else { 0 }) | kept as u64;
        (bits, flags)
    }
}

fn invalid(f: Fmt) -> (u64, u64) {
    (f.canonical_nan(), FFLAG_NV)
}

/// If either operand is a NaN, the canonical NaN (with Invalid for a signalling one).
fn nan_result(a: &Unpacked, b: &Unpacked, f: Fmt) -> Option<(u64, u64)> {
    let (an, bn) = (matches!(a.class, Class::Nan { .. }), matches!(b.class, Class::Nan { .. }));
    if !an && !bn {
        return None;
    }
    let snan = matches!(a.class, Class::Nan { signalling: true }) || matches!(b.class, Class::Nan { signalling: true });
    Some((f.canonical_nan(), if snan { FFLAG_NV } else { 0 }))
}

/// Add two exact values `a = (sign, sig, exp)`, `b = (sign, sig, exp)` (each `sig` a
/// u128 up to ~110 bits) and return `(sign, sig, exp, sticky)` for `round_pack`, or
/// `None` if the sum is exactly zero.
fn add_exact(
    a: (bool, u128, i32),
    b: (bool, u128, i32),
) -> Option<(bool, u128, i32, bool)> {
    // Put each operand's top bit at position T so they can be aligned by exponent.
    let norm = |(s, sig, e): (bool, u128, i32)| -> (bool, u128, i32) {
        if sig == 0 {
            return (s, 0, i32::MIN / 2);
        }
        let msb = 127 - sig.leading_zeros();
        (s, sig << (T - msb), e - (T - msb) as i32)
    };
    let (a, b) = (norm(a), norm(b));
    // `big` has the larger magnitude order (and, if equal, the larger significand).
    let (big, small) = if a.2 > b.2 || (a.2 == b.2 && a.1 >= b.1) { (a, b) } else { (b, a) };
    if small.1 == 0 {
        return if big.1 == 0 { None } else { Some((big.0, big.1, big.2, false)) };
    }
    let diff = (big.2 - small.2) as u32;
    let (aligned, sticky) = if diff >= 127 {
        (0u128, true)
    } else {
        let lost = small.1 & ((1u128 << diff) - 1) != 0;
        (small.1 >> diff, lost)
    };
    if big.0 == small.0 {
        // Same sign: magnitudes add. Leave one bit of headroom.
        let sum = big.1 + aligned;
        Some((big.0, sum, big.2, sticky))
    } else {
        // Opposite signs: magnitudes subtract. If the smaller operand lost low bits to
        // the alignment its true value is slightly larger than `aligned`, so the true
        // difference is slightly less than `big - aligned`: take one off and keep the
        // sticky flag, which is the same value to within the sticky bit.
        let mut diff_sig = big.1 - aligned;
        if sticky {
            diff_sig -= 1;
        }
        if diff_sig == 0 && !sticky {
            return None;
        }
        Some((big.0, diff_sig, big.2, sticky))
    }
}

fn zero_sum_sign(a: bool, b: bool, rm: Rm) -> bool {
    // x + (-x) is +0, except in round-down where it is -0; and (-0) + (-0) is -0.
    if a == b {
        a
    } else {
        rm == Rm::Rdn
    }
}

pub fn add(a_bits: u64, b_bits: u64, f: Fmt, rm: Rm) -> (u64, u64) {
    add_signed(a_bits, b_bits, false, f, rm)
}
pub fn sub(a_bits: u64, b_bits: u64, f: Fmt, rm: Rm) -> (u64, u64) {
    add_signed(a_bits, b_bits, true, f, rm)
}

fn add_signed(a_bits: u64, b_bits: u64, negate_b: bool, f: Fmt, rm: Rm) -> (u64, u64) {
    let a = unpack(a_bits, f);
    let mut b = unpack(b_bits, f);
    b.sign ^= negate_b;
    if let Some(r) = nan_result(&a, &b, f) {
        return r;
    }
    match (a.class, b.class) {
        (Class::Inf, Class::Inf) => {
            if a.sign != b.sign {
                invalid(f)
            } else {
                (f.inf(a.sign), 0)
            }
        }
        (Class::Inf, _) => (f.inf(a.sign), 0),
        (_, Class::Inf) => (f.inf(b.sign), 0),
        (Class::Zero, Class::Zero) => (f.zero(zero_sum_sign(a.sign, b.sign, rm)), 0),
        (Class::Zero, Class::Finite { .. }) => (if negate_b { b_bits ^ f.sign_bit() } else { b_bits }, 0),
        (Class::Finite { .. }, Class::Zero) => (a_bits, 0),
        (Class::Finite { sig: sa, exp: ea }, Class::Finite { sig: sb, exp: eb }) => {
            match add_exact((a.sign, sa as u128, ea), (b.sign, sb as u128, eb)) {
                // Exact cancellation of two non-zero values: +0, except in round-down,
                // where the IEEE rule makes it -0.
                None => (f.zero(rm == Rm::Rdn), 0),
                Some((s, sig, e, st)) => round_pack(s, e, sig, st, f, rm),
            }
        }
        _ => unreachable!(),
    }
}

pub fn mul(a_bits: u64, b_bits: u64, f: Fmt, rm: Rm) -> (u64, u64) {
    let a = unpack(a_bits, f);
    let b = unpack(b_bits, f);
    if let Some(r) = nan_result(&a, &b, f) {
        return r;
    }
    let sign = a.sign != b.sign;
    match (a.class, b.class) {
        (Class::Inf, Class::Zero) | (Class::Zero, Class::Inf) => invalid(f),
        (Class::Inf, _) | (_, Class::Inf) => (f.inf(sign), 0),
        (Class::Zero, _) | (_, Class::Zero) => (f.zero(sign), 0),
        (Class::Finite { sig: sa, exp: ea }, Class::Finite { sig: sb, exp: eb }) => {
            round_pack(sign, ea + eb, sa as u128 * sb as u128, false, f, rm)
        }
        _ => unreachable!(),
    }
}

pub fn div(a_bits: u64, b_bits: u64, f: Fmt, rm: Rm) -> (u64, u64) {
    let a = unpack(a_bits, f);
    let b = unpack(b_bits, f);
    if let Some(r) = nan_result(&a, &b, f) {
        return r;
    }
    let sign = a.sign != b.sign;
    match (a.class, b.class) {
        (Class::Inf, Class::Inf) | (Class::Zero, Class::Zero) => invalid(f),
        (Class::Inf, _) => (f.inf(sign), 0),
        (_, Class::Inf) => (f.zero(sign), 0),
        (Class::Zero, _) => (f.zero(sign), 0),
        (_, Class::Zero) => (f.inf(sign), FFLAG_DZ), // finite / 0
        (Class::Finite { sig: sa, exp: ea }, Class::Finite { sig: sb, exp: eb }) => {
            // Scale the dividend so the quotient has plenty of bits: sa < 2^53, so
            // sa << 64 fits in a u128 and the quotient has at least 11 extra bits
            // beyond the 53 we need, plus the remainder as the sticky bit.
            //
            // A subnormal operand has few significant bits, which would leave the
            // quotient with few too - so normalise both operands to have bit 63 set
            // first. The quotient then always carries 63+ bits.
            let (la, lb) = (sa.leading_zeros(), sb.leading_zeros());
            let (sa, sb) = (sa << la, sb << lb);
            let (ea, eb) = (ea - la as i32, eb - lb as i32);
            let num = (sa as u128) << 64;
            let q = num / sb as u128;
            let r = num % sb as u128;
            round_pack(sign, ea - eb - 64, q, r != 0, f, rm)
        }
        _ => unreachable!(),
    }
}

/// Floor of the square root of a u128, by Newton's method started from above (a power
/// of two at least as large as the root), which converges monotonically down to the
/// floor.
fn isqrt(n: u128) -> u128 {
    if n == 0 {
        return 0;
    }
    let mut x = 1u128 << ((128 - n.leading_zeros() + 1) / 2);
    loop {
        let y = (x + n / x) / 2;
        if y >= x {
            return x;
        }
        x = y;
    }
}

pub fn sqrt(a_bits: u64, f: Fmt, rm: Rm) -> (u64, u64) {
    let a = unpack(a_bits, f);
    match a.class {
        Class::Nan { signalling } => (f.canonical_nan(), if signalling { FFLAG_NV } else { 0 }),
        Class::Zero => (a_bits, 0), // sqrt(+-0) = +-0
        _ if a.sign => invalid(f),
        Class::Inf => (f.inf(false), 0),
        Class::Finite { sig, exp } => {
            // sqrt(sig * 2^exp): make the exponent even, then scale the significand up
            // so the integer square root carries ~60 bits.
            let (mut s, mut e) = (sig as u128, exp);
            // A subnormal has few significant bits; bring it up to at least 53 (by an
            // even amount, which keeps the exponent's parity) so the root has enough.
            let bits = 128 - s.leading_zeros() as i32;
            if bits < 53 {
                let shift = ((53 - bits + 1) / 2) * 2;
                s <<= shift;
                e -= shift;
            }
            if e & 1 != 0 {
                s <<= 1;
                e -= 1;
            }
            const K: u32 = 36;
            let scaled = s << (2 * K);
            let r = isqrt(scaled);
            let exact = r * r == scaled;
            round_pack(false, e / 2 - K as i32, r, !exact, f, rm)
        }
    }
}

/// Fused multiply-add: `a * b + c` with a single rounding.
pub fn fma(a_bits: u64, b_bits: u64, c_bits: u64, f: Fmt, rm: Rm) -> (u64, u64) {
    let a = unpack(a_bits, f);
    let b = unpack(b_bits, f);
    let c = unpack(c_bits, f);

    let any_nan = [&a, &b, &c].iter().any(|u| matches!(u.class, Class::Nan { .. }));
    let any_snan = [&a, &b, &c].iter().any(|u| matches!(u.class, Class::Nan { signalling: true }));
    // inf * 0 is invalid whether or not the addend is a NaN.
    let inf_times_zero = matches!((a.class, b.class), (Class::Inf, Class::Zero) | (Class::Zero, Class::Inf));
    if inf_times_zero {
        return invalid(f);
    }
    if any_nan {
        return (f.canonical_nan(), if any_snan { FFLAG_NV } else { 0 });
    }

    let psign = a.sign != b.sign;
    let prod_inf = matches!(a.class, Class::Inf) || matches!(b.class, Class::Inf);
    if prod_inf {
        return match c.class {
            Class::Inf if c.sign != psign => invalid(f),
            _ => (f.inf(psign), 0),
        };
    }
    if let Class::Inf = c.class {
        return (f.inf(c.sign), 0);
    }

    // The product, exactly (zero if either factor is zero).
    let prod = match (a.class, b.class) {
        (Class::Finite { sig: sa, exp: ea }, Class::Finite { sig: sb, exp: eb }) => {
            Some((sa as u128 * sb as u128, ea + eb))
        }
        _ => None,
    };

    match (prod, c.class) {
        (None, Class::Zero) => (f.zero(zero_sum_sign(psign, c.sign, rm)), 0),
        (None, _) => (c_bits, 0),
        (Some((psig, pexp)), Class::Zero) => round_pack(psign, pexp, psig, false, f, rm),
        (Some((psig, pexp)), Class::Finite { sig: sc, exp: ec }) => {
            match add_exact((psign, psig, pexp), (c.sign, sc as u128, ec)) {
                None => (f.zero(rm == Rm::Rdn), 0),
                Some((s, sig, e, st)) => round_pack(s, e, sig, st, f, rm),
            }
        }
        _ => unreachable!(),
    }
}

/// Convert between formats. Widening is exact; narrowing rounds.
pub fn convert(a_bits: u64, from: Fmt, to: Fmt, rm: Rm) -> (u64, u64) {
    let a = unpack(a_bits, from);
    match a.class {
        Class::Nan { signalling } => (to.canonical_nan(), if signalling { FFLAG_NV } else { 0 }),
        Class::Inf => (to.inf(a.sign), 0),
        Class::Zero => (to.zero(a.sign), 0),
        Class::Finite { sig, exp } => round_pack(a.sign, exp, sig as u128, false, to, rm),
    }
}

/// Convert an integer (given as sign and magnitude) to a float.
pub fn from_int(negative: bool, magnitude: u64, f: Fmt, rm: Rm) -> (u64, u64) {
    if magnitude == 0 {
        return (f.zero(false), 0);
    }
    round_pack(negative, 0, magnitude as u128, false, f, rm)
}

/// The integer a float rounds to, with the flags RISC-V requires.
///
/// `bits` and `signed` select the destination (32 or 64 bits, signed or unsigned).
/// An out-of-range value saturates to the largest or smallest representable one and
/// raises *only* Invalid (not Inexact); a NaN converts to the largest positive value.
/// The result is returned as a u64 holding the value *before* any 32-to-64-bit sign
/// extension, which the caller applies.
pub fn to_int(a_bits: u64, f: Fmt, rm: Rm, bits: u32, signed: bool) -> (u64, u64) {
    let max_pos: u64 = match (bits, signed) {
        (32, true) => i32::MAX as u64,
        (32, false) => u32::MAX as u64,
        (_, true) => i64::MAX as u64,
        _ => u64::MAX,
    };
    let min_neg: u64 = match (bits, signed) {
        (32, true) => i32::MIN as i64 as u64,
        (_, true) => i64::MIN as u64,
        _ => 0,
    };

    let a = unpack(a_bits, f);
    let (sign, sig, exp) = match a.class {
        Class::Nan { .. } => return (max_pos, FFLAG_NV),
        Class::Inf => return (if a.sign { min_neg } else { max_pos }, FFLAG_NV),
        Class::Zero => return (0, 0),
        Class::Finite { sig, exp } => (a.sign, sig, exp),
    };

    // The integer part and what is left over, as (integer, half, rest).
    let (int, half, rest): (u128, bool, bool) = if exp >= 0 {
        if exp > 70 {
            // Far larger than any 64-bit integer.
            return (if sign { min_neg } else { max_pos }, FFLAG_NV);
        }
        ((sig as u128) << exp, false, false)
    } else {
        let shift = (-exp) as u32;
        if shift >= 128 {
            (0, false, true)
        } else {
            let int = (sig as u128) >> shift;
            let dropped = (sig as u128) & ((1u128 << shift) - 1);
            let h = 1u128 << (shift - 1);
            (int, dropped & h != 0, dropped & (h - 1) != 0)
        }
    };
    let inexact = half || rest;
    let lsb = int & 1 != 0;
    let inc = match rm {
        Rm::Rne => half && (rest || lsb),
        Rm::Rtz => false,
        Rm::Rdn => sign && inexact,
        Rm::Rup => !sign && inexact,
        Rm::Rmm => half,
    };
    let int = int + inc as u128;

    // Range check against the destination.
    let (fits, value): (bool, u64) = if sign {
        if signed {
            let lim = (min_neg as i64 as i128).unsigned_abs();
            (int <= lim, (-(int as i128)) as i64 as u64)
        } else {
            (int == 0, 0)
        }
    } else {
        (int <= max_pos as u128, int as u64)
    };
    if !fits {
        return (if sign { min_neg } else { max_pos }, FFLAG_NV);
    }
    (value, if inexact { FFLAG_NX } else { 0 })
}

/// Total ordering key for non-NaN values: -0 and +0 compare equal.
fn order_key(bits: u64, f: Fmt) -> i128 {
    let mag = (bits & !f.sign_bit()) as i128;
    if bits & f.sign_bit() != 0 {
        -mag
    } else {
        mag
    }
}

/// Compare two non-NaN values: -1, 0 or 1.
pub fn compare(a: u64, b: u64, f: Fmt) -> i32 {
    let (ka, kb) = (order_key(a, f), order_key(b, f));
    (ka > kb) as i32 - (ka < kb) as i32
}

#[cfg(test)]
mod tests {
    use super::*;

    fn f32bits(x: f32) -> u64 {
        x.to_bits() as u64
    }
    fn f64bits(x: f64) -> u64 {
        x.to_bits()
    }

    #[test]
    fn simple_arithmetic_matches_the_host_in_round_to_nearest() {
        let vals = [0.0f64, -0.0, 1.0, -1.0, 0.1, 3.5, 1e300, 1e-300, f64::MIN_POSITIVE, 5e-324, 123456.789];
        for &a in &vals {
            for &b in &vals {
                assert_eq!(add(f64bits(a), f64bits(b), F64, Rm::Rne).0, f64bits(a + b), "{a}+{b}");
                assert_eq!(sub(f64bits(a), f64bits(b), F64, Rm::Rne).0, f64bits(a - b), "{a}-{b}");
                assert_eq!(mul(f64bits(a), f64bits(b), F64, Rm::Rne).0, f64bits(a * b), "{a}*{b}");
                if b != 0.0 {
                    assert_eq!(div(f64bits(a), f64bits(b), F64, Rm::Rne).0, f64bits(a / b), "{a}/{b}");
                }
            }
            if a >= 0.0 {
                assert_eq!(sqrt(f64bits(a), F64, Rm::Rne).0, f64bits(a.sqrt()), "sqrt {a}");
            }
        }
    }

    #[test]
    fn single_precision_arithmetic() {
        let vals = [0.0f32, 1.0, -2.5, 0.1, 3.4e38, 1e-38, 1.4e-45, 16777216.0];
        for &a in &vals {
            for &b in &vals {
                assert_eq!(add(f32bits(a), f32bits(b), F32, Rm::Rne).0, f32bits(a + b), "{a}+{b}");
                assert_eq!(mul(f32bits(a), f32bits(b), F32, Rm::Rne).0, f32bits(a * b), "{a}*{b}");
            }
        }
    }

    #[test]
    fn fused_multiply_add_rounds_once() {
        let (a, b, c) = (1.0 + 2f64.powi(-30), 1.0 + 2f64.powi(-30), -1.0);
        let fused = fma(f64bits(a), f64bits(b), f64bits(c), F64, Rm::Rne).0;
        assert_eq!(fused, f64bits(a.mul_add(b, c)));
        // Unfused, the product rounds first and the answer differs in the last place.
        assert_ne!(fused, f64bits(a * b + c));
    }

    #[test]
    fn directed_rounding_and_ties() {
        // 1 + 2^-53 is exactly halfway between 1.0 and the next double.
        let one = f64bits(1.0);
        let tiny = f64bits(2f64.powi(-53));
        assert_eq!(add(one, tiny, F64, Rm::Rne).0, one); // ties to even: down
        assert_eq!(add(one, tiny, F64, Rm::Rmm).0, f64bits(1.0 + f64::EPSILON)); // ties away: up
        assert_eq!(add(one, tiny, F64, Rm::Rup).0, f64bits(1.0 + f64::EPSILON));
        assert_eq!(add(one, tiny, F64, Rm::Rdn).0, one);
        assert_eq!(add(one, tiny, F64, Rm::Rtz).0, one);
    }

    #[test]
    fn flags() {
        // inexact
        assert_eq!(div(f64bits(1.0), f64bits(3.0), F64, Rm::Rne).1, FFLAG_NX);
        // divide by zero
        assert_eq!(div(f64bits(1.0), f64bits(0.0), F64, Rm::Rne), (F64.inf(false), FFLAG_DZ));
        // invalid
        assert_eq!(div(f64bits(0.0), f64bits(0.0), F64, Rm::Rne).1, FFLAG_NV);
        // overflow
        assert_eq!(mul(f64bits(1e300), f64bits(1e300), F64, Rm::Rne), (F64.inf(false), FFLAG_OF | FFLAG_NX));
        // underflow (tiny and inexact)
        let r = mul(f64bits(1e-300), f64bits(1e-300), F64, Rm::Rne);
        assert_eq!(r.0, 0);
        assert_eq!(r.1, FFLAG_UF | FFLAG_NX);
        // overflow in the toward-zero mode gives the largest finite value
        assert_eq!(mul(f64bits(1e300), f64bits(1e300), F64, Rm::Rtz).0, f64bits(f64::MAX));
    }

    #[test]
    fn conversions_between_integers_and_floats() {
        assert_eq!(to_int(f64bits(3.5), F64, Rm::Rne, 32, true), (4, FFLAG_NX));
        assert_eq!(to_int(f64bits(2.5), F64, Rm::Rne, 32, true), (2, FFLAG_NX));
        assert_eq!(to_int(f64bits(2.5), F64, Rm::Rmm, 32, true), (3, FFLAG_NX));
        assert_eq!(to_int(f64bits(-1.1), F64, Rm::Rtz, 32, true), (-1i64 as u64, FFLAG_NX));
        // out of range saturates and raises only Invalid
        assert_eq!(to_int(f64bits(3e9), F64, Rm::Rne, 32, true), (i32::MAX as u64, FFLAG_NV));
        assert_eq!(to_int(f64bits(-1.0), F64, Rm::Rne, 32, false), (0, FFLAG_NV));
        assert_eq!(to_int(F64.canonical_nan(), F64, Rm::Rne, 64, true), (i64::MAX as u64, FFLAG_NV));
        assert_eq!(from_int(true, 5, F64, Rm::Rne), (f64bits(-5.0), 0));
        // 2^53 + 1 is not representable: rounds, inexact
        assert_eq!(from_int(false, (1u64 << 53) + 1, F64, Rm::Rne), (f64bits(9007199254740992.0), FFLAG_NX));
    }

    #[test]
    fn sqrt_of_negative_is_invalid() {
        assert_eq!(sqrt(f64bits(-4.0), F64, Rm::Rne), (F64.canonical_nan(), FFLAG_NV));
        assert_eq!(sqrt(f64bits(-0.0), F64, Rm::Rne), (f64bits(-0.0), 0));
        assert_eq!(sqrt(f64bits(2.0), F64, Rm::Rne).0, f64bits(2f64.sqrt()));
    }
}
