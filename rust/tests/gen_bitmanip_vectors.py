#!/usr/bin/env python3
"""Generate test vectors for Zba, Zbb, Zbc, Zbs and Zicond from an independent model.

This is the *reference* half of a differential test. The emulator's own
implementation (src/cpu_bitmanip.cpp, include/bitmanip.hpp) is checked against
the results computed here, over tens of thousands of random and edge-case
operand pairs. The two are written in different languages, in different styles,
from the specification tables - so a mistake has to be made identically in both
to go unnoticed. (Counting bits, for instance, is done here on strings of
'0' and '1' characters; the C++ uses std::countl_zero.)

Each output line is:   <instruction hex> <rs1 hex> <rs2 hex> <expected rd hex> <name>

The instruction always uses rd = x10, rs1 = x11, rs2 = x12. Immediate-form
instructions ignore rs2.

Usage: gen_bitmanip_vectors.py OUTPUT_FILE [random_per_instruction]
"""

import random
import sys

M64 = (1 << 64) - 1
M32 = (1 << 32) - 1

RD, RS1, RS2 = 10, 11, 12

OP, OP32, OPIMM, OPIMM32 = 0x33, 0x3B, 0x13, 0x1B


# --- encoders (from the instruction-format diagrams) ---------------------------------------

def enc_r(opcode, funct3, funct7):
    return (funct7 << 25) | (RS2 << 20) | (RS1 << 15) | (funct3 << 12) | (RD << 7) | opcode


def enc_i(opcode, funct3, imm12):
    return ((imm12 & 0xFFF) << 20) | (RS1 << 15) | (funct3 << 12) | (RD << 7) | opcode


def enc_shift(opcode, funct3, funct6, shamt):
    # RV64 shift-immediates: a 6-bit function code above a 6-bit shift amount.
    return enc_i(opcode, funct3, (funct6 << 6) | shamt)


# --- the model: plain integer arithmetic, no tricks -------------------------------------------

def signed(x):
    return x - (1 << 64) if x >> 63 else x


def sext(x, bits):
    x &= (1 << bits) - 1
    if x >> (bits - 1):
        x -= 1 << bits
    return x & M64


def bits_of(x, width=64):
    return format(x & ((1 << width) - 1), '0%db' % width)


def clz(x, width=64):
    s = bits_of(x, width)
    return width if '1' not in s else s.index('1')


def ctz(x, width=64):
    s = bits_of(x, width)[::-1]
    return width if '1' not in s else s.index('1')


def cpop(x, width=64):
    return bits_of(x, width).count('1')


def rotl(x, n, width):
    n %= width
    mask = (1 << width) - 1
    x &= mask
    return ((x << n) | (x >> (width - n))) & mask if n else x


def rotr(x, n, width):
    return rotl(x, width - (n % width), width)


def rev8(x):
    return int.from_bytes((x & M64).to_bytes(8, 'little'), 'big')


def orc_b(x):
    out = 0
    for i in range(8):
        if (x >> (8 * i)) & 0xFF:
            out |= 0xFF << (8 * i)
    return out


def carryless(a, b):
    product = 0
    for i in range(64):
        if (b >> i) & 1:
            product ^= a << i
    return product        # up to 127 bits


# --- the instruction table --------------------------------------------------------------------
#
# (name, encoding, model(rs1, rs2, shamt) -> rd, takes_immediate)
# For immediate forms the "shamt" argument carries the shift amount.

def reg(name, opcode, f3, f7, fn):
    return (name, enc_r(opcode, f3, f7), fn, None)


def unary(name, opcode, f3, imm12, fn):
    return (name, enc_i(opcode, f3, imm12), lambda a, b, s, fn=fn: fn(a), None)


def shifty(name, opcode, f3, funct6, fn, shamt_bits=6):
    return (name, (opcode, f3, funct6), fn, shamt_bits)


