# ---------------------------------------------------------------------------
# M and A extension self-test (phase 3).
#
# Assembled by the GNU assembler, so the emulator is decoding an independent
# implementation of these encodings. One bit per passing check in a0;
# expected result 0x7fff (15 checks).
# ---------------------------------------------------------------------------

    .section .text
    .globl _start
_start:
    li      a0, 0

# Note this uses s0, not a temporary: t1 holds a live pointer across several of
# the checks below, and a macro that quietly clobbered it would corrupt the very
# addresses being tested.
.macro PASS_IF_NE n
    beqz    t0, 1f
    li      s0, (1 << \n)
    or      a0, a0, s0
1:
.endm

# 0: MUL
    li      t2, 6
    li      t3, 7
    mul     t4, t2, t3
    li      t5, 42
    xor     t0, t4, t5
    seqz    t0, t0
    PASS_IF_NE 0

# 1: MULH of -1 * -1 is 1, so the upper half is 0.
    li      t2, -1
    mulh    t4, t2, t2
    seqz    t0, t4
    PASS_IF_NE 1

# 2: MULHU reads the same bits as unsigned: (2^64-1)^2 has upper half 2^64-2.
    mulhu   t4, t2, t2
    li      t5, -2
    xor     t0, t4, t5
    seqz    t0, t0
    PASS_IF_NE 2

# 3: MULHSU reads rs1 signed, rs2 unsigned: -1 * (2^64-1) has all-ones upper.
    mulhsu  t4, t2, t2
    li      t5, -1
    xor     t0, t4, t5
    seqz    t0, t0
    PASS_IF_NE 3

# 4: division by zero returns -1 and never traps.
    li      t2, 42
    li      t3, 0
    div     t4, t2, t3
    li      t5, -1
    xor     t0, t4, t5
    seqz    t0, t0
    PASS_IF_NE 4

# 5: remainder by zero returns the dividend.
    rem     t4, t2, t3
    li      t5, 42
    xor     t0, t4, t5
    seqz    t0, t0
    PASS_IF_NE 5

# 6: DIVU by zero returns all ones.
    divu    t4, t2, t3
    li      t5, -1
    xor     t0, t4, t5
    seqz    t0, t0
    PASS_IF_NE 6

# 7: signed overflow. INT64_MIN / -1 does not fit, and is defined to wrap back
#    to INT64_MIN rather than trapping.
    li      t2, 1
    slli    t2, t2, 63              # INT64_MIN
    li      t3, -1
    div     t4, t2, t3
    xor     t0, t4, t2
    seqz    t0, t0
    PASS_IF_NE 7

# 8: and the matching remainder is 0.
    rem     t4, t2, t3
    seqz    t0, t4
    PASS_IF_NE 8

# 9: signed division truncates toward zero: -7 / 2 = -3, not -4.
    li      t2, -7
    li      t3, 2
    div     t4, t2, t3
    li      t5, -3
    xor     t0, t4, t5
    seqz    t0, t0
    PASS_IF_NE 9

# 10: MULW sign-extends its 32-bit result.
    li      t2, 0x40000000
    li      t3, 2
    mulw    t4, t2, t3              # 0x80000000 as 32 bits -> negative
    li      t5, 0xffffffff80000000
    xor     t0, t4, t5
    seqz    t0, t0
    PASS_IF_NE 10

# 11: amoadd.d returns the ORIGINAL value and stores the sum.
    la      t1, scratch
    li      t2, 100
    sd      t2, 0(t1)
    li      t3, 5
    amoadd.d t4, t3, (t1)           # t4 = 100, [scratch] = 105
    ld      t5, 0(t1)
    li      t6, 100
    xor     t0, t4, t6
    li      t6, 105
    xor     t6, t5, t6
    or      t0, t0, t6
    seqz    t0, t0
    PASS_IF_NE 11

# 12: amomin is signed, amominu unsigned - they disagree on the same bits.
    li      t2, -5
    sd      t2, 0(t1)
    li      t3, 3
    amomin.d t4, t3, (t1)           # min(-5, 3) = -5
    ld      t5, 0(t1)
    li      t6, -5
    xor     t0, t5, t6

    li      t2, -5
    sd      t2, 0(t1)
    amominu.d t4, t3, (t1)          # unsigned: -5 is huge, so min is 3
    ld      t5, 0(t1)
    li      t6, 3
    xor     t6, t5, t6
    or      t0, t0, t6
    seqz    t0, t0
    PASS_IF_NE 12

# 13: an LR/SC pair succeeds when the reservation holds, and a second SC fails
#     because the reservation was consumed.
    li      t2, 11
    sd      t2, 0(t1)
    li      t3, 99
    lr.d    t4, (t1)                # t4 = 11
    sc.d    t5, t3, (t1)            # succeeds -> 0
    sc.d    t6, t3, (t1)            # no reservation left -> non-zero
    li      t2, 11
    xor     t0, t4, t2              # lr returned the old value
    or      t0, t0, t5              # first sc returned 0
    seqz    t2, t6                  # second sc must be non-zero
    or      t0, t0, t2
    seqz    t0, t0
    PASS_IF_NE 13

# 14: a real spinlock, the pattern xv6 uses. Acquire with an LR/SC retry loop,
#     then release with a plain store.
    la      t1, lockvar
    sw      zero, 0(t1)
    li      t2, 1
acquire:
    lr.w    t3, (t1)
    bnez    t3, acquire             # someone holds it - spin
    sc.w    t4, t2, (t1)
    bnez    t4, acquire             # reservation lost - retry
    # critical section would go here
    lw      t5, 0(t1)               # the lock reads as held
    sw      zero, 0(t1)             # release
    lw      t6, 0(t1)               # and now free

    li      t2, 1
    xor     t0, t5, t2
    or      t0, t0, t6
    seqz    t0, t0
    PASS_IF_NE 14

done:
    ebreak

    .section .data
    .align 3
scratch:
    .dword 0
lockvar:
    .dword 0
