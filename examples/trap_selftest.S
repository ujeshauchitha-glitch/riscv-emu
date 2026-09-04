# ---------------------------------------------------------------------------
# CSR and trap-handling self-test (phase 2).
#
# Installs a real trap handler, then deliberately causes traps and checks that
# the machine recorded them correctly and that MRET returns cleanly. Assembled
# by the GNU assembler, so it exercises the emulator against an independent
# implementation of the CSR instruction encodings.
#
# One bit per passing check accumulates in a0. Expected result: 0xfff.
#
# Register conventions:
#   a0        result accumulator
#   s1        mcause from the most recent trap
#   s2        mepc   from the most recent trap
#   s3        mtval  from the most recent trap
#   t0-t5     scratch
# ---------------------------------------------------------------------------

    .section .text
    .globl _start
_start:
    li      a0, 0
    li      s1, 0
    li      s2, 0
    li      s3, 0

    # Install the trap handler. mtvec mode 0 (direct): every trap goes here.
    la      t0, handler
    csrw    mtvec, t0

.macro PASS_IF_NE n
    beqz    t0, 1f
    li      t1, (1 << \n)
    or      a0, a0, t1
1:
.endm

# 0: mhartid reads back 0, via csrr - which assembles to csrrs with rs1 = x0,
#    a pure read, and is therefore legal even though mhartid is read-only.
    csrr    t2, mhartid
    seqz    t0, t2
    PASS_IF_NE 0

# 1: mscratch round-trips an arbitrary 64-bit value.
    li      t2, 0x123456789abcdef
    csrw    mscratch, t2
    csrr    t3, mscratch
    xor     t0, t2, t3
    seqz    t0, t0
    PASS_IF_NE 1

# 2: csrrw returns the OLD value while installing the new one.
    li      t2, 0xaaaa
    csrrw   t3, mscratch, t2
    li      t4, 0x123456789abcdef
    xor     t0, t3, t4
    seqz    t0, t0
    PASS_IF_NE 2

# 3: csrrs sets bits and csrrc clears them, leaving the rest alone.
    li      t2, 0xf0
    csrw    mscratch, t2
    li      t3, 0x0f
    csrrs   x0, mscratch, t3        # mscratch |= 0x0f  -> 0xff
    li      t3, 0xf0
    csrrc   x0, mscratch, t3        # mscratch &= ~0xf0 -> 0x0f
    csrr    t4, mscratch
    li      t2, 0x0f
    xor     t0, t4, t2
    seqz    t0, t0
    PASS_IF_NE 3

# 4: the immediate forms take a 5-bit literal, not a register number.
    csrwi   mscratch, 31
    csrr    t2, mscratch
    li      t3, 31
    xor     t0, t2, t3
    seqz    t0, t0
    PASS_IF_NE 4

# 5: ECALL from machine mode traps with cause 11.
ecall_site:
    ecall
    li      t2, 11
    xor     t0, s1, t2
    seqz    t0, t0
    PASS_IF_NE 5

# 6: mepc pointed at the ECALL itself, not past it.
    la      t2, ecall_site
    xor     t0, s2, t2
    seqz    t0, t0
    PASS_IF_NE 6

# 7: we are running again after MRET, which is only possible if the handler
#    advanced mepc and returned correctly. Reaching this line proves it.
    li      t0, 1
    PASS_IF_NE 7

# 8: an unimplemented instruction traps with cause 2 (illegal instruction).
illegal_site:
    .word   0xffffffff
    li      t2, 2
    xor     t0, s1, t2
    seqz    t0, t0
    PASS_IF_NE 8

# 9: mtval holds the offending instruction's bits.
    li      t2, 0xffffffff
    xor     t0, s3, t2
    seqz    t0, t0
    PASS_IF_NE 9

# 10: writing an unimplemented CSR traps as illegal. This is how software
#     probes for optional features, so it must fault rather than be accepted.
    csrw    0x7c0, zero
    li      t2, 2
    xor     t0, s1, t2
    seqz    t0, t0
    PASS_IF_NE 10

# 11: a machine software interrupt fires once enabled, and mcause has its top
#     bit set to distinguish an interrupt from an exception.
    li      t2, 8                   # MSIE and MSIP are both bit 3
    csrw    mie, t2
    csrsi   mstatus, 8              # mstatus.MIE
    csrw    mip, t2                 # raise it (the CLINT does this for real)
    nop                             # the interrupt lands here
    csrci   mstatus, 8              # disable again before checking

    li      t2, 3
    li      t3, 1
    slli    t3, t3, 63              # the interrupt bit
    or      t2, t2, t3              # expected mcause = (1 << 63) | 3
    xor     t0, s1, t2
    seqz    t0, t0
    PASS_IF_NE 11

    j       done

# ---------------------------------------------------------------------------
# Trap handler.
#
# Records mcause/mepc/mtval, then decides how to resume:
#
#   exception -> mepc points AT the faulting instruction, so advance past it;
#                otherwise MRET would re-execute it and loop forever
#   interrupt -> mepc points at an instruction that has NOT run, so leave it
#                alone and quiet the interrupt source instead
# ---------------------------------------------------------------------------
    .align 2
handler:
    csrr    s1, mcause
    csrr    s2, mepc
    csrr    s3, mtval

    bltz    s1, handle_interrupt    # mcause bit 63 set -> interrupt

    addi    t5, s2, 4
    csrw    mepc, t5
    mret

handle_interrupt:
    csrw    mip, zero               # clear the pending bit so it stops firing
    mret

done:
    # Uninstall the handler before stopping. With mtvec set, EBREAK would
    # simply trap into the handler, which would advance past it and wander off
    # into whatever follows. Clearing mtvec makes the breakpoint fatal, which
    # is how the emulator reports a clean stop.
    csrw    mtvec, zero
    ebreak
