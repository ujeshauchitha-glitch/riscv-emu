# RISC-V Emulator

An RV64 emulator written in modern C++20, built to boot a real operating system —
**xv6-riscv** first, then **Linux**.

No dependencies: a C++20 compiler and CMake ≥ 3.16 are all you need.

## Status

**Phases 0-2 done, seven to go.** RV64I runs, and traps now dispatch to a
handler the guest installs - so `ECALL` behaves like a system call rather than
halting the machine. Both self-tests in `examples/` are assembled by the GNU
assembler and pass under the emulator.

Next up: the M extension (multiply/divide) and A extension (atomics). xv6's
spinlocks are built on the atomics.

## What works today

| | Status |
|---|---|
| **RV64I** base integer instruction set | ✅ complete |
| Six-format instruction decoder | ✅ |
| Bus with memory-mapped I/O, DRAM at `0x8000_0000` | ✅ |
| Trap causes and reporting | ✅ |
| **Zicsr** — CSRs, `MRET`, `WFI`, trap dispatch, interrupts | ✅ complete |
| M — multiply / divide | ⬜ phase 3 |
| A — atomics, `LR`/`SC` | ⬜ phase 3 |
| Devices — UART, CLINT, PLIC, virtio | ⬜ phases 4, 7 |
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

# Run a flat binary image, loaded at 0x8000_0000
./build/riscv_emu path/to/image.bin

# The bare-metal self-tests (built when a RISC-V toolchain is installed).
# Each stops on ebreak with one bit set in a0 per passing check.
./build/riscv_emu --dump build/rv64i_selftest.bin   # expect a0 = 0x3fff
./build/riscv_emu --dump build/trap_selftest.bin    # expect a0 = 0xfff
```

`--help` lists all options.

## Test

```bash
cd build && ctest --output-on-failure
```

Seven suites: unit tests for the decoder, the bus, the CPU, the RV64I
instruction set and the CSR/trap machinery, plus two bare-metal self-tests
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
| `0x0010_0000` | syscon (poweroff / reboot) | phase 4 |
| `0x0200_0000` | CLINT (timer, software interrupts) | phase 4 |
| `0x0C00_0000` | PLIC (external interrupts) | phase 7 |
| `0x1000_0000` | UART0 (NS16550A console) | phase 4 |
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
| 3 | M and A extensions | ⬜ next |
| 4 | ELF loader, UART, CLINT — first real output | ⬜ |
| 5 | riscv-tests + CI | ⬜ |
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

## Goals

- Learn computer architecture by building a machine that runs real software
- Understand instruction decoding, privilege modes, virtual memory, and MMIO
- Get to the point where an unmodified kernel boots to a shell
- Keep the codebase modular, tested, and readable
