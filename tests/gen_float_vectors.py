#!/usr/bin/env python3
"""Generate floating-point test vectors from an exact rational reference.

This is the *reference* half of a differential test for the emulator's soft-float unit
(src/softfloat.rs). It computes every result with Python's `fractions.Fraction`, which is
exact, and rounds it to the target format by the textbook definition - so the answer
never passes through a host floating-point unit, and there is no hardware rounding mode
or flag register to disagree with.

It covers what a host FPU cannot be asked to check in one place: all five RISC-V rounding
modes (RNE, RTZ, RDN, RUP, RMM) on binary32 and binary64, the five exception flags
(NX, UF, OF, DZ, NV), tininess detected *after* rounding, subnormals in and out, the sign of
an exact zero, and saturating float-to-integer conversion.

Each output line is:

    <op> <fmt> <rm> <a> <b> <c> <expected result> <expected flags>

all numbers in hex; `fmt` is s or d (the float width), or for conversions the two formats
joined, e.g. `d2s`, `i64`, `u32` etc. Unused operands are 0.

Usage: gen_float_vectors.py OUTPUT_FILE [random_per_case]
"""

import random
import sys
from fractions import Fraction

RNE, RTZ, RDN, RUP, RMM = range(5)
NX, UF, OF, DZ, NV = 1, 2, 4, 8, 16


class Fmt:
    def __init__(self, name, ebits, fbits):
        self.name = name
        self.ebits = ebits
        self.fbits = fbits
        self.p = fbits + 1
        self.bias = (1 << (ebits - 1)) - 1
        self.emax = self.bias
        self.emin = 1 - self.bias
        self.width = 1 + ebits + fbits
        self.expmax = (1 << ebits) - 1
        self.qnan = (self.expmax << fbits) | (1 << (fbits - 1))

    def pack(self, sign, expfield, frac):
        return (sign << (self.ebits + self.fbits)) | (expfield << self.fbits) | frac

    def inf(self, sign):
        return self.pack(sign, self.expmax, 0)

    def zero(self, sign):
        return self.pack(sign, 0, 0)

    def maxfinite(self, sign):
        return self.pack(sign, self.expmax - 1, (1 << self.fbits) - 1)


F32 = Fmt('s', 8, 23)
F64 = Fmt('d', 11, 52)


def classify(f, bits):
    sign = bits >> (f.ebits + f.fbits)
    e = (bits >> f.fbits) & f.expmax
    m = bits & ((1 << f.fbits) - 1)
    if e == f.expmax:
        if m == 0:
            return ('inf', sign)
        return ('snan' if not (m >> (f.fbits - 1)) else 'qnan', sign)
    if e == 0 and m == 0:
        return ('zero', sign)
    return ('fin', sign)


def to_fraction(f, bits):
    sign = bits >> (f.ebits + f.fbits)
    e = (bits >> f.fbits) & f.expmax
    m = bits & ((1 << f.fbits) - 1)
    if e == 0:
        v = Fraction(m, 1) * Fraction(2) ** (f.emin - f.fbits)
    else:
        v = Fraction((1 << f.fbits) | m, 1) * Fraction(2) ** (e - f.bias - f.fbits)
    return -v if sign else v


def floor_log2(x):
    """floor(log2(x)) for a positive Fraction."""
    n, d = x.numerator, x.denominator
    e = n.bit_length() - d.bit_length()
    # 2^e <= x < 2^(e+1) after adjustment
    if e >= 0:
        if n < (d << e):
            e -= 1
    else:
        if (n << -e) < d:
            e -= 1
    return e


def round_int(q, rm, sign, sticky=False):
    """Round the non-negative Fraction q to an integer. Returns (integer, inexact).
    `sticky` means the true value is infinitesimally above q."""
    n = q.numerator // q.denominator
    rem = q - n
    inexact = rem != 0 or sticky
    if not inexact:
        return n, False
    if rm == RTZ:
        up = False
    elif rm == RDN:
        up = bool(sign)
    elif rm == RUP:
        up = not sign
    else:
        half = Fraction(1, 2)
        if rem > half or (rem == half and sticky):
            up = True
        elif rem < half:
            up = False
        else:  # exactly a tie
            up = (n & 1 == 1) if rm == RNE else True
    return (n + 1 if up else n), True


