# RISC-V Emulator

An RV64 emulator written in modern C++20, built to boot a real operating system —
**xv6-riscv** first, then **Linux**.

No dependencies: a C++20 compiler and CMake ≥ 3.16 are all you need.

## Status

**It boots an OS.** xv6-riscv reaches a shell prompt, the prompt can be typed
at, and `ls` and `cat` read real data off a virtio disk image:

```
xv6 kernel is booting

init: starting sh
$ ls
.              1 1 1024
..             1 1 1024
README         2 2 2441
cat            2 3 36728
echo           2 4 35584
...
$ cat README
xv6 is a re-implementation of Dennis Ritchie's and Ken Thompson's Unix
Version 6 (v6).  ...
```

Every [`riscv-tests`](https://github.com/riscv-software-src/riscv-tests) case
the emulator can build passes:

```
rv64ui 54/54   rv64um 13/13   rv64ua 19/19   rv64mi 11/11   rv64si 4/4
101/101 passed
```

**Phases 0-7 done, two to go.** Next: the compressed and floating-point
extensions, a device tree and SBI - and then Linux.

[`docs/07-booting-xv6.md`](docs/07-booting-xv6.md) explains how the boot works
and the two bugs that stood between "runs a kernel" and "boots xv6".

## What works today

| | Status |
|---|---|
| **RV64I** base integer instruction set | ✅ complete |
| Six-format instruction decoder | ✅ |
| Bus with memory-mapped I/O, DRAM at `0x8000_0000` | ✅ |
| Trap causes and reporting | ✅ |
| **Zicsr** — CSRs, `MRET`, `WFI`, trap dispatch, interrupts | ✅ complete |
| **M** — multiply / divide | ✅ complete |
| **A** — atomics, `LR`/`SC` | ✅ complete |
| **Devices** — UART, CLINT, syscon | ✅ complete |
| **ELF64 loader** | ✅ complete |
| **S-mode + U-mode**, trap delegation | ✅ complete |
| **Sv39 virtual memory**, TLB | ✅ complete |
| **Devices** — PLIC, virtio-blk | ✅ complete |
| **Interactive console** — host stdin as the UART's receive line | ✅ complete |
| **Boots xv6-riscv to a shell** | ✅ |
| PMP — registers stored, *not enforced* | ⚠️ partial |
| C — compressed instructions | ⬜ phase 8 |
| F/D — floating point | ⬜ phase 8 |

Instructions that are not implemented yet raise an illegal-instruction trap
rather than being silently mis-executed, so a program that needs them fails
loudly at the exact instruction.

## Quick start

**Boot xv6 and get a shell:**

```bash
./scripts/boot-xv6.sh
```

Fetches xv6, builds it for the extensions this emulator implements, builds the
emulator, and boots it. About half a minute later you get a live `$` prompt —
try `ls`, `cat README`, `usertests`. Press **Ctrl-A then X** to leave (Ctrl-C
goes to the guest). Needs a RISC-V toolchain: `apt-get install
gcc-riscv64-unknown-elf`.

**Build and test everything:**

```bash
./run-all.sh
```

Builds the project, runs all thirteen test suites, then runs the demo and every
self-test and reports what each produced. Use `--quick` to skip the clean
rebuild.

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Run

```bash
# Built-in demo: prints over the UART, then powers the machine off
./build/riscv_emu

# One line per retired instruction, on stderr
./build/riscv_emu --trace

# Run an ELF64 image, or a flat binary loaded at 0x8000_0000.
# The format is detected from the file's magic number.
./build/riscv_emu path/to/kernel.elf

# Boot xv6 by hand (scripts/boot-xv6.sh does all of this for you)
./build/riscv_emu --disk fs.img --max-steps 1000000000000 kernel/kernel

# The bare-metal self-tests (built when a RISC-V toolchain is installed).
# Each stops on ebreak with one bit set in a0 per passing check.
./build/riscv_emu --dump build/rv64i_selftest.bin   # expect a0 = 0x3fff
./build/riscv_emu --dump build/trap_selftest.bin    # expect a0 = 0xfff
./build/riscv_emu --dump build/muldiv_atomic_selftest.bin  # expect a0 = 0x7fff
./build/riscv_emu build/device_selftest.bin         # prints, then powers off
```

`--help` lists all options. [`docs/RUNNING.md`](docs/RUNNING.md) is the full
guide: every flag, how to write and assemble your own guest program, and how to
read a trace or a stop message when something goes wrong.

## Test

```bash
cd build && ctest --output-on-failure
```

Thirteen suites: unit tests for the decoder, the bus, the CPU, the RV64I
instruction set, the CSR/trap machinery, the M/A extensions, the devices,
supervisor mode with the MMU, and the PLIC and virtio block device, plus four
bare-metal self-tests
assembled with a real RISC-V toolchain. The self-tests are skipped automatically
when no toolchain is present - to enable them:

```bash
apt-get install gcc-riscv64-unknown-elf
```

Plus the official reference suite:

```bash
./scripts/run-riscv-tests.sh
```

That one matters most. The tests in this repository were written alongside the
emulator, so they cannot catch a misreading of the specification - both halves
would share it. `riscv-tests` is written by the people who wrote the spec. It is
fetched into `third_party/` on first run and needs a RISC-V toolchain.

## Layout

```
include/          src/
  types.hpp         Widths, memory map, sign_extend
  trap.hpp          Exception and interrupt cause codes      trap.cpp
  result.hpp        Result<T> / Status — traps as values
  device.hpp        The Device interface, AccessType
  bus.hpp           Address decoding                         bus.cpp
  dram.hpp          Guest RAM at 0x8000_0000                 dram.cpp
  decoder.hpp       u32 -> DecodedInst                       decoder.cpp
  cpu.hpp           Registers, fetch / decode / execute      cpu.cpp
  csr.hpp           Control and status registers             csr.cpp
  uart.hpp          NS16550A console                         uart.cpp
  clint.hpp         Timer and software interrupts            clint.cpp
  syscon.hpp        Poweroff / reboot                        syscon.cpp
  elf_loader.hpp    ELF64 image loading                      elf_loader.cpp
  mmu.hpp           Sv39 translation and the TLB             mmu.cpp
                    Command line                             main.cpp

examples/         Bare-metal assembly + linker script
tests/            Unit tests and the self-test harness
docs/             Design notes, one per phase
```

## Machine model

The physical memory map matches the QEMU `virt` machine, so that unmodified
guest kernels — which are linked expecting exactly this layout — can be booted.

| Address | Device | |
|---|---|---|
| `0x0010_0000` | syscon (poweroff / reboot) | ✅ |
| `0x0200_0000` | CLINT (timer, software interrupts) | ✅ |
| `0x0C00_0000` | PLIC (external interrupts) | ✅ |
| `0x1000_0000` | UART0 (NS16550A console) | ✅ |
| `0x1000_1000` | virtio-mmio (block device) | ✅ |
| `0x8000_0000` | DRAM | ✅ |

Addresses claimed by no device raise an access fault rather than reading zero,
so a kernel that jumps into the weeds stops immediately with a clear cause.

## Roadmap

| # | Phase | |
|---|---|---|
| 0 | Foundation: bus, decoder, trap plumbing | ✅ |
| 1 | Complete RV64I | ✅ |
| 2 | Zicsr + M-mode traps | ✅ |
| 3 | M and A extensions | ✅ |
| 4 | ELF loader, UART, CLINT — first real output | ✅ |
| 5 | riscv-tests + CI | ✅ |
| 6 | S-mode + Sv39 MMU | ✅ |
| 7 | PLIC + virtio-blk — **boot xv6** | ✅ |
| 8 | Linux prerequisites (C, F/D, DTB, SBI) | ⬜ next |
| 9 | **Boot Linux** | ⬜ |

Phase 7 was the milestone: xv6 boots to a shell. Linux is the stretch.

[`docs/PHASES.md`](docs/PHASES.md) has the detail behind each phase.

## Design notes

[`RUNNING.md`](docs/RUNNING.md) covers how to build, run, test and debug.

Each phase also ships an explainer alongside its code:

- [`00-architecture.md`](docs/00-architecture.md) — why memory became a bus, why
  traps are return values rather than exceptions, why the B and J immediate
  encodings look scrambled
- [`01-rv64i.md`](docs/01-rv64i.md) — the instruction set, and the RV64 fine
  print that is easy to get wrong (`*W` sign extension, 6- vs 5-bit shift
  amounts, where a misaligned-jump trap is reported)
- [`02-csrs-and-traps.md`](docs/02-csrs-and-traps.md) - what a CSR is, the exact
  trap-entry and `MRET` sequences, and why `mepc` points at a faulting
  instruction but past an interrupted one
- [`03-m-and-a.md`](docs/03-m-and-a.md) - why RISC-V division never traps, and
  why a trap has to break an LR/SC reservation
- [`04-devices-and-mmio.md`](docs/04-devices-and-mmio.md) - memory-mapped I/O,
  why the timer clock counts instructions rather than seconds, and why interrupt
  pending bits belong to hardware rather than software
- [`05-testing.md`](docs/05-testing.md) - how a riscv-test reports its result,
  the counter bug the suite found, and which tests are excluded and why
- [`06-privilege-and-paging.md`](docs/06-privilege-and-paging.md) - why sstatus
  is a view rather than a copy, how an Sv39 walk works, and why the
  sign-extension rule creates the user/kernel address split
- [`07-booting-xv6.md`](docs/07-booting-xv6.md) - the claim/complete handshake
  and why it is what keeps a handler from re-entering itself, how a virtqueue
  works, and the two bugs that stood between "runs a kernel" and "boots xv6"

## Goals

- Learn computer architecture by building a machine that runs real software
- Understand instruction decoding, privilege modes, virtual memory, and MMIO
- Get to the point where an unmodified kernel boots to a shell
- Keep the codebase modular, tested, and readable
