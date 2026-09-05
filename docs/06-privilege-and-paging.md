# Phase 6 — Supervisor mode and Sv39 paging

The largest phase, and the last major piece before xv6 can boot. Two things
arrive together because neither is useful alone: a kernel needs somewhere less
privileged than machine mode to run, and it needs virtual memory to give
processes address spaces.

```
rv64ui 54/54   rv64um 13/13   rv64ua 19/19   rv64mi 11/11   rv64si 4/4
101/101 passed
```

Every riscv-test the emulator can build now passes — including `rv64mi/illegal`,
which phase 5 had to exclude for want of supervisor mode.

---

## Three privilege levels

| | |
|---|---|
| **M** machine | firmware. Can do everything. Where the hart starts. |
| **S** supervisor | where a kernel runs. Can manage page tables. |
| **U** user | where processes run. No CSRs, no privileged instructions. |

The rules are enforced in two places. CSR access is checked against the
privilege encoded in bits [9:8] of the CSR *address*, so no per-register table
is needed. Privileged instructions check `priv` directly — `SRET` from user mode
is illegal, and so is `SFENCE.VMA`.

## sstatus, sie and sip are views, not copies

This is the detail most worth getting right, and the easiest to get wrong.

`sstatus` is not a register. It is a **window onto `mstatus`** showing only the
bits supervisor mode is allowed to see:

```cpp
case csr::SSTATUS: return raw_[csr::MSTATUS] & csr::SSTATUS_MASK;
```

Writing it writes through to the same bits:

```cpp
raw_[csr::MSTATUS] = (raw_[csr::MSTATUS] & ~csr::SSTATUS_MASK) |
                     (value & csr::SSTATUS_MASK);
```

Modelling them as separate storage is a classic emulator bug, and the symptom is
brutal to diagnose: a kernel clears `sstatus.SIE` to enter a critical section,
the write lands in a copy that nothing consults, and interrupts keep arriving
inside code that has every right to assume they cannot. `sie`/`sip` work the
same way over `mie`/`mip`.

The masking is also a security boundary: a supervisor writing all-ones to
`sstatus` must not thereby set `mstatus.MIE` or `MPP`. `tests/test_supervisor.cpp`
checks exactly that.

## Trap delegation

Without delegation, every page fault and every system call in a running system
would trap to machine mode, which would then have to forward it to the kernel
that actually handles it. `medeleg` and `mideleg` let those go straight to
supervisor mode.

```cpp
const bool to_supervisor = (priv <= PRIV_SUPERVISOR) && deleg;
```

Note the first half. **Traps taken in machine mode are never delegated** —
there is nothing more privileged to delegate *from*. A machine-mode `ECALL`
lands in `mtvec` however `medeleg` is set.

When a trap is delegated it uses the supervisor set throughout: `sepc`,
`scause`, `stval`, `stvec`, and `sstatus.SPP`/`SPIE`/`SIE`. `SPP` is a single
bit because a supervisor trap can only have come from supervisor or user mode.

`SRET` mirrors `MRET` over those fields. Both reset the saved privilege to the
least-privileged mode afterwards — leaving `MPP` at machine would let a later
`MRET` escalate.

## Sv39

A 39-bit address space through three levels of page table:

```
 38      30 29      21 20      12 11         0
+----------+----------+----------+------------+
|  VPN[2]  |  VPN[1]  |  VPN[0]  |   offset   |
+----------+----------+----------+------------+
```

Each 9-bit index selects one of 512 entries; 512 × 8 bytes is exactly one 4 KiB
page, which is why the numbers are what they are. The walk starts at the root
table `satp` names, and each entry either points at the next level or is a
**leaf** that ends the walk.

A leaf found early is a superpage: at level 1 it maps 2 MiB, at level 2 it maps
1 GiB. That is how a kernel maps its own large regions without building
thousands of entries. A superpage must be physically aligned to the size it
maps — the low page-number bits have to be zero — and a misaligned one is a
fault, not a truncated mapping.

**`R`, `W`, `X` all clear means "pointer to the next level".** `W` without `R`
is *reserved*, not a write-only page, and faults.

### The sign-extension rule creates the memory map

