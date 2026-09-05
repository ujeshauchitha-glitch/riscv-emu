# Phase 2 — CSRs and M-mode traps

This is the phase where the emulator stops being a calculator and becomes a
machine an operating system could run on.

Until now a trap stopped everything. Now it does what real hardware does:
records why it happened, saves enough state to get back, and jumps to a handler
the guest installed. `ECALL` — which is how every system call in every RISC-V OS
is made — finally behaves like a system call instead of a halt.

---

## What a CSR is

Control and Status Registers are a second register file, separate from
`x0`–`x31`, addressed by a 12-bit number and reachable only through six
dedicated instructions. They hold everything the integer registers cannot: the
current privilege mode, where the trap handler lives, why the last trap
happened, which interrupts are enabled, and (from phase 6) where the page tables
are.

Two things are encoded in the CSR *address itself*, which is a genuinely elegant
piece of design:

```
  bits [11:10]   00, 01, 10 = read/write        11 = read-only
  bits  [9:8]    lowest privilege level allowed to touch it
```

So a write to any CSR numbered `0xC00`–`0xFFF` is illegal, and a user-mode read
of a machine CSR is illegal, without any per-register table needing to say so.
`csr::is_read_only()` and `csr::min_privilege()` in `include/csr.hpp` are one
line each.

## Registers implemented

| CSR | Purpose |
|---|---|
| `mstatus` | Global interrupt enable, saved state across traps |
| `mtvec` | Trap handler address + mode |
| `mepc` | Where to resume |
| `mcause` | Why we trapped |
| `mtval` | Extra detail: faulting address, or instruction bits |
| `mie` / `mip` | Interrupt enable / pending |
| `misa` | Which extensions exist |
| `mhartid` | Which hart this is |
| `mscratch` | Free scratch space for the handler |
| `medeleg` / `mideleg` | Delegation — stored now, honoured in phase 6 |
| `mcycle` / `minstret` + shadows | Counters |

Anything else raises an illegal-instruction trap. That is not a limitation: it
is the actual mechanism by which software detects optional features. Try the
CSR, catch the trap, conclude it is absent.

---

## The trap sequence

Every step is fixed by the spec, and each one matters:

```
mepc    <- the address to resume at
mcause  <- why we trapped
mtval   <- extra detail
MPIE    <- MIE                 save whether interrupts were enabled
MIE     <- 0                   disable them
MPP     <- current privilege   remember where to return to
pc      <- mtvec
```

`MPIE` and `MPP` are the whole reason a return is possible. Without somewhere to
stash the previous interrupt-enable and the previous privilege level, `MRET`
would have nothing to restore and there would be no way back.

Clearing `MIE` on entry is what stops a handler being immediately re-entered by
the same source before it has had a chance to quiet it.

`MRET` exactly reverses it:

```
MIE       <- MPIE
MPIE      <- 1                 set, not cleared - easy to get backwards
privilege <- MPP
MPP       <- least-privileged supported mode
pc        <- mepc
```

## Where `mepc` points, and why it differs

This is the single most important detail in the phase.

**For an exception, `mepc` points *at* the faulting instruction — not past it.**

That is right for a page fault: the handler maps the missing page and returns,
and the instruction retries and now succeeds. Advancing past it would skip the
very instruction the fault existed to enable.

But it means that for `ECALL` the handler must add 4 itself before returning,
or `MRET` re-executes the `ECALL` and loops forever. That `addi mepc, mepc, 4`
in a syscall handler is not a quirk — it is the mechanism.

**For an interrupt, `mepc` points at an instruction that has *not run*.**

An interrupt is not caused by an instruction. It is an external event that
happens *between* them. So the instruction at the PC must not execute, `mepc`
records it, and `MRET` resumes exactly there — nothing is skipped and nothing is
repeated.

The handler in `examples/trap_selftest.S` shows both paths, choosing between
them by testing the sign bit of `mcause`:

```asm
handler:
    csrr    s1, mcause
    csrr    s2, mepc
    bltz    s1, handle_interrupt    # bit 63 set -> interrupt
    addi    t5, s2, 4               # exception: step over it
    csrw    mepc, t5
    mret
handle_interrupt:
    csrw    mip, zero               # quiet the source; mepc untouched
    mret
```

## Interrupts are checked before the fetch

In `Cpu::step()` the interrupt check happens *before* fetching, not after
executing. That placement is the direct consequence of the paragraph above: if
an interrupt is taken, the instruction at the PC must not run at all.

Three conditions gate delivery:

```cpp
mstatus.MIE            // globally enabled
&& mie  bit set        // this source enabled
&& mip  bit set        // this source pending
```

Priority is fixed by the spec and is *not* bit order: external, then software,
then timer, machine before supervisor.

Nothing raises an interrupt yet — `mip` is driven by devices, and the CLINT
arrives in phase 4. The machinery is here and tested by writing `mip` directly,
which is what the CLINT will do for real.

## `mcause`: the top bit distinguishes the two

`mcause` holds a cause number, with **bit 63 set for an interrupt and clear for
an exception**. That is why the same register can describe both, and why a
handler's first move is usually `bltz mcause` — a negative value means
interrupt.

