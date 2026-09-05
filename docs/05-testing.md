# Phase 5 — riscv-tests and CI

Everything before this phase was checked against tests written alongside the
emulator. That catches mistakes, but it cannot catch a *misreading of the
specification*, because the test and the code would share it. This phase brings
in the suite the RISC-V project itself maintains.

```
== rv64ui ==   54/54 passed
== rv64um ==   13/13 passed
== rv64ua ==   19/19 passed
== rv64mi ==   10/10 passed
96/96 passed, 3 excluded (unimplemented features)
```

It found a real bug on the first run. More on that below.

---

## Why this matters more than the tests written so far

The unit tests in `tests/` build their programs with encoders written separately
from `src/decoder.cpp`, and the self-tests in `examples/` go further by using
the real GNU assembler. Both are genuine independent checks of the *encoding*.

Neither is an independent check of the *semantics*. If I misread what `SRAIW`
does, I would write the emulator and the test to match, and both would agree.

`riscv-tests` is written by the people who wrote the specification. When
`rv64ui-add` passes, that is evidence about the ISA, not about my
self-consistency. This is also why phase 6's MMU work is now tractable: with the
instruction set independently verified, a kernel that misbehaves is far more
likely to be an MMU bug than a stale `ADDIW` mistake.

## What is run

| Suite | Covers | Result |
|---|---|---|
| `rv64ui` | the base integer instruction set | 54/54 |
| `rv64um` | multiply and divide | 13/13 |
| `rv64ua` | atomics, LR/SC | 19/19 |
| `rv64mi` | machine mode: traps, CSRs, misaligned access | 10/10 |

`rv64si` (supervisor) and `rv64uf`/`rv64ud` (floating point) are not built:
those need phases 6 and 8.

```bash
./scripts/run-riscv-tests.sh              # everything
./scripts/run-riscv-tests.sh rv64ui       # one suite
```

The suite is cloned into `third_party/` on first run and is not vendored.

## Supplying our own test environment

riscv-tests keeps its environment — the `_start` stub, the trap handler, the
link script, the pass/fail macros — in a **separate `riscv-test-env`
submodule**. That submodule was not reachable from this build environment.

Rather than treat that as a blocker, `tests/riscv-tests-env/` provides an
equivalent bare-metal ("p") environment: `riscv_test.h` and `link.ld`. It turns
out to be a better arrangement anyway — the suite now builds from the main
repository alone, with one less moving part in CI.

Writing it means understanding exactly how a test reports its result, which is
worth knowing:

```
tohost = 1                 every check passed
tohost = (n << 1) | 1      check n failed
```

The low bit marks the word as valid and the rest identifies the failure — which
is why *passing is 1, not 0*. Each test keeps its current check number in `gp`
(`TESTNUM`) so a failure names itself.

The environment also installs a default trap handler that reports failure. That
is what lets a test assert an operation does **not** trap: if it does, the
default handler fires and the test fails. Tests that expect traps define their
own `mtvec_handler`, which the default one jumps to when the weak symbol
resolves.

One subtlety worth recording: an unexpected trap before any numbered check would
leave `TESTNUM` at 0, and `(0 << 1) | 1` is `1` — indistinguishable from
success. The handler substitutes a distinctive number first.

## HTIF: how the emulator learns the result

A riscv-test does not power the machine off. It writes its result to `tohost`
and then spins forever, so there is nothing to observe except the write itself.

Two pieces were added:

**Symbol lookup.** The ELF loader now walks the section headers for a symbol
table and resolves `tohost` by name (`find_symbol` in `src/elf_loader.cpp`). No
hard-coded address, and nothing to pass on the command line.

**Polling.** `Cpu::run()` reads that address after each instruction and stops
when it becomes non-zero. That costs a bus lookup per instruction, so it is only
active when an image actually declares the symbol — an ordinary program pays
nothing.

## The bug it found

`rv64mi-instret_overflow` failed immediately. It does this:

```asm
TEST_CASE(2, a0, 0, csrwi minstret, 0; csrr a0, minstret);
```

Write 0 to `minstret`, read it straight back, expect 0. The emulator returned
something else, because `step()` was doing:

```cpp
csrs.write(csr::MINSTRET, instret);   // assign from the emulator's own count
```

Assigning rather than incrementing means a guest write to `minstret` is silently
discarded on the very next instruction. Software that sets the counter — to
measure a region, or to resume it after a context switch — would find its write
had no effect.

The fix is `CsrFile::tick_counters()`, which increments. There is a second part:
the spec says an instruction that *writes* a counter does not also increment it,
so the value read back is exactly what was written. Hence `counter_written_`,
set by `csr_write` and checked at the end of `step()`.

Nothing I would have thought to test. That is the point of a reference suite.

## Tests that are excluded, and why

Three `rv64mi` tests need features the emulator does not implement. They are
listed explicitly in `scripts/run-riscv-tests.sh` **with a reason each**, and
reported as excluded rather than dropped — an unexplained gap in a reference
suite is worse than a visible failure, because nobody can tell whether it was a
decision or an oversight.

| Test | Needs |
|---|---|
| `illegal` | supervisor mode — it waits on a supervisor software interrupt, then `mret`s into S-mode. Phase 6. |
| `breakpoint` | the debug trigger module (`tdata1`/`tselect`). Optional; not planned. |
| `pmpaddr` | physical memory protection. Optional; xv6 and Linux do not require it. |

`illegal` is worth re-running after phase 6.

## Continuous integration

`.github/workflows/ci.yml` runs on every push and pull request: install the
toolchain, configure with `-Werror`, build, run all eleven CTest suites, then
run the riscv-tests suite.

`-Werror` is applied in CI only. A warning should fail the build there, but it
should not stop someone building locally on a compiler whose defaults differ.

The repository had no CI at all before this phase.

---

## Running it yourself

```bash
./run-all.sh                     # build, unit tests, self-tests, demos
./scripts/run-riscv-tests.sh     # the reference suite
```

A failure names the check that failed, so you can go straight to it:

```
FAIL sraiw            FAIL: test 7 failed, after 62 instruction(s)

  Re-run one with:
    build/riscv_emu --trace build/riscv-tests/rv64ui-sraiw.elf
```

Test 7 is the seventh `TEST_*` macro in `third_party/riscv-tests/isa/rv64ui/sraiw.S`.

---

**Next:** phase 6 adds supervisor mode and the Sv39 MMU — the largest single
phase, and the last major piece before xv6 can boot.