Bits 63:39 of a virtual address must all equal bit 38. An address that violates
this is not merely unmapped, it is unrepresentable.

That single rule is what produces the familiar layout of every 64-bit OS: low
addresses for user space, high addresses for the kernel, and a vast hole between
them. The hole is not a convention anyone chose — it is the range of addresses
the hardware cannot express.

## Permission bits that exist for security, not addressing

Three checks have nothing to do with locating memory.

**The `U` bit** marks a page as user-accessible. Supervisor mode touching a user
page is *forbidden by default*. That is deliberate hardening: without it, a
kernel tricked into dereferencing a user-supplied pointer would read or write
user memory with its own privileges.

**`SUM`** ("permit Supervisor User Memory access") lifts that restriction for
the windows where a kernel genuinely means to do it — `copyin`, `copyout`. It
applies to loads and stores **only, never to instruction fetch**: the kernel must
never execute user pages whatever `SUM` says. The test checks that specifically.

**`MXR`** ("Make eXecutable Readable") lets a load succeed on an execute-only
page. Kernels use it to inspect their own code.

### Accessed and dirty bits

The spec allows an implementation either to fault so software can set `A`/`D`,
or to set them itself. Setting them is simpler and is what most hardware does.
A load sets `A`; a store sets `A` and `D`.

## The TLB, and why SFENCE.VMA must exist

Every translation costs three extra memory reads. Doing that on every guest
access is the difference between a kernel booting in seconds and in minutes, so
translations are cached, keyed by `(VPN, ASID)`.

That cache is precisely why `SFENCE.VMA` exists. **Nothing about writing a page
table entry in memory tells the hardware to forget what it cached.** A kernel
that unmaps a page and does not fence would find the old mapping still works —
which is a security hole, not just a stale-data bug.

Writing `satp` flushes too: changing the root table changes the entire address
space.

## The virtualisation trap controls

`mstatus.TVM`, `TSR` and `TW` make operations that supervisor mode would
normally perform freely trap to machine mode instead, so firmware or a
hypervisor can intercept them. `TVM` covers `SFENCE.VMA` **and access to
`satp`** — both are page-table management. `TSR` covers `SRET`.

These are worth mentioning because of how the last failing test was found. The
emulator *checked* `TVM` and `TSR` in `SFENCE.VMA` and `SRET`, but the bits were
not in `MSTATUS_MASK`, so nothing could ever set them — the checks were dead
code that looked correct. `rv64mi/illegal` sets `TVM` and expects the next
`SFENCE.VMA` to trap; it did not, and that is what exposed the gap.

That is the second time the reference suite has caught something that reads
perfectly well on the page.

## A bug in the test environment, not the emulator

`rv64si/ma_fetch` failed with what looked like a wrong `sepc`. It was not. The
trap entry in `tests/riscv-tests-env/riscv_test.h` did:

```asm
la  t0, stvec_handler
jr  t0
```

— clobbering `t0` before the handler ran. The tests inspect their registers
after a trap, and `ma_fetch` compares `t0` against `sepc + 4`. The emulator's
`sepc` was correct all along.

The fix resolves the weak handler symbol at `_start`, where no test has begun
and clobbering is harmless, and points `stvec`/`mtvec` straight at it. A trap
entry that touches no registers is also simply the right design.

---

## Testing

**`tests/test_supervisor.cpp`** — 78 checks: the view semantics and their
masking, privilege transitions, delegation (including that machine-mode traps
are never delegated), the Sv39 walk, every permission rule, `SUM` and `MXR`,
`A`/`D` updates, the non-canonical address rule, superpage alignment, TLB
caching and flushing, and `satp`'s rejection of Sv48.

**riscv-tests** — `rv64si` now builds and passes 4/4, and `rv64mi` is complete at
11/11. The environment gained a supervisor entry path (`RVTEST_RV64S` drops into
S-mode via `MRET` with everything delegated).

Three `rv64si` tests still do not build (`csr`, `dirty`, `icache-alias`); they
need environment features beyond what a bare-metal "p" environment provides.
Two `rv64mi` tests remain excluded for optional extensions (debug triggers, PMP).

---

**Next:** phase 7 adds the PLIC and a virtio block device — and boots xv6.