## `mtvec` modes, and a trap worth knowing about

`mtvec` holds a base address in its upper bits and a mode in the low two:

- **Direct** (mode 0) — every trap goes to the base address.
- **Vectored** (mode 1) — *interrupts* go to `base + 4 × cause`, so each source
  gets its own entry in a jump table.

The catch: **in vectored mode, exceptions still go to the base address.** Only
interrupts are vectored. Applying the multiply to exceptions sends a page fault
somewhere into the middle of the table. `tests/test_csr.cpp` covers this
explicitly.

The mode field is WARL, so writing a reserved mode (2 or 3) is coerced rather
than stored.

## Write masking, and why it is not optional

Most CSR fields are WARL ("write any, read legal") or WPRI ("reserved, writes
ignored"). Storing whatever the guest supplies would let it set bits for
hardware we do not implement and then read them back — something real hardware
would never do, and exactly the kind of difference that makes a kernel take a
path we cannot support.

So writes are masked per register (`CsrFile::write_mask`):

- `mstatus` keeps only `MIE`, `MPIE`, `MPP` for now; phase 6 widens it
- `mepc` has its low bits hardwired to zero — bit 0 always, and bit 1 too while
  instructions are all 32-bit (phase 8 relaxes this when the C extension lands)
- `misa` ignores writes: we do not support disabling extensions
- `mie`/`mip` keep only the machine-level bits until supervisor mode exists
- `medeleg`/`mideleg` are stored but inert until phase 6 — early boot code
  writes them, and reading back what you wrote is less surprising than not

## The six CSR instructions, and when they *don't* access

```
CSRRW  rd, csr, rs1     rd = csr; csr = rs1
CSRRS  rd, csr, rs1     rd = csr; csr |= rs1
CSRRC  rd, csr, rs1     rd = csr; csr &= ~rs1
CSRRWI/CSRRSI/CSRRCI    the same, with a 5-bit zero-extended immediate
```

Each is an atomic read-modify-write, which is why setting one bit of `mstatus`
needs no lock even on a real multi-hart machine.

The subtlety is suppression:

- `CSRRW`/`CSRRWI` with `rd == x0` **must not read** the CSR
- `CSRRS`/`CSRRC` (and immediate forms) with a zero source **must not write**

This matters because some CSRs have access side effects — reading a PLIC claim
register acknowledges an interrupt — and a suppressed access that happened
anyway would silently consume events.

It also explains a nice consequence: `csrr rd, csr` assembles to `CSRRS` with
`rs1 = x0`, so it is a pure read and is legal on a **read-only** CSR. That is
how software reads `mhartid`. Both halves are tested.

## Traps are fatal while `mtvec` is zero

A hart comes out of reset with `mtvec = 0`. A trap would vector to address 0,
fault on the unmapped fetch, and vector to 0 again — forever. Real hardware does
precisely this, and it presents as a hang with no clue what went wrong.

So until a guest installs a handler, a trap stops the emulator and reports the
cause, with the PC left on the faulting instruction. Once `mtvec` is non-zero,
traps dispatch normally. This is a debugging affordance, not spec behaviour, and
it is flagged as such (`Cpu::trap_fatal_without_handler`). Phase 4's syscon
device gives guests a proper way to exit.

## WFI is a no-op

The spec explicitly permits it: `WFI` is a hint, and software must re-check the
condition it was waiting on regardless of why the instruction completed. Running
the wait loop hot costs only host CPU. Phase 4 can make it genuinely idle once
there is a timer to sleep until.

---

## Testing

**`tests/test_csr.cpp`** — 50 checks covering CSR semantics, the suppression
rules, per-register masking, the full trap → handler → `MRET` round trip,
vectored-vs-direct dispatch, and interrupt gating and priority.

**`examples/trap_selftest.S`** — 12 checks through the real GNU assembler, with
an actual trap handler. It causes an `ECALL`, an illegal instruction, an illegal
CSR access and a software interrupt, and verifies `mcause`, `mepc` and `mtval`
for each. `a0 == 0xfff` means all passed.

Writing these turned up a mistake worth recording, because it is a real property
of the system rather than a typo: the earlier tests used `EBREAK` as a "stop the
machine" marker. That worked only because `mtvec` was zero. Once a test installs
a handler, `EBREAK` traps into it like anything else — and a handler that itself
ends in `EBREAK` re-enters forever, quietly overwriting `mcause` and `mepc`
along the way. Test programs now end with a self-loop (`HALT()` in
`tests/machine.hpp`), which stops making progress without touching any
architectural state.

Two assertions in `tests/test_rv64i.cpp` also had to change: phase 1 asserted
that CSR instructions and `MRET` trap as illegal. They no longer do, because
this phase implements them. That is the test being superseded, not a regression.

```bash
cmake -S . -B build && cmake --build build
cd build && ctest --output-on-failure
```

---

**Next:** phase 3 adds the M extension (multiply and divide) and the A extension
(atomics and `LR`/`SC`). xv6's spinlocks are built on the atomics, so they have
to be right.
