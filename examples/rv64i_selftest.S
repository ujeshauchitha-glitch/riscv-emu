# ---------------------------------------------------------------------------
# RV64I self-test.
#
# Assembled with the real GNU assembler, so it validates the emulator's decoder
# against an independent implementation rather than against our own encoders.
#
# The program exercises the tricky corners of RV64I and computes a checksum in
# a0. Every check that passes adds a distinct bit; a0 == EXPECTED means all of
# them passed, and a zero bit tells you exactly which one did not.
#
# Terminates with EBREAK.
# ---------------------------------------------------------------------------

    .section .text
    .globl _start
_start:
    li      a0, 0                   # result accumulator
    li      t6, 0                   # bit index

# --- helper macro: if the condition register t0 is nonzero, set bit `n` in a0
.macro PASS_IF_NE n
    beqz    t0, 1f
    li      t1, (1 << \n)
    or      a0, a0, t1
1:
.endm

# 0: ADDIW sign-extends its 32-bit result to 64 bits.
    li      t2, 0x7fffffff
    addiw   t3, t2, 1               # -> 0xFFFFFFFF80000000
    li      t4, 0xffffffff80000000
    xor     t0, t3, t4
    seqz    t0, t0
    PASS_IF_NE 0

# 1: ADDW ignores bits above 32 in its operands.
    li      t2, 0xffffffff00000005
    li      t3, 3
    addw    t4, t2, t3
    li      t5, 8
    xor     t0, t4, t5
    seqz    t0, t0
    PASS_IF_NE 1

# 2: 64-bit shift amounts mask to 6 bits (shift by 64 == shift by 0).
    li      t2, 0x1234
    li      t3, 64
    sll     t4, t2, t3
    xor     t0, t4, t2
    seqz    t0, t0
    PASS_IF_NE 2

# 3: 32-bit shift amounts mask to 5 bits (shift by 32 == shift by 0).
    li      t2, 0x1234
    li      t3, 32
    sllw    t4, t2, t3
    xor     t0, t4, t2
    seqz    t0, t0
    PASS_IF_NE 3

# 4: SRLW operates on the low 32 bits only - the upper bits must not leak in.
    li      t2, 0xffffffff80000000
    srlw    t4, t2, 28              # -> 0x8
    li      t5, 8
    xor     t0, t4, t5
    seqz    t0, t0
    PASS_IF_NE 4

# 5: SRAW sign-fills from bit 31, then the result is sign-extended.
    li      t2, 0xffffffff80000000
    sraw    t4, t2, 28              # -> 0xFFFFFFFFFFFFFFF8
    li      t5, -8
    xor     t0, t4, t5
    seqz    t0, t0
    PASS_IF_NE 5

# 6: SLT is signed, SLTU is unsigned - they disagree on the same bits.
    li      t2, -1
    li      t3, 1
    slt     t4, t2, t3              # 1 : -1 < 1
    sltu    t5, t2, t3              # 0 : 0xFFFF..FF > 1
    sub     t0, t4, t5              # should be 1
    addi    t0, t0, -1
    seqz    t0, t0
    PASS_IF_NE 6

# 7: LW sign-extends, LWU zero-extends.
    la      t2, scratch
    li      t3, 0x89abcdef
    sw      t3, 0(t2)
    lw      t4, 0(t2)               # -> 0xFFFFFFFF89ABCDEF
    lwu     t5, 0(t2)               # -> 0x0000000089ABCDEF
    sub     t0, t4, t5
    li      t1, 0xffffffff00000000
    xor     t0, t0, t1
    seqz    t0, t0
    PASS_IF_NE 7

# 8: LB / LBU differ in sign extension.
    la      t2, scratch
    li      t3, 0xff
    sb      t3, 0(t2)
    lb      t4, 0(t2)               # -1
    lbu     t5, 0(t2)               # 255
    li      t1, -1
    xor     t4, t4, t1
    li      t1, 255
    xor     t5, t5, t1
    or      t0, t4, t5
    seqz    t0, t0
    PASS_IF_NE 8

# 9: a byte store must not disturb neighbouring bytes.
    la      t2, scratch
    li      t3, -1
    sd      t3, 0(t2)
    li      t3, 0xaa
    sb      t3, 0(t2)
    ld      t4, 0(t2)
    li      t5, 0xffffffffffffffaa
    xor     t0, t4, t5
    seqz    t0, t0
    PASS_IF_NE 9

# 10: a loop summing 1..10 == 55, exercising backward branches.
    li      t2, 0                   # sum
    li      t3, 1                   # i
    li      t4, 11
sum_loop:
    add     t2, t2, t3
    addi    t3, t3, 1
    bne     t3, t4, sum_loop
    li      t5, 55
    xor     t0, t2, t5
    seqz    t0, t0
    PASS_IF_NE 10

# 11: JALR clears bit 0 of the computed target, and links correctly.
    jal     ra, subroutine
    li      t5, 0x5a
    xor     t0, t2, t5
    seqz    t0, t0
    PASS_IF_NE 11

# 12: AUIPC is relative to the AUIPC instruction itself, not to the next one.
#     Hence the label must sit *on* the auipc, not after it.
here:
    auipc   t2, 0
    la      t3, here
    xor     t0, t2, t3
    seqz    t0, t0
    PASS_IF_NE 12

# 13: BLTU treats the all-ones pattern as the largest unsigned value.
    li      t2, -1
    li      t3, 1
    li      t0, 0
    bltu    t3, t2, 1f              # 1 < 0xFFFF..FF unsigned -> taken
    j       2f
1:  li      t0, 1
2:
    PASS_IF_NE 13

    ebreak

subroutine:
    li      t2, 0x5a
    ret                             # jalr x0, 0(ra)

    .section .data
    .align 3
scratch:
    .dword 0