TABLE = [
    # Zba
    reg('sh1add',    OP,   2, 0x10, lambda a, b, s: ((a << 1) + b) & M64),
    reg('sh2add',    OP,   4, 0x10, lambda a, b, s: ((a << 2) + b) & M64),
    reg('sh3add',    OP,   6, 0x10, lambda a, b, s: ((a << 3) + b) & M64),
    reg('add.uw',    OP32, 0, 0x04, lambda a, b, s: ((a & M32) + b) & M64),
    reg('sh1add.uw', OP32, 2, 0x10, lambda a, b, s: (((a & M32) << 1) + b) & M64),
    reg('sh2add.uw', OP32, 4, 0x10, lambda a, b, s: (((a & M32) << 2) + b) & M64),
    reg('sh3add.uw', OP32, 6, 0x10, lambda a, b, s: (((a & M32) << 3) + b) & M64),
    shifty('slli.uw', OPIMM32, 1, 0x02, lambda a, b, s: ((a & M32) << s) & M64),

    # Zbb
    reg('andn', OP, 7, 0x20, lambda a, b, s: a & (~b & M64)),
    reg('orn',  OP, 6, 0x20, lambda a, b, s: a | (~b & M64)),
    reg('xnor', OP, 4, 0x20, lambda a, b, s: ~(a ^ b) & M64),
    reg('min',  OP, 4, 0x05, lambda a, b, s: a if signed(a) < signed(b) else b),
    reg('minu', OP, 5, 0x05, lambda a, b, s: a if a < b else b),
    reg('max',  OP, 6, 0x05, lambda a, b, s: a if signed(a) > signed(b) else b),
    reg('maxu', OP, 7, 0x05, lambda a, b, s: a if a > b else b),
    reg('rol',  OP, 1, 0x30, lambda a, b, s: rotl(a, b % 64, 64)),
    reg('ror',  OP, 5, 0x30, lambda a, b, s: rotr(a, b % 64, 64)),
    reg('rolw', OP32, 1, 0x30, lambda a, b, s: sext(rotl(a, b % 32, 32), 32)),
    reg('rorw', OP32, 5, 0x30, lambda a, b, s: sext(rotr(a, b % 32, 32), 32)),
    unary('clz',    OPIMM, 1, 0x600, lambda a: clz(a)),
    unary('ctz',    OPIMM, 1, 0x601, lambda a: ctz(a)),
    unary('cpop',   OPIMM, 1, 0x602, lambda a: cpop(a)),
    unary('sext.b', OPIMM, 1, 0x604, lambda a: sext(a, 8)),
    unary('sext.h', OPIMM, 1, 0x605, lambda a: sext(a, 16)),
    unary('clzw',   OPIMM32, 1, 0x600, lambda a: clz(a, 32)),
    unary('ctzw',   OPIMM32, 1, 0x601, lambda a: ctz(a, 32)),
    unary('cpopw',  OPIMM32, 1, 0x602, lambda a: cpop(a, 32)),
    unary('rev8',   OPIMM, 5, 0x6B8, lambda a: rev8(a)),
    unary('orc.b',  OPIMM, 5, 0x287, lambda a: orc_b(a)),
    (  # zext.h is a register-register encoding with rs2 = 0
        'zext.h',
        (0x04 << 25) | (0 << 20) | (RS1 << 15) | (4 << 12) | (RD << 7) | OP32,
        lambda a, b, s: a & 0xFFFF, None),
    shifty('rori',  OPIMM,   5, 0x18, lambda a, b, s: rotr(a, s, 64)),
    shifty('roriw', OPIMM32, 5, 0x18, lambda a, b, s: sext(rotr(a, s, 32), 32), shamt_bits=5),

    # Zbs
    reg('bclr', OP, 1, 0x24, lambda a, b, s: a & ~(1 << (b % 64)) & M64),
    reg('bext', OP, 5, 0x24, lambda a, b, s: (a >> (b % 64)) & 1),
    reg('binv', OP, 1, 0x34, lambda a, b, s: a ^ (1 << (b % 64))),
    reg('bset', OP, 1, 0x14, lambda a, b, s: a | (1 << (b % 64))),
    shifty('bclri', OPIMM, 1, 0x12, lambda a, b, s: a & ~(1 << s) & M64),
    shifty('bexti', OPIMM, 5, 0x12, lambda a, b, s: (a >> s) & 1),
    shifty('binvi', OPIMM, 1, 0x1A, lambda a, b, s: a ^ (1 << s)),
    shifty('bseti', OPIMM, 1, 0x0A, lambda a, b, s: a | (1 << s)),

    # Zbc
    reg('clmul',  OP, 1, 0x05, lambda a, b, s: carryless(a, b) & M64),
    reg('clmulr', OP, 2, 0x05, lambda a, b, s: (carryless(a, b) >> 63) & M64),
    reg('clmulh', OP, 3, 0x05, lambda a, b, s: (carryless(a, b) >> 64) & M64),

    # Zicond
    reg('czero.eqz', OP, 5, 0x07, lambda a, b, s: 0 if b == 0 else a),
    reg('czero.nez', OP, 7, 0x07, lambda a, b, s: 0 if b != 0 else a),
]

