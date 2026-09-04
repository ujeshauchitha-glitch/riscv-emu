# RISC-V Emulator — Phase Log

The goal: an RV64 emulator that boots a real operating system — **xv6-riscv**
first, then **Linux**.

Target ISA for xv6: `RV64IMA_Zicsr_Zifencei`, Sv39 paging, M/S/U privilege
modes. Linux additionally needs the C and F/D extensions, a device tree, and SBI.

| # | Phase | Status |
|---|-------|--------|
| 1 | Project setup | ✅ done |
| 2 | CPU state — registers, PC | ✅ done |
| 3 | Fetch / decode / ADDI | ✅ done |
| 0 | Foundation restructure + defect fixes | ✅ done |
| 1 | Complete RV64I | ✅ done |
| 2 | Zicsr + M-mode traps | ✅ done |
| 3 | M and A extensions | ✅ done |
| 4 | ELF loader, UART, CLINT — first output | ⬜ next |
| 5 | riscv-tests + CI | ⬜ |
| 6 | S-mode + Sv39 MMU | ⬜ |
| 7 | PLIC + virtio-blk — **boot xv6** | ⬜ |
| 8 | Linux prerequisites (C, F/D, DTB, SBI) | ⬜ |
| 9 | **Boot Linux** | ⬜ |

The first three phases were numbered before the roadmap existed. Phase 0 is
numbered as it is because it is foundational work that logically precedes the
instruction set, even though it was done fourth.

---

## Phase 1: Project setup — done

- CMake build system.
- Folder structure: `src/`, `include/`, `docs/`.

*(An earlier version of this log claimed a `tests/` directory was created here.
It was not; it arrived in phase 0 below.)*

## Phase 2: CPU state — done

- 32 × 64-bit integer registers, `x0` hardwired to zero.
- Program counter.

## Phase 3: Fetch / decode / ADDI — done

- Instruction fetch from memory.
- Field extraction and a first executing instruction, ADDI.

## Phase 0: Foundation restructure + defect fixes — done

No new instructions. This phase fixed what was committed and put an architecture
in place capable of carrying the rest of the roadmap.

**Defects fixed**

- Unbounded array indexing in `Memory` — any out-of-range address read or wrote
  host memory outside the array, and since the array was stack-allocated an
  out-of-range write corrupted the host stack.
- OP-IMM dispatched on opcode alone, so `SLTI`, `SLTIU`, `XORI`, `ORI`, `ANDI`,
  `SLLI`, `SRLI` and `SRAI` all silently executed as `ADDI`.
- The PC advanced past unrecognised instructions instead of trapping.
- The I-type immediate was decoded unconditionally, before the instruction
  format was known.
- Seven lines of `stdout` tracing per instruction, unconditionally.

**Structural changes**

- `Memory` (flat array based at 0) replaced by `Bus` + `Device`, with DRAM at
  `0x8000_0000` and MMIO regions reserved for the devices to come.
- `execute(opcode, rd, rs1, imm)` replaced by `DecodedInst`, which carries every
  field. The old signature could not represent R-, S- or B-type instructions at
  all, so nothing beyond I-type could have been added.
- Traps modelled as return values (`Result<T>` / `Status`) rather than C++
  exceptions.
- Split into compilation units; core built as a static library so tests and the
  executable share it.
- `tests/` created for real, with 3 suites wired into CTest.

**Docs:** [`00-architecture.md`](00-architecture.md)

## Phase 1: Complete RV64I — done

Every base integer instruction is implemented and tested. The emulator now runs
real compiled code.

**Added:** `LUI`, `AUIPC`, `JAL`, `JALR`, all six branches, all seven loads, all
four stores, the ten register-register ALU ops, the nine RV64 `*W` forms,
`FENCE`/`FENCE.I`, `ECALL`, `EBREAK`.

**Fine print handled**

- `*W` results are sign-extended from 32 to 64 bits, and `*W` inputs ignore bits
  32–63. Zero-extending instead passes every small-number test and then breaks a
  kernel thousands of instructions after the actual mistake.
- 64-bit shifts mask the shift amount to 6 bits and select on `funct6`; 32-bit
  shifts mask to 5 and select on `funct7`. Masking is required, not optional —
  an unmasked shift by ≥ 64 is undefined behaviour in C++.
