# Running the emulator

Everything you can build, run and test, in one place.

**In a hurry?** `./run-all.sh` builds the project, runs every test, and runs every
demo, printing what each one did.

---

## 1. Prerequisites

**Required** — a C++20 compiler and CMake ≥ 3.16. Nothing else; the emulator has
no third-party dependencies.

```bash
sudo apt-get install build-essential cmake     # Debian / Ubuntu
```

**Optional but recommended** — a RISC-V cross-toolchain. Without it the project
builds and the unit tests run, but the four bare-metal self-tests are skipped,
and you cannot assemble your own guest programs.

```bash
sudo apt-get install gcc-riscv64-unknown-elf
```

CMake reports which it found at configure time:

```
-- RISC-V toolchain not found; skipping the bare-metal self-test
```

---

## 2. Build

```bash
cmake -S . -B build
cmake --build build
```

This produces:

| | |
|---|---|
| `build/riscv_emu` | the emulator |
| `build/test_*` | seven unit-test binaries |
| `build/*_selftest.bin` | four bare-metal guest programs (toolchain only) |

The default build type is **Release**, because the interpreter loop runs
billions of times when booting an OS and an unoptimised build turns a
seconds-long boot into a minutes-long one. For debugging:

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
```

---

## 3. Run

### The built-in demo

With no arguments the emulator runs a small program compiled into the binary: it
writes a string to the UART one byte at a time, then powers the machine off.

```bash
./build/riscv_emu
```
```
hello, RISC-V

