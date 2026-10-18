# Phase 14: faster, and more instructions

## Speed

A booting kernel retires billions of instructions, so the interpreter's speed is the
difference between a boot you wait for and one you walk away from. `tests/bench_cpu`
measures it on a loop mixing the instruction classes a kernel spends its time on — ALU
operations, a load, a store, a taken branch:

| workload | before | after |
|---|---:|---:|
| bare machine | 24.6 MIPS | 74–84 MIPS |
| Sv39, supervisor mode | 19.0 | 73 |
| with CLINT, PLIC and UART polled | 20.4 | 53 → 83 (via the platform) |
| **Sv39 + devices** (what a Linux boot looks like) | **17.1** | **81** |

*(Measured with GCC 15 on one core of a Windows laptop; the ratio matters more than the
absolute numbers.)* Four changes, in the order they paid off:

### A decoded-instruction cache

Fetching an instruction cost two translated 16-bit reads through the MMU and the bus,
then a decode — for every instruction, though a kernel runs the same few thousand over
and over. The CPU now remembers the *decoded* result, keyed by PC.

An entry is trusted only while four things hold: the PC matches (it is direct-mapped),
the privilege matches (the same address can be executable in one mode and a fault in
another), nothing has **written to a page the cache took code from**, and nobody has
flushed it (`SFENCE.VMA`, a `satp` write, `FENCE.I`).

The third is the subtle one. The guest is *supposed* to run `FENCE.I` after modifying
code — but a device DMAing a page of code into RAM never does. So the CPU tells DRAM
which pages it has cached code from, and any store landing on one of them — from the
guest or from a device — bumps an epoch every cache checks. Invalidation is
all-or-nothing, which is fine: code pages are written rarely compared with how often
they are executed. Instructions straddling a page boundary aren't cached; they need two
translations.

### A direct-mapped TLB

The TLB was a `std::unordered_map`, so every paged access paid for a hash and a pointer
chase. It is now a 1024-slot array indexed by the low bits of the virtual page number.
Two hot pages that collide evict each other and cost one table walk, which is rare
enough not to matter.

### RAM first, without virtual dispatch

Nearly every access a running guest makes lands in RAM, but the bus scanned its device
list with virtual calls to find it. `Bus::load`/`store` now test RAM first and call it
directly, and `Dram` accesses are one `memcpy` instead of a byte loop.

### Don't poll the devices every instruction

The single-CPU path ticked the timer and asked the interrupt controller what was
pending on every instruction. The `Platform` does it once per scheduling round
([phase 10](10-multiple-harts.md)) — which is what took "with devices" from 53 to 83.

## Instructions

The bit-manipulation extensions **Zba**, **Zbb**, **Zbc**, **Zbs**, and conditional-zero
**Zicond** — 45 instructions — are what a distribution built for a modern core assumes:
rotates and bit counts in hashing, `sh3add` to index arrays of 8-byte elements, `clz` in
allocators, `czero` for branch-free selects. `misa` reports `B`, and the device tree's
`riscv,isa` advertises them (override with `--isa`) so a kernel and C library use them.

None adds an opcode. Each is a set of new `(funct7, funct3)` or `(funct12, funct3)`
combinations *inside* ALU opcodes the base ISA already uses, so they are tried as a
pre-check in the four existing dispatchers (`src/cpu_bitmanip.cpp`) and anything else
falls through exactly as before. The "unrecognised encodings must trap" rule still holds,
and is tested for the neighbours.

### Differential testing

The project's testing bar for a batch of instructions is a comparison against an
independently written reference model, because a hand-written suite shares its author's
misreadings of the spec. [`tests/gen_bitmanip_vectors.py`](../tests/gen_bitmanip_vectors.py)
is that model: written in a different language and style — bit counts are done on strings
of `'0'` and `'1'` characters, where the C++ uses `std::countl_zero` — it emits **89,122
random and edge-case vectors** across all 45 instructions, built from the specification
tables. `test_bitmanip` runs each through the real CPU and checks the result *and* that
the instruction left its source registers alone.

0 mismatches. To check the check, four deliberate breakages — `clmulr` dropping a bit,
`sh3add` shifting by 2, `rolw` not sign-extending, `min` comparing unsigned — each failed.

The generator also asserts its encoding table against five **real assembler outputs**
(`sh1add`, `add.uw`, `czero.eqz`, `clz`, `rev8`), and the hand-written C++ cases use real
encodings too, so a misreading of the encoding tables can't hide behind a model that
shares it.

### What is *not* implemented

The **vector extension (V)**. It is large — 32 vector registers of configurable length, a
hundred-odd instructions with masking, widening, and fault-only-first loads — and nothing
Debian's riscv64 baseline (RV64GC) requires. A distribution built for the newer RVA23
profile does require it, and would not run here.

Likewise `pack`/`zip` (Zbkb) and the cache-block instructions (Zicbom/Zicboz) are absent.