# Real assembler output for a handful of these (rd=a0, rs1=a1, rs2=a2), as a check
# that the encoding table above is the right one and not merely self-consistent.
KNOWN_ENCODINGS = {
    'sh1add':    0x20C5A533,
    'add.uw':    0x08C5853B,
    'czero.eqz': 0x0EC5D533,
    'clz':       0x60059513,
    'rev8':      0x6B85D513,
}

SPECIAL = [
    0, 1, 2, 3, 0xFF, 0x80, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
    0x100000000, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, M64,
    0x0123456789ABCDEF, 0xFEDCBA9876543210, 0xAAAAAAAAAAAAAAAA, 0x5555555555555555,
]


def rand64(rng):
    """A random 64-bit value, biased toward the shapes that expose bugs."""
    kind = rng.randrange(6)
    if kind == 0:
        return rng.getrandbits(64)
    if kind == 1:
        return rng.getrandbits(rng.randrange(1, 65))            # few significant bits
    if kind == 2:
        return (1 << rng.randrange(64)) & M64                   # one bit set
    if kind == 3:
        return (~(1 << rng.randrange(64))) & M64                # one bit clear
    if kind == 4:
        return rng.getrandbits(32) | (rng.getrandbits(32) << 32 if rng.random() < 0.3 else 0)
    return rng.choice(SPECIAL)


def main():
    out_path = sys.argv[1]
    random_count = int(sys.argv[2]) if len(sys.argv) > 2 else 1500
    rng = random.Random(0x7B17A9)

    # Self-check the encoding table against the known assembler output.
    encodings = {name: enc for (name, enc, _fn, extra) in TABLE if extra is None}
    for name, want in KNOWN_ENCODINGS.items():
        got = encodings[name]
        if got != want:
            sys.exit('encoding table disagrees with the assembler for %s: %08x vs %08x' % (name, got, want))

    lines = 0
    with open(out_path, 'w') as f:
        for (name, enc, fn, extra) in TABLE:
            is_shift_imm = extra is not None            # `enc` is (opcode, f3, funct6)
            shamt_bits = extra if is_shift_imm else 0

            def emit(a, b, s=0):
                nonlocal lines
                if is_shift_imm:
                    opcode, f3, f6 = enc
                    word = enc_shift(opcode, f3, f6, s) if shamt_bits == 6 else \
                           enc_i(opcode, f3, (f6 << 6) | s)
                else:
                    word = enc
                want = fn(a, b, s) & M64
                f.write('%08x %016x %016x %016x %s\n' % (word, a, b, want, name))
                lines += 1

            # Every pair of special values (and every shift amount for the
            # immediate forms), then random ones.
            if is_shift_imm:
                for a in SPECIAL:
                    for s in range(1 << shamt_bits):
                        emit(a, 0, s)
                for _ in range(random_count):
                    emit(rand64(rng), 0, rng.randrange(1 << shamt_bits))
            else:
                for a in SPECIAL:
                    for b in SPECIAL:
                        emit(a, b)
                for _ in range(random_count):
                    a = rand64(rng)
                    # Shift-like register forms care about the low bits of rs2, so
                    # sometimes keep rs2 small to exercise every amount.
                    b = rng.randrange(0, 130) if rng.random() < 0.4 else rand64(rng)
                    emit(a, b)

    print('wrote %d vectors for %d instructions to %s' % (lines, len(TABLE), out_path))


if __name__ == '__main__':
    main()
