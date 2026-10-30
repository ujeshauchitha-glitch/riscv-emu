# The Rust port

The emulator was written in C++20 through phase 14 (SMP, virtio, networking, GDB, extra
extensions, the Debian boot script) and then rewritten in Rust. This note records why, what
changed, and how the two were compared, so the claim "same behaviour" rests on evidence
rather than on the rewrite having gone smoothly.

The C++ sources are in the git history (they were last on `main` at commit `df2c5fb`).

## What is the same

Everything observable: the instruction set (RV64GC plus Zba/Zbb/Zbc/Zbs/Zicond), machine /
supervisor / user modes, Sv39 with the same TLB and decoded-instruction cache design, the
deterministic round-robin scheduler, the memory map, every device, SBI, the generated device
tree, the GDB stub, and every command-line flag, message and exit code.

## What changed, and why it is better

| Area | C++ | Rust |
|---|---|---|
| Traps | `Result<T>` hand-rolled | `Result<T, Trap>`; `?` propagates |
| Memory safety | bounds checks by convention | bounds checks by convention **plus** no raw pointers outside one documented `UnsafeCell` in `dram.rs` and the Win32 console calls in `uart.rs` |
| Integer overflow | undefined behaviour for signed, silent for unsigned | explicit `wrapping_*` where the architecture wraps; checked in debug builds |
| Floating point | host FPU plus `fenv` | a self-contained integer soft-float unit (`softfloat.rs`): the five rounding modes and the five flags exactly, independent of the host; RMM is exact |
| Build | CMake, a C++20 compiler | `cargo build --release`; no third-party crates |
| Host sockets, console | per-platform C APIs | `std::net` plus a thread for blocking stdin |

## How the port was checked against the C++

1. **The official riscv-tests suite: 128/128.**
2. **Instruction traces**: with `--trace`, the Rust and C++ binaries print identical traces on
   127 of the 128 riscv-tests programs (the 128th differed only because the C++ binary used for
   comparison predated a fix to `sstatus.UXL`).
3. **A whole xv6 boot**: register and CSR dumps are identical at 20M, 100M, 400M, 900M and
   1.5B retired instructions on one hart, and at 1.5B on three harts. That run covers paging,
   the timer, the PLIC, the virtio disk and the console.
4. **Independent references**: 89,122 bit-manipulation vectors from a Python model; 45,000
   soft-float vectors from an exact-rational Python model (all rounding modes and flags); 300,000
   random cases per operation against the host FPU in round-to-nearest.
5. **The unit tests were ported**, not rewritten from scratch: about 300 tests covering the same
   cases as the C++ suites, plus new ones for the command line and the bugs found along the way.

## Differences you may notice

- `--rng-seed` seeds the device and the kernel's `/chosen/rng-seed` with a different generator
  than the C++ (`mt19937_64`), so a given seed gives different bytes. A seed is still reproducible
  within the Rust build.
- The console reader is a thread, so input is seen on the next scheduler poll rather than the
  next non-blocking read.
- Speed: about 85-100 MIPS on the benchmark in `examples/bench_cpu.rs`, on par with or a little
  above the C++.

## Bugs found and fixed along the way

- A soft-float division and square root lost precision on subnormal inputs (found by the exact
  reference vectors).
- A guest waiting on the UART pinned a host core; the machine now yields briefly while real
  input can still arrive.
- An impossible `--dram-size-mb`, an ELF segment with an enormous `p_memsz`, and a segment aimed
  at a device are now refused cleanly (the same fixes were applied to the C++ on the `cpp-fixes`
  branch).
- virtio-blk: a snapshot write to a short last sector failed; and a large batch of queued requests
  was served inside one doorbell write, which now does bounded work per pass.