machine powered off after 79 instruction(s)
```

That single line of output exercises the whole stack — instruction decode,
the bus, memory-mapped I/O to the console, and the syscon poweroff device.

### Running a program

```bash
./build/riscv_emu path/to/image
```

The format is detected from the file's magic number:

- **ELF64** — program headers are walked and each `PT_LOAD` segment is copied to
  its physical address; execution starts at `e_entry`.
- **Anything else** — treated as a flat binary, loaded at `0x8000_0000`, and
  execution starts there.

### All options

| Option | Meaning |
|---|---|
| `--trace` | one line per retired instruction, on stderr |
| `--dump` | dump all registers when execution stops |
| `--max-steps N` | stop after N instructions (default 100,000,000) |
| `--dram-size-mb N` | guest RAM in MiB (default 128) |
| `--timer-divisor N` | instructions per `mtime` tick (default 1) |
| `--help` | the same list |

Guest console output goes to **stdout**; the emulator's own diagnostics and the
trace go to **stderr**. So this captures only what the guest printed:

```bash
./build/riscv_emu image.bin 2>/dev/null
```

---

## 4. Test

```bash
cd build && ctest --output-on-failure
```

Eleven suites. Seven are unit tests linked against the emulator core:

| Suite | Covers |
|---|---|
| `test_decoder` | all six immediate encodings, field extraction, RV64 shift fields |
| `test_bus` | bounds checks, endianness, address decoding, trap causes |
| `test_cpu` | OP-IMM dispatch, illegal-instruction traps, PC-on-trap |
| `test_rv64i` | the whole base integer instruction set |
| `test_csr` | CSR semantics, trap entry, `MRET`, interrupt gating |
| `test_muldiv_atomic` | M edge cases, AMOs, the LR/SC reservation |
| `test_devices` | UART, CLINT, syscon, and the ELF loader |

Four are **bare-metal self-tests**: real assembly programs, assembled by the GNU
assembler and executed by the emulator. They matter because they validate the
decoder against an independent implementation rather than against the encoders
in `tests/machine.hpp`.

| Self-test | Result | What it does |
|---|---|---|
| `rv64i_selftest` | `a0 = 0x3fff` | 14 checks over RV64I's fine print |
| `trap_selftest` | `a0 = 0xfff` | 12 checks with a real trap handler |
| `muldiv_atomic_selftest` | `a0 = 0x7fff` | 15 checks, ending with an LR/SC spinlock |
| `device_selftest` | `a0 = 0x1f` | prints, takes a timer interrupt, powers off |

Run one directly:

```bash
./build/riscv_emu --dump build/rv64i_selftest.bin
```

**How to read a self-test failure.** Each accumulates one bit in `a0` per
passing check, so a failure message names the bit that is clear:

```
self-test failed: a0 = 0x3ffb, expected 0x3fff.
Each bit is one sub-test; a clear bit identifies which one failed.
```

`0x3ffb` is missing bit 2, so check 2 failed — find `PASS_IF_NE 2` in the
corresponding `examples/*.S` and the comment above it says what it was testing.

Run a single unit suite for faster iteration:

```bash
./build/test_rv64i
cd build && ctest -R csr --output-on-failure    # or by name pattern
```

---

## 5. Write and run your own guest program

The emulator's console makes this genuinely usable. A minimal program:

```asm
# hello.S
    .equ UART0,    0x10000000
    .equ SYSCON,   0x00100000
    .equ POWEROFF, 0x5555

    .section .text
    .globl _start
_start:
    li      t0, UART0
    la      t1, msg
1:  lbu     t2, 0(t1)
    beqz    t2, done
    sb      t2, 0(t0)          # writing here prints a character
    addi    t1, t1, 1
    j       1b
done:
    li      t0, SYSCON
    li      t1, POWEROFF
    sw      t1, 0(t0)          # stop the machine
    j       .

    .section .data
msg:
    .asciz "hello from my own program\n"
```

Assemble, link at `0x8000_0000`, and run:

```bash
riscv64-unknown-elf-as -march=rv64ima_zicsr -mabi=lp64 -o hello.o hello.S
riscv64-unknown-elf-ld --no-warn-rwx-segments -T examples/link.ld -o hello.elf hello.o
./build/riscv_emu hello.elf
```

`examples/link.ld` places everything at `0x8000_0000`, which is where the
emulator loads a flat binary and where a kernel expects RAM to start.

Pick `-march` to match what you use: `rv64i`, `rv64i_zicsr` for CSR
instructions, `rv64ima_zicsr` for multiply/divide and atomics. Asking for an
extension the emulator does not implement yet (`c`, `f`, `d`) will assemble
fine and then trap as an illegal instruction at the first one used.

The four programs in `examples/` are worth reading as references — they are real
working code, not toys.

---

## 6. Debugging

### Instruction tracing

```bash
./build/riscv_emu --trace hello.elf 2>trace.log
```

Each line shows the PC, the raw instruction word, the mnemonic, and only the
operands that instruction's format actually uses:

```
0x0000000080000000  100002b7  lui    rd=x5(t0) imm=0x10000000
0x0000000080000004  00000317  auipc  rd=x6(t1) imm=0
0x0000000080000010  00038863  beq    rs1=x7(t2) rs2=x0(zero) imm=16 -> 0x80000020
```

Branches and jumps print the absolute target, which is what you want when
following control flow.

Tracing a booting kernel produces enormous files — pair it with `--max-steps`.

### Register dumps

`--dump` prints every register with its ABI name when execution stops. Combined
with the stop reason, that is usually enough to see what went wrong.

### Reading the stop message

The emulator always says why it stopped.

**`machine powered off after N instruction(s)`** — the guest wrote the poweroff
word to syscon. A clean exit.

**`stopped after N instruction(s): <cause> (cause C, tval 0xX) at pc 0xP`** — a
trap was raised while no handler was installed (`mtvec` is still zero), so the
emulator stopped rather than looping at address 0 forever. Common causes:

| Cause | Usually means |
|---|---|
| `illegal instruction` | an instruction from an extension not implemented yet, or the PC ran off into data. `tval` holds the instruction bits. |
| `breakpoint` | the program executed `ebreak`. Often deliberate. |
| `load/store access fault` | an address no device claims. `tval` is the address. |
| `instruction access fault` | the PC left mapped memory — usually a bad jump. |
| `ecall from M-mode` | an `ecall` with no handler installed. |

**`step budget exhausted after N instruction(s)`** — the program never
terminated. Often an infinite loop; raise `--max-steps` if it just needs longer.

### A trap that dispatches instead of stopping

Once a guest sets `mtvec`, traps stop being fatal and go to its handler. If your
program then appears to hang, its handler is probably re-entering — a handler
that ends in `ebreak` traps into itself forever. End with a self-loop instead,
or clear `mtvec` before the `ebreak`, as `examples/trap_selftest.S` does.

---

## 7. Run absolutely everything

```bash
./run-all.sh
```

Builds from scratch, runs all eleven suites, then runs the built-in demo and
each self-test and reports what each produced. Useful after changing anything,
and as a single command to check out a fresh clone with.

```bash
./run-all.sh --quick     # skip the clean rebuild
```

---

## See also

- [`../README.md`](../README.md) — what the project is and where it is going
- [`PHASES.md`](PHASES.md) — the roadmap and what each phase delivered
- The numbered design notes (`00-` … `04-`) explain *why* the emulator is built
  the way it is. Each records the state at the end of its phase; this page and
  the README always describe the present.