def round_value(f, x, rm, sticky=False):
    """Round the non-zero Fraction x (sign included) to format f.
    Returns (bits, flags). x is the exact result (sticky: infinitesimally larger in magnitude)."""
    sign = 1 if x < 0 else 0
    m = -x if sign else x
    e = floor_log2(m)
    flags = 0

    # tininess after rounding: round to p bits with an unbounded exponent range
    q_unb = Fraction(2) ** (e - (f.p - 1))
    n_unb, _ = round_int(m / q_unb, rm, sign, sticky)
    rounded_unb = Fraction(n_unb) * q_unb
    tiny = rounded_unb < Fraction(2) ** f.emin

    # the actual rounding, with the subnormal quantum when below the normal range
    eff = max(e, f.emin)
    q = Fraction(2) ** (eff - (f.p - 1))
    n, inexact = round_int(m / q, rm, sign, sticky)
    val = Fraction(n) * q

    if val >= Fraction(2) ** (f.emax + 1):
        # overflow
        flags |= OF | NX
        to_inf = (rm == RNE or rm == RMM) or (rm == RDN and sign) or (rm == RUP and not sign)
        return (f.inf(sign) if to_inf else f.maxfinite(sign)), flags

    if inexact:
        flags |= NX
        if tiny:
            flags |= UF
    if val == 0:
        return f.zero(sign), flags
    # encode val
    if val < Fraction(2) ** f.emin:
        frac = int(val / Fraction(2) ** (f.emin - f.fbits))
        return f.pack(sign, 0, frac), flags
    ev = floor_log2(val)
    frac = int((val / Fraction(2) ** ev - 1) * (1 << f.fbits))
    return f.pack(sign, ev + f.bias, frac), flags


def nan_result(f, *operands):
    """Canonical NaN, with NV if any operand is a signalling NaN."""
    flags = NV if any(classify(f, o)[0] == 'snan' for o in operands) else 0
    return f.qnan, flags


def zero_sum_sign(rm):
    return 1 if rm == RDN else 0


def ref_add(f, a, b, rm):
    ca, sa = classify(f, a)
    cb, sb = classify(f, b)
    if 'snan' in (ca, cb) or 'qnan' in (ca, cb):
        return nan_result(f, a, b)
    if ca == 'inf' and cb == 'inf':
        return (f.qnan, NV) if sa != sb else (f.inf(sa), 0)
    if ca == 'inf':
        return f.inf(sa), 0
    if cb == 'inf':
        return f.inf(sb), 0
    if ca == 'zero' and cb == 'zero':
        return f.zero(sa & sb if sa == sb else zero_sum_sign(rm)), 0
    s = to_fraction(f, a) + to_fraction(f, b)
    if s == 0:
        return f.zero(zero_sum_sign(rm)), 0
    return round_value(f, s, rm)


def negate(f, b):
    return b ^ (1 << (f.width - 1))


def ref_sub(f, a, b, rm):
    # NaN operands keep their NaN-ness; the sign flip does not matter for them
    return ref_add(f, a, negate(f, b), rm)


def ref_mul(f, a, b, rm):
    ca, sa = classify(f, a)
    cb, sb = classify(f, b)
    if 'snan' in (ca, cb) or 'qnan' in (ca, cb):
        return nan_result(f, a, b)
    sign = sa ^ sb
    if (ca == 'inf' and cb == 'zero') or (ca == 'zero' and cb == 'inf'):
        return f.qnan, NV
    if ca == 'inf' or cb == 'inf':
        return f.inf(sign), 0
    if ca == 'zero' or cb == 'zero':
        return f.zero(sign), 0
    return round_value(f, to_fraction(f, a) * to_fraction(f, b), rm)


def ref_div(f, a, b, rm):
    ca, sa = classify(f, a)
    cb, sb = classify(f, b)
    if 'snan' in (ca, cb) or 'qnan' in (ca, cb):
        return nan_result(f, a, b)
    sign = sa ^ sb
    if (ca == 'inf' and cb == 'inf') or (ca == 'zero' and cb == 'zero'):
        return f.qnan, NV
    if ca == 'inf':
        return f.inf(sign), 0
    if cb == 'inf':
        return f.zero(sign), 0
    if cb == 'zero':
        return f.inf(sign), DZ
    if ca == 'zero':
        return f.zero(sign), 0
    return round_value(f, to_fraction(f, a) / to_fraction(f, b), rm)


