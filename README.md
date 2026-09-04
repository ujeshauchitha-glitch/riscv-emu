# RISC-V Emulator

An RV64 emulator written in modern C++20, built to boot a real operating system —
**xv6-riscv** first, then **Linux**.

## Goals

- Learn computer architecture by building a machine that runs real software
- Understand instruction decoding, privilege modes, virtual memory, and MMIO
- Get to the point where an unmodified kernel boots to a shell
- Keep the codebase modular, tested, and readable

## Build

```bash
cmake -S . -B build
cmake --build build
```

No dependencies beyond a C++20 compiler and CMake ≥ 3.16.

## Run

```bash
# Built-in demo: one of each OP-IMM instruction, then a register dump
./build/riscv_emu

# With an instruction trace (stderr)
./build/riscv_emu --trace

# Run a flat binary image, loaded at 0x8000_0000
./build/riscv_emu path/to/image.bin
```

`--help` lists all options.

## Test

```bash
cd build && ctest --output-on-failure
```

The suite includes a bare-metal self-test assembled with a real RISC-V
toolchain. It is skipped automatically if one is not installed; to enable it:

```bash
apt-get install gcc-riscv64-unknown-elf
```

## Status

**Phase 1 complete** — the full RV64I base integer instruction set runs. The
emulator executes real compiled code: `examples/rv64i_selftest.S` is assembled by
the GNU assembler and passes all 14 of its checks.

Next: CSRs and trap handling, so `ECALL` dispatches to a handler instead of
halting.

See [`docs/PHASES.md`](docs/PHASES.md) for the full roadmap and progress.
Design notes: [`00-architecture.md`](docs/00-architecture.md) (how the emulator
is put together and why), [`01-rv64i.md`](docs/01-rv64i.md) (the instruction set,
and the RV64 fine print that is easy to get wrong).

## Machine model

Physical memory map, matching the QEMU `virt` machine so that unmodified guest
kernels can be booted:

| Address | Device | Phase |
|---|---|---|
| `0x0010_0000` | syscon (poweroff/reboot) | 4 |
| `0x0200_0000` | CLINT (timer, software interrupts) | 4 |
| `0x0C00_0000` | PLIC (external interrupts) | 7 |
| `0x1000_0000` | UART0 (NS16550A console) | 4 |
| `0x1000_1000` | virtio-mmio (block device) | 7 |
| `0x8000_0000` | DRAM | ✅ |
