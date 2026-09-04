# RISC-V Emulator

An RV64 emulator written in modern C++20, built to boot a real operating system —
**xv6-riscv** first, then **Linux**.

No dependencies: a C++20 compiler and CMake ≥ 3.16 are all you need.

## Status

**Phases 0-4 done, five to go.** The emulator implements RV64IMA with M-mode
traps, and now has devices: a guest prints to the console, takes timer
interrupts, and powers the machine off itself.

```
$ ./build/riscv_emu
hello, RISC-V

machine powered off after 79 instruction(s)
```

Next up: the official `riscv-tests` suite and CI, which is what makes the MMU
work in phase 6 tractable.

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
| Devices — PLIC, virtio-blk | ⬜ phase 7 |
| S-mode + Sv39 virtual memory | ⬜ phase 6 |
| C — compressed instructions | ⬜ phase 8 |
| F/D — floating point | ⬜ phase 8 |

Instructions that are not implemented yet raise an illegal-instruction trap
rather than being silently mis-executed, so a program that needs them fails
loudly at the exact instruction.

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Run

```bash
# Built-in demo: the OP-IMM instruction group, then a register dump
./build/riscv_emu

# One line per retired instruction, on stderr
./build/riscv_emu --trace

# Run an ELF64 image, or a flat binary loaded at 0x8000_0000.
# The format is detected from the file's magic number.
./build/riscv_emu path/to/kernel.elf

# The bare-metal self-tests (built when a RISC-V toolchain is installed).
# Each stops on ebreak with one bit set in a0 per passing check.
./build/riscv_emu --dump build/rv64i_selftest.bin   # expect a0 = 0x3fff
./build/riscv_emu --dump build/trap_selftest.bin    # expect a0 = 0xfff
./build/riscv_emu --dump build/muldiv_atomic_selftest.bin  # expect a0 = 0x7fff
./build/riscv_emu build/device_selftest.bin         # prints, then powers off
```

`--help` lists all options.

## Test

```bash
cd build && ctest --output-on-failure
```

Eleven suites: unit tests for the decoder, the bus, the CPU, the RV64I
instruction set, the CSR/trap machinery, the M/A extensions and the devices,
plus four bare-metal self-tests
assembled with a real RISC-V toolchain. The self-tests are skipped automatically
when no toolchain is present - to enable them:

```bash
apt-get install gcc-riscv64-unknown-elf
```

The unit tests build their programs with encoders written separately from the
emulator's own decoder, and the self-test goes further by validating against an
independent assembler. From phase 5 the official
[`riscv-tests`](https://github.com/riscv-software-src/riscv-tests) suite takes
over as the primary correctness signal, running in CI.

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
| `0x0C00_0000` | PLIC (external interrupts) | phase 7 |
| `0x1000_0000` | UART0 (NS16550A console) | ✅ |
| `0x1000_1000` | virtio-mmio (block device) | phase 7 |
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
| 5 | riscv-tests + CI | ⬜ next |
| 6 | S-mode + Sv39 MMU | ⬜ |
| 7 | PLIC + virtio-blk — **boot xv6** | ⬜ |
| 8 | Linux prerequisites (C, F/D, DTB, SBI) | ⬜ |
| 9 | **Boot Linux** | ⬜ |

Phase 7 is the milestone: xv6 booting to a shell. Linux is the stretch.

[`docs/PHASES.md`](docs/PHASES.md) has the detail behind each phase.

## Design notes

Each phase ships an explainer alongside the code:

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

## Goals

- Learn computer architecture by building a machine that runs real software
- Understand instruction decoding, privilege modes, virtual memory, and MMIO
- Get to the point where an unmodified kernel boots to a shell
- Keep the codebase modular, tested, and readable
