// Test environment for the official riscv-tests suite.
//
// riscv-tests keeps its environment in a separate `riscv-test-env` submodule.
// This is a self-contained replacement for the "p" (physical, bare-metal)
// environment, so the suite can be built without that extra dependency.
//
// A test is a sequence of numbered checks. Each sets TESTNUM (the gp register)
// to its own number before running, and branches to `fail` if the result is
// wrong. At the end TEST_PASSFAIL expands to RVTEST_PASS/RVTEST_FAIL.
//
// The result is reported through the HTIF convention, which the emulator
// understands: write a non-zero value to the `tohost` symbol.
//
//     1              every check passed
//     (n << 1) | 1   check n failed
//
// So the low bit says "finished" and the rest identifies the failure. That is
// why passing is 1 and not 0.

#ifndef _ENV_RISCV_TEST_H
#define _ENV_RISCV_TEST_H

// The tests keep the current check number in gp throughout.
#define TESTNUM gp

// Privilege-mode selectors. Every test here runs in machine mode, which is the
// only mode implemented so far, so these are no-ops. RVTEST_RV64S would need
// supervisor mode (phase 6) and RVTEST_RV64UF floating point (phase 8); tests
// using those are not built yet.
#define RVTEST_RV64U
#define RVTEST_RV64M
#define RVTEST_RV64S
#define RVTEST_RV32U
#define RVTEST_RV32M

// Report success: tohost = 1.
#define RVTEST_PASS                                                            \
        fence;                                                                 \
        li      TESTNUM, 1;                                                    \
        la      t0, tohost;                                                    \
        sd      TESTNUM, 0(t0);                                                \
99:     j       99b;

// Report failure: tohost = (TESTNUM << 1) | 1, so the emulator can name the
// check that failed.
#define RVTEST_FAIL                                                            \
        fence;                                                                 \
        slli    TESTNUM, TESTNUM, 1;                                           \
        ori     TESTNUM, TESTNUM, 1;                                           \
        la      t0, tohost;                                                    \
        sd      TESTNUM, 0(t0);                                                \
98:     j       98b;

// Entry point, plus the default trap handler.
//
// A test that expects to take traps defines its own `mtvec_handler`; the weak
// symbol resolves to 0 when it does not, and any trap is then a failure. That
// is the behaviour the machine-mode tests rely on to check that an operation
// which should NOT trap indeed does not.
#define RVTEST_CODE_BEGIN                                                      \
        .section .text.init;                                                   \
        .align  6;                                                             \
        .weak   mtvec_handler;                                                 \
        .globl  _start;                                                        \
_start:                                                                        \
        la      t0, __default_trap;                                            \
        csrw    mtvec, t0;                                                     \
        li      TESTNUM, 0;                                                    \
        j       __test_start;                                                  \
                                                                               \
        .align  2;                                                             \
__default_trap:                                                                \
        la      t0, mtvec_handler;                                             \
        beqz    t0, __trap_is_failure;                                         \
        jr      t0;                                                            \
__trap_is_failure:                                                             \
        /* TESTNUM == 0 would encode as 1, which means "passed" - use a         \
           distinctive number instead so an early trap is not read as a pass */\
        bnez    TESTNUM, 97f;                                                  \
        li      TESTNUM, 1023;                                                 \
97:     fence;                                                                 \
        slli    TESTNUM, TESTNUM, 1;                                           \
        ori     TESTNUM, TESTNUM, 1;                                           \
        la      t0, tohost;                                                    \
        sd      TESTNUM, 0(t0);                                                \
96:     j       96b;                                                           \
                                                                               \
        .align  2;                                                             \
__test_start:

// The HTIF communication words. The emulator finds `tohost` by name in the
// ELF symbol table and watches it.
#define RVTEST_CODE_END                                                        \
        .section .tohost, "aw", @progbits;                                     \
        .align  6;                                                             \
        .global tohost;                                                        \
tohost: .dword 0;                                                              \
        .align  6;                                                             \
        .global fromhost;                                                      \
fromhost: .dword 0;

#define RVTEST_DATA_BEGIN  .section .data; .align 4;
#define RVTEST_DATA_END

// ---------------------------------------------------------------------------
// Architectural constants.
//
// The tests reference these by name; upstream they come from the environment's
// encoding.h. Only the ones the suites actually use are defined here, and the
// values are fixed by the privileged specification.
// ---------------------------------------------------------------------------

// Exception causes (mcause, with the interrupt bit clear).
#define CAUSE_MISALIGNED_FETCH      0x0
#define CAUSE_FETCH_ACCESS          0x1
#define CAUSE_ILLEGAL_INSTRUCTION   0x2
#define CAUSE_BREAKPOINT            0x3
#define CAUSE_MISALIGNED_LOAD       0x4
#define CAUSE_LOAD_ACCESS           0x5
#define CAUSE_MISALIGNED_STORE      0x6
#define CAUSE_STORE_ACCESS          0x7
#define CAUSE_USER_ECALL            0x8
#define CAUSE_SUPERVISOR_ECALL      0x9
#define CAUSE_MACHINE_ECALL         0xb
#define CAUSE_FETCH_PAGE_FAULT      0xc
#define CAUSE_LOAD_PAGE_FAULT       0xd
#define CAUSE_STORE_PAGE_FAULT      0xf

// Privilege levels.
#define PRV_U 0
#define PRV_S 1
#define PRV_M 3

// mstatus / sstatus fields.
#define MSTATUS_SIE   0x00000002
#define MSTATUS_MIE   0x00000008
#define MSTATUS_SPIE  0x00000020
#define MSTATUS_MPIE  0x00000080
#define MSTATUS_SPP   0x00000100
#define MSTATUS_MPP   0x00001800
#define MSTATUS_MPRV  0x00020000
#define MSTATUS_SUM   0x00040000
#define MSTATUS_MXR   0x00080000
#define MSTATUS_TVM   0x00100000
#define MSTATUS_TW    0x00200000
#define MSTATUS_TSR   0x00400000

#define SSTATUS_SIE   MSTATUS_SIE
#define SSTATUS_SPIE  MSTATUS_SPIE
#define SSTATUS_SPP   MSTATUS_SPP
#define SSTATUS_SUM   MSTATUS_SUM
#define SSTATUS_MXR   MSTATUS_MXR

// Interrupt pending / enable bits. The bit position is the cause number.
#define MIP_SSIP  (1 << 1)
#define MIP_MSIP  (1 << 3)
#define MIP_STIP  (1 << 5)
#define MIP_MTIP  (1 << 7)
#define MIP_SEIP  (1 << 9)
#define MIP_MEIP  (1 << 11)
#define SIP_SSIP  MIP_SSIP
#define SIP_STIP  MIP_STIP

// Debug-trigger fields. The trigger module is optional and not implemented
// here, so tests using these detect its absence and skip themselves.
#define MCONTROL_M       (1 << 6)
#define MCONTROL_EXECUTE (1 << 2)
#define MCONTROL_STORE   (1 << 1)
#define MCONTROL_LOAD    (1 << 0)

#endif