def isqrt(n):
    import math
    return math.isqrt(n)


def ref_sqrt(f, a, rm):
    ca, sa = classify(f, a)
    if ca in ('snan', 'qnan'):
        return nan_result(f, a)
    if ca == 'zero':
        return a, 0
    if sa:
        return f.qnan, NV
    if ca == 'inf':
        return f.inf(0), 0
    x = to_fraction(f, a)
    # enough fraction bits that even the smallest subnormal's root keeps ~200 significant bits
    K = 200 + max(0, -floor_log2(x) // 2 + 1)
    scaled = x * Fraction(4) ** K
    n = scaled.numerator // scaled.denominator
    r = isqrt(n)
    exact = (Fraction(r) * r == scaled)
    return round_value(f, Fraction(r, 1 << K), rm, sticky=not exact)


def ref_fma(f, a, b, c, rm):
    ca, sa = classify(f, a)
    cb, sb = classify(f, b)
    cc, sc = classify(f, c)
    inf_times_zero = (ca == 'inf' and cb == 'zero') or (ca == 'zero' and cb == 'inf')
    if 'snan' in (ca, cb, cc) or 'qnan' in (ca, cb, cc):
        flags = NV if ('snan' in (ca, cb, cc) or inf_times_zero) else 0
        return f.qnan, flags
    if inf_times_zero:
        return f.qnan, NV
    psign = sa ^ sb
    if ca == 'inf' or cb == 'inf':
        if cc == 'inf' and sc != psign:
            return f.qnan, NV
        return f.inf(psign), 0
    if cc == 'inf':
        return f.inf(sc), 0
    prod = to_fraction(f, a) * to_fraction(f, b)
    pzero = (ca == 'zero' or cb == 'zero')
    if pzero and cc == 'zero':
        return f.zero(psign if psign == sc else zero_sum_sign(rm)), 0
    total = prod + to_fraction(f, c)
    if total == 0:
        return f.zero(zero_sum_sign(rm)), 0
    return round_value(f, total, rm)


def ref_f2f(src, dst, a, rm):
    ca, sa = classify(src, a)
    if ca in ('snan', 'qnan'):
        return nan_result(src, a)[0] if False else dst.qnan, (NV if ca == 'snan' else 0)
    if ca == 'inf':
        return dst.inf(sa), 0
    if ca == 'zero':
        return dst.zero(sa), 0
    return round_value(dst, to_fraction(src, a), rm)


def ref_i2f(dst, value, rm):
    if value == 0:
        return dst.zero(0), 0
    return round_value(dst, Fraction(value), rm)


def ref_f2i(src, a, rm, bits, signed):
    ca, sa = classify(src, a)
    lo = -(1 << (bits - 1)) if signed else 0
    hi = (1 << (bits - 1)) - 1 if signed else (1 << bits) - 1

    def reg(v):  # what lands in the 64-bit register
        v &= (1 << bits) - 1
        if bits == 32 and v >> 31:
            v |= ((1 << 32) - 1) << 32
        return v

    if ca in ('snan', 'qnan'):
        return reg(hi), NV
    if ca == 'inf':
        return reg(lo if sa else hi), NV
    x = to_fraction(src, a)
    mag = -x if x < 0 else x
    n, inexact = round_int(mag, rm, 1 if x < 0 else 0)
    v = -n if x < 0 else n
    if v < lo or v > hi:
        return reg(hi if v > hi else lo), NV
    return reg(v), (NX if inexact else 0)


# --- operand generation -----------------------------------------------------------------

def rand_bits(f, rng):
    kind = rng.random()
    sign = rng.getrandbits(1)
    if kind < 0.08:
        return f.zero(sign)
    if kind < 0.13:
        return f.inf(sign)
    if kind < 0.18:  # NaNs of both kinds
        frac = rng.getrandbits(f.fbits) | 1
        if rng.random() < 0.5:
            frac |= 1 << (f.fbits - 1)
        else:
            frac &= ~(1 << (f.fbits - 1))
            frac |= 1
        return f.pack(sign, f.expmax, frac)
    choice = rng.random()
    if choice < 0.2:
        e = 0  # subnormal
    elif choice < 0.3:
        e = 1
    elif choice < 0.4:
        e = f.expmax - 1
    elif choice < 0.55:
        e = f.bias + rng.randint(-3, 3)
    else:
        e = rng.randint(0, f.expmax - 1)
    mchoice = rng.random()
    if mchoice < 0.1:
        m = 0
    elif mchoice < 0.2:
        m = (1 << f.fbits) - 1
    elif mchoice < 0.3:
        m = 1
    elif mchoice < 0.4:
        m = 1 << (f.fbits - 1)
    else:
        m = rng.getrandbits(f.fbits)
    if e == 0 and m == 0 and rng.random() < 0.5:
        m = 1
    return f.pack(sign, e, m)


def near(f, a, rng):
    """An operand whose magnitude is close to a's, so adds cancel and subnormals appear."""
    ca, _ = classify(f, a)
    if ca not in ('fin',):
        return rand_bits(f, rng)
    e = (a >> f.fbits) & f.expmax
    e2 = min(max(e + rng.randint(-2, 2), 0), f.expmax - 1)
    m = (a & ((1 << f.fbits) - 1)) ^ (rng.getrandbits(3) if rng.random() < 0.5 else rng.getrandbits(f.fbits))
    return f.pack(rng.getrandbits(1), e2, m & ((1 << f.fbits) - 1))


def main():
    out = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 400
    rng = random.Random(0xF10A7)
    lines = []

    def emit(op, fmt, rm, a, b, c, res, fl):
        lines.append('%s %s %d %x %x %x %x %x' % (op, fmt, rm, a, b, c, res, fl))

    for f in (F32, F64):
        for rm in range(5):
            for _ in range(n):
                a = rand_bits(f, rng)
                b = near(f, a, rng) if rng.random() < 0.5 else rand_bits(f, rng)
                c = near(f, a, rng) if rng.random() < 0.5 else rand_bits(f, rng)
                for op, fn in (('add', ref_add), ('sub', ref_sub), ('mul', ref_mul), ('div', ref_div)):
                    r, fl = fn(f, a, b, rm)
                    emit(op, f.name, rm, a, b, 0, r, fl)
                r, fl = ref_sqrt(f, a, rm)
                emit('sqrt', f.name, rm, a, 0, 0, r, fl)
                r, fl = ref_fma(f, a, b, c, rm)
                emit('fma', f.name, rm, a, b, c, r, fl)

    for rm in range(5):
        for _ in range(n):
            a = rand_bits(F64, rng)
            r, fl = ref_f2f(F64, F32, a, rm)
            emit('cvt', 'd2s', rm, a, 0, 0, r, fl)
            a = rand_bits(F32, rng)
            r, fl = ref_f2f(F32, F64, a, rm)
            emit('cvt', 's2d', rm, a, 0, 0, r, fl)

            for name, bits_, signed in (('i32', 32, True), ('u32', 32, False), ('i64', 64, True), ('u64', 64, False)):
                if rng.random() < 0.5:
                    v = rng.getrandbits(bits_)
                else:
                    v = (1 << rng.randint(0, bits_ - 1)) + rng.randint(-3, 3)
                    v &= (1 << bits_) - 1
                iv = v - (1 << bits_) if signed and v >> (bits_ - 1) else v
                for dst in (F32, F64):
                    r, fl = ref_i2f(dst, iv, rm)
                    emit('i2f', name + dst.name, rm, v, 0, 0, r, fl)
                for src in (F32, F64):
                    a = rand_bits(src, rng)
                    if rng.random() < 0.3:
                        # a value near the integer range boundaries
                        e = src.bias + rng.randint(bits_ - 3, bits_ + 1) if bits_ < src.bias else src.bias + bits_
                        e = min(max(e, 1), src.expmax - 1)
                        a = src.pack(rng.getrandbits(1), e, rng.getrandbits(src.fbits))
                    r, fl = ref_f2i(src, a, rm, bits_, signed)
                    emit('f2i', name + src.name, rm, a, 0, 0, r, fl)

    with open(out, 'w') as fh:
        fh.write('\n'.join(lines) + '\n')
    print('wrote %d vectors to %s' % (len(lines), out))


if __name__ == '__main__':
    main()
