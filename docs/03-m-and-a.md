# Phase 3 — M and A extensions

Two extensions, both required before xv6 can boot: **M** (multiply and divide)
and **A** (atomics). `misa` now advertises `RV64IMA`.

The M extension is mostly unremarkable arithmetic with a handful of edge cases
that must be exact. The A extension is more interesting, because `LR`/`SC` is
where a single-hart emulator can be subtly and invisibly wrong.

---

## M: division never traps

This is the defining property, and it surprises people coming from x86 where
`idiv` by zero raises `#DE`.

| Case | Result |
|---|---|
| `DIV` by zero | `-1` |
| `DIVU` by zero | all ones (2⁶⁴−1) |
| `REM` by zero | the dividend |
| `REMU` by zero | the dividend |
| `DIV(INT64_MIN, -1)` | `INT64_MIN` |
| `REM(INT64_MIN, -1)` | `0` |

Every one of those is *specified*, not implementation-defined. The rationale is
hardware simplicity: no divider needs a fault path, and software that cares
checks its divisor first. It also means a divide can never be the thing that
traps in the middle of a critical section.

The overflow case is the one C++ cannot express. `INT64_MIN / -1` is
mathematically 2⁶³, which does not fit in a signed 64-bit result. In C++ that is
undefined behaviour, and on x86 it raises `SIGFPE` and kills the process — so it
must be special-cased before the division happens, not after:

```cpp
if (sb == 0) {
    write_reg(inst.rd, ~0ull);                  // -1
} else if (sa == INT64_MIN && sb == -1) {
    write_reg(inst.rd, static_cast<u64>(sa));   // wraps back to INT64_MIN
} else {
    write_reg(inst.rd, static_cast<u64>(sa / sb));
}
```

Signed division truncates toward zero, so `-7 / 2` is `-3` (not `-4`), and the
remainder takes the sign of the *dividend*.

## The MULH family

`MUL` gives the low 64 bits of the product, where signedness makes no difference
— that is why there is only one `MUL`. The upper half does depend on
signedness, so there are three instructions for it:

- `MULH` — signed × signed
- `MULHU` — unsigned × unsigned
- `MULHSU` — signed × unsigned

`MULHSU` looks like an oddity until you see what it is for: multi-word
arithmetic. The limbs of a bignum are unsigned, but the topmost limb carries the
sign, so a signed×unsigned multiply is exactly the operation the cross terms
need.

Getting the full 128-bit product needs `__int128`, a GCC/Clang extension rather
than standard C++. Every compiler that can build this project has it, and
hand-rolling a four-way 32×32 split would add bugs without buying portability we
would actually use.

The `*W` forms follow the usual RV64 rule — compute on 32 bits, sign-extend the
result to 64. Note there is no `MULHW`: the full product of two 32-bit values
fits in 64 bits, so `MULW` alone suffices.

---

## A: LR/SC and why it is not compare-and-swap

`LR` (load-reserved) loads a word and registers a *reservation* on its address.
`SC` (store-conditional) stores only if that reservation still holds, and reports
which happened: **0 in `rd` means the store succeeded**, non-zero means it did
not. Software retries on failure.

Why not a plain compare-and-swap? The **ABA problem**. CAS compares values, so it
cannot distinguish "nobody touched this" from "somebody changed it and changed it
back" — and for a pointer that was freed and reallocated, those are very
different situations. A reservation is broken by *any* intervening write, so
`LR`/`SC` notices what CAS cannot.

This is what xv6's spinlocks are built on, and the pattern is in
`examples/muldiv_atomic_selftest.S`:

```asm
acquire:
    lr.w    t3, (t1)
    bnez    t3, acquire     # someone holds it - spin
    sc.w    t4, t2, (t1)
    bnez    t4, acquire     # reservation lost - retry
```

### The bug a single-hart emulator invites

On a single hart, nothing can write memory behind our back. So a reservation
would never break, every `SC` would succeed, and every test would pass.

That is wrong in one way that matters. **A trap is a context switch.** If a
thread executes `LR`, takes a timer interrupt, and the scheduler runs a different
thread that also takes the lock, the first thread's `SC` must fail when it
eventually resumes — otherwise both threads believe they hold the lock and the
mutual exclusion the whole mechanism exists to provide is gone.

So `enter_trap()` clears the reservation. Real hardware does the same thing, for
the same reason.

This is exactly the sort of thing that passes every naive test, so
`tests/test_muldiv_atomic.cpp` tests it with a control: the same program with a
`NOP` between the `LR` and the `SC` succeeds, and swapping *only* that
instruction for an `ECALL` makes it fail. That isolates the trap as the cause
rather than the extra instruction.

The reservation is also consumed by `SC` whether it succeeded or not — leaving it
set would let a later `SC` succeed against a stale reservation.

## The AMOs

`AMOADD`, `AMOSWAP`, `AMOXOR`, `AMOOR`, `AMOAND`, `AMOMIN`, `AMOMAX`, `AMOMINU`,
`AMOMAXU`, each in `.W` and `.D`. Each loads from the address, applies the
operation with `rs2`, stores the result, and returns the **original** value in
`rd`.

Two details worth noting:

- `rd` gets the value from *before* the operation, and `rd` may be the same
  register as `rs2` — so the source has to be read before the destination is
  written.
- The signed and unsigned min/max forms disagree on the same bits: `AMOMIN` of
  `-5` and `3` gives `-5`, while `AMOMINU` gives `3`, because unsigned `-5` is
  enormous.

On real hardware the load-modify-store sequence is indivisible. Here we execute
one instruction at a time and nothing else touches memory, so it already is.

The `aq` and `rl` bits (instruction bits 26 and 25) are memory-ordering hints. A
single-hart in-order machine can ignore them for the same reason `FENCE` is a
no-op here.

### Atomics must be aligned

Unlike ordinary loads and stores — which this emulator supports misaligned,
matching QEMU — the spec **requires** natural alignment for atomics. A misaligned
one raises `StoreAMOAddressMisaligned` rather than being emulated. Real hardware
implements atomics in the cache controller, where an operation spanning two cache
lines cannot be made atomic at all.

---

## Testing

**`tests/test_muldiv_atomic.cpp`** — the division edge cases, the MULH family
including the signed/unsigned disagreements, `*W` sign extension, every AMO, and
the LR/SC reservation semantics with the trap control described above.

**`examples/muldiv_atomic_selftest.S`** — 15 checks through the real GNU
assembler, ending with a working spinlock acquire/release. `a0 == 0x7fff`.

Writing that self-test produced a nice illustration of why the assembler-level
tests earn their place. It failed with a store/AMO access fault at address
`0x800` — and the cause was that the `PASS_IF_NE` macro clobbers `t1`, which the
later checks also use as a live scratch pointer. After check 11 that left
`t1 = 1 << 11 = 0x800`, exactly the faulting address. A bug in the test, but the
kind of realistic register-pressure mistake that unit tests built from
hand-written encoders never produce.

Three assertions in earlier suites were superseded: `test_rv64i.cpp` asserted
`mul` traps as illegal, and `test_csr.cpp` asserted `misa` does not advertise M
or A. Both were true until this phase implemented them.

---

**Next:** phase 4 adds an ELF loader, a UART and the CLINT — the first phase
where a guest program can print something and where the timer interrupt has a
real source.
