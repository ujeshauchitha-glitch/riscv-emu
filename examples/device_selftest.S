# ---------------------------------------------------------------------------
# Device self-test (phase 4): UART, CLINT and syscon.
#
# Prints a message over the console, arms the timer, waits for the interrupt to
# arrive, then powers the machine off through syscon - the first program here
# that stops on its own terms rather than by trapping.
#
# One bit per check in a0; expected 0x1f. The console output is checked too.
# ---------------------------------------------------------------------------

    .equ UART0,    0x10000000
    .equ CLINT,    0x02000000
    .equ MTIMECMP, CLINT + 0x4000
    .equ MTIME,    CLINT + 0xbff8
    .equ SYSCON,   0x00100000
    .equ POWEROFF, 0x5555

    .section .text
    .globl _start
_start:
    li      a0, 0
    li      s2, 0                   # set by the timer handler

    la      t0, handler
    csrw    mtvec, t0

# s0 is the macro's scratch: t-registers hold live pointers below.
.macro PASS_IF_NE n
    beqz    t0, 1f
    li      s0, (1 << \n)
    or      a0, a0, s0
1:
.endm

# 0: the UART reports its transmitter ready (LSR bit 5).
    li      t1, UART0
    lbu     t2, 5(t1)
    andi    t0, t2, 0x20
    PASS_IF_NE 0

# 1: writing bytes to the UART puts them on the console.
    la      t3, message
puts:
    lbu     t2, 0(t3)
    beqz    t2, puts_done
    sb      t2, 0(t1)
    addi    t3, t3, 1
    j       puts
puts_done:
    li      t0, 1
    PASS_IF_NE 1

# 2: mtime advances as instructions retire.
    li      t1, MTIME
    ld      t2, 0(t1)
    nop
    nop
    ld      t3, 0(t1)
    sltu    t0, t2, t3              # strictly greater than before
    PASS_IF_NE 2

# 3: mtimecmp round-trips a 64-bit value.
    li      t1, MTIMECMP
    li      t2, 0x123456789
    sd      t2, 0(t1)
    ld      t3, 0(t1)
    xor     t0, t2, t3
    seqz    t0, t0
    PASS_IF_NE 3

# 4: the timer interrupt actually fires. Set a deadline a little ahead of now,
#    enable it, and spin until the handler reports it arrived.
    li      t1, MTIME
    ld      t2, 0(t1)
    addi    t2, t2, 50
    li      t1, MTIMECMP
    sd      t2, 0(t1)

    li      t2, 0x80                # MTIE is bit 7
    csrw    mie, t2
    csrsi   mstatus, 8              # mstatus.MIE
wait_timer:
    beqz    s2, wait_timer          # the handler sets s2
    csrci   mstatus, 8
    li      t0, 1
    PASS_IF_NE 4

# Stop the machine the way a real guest does.
    li      t1, SYSCON
    li      t2, POWEROFF
    sw      t2, 0(t1)
    j       .                       # never reached

# ---------------------------------------------------------------------------
    .align 2
handler:
    csrr    t4, mcause
    bgez    t4, not_ours            # exceptions are not expected here

    # A timer interrupt is acknowledged by moving the deadline out of reach.
    # MTIP is owned by the CLINT and cannot be cleared by writing mip.
    li      t5, MTIMECMP
    li      t6, -1
    sd      t6, 0(t5)
    li      s2, 1
    mret

not_ours:
    # Step over the faulting instruction so a mistake shows up as a wrong a0
    # rather than an endless trap loop.
    csrr    t5, mepc
    addi    t5, t5, 4
    csrw    mepc, t5
    mret

    .section .data
    .align 3
message:
    .asciz "uart works\n"