- `JALR` clears bit 0 *after* the addition, and computes its target before
  writing `rd` (they may be the same register).
- A misaligned jump target traps on the *jump*, not on the target, so the PC
  left behind points at the instruction that caused it.
- A faulting load leaves its destination register untouched.
- `funct7 == 0x01` (M extension) and `funct3 != 0` on SYSTEM (CSR ops) trap as
  illegal rather than being mistaken for `ADD` or `ECALL`.

**Testing:** 95 unit-test checks, plus a 14-check bare-metal self-test in
`examples/rv64i_selftest.S` assembled by the real GNU assembler and run under
CTest — validating the decoder against an independent implementation rather than
against our own encoders.

**Docs:** [`01-rv64i.md`](01-rv64i.md)

## Phase 2: Zicsr + M-mode traps — done

A trap now does what hardware does: records why it happened, saves enough state
to return, and jumps to a handler the guest installed. `ECALL` behaves like a
system call instead of halting the machine.

**Added:** the CSR file (`mstatus`, `mtvec`, `mepc`, `mcause`, `mtval`,
`mie`/`mip`, `misa`, `mhartid`, `mscratch`, `medeleg`/`mideleg`, counters), the
six CSR instructions, `MRET`, `WFI`, trap entry and dispatch, and interrupt
delivery with spec priority ordering.

**Fine print handled**

- `mepc` points *at* a faulting instruction (so a page fault can retry) but at
  the *not-yet-run* instruction for an interrupt. A syscall handler's
  `addi mepc, mepc, 4` is the mechanism, not a quirk.
- Interrupts are checked before the fetch, because an interrupt happens between
  instructions rather than being caused by one.
- In vectored `mtvec` mode only *interrupts* are vectored; exceptions still go
  to the base address.
- `CSRRW` with `rd == x0` suppresses the read; `CSRRS`/`CSRRC` with a zero
  source suppress the write. Some CSRs have access side effects, so a
  suppressed access that happened anyway would consume events. This is also why
  `csrr` works on read-only CSRs.
- Writes are masked per register: `mstatus` keeps only implemented bits,
  `mepc` has its low bits hardwired to zero, `misa` ignores writes, `mtvec`
  coerces a reserved mode.
- Unimplemented CSRs trap — that is how software probes for features.
- While `mtvec` is zero a trap stops the emulator rather than looping to
  address 0 forever. A debugging affordance, flagged as such.

**Testing:** 50 unit-test checks, plus a 12-check trap self-test in
`examples/trap_selftest.S` with a real handler, assembled by GNU `as`.

**Docs:** [`02-csrs-and-traps.md`](02-csrs-and-traps.md)

## Phase 3: M and A extensions — done

`misa` now advertises RV64IMA. Both extensions are prerequisites for xv6.

**Added:** `MUL`, `MULH`, `MULHSU`, `MULHU`, `DIV`, `DIVU`, `REM`, `REMU` and
their `*W` forms; `LR`/`SC` in `.W`/`.D`; all nine AMOs in `.W`/`.D`.

**Fine print handled**

- RISC-V division *never traps*. Divide-by-zero and signed overflow have
  specified results. `INT64_MIN / -1` is the case C++ cannot express — it is
  undefined behaviour and raises SIGFPE on x86 — so it is special-cased before
  the division happens.
- `MULHSU` exists for multi-word arithmetic, where a bignum's limbs are unsigned
  but the top one carries the sign.
- A trap clears the LR/SC reservation. On a single hart nothing can break a
  reservation, so every SC would otherwise succeed — but a trap is a context
  switch, and a thread must not inherit another's reservation. Tested with a
  control that isolates the trap as the cause.
- The reservation is consumed by SC whether or not it succeeded.
- Atomics must be naturally aligned, unlike ordinary loads and stores.
- Signed and unsigned min/max AMOs disagree on the same bits.

**Testing:** unit tests for every edge case above, plus a 15-check self-test in
`examples/muldiv_atomic_selftest.S` ending with a working LR/SC spinlock.

**Docs:** [`03-m-and-a.md`](03-m-and-a.md)
