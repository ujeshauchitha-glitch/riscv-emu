# Phase 8, part 1 — The C extension

Phase 7 got xv6 booting, but only after rebuilding it. Its stock build targets
`-march=rv64gc`, and the `c` there means **compressed instructions**: 16-bit
encodings for the operations programs use most. xv6's kernel contains 5,253 of
them, and the emulator could not decode a single one.

That is not a small footnote. Almost every RISC-V binary you will encounter —
every Linux kernel, every distribution package, every compiler default — is
built with C. An emulator without it can only run software built specially for
it, which is a different thing from running real software.

With this part done, **stock unmodified xv6 boots**. No patched Makefile, no
special `-march`, nothing.

---

## The idea

Compressed instructions are not a second instruction set. Each one is *defined
by the specification* as being exactly equivalent to one 32-bit base
instruction:

| Compressed | Is defined to be |
|---|---|
| `c.addi4spn a0, sp, 8` | `addi a0, sp, 8` |
| `c.mv a0, a1` | `add a0, x0, a1` |
| `c.jr a0` | `jalr x0, 0(a0)` |
| `c.li a0, -32` | `addi a0, x0, -32` |
| `c.j label` | `jal x0, label` |

So the whole extension can be implemented as a **translation from 16 bits to
32**, with the existing decoder and every existing execute path left untouched:

```cpp
u32 decompress(u16 half);
```

That matters for more than brevity. A parallel 16-bit execute path would be a
second implementation of instructions that already work — a second ADDI, a
second JALR — and every bug fixed in one would have to be found again in the
other. Here there is only ever one implementation of ADDI, and the compressed
form reaches it by the same route as the long form.

The cost is one extra table lookup per compressed instruction, which is
immeasurable next to the memory accesses an emulated instruction already does.

---

## Where it touches the CPU

Almost nowhere, which was the point. Three places:

### Fetch happens in two halves

Instruction length is not known until the first halfword has been read. RISC-V
marks it in the low bits: `[1:0] == 11` means 32 bits or wider, anything else
means 16. That is why no compressed encoding can begin with those two bits set,
and why the two lengths mix freely with no mode bit anywhere.

```cpp
Result<u64> low = mem_load(pc, 2, AccessType::Instruction);
if (!is_32bit_instruction(low16)) return decode16(low16, 0);
Result<u64> high = mem_load(pc + 2, 2, AccessType::Instruction);
```

Two accesses rather than one is not a formality. A 32-bit instruction can
**straddle a page boundary**, with its first halfword on a mapped page and its
second on one that is not. The correct behaviour is an instruction page fault
naming the address of the *second* halfword. A single four-byte access would
report the wrong address — or succeed against a page the program was never
allowed to execute from. Splitting the fetch gives each halfword its own
translation, which is what the hardware does.

### The PC advances by the instruction's own length

```cpp
next_pc_ = pc + inst.length;
```

This is the only line in the execution loop that knows the C extension exists.

### IALIGN becomes 16

Without C, instructions must be 4-byte aligned and a jump to a 2-byte-aligned
address raises `InstructionAddressMisaligned`. With C the requirement is 2-byte
alignment.

Which has a consequence worth stating plainly: **a misaligned jump has become
impossible to express.** `JAL` and the branches have bit 0 of their immediate
hardwired to zero, and `JALR` clears bit 0 of its computed target as defined
behaviour. No control-transfer instruction can produce an odd address. The check
in `set_branch_target()` stays as a backstop for a PC set some other way, but no
program can trigger it.

---

## Two bugs, both caught by real software

Neither was caught by the unit tests, which is worth dwelling on: those tests
check that `decompress()` produces the right 32-bit words, and it did. Both bugs
were in code *around* the decompressor that had been quietly assuming every
instruction is four bytes long.

### The link address

`riscv-tests` `rv64uc/rvc` check 36 failed. It is this:

```asm
la   t0, 1f
li   ra, 0
c.jalr t0        # jump to t0, link into ra
c.j  2f
1: c.j 1f
2: j fail
1: sub ra, ra, t0    # expects ra - t0 == -2
```

`C.JALR` is two bytes, so it must link `pc + 2`. The JALR execute path had:

```cpp
const u64 link = pc + 4;   // wrong
```

Hardcoded since phase 1, correct for every instruction that existed until now.
A compiler that called a function through `c.jalr` would get a return address
two bytes past where it should be, and the return would skip whatever followed
the call. The fix is `pc + inst.length`.

This is exactly what a reference suite is for. The bug is invisible in isolation
— `decompress()` expands `c.jalr` perfectly correctly — and it only appears when
the expansion meets an execute path written under an assumption that no longer
holds.

### `mepc` was rounding down

With the fix above, stock xv6 got 70 instructions in and died with an illegal
instruction at `0x800000be`, reading `mhartid` — a machine-mode CSR — while in
supervisor mode. The trace showed why: `mret` had returned into the middle of
`strlen`, which then ran an epilogue it never entered and returned into
machine-mode code.

`start()` does this:

```asm
80000074: auipc a5, 0x1
80000078: addi  a5, a5, -646   # 80000dee <main>
8000007c: csrw  mepc, a5
```

`main` is at `0x8000_0dee`. Two-byte aligned — completely ordinary with C. But
`mepc`'s write path was:

```cpp
raw_[a] = value & ~3ull;
```

Bit 0 of an instruction address is always zero, so masking it is right. Bit 1 is
only zero when IALIGN is 32. Masking it too turned `0x…dee` into `0x…dec`, and
`mret` returned two bytes early into the tail of the function the linker
happened to place before `main`.

What makes this one instructive is that the comment above it *said* what it was
doing and why:

> because we only support 32-bit-aligned instructions (no C extension yet), bit
> 1 is too. Phase 8 relaxes this to just bit 0 when compressed instructions
> arrive.

The code was correct when written, documented its own assumption, and named the
phase that would invalidate it. It still broke, because implementing the C
extension meant thinking about the decoder and the fetch path, and this was
neither. A note in a comment is not a mechanism.

Both `mepc` and `sepc` now mask only bit 0, and
`test_epc_registers_keep_bit_1_with_the_c_extension` pins the behaviour with
`0x…dee` itself as the value, so the test fails for the same reason xv6 did.

---

## Testing

**Against the real assembler.** The core of `tests/test_compressed.cpp` is a
table of 57 rows, each pairing a compressed encoding with the encoding of its
documented expansion — both produced by `riscv64-unknown-elf-as`, not by
anything in this repository:

```cpp
{0x1ffc, 0x3fc10793, "c.addi4spn a5, sp, 1020", "addi a5, sp, 1020"},
{0x5ef8, 0x07c6a703, "c.lw a4, 124(a3)",        "lw a4, 124(a3)"},
```

This is the only kind of check worth having for a table of hand-transcribed bit
positions. The immediates deliberately include both extremes of every field's
range: a bit placed in the wrong position often still gives the right answer for
small values and only diverges at the top.

**The official suite.** `rv64uc` is now part of `scripts/run-riscv-tests.sh`,
built with `c` in its `-march` while every other suite is deliberately built
without it — so a failure elsewhere is never confounded by the decompressor.
**102/102 tests pass.**

**Real software.** Stock xv6, 5,253 compressed instructions, boots to a shell
and runs `ls`, `cat` and the full `usertests`.

---

## Why the immediates look like that

Worth one paragraph, because it is the part that looks arbitrary and is not.

`C.J`'s 11-bit offset is scattered across the instruction as
`[11|4|9:8|10|6|7|3:1|5]`. That ordering is not obfuscation and it is not
historical accident: it is chosen so that each bit lands in the same *physical
wire position* it occupies in the 32-bit `JAL` encoding wherever possible. The
expansion is then mostly a rewiring rather than a shuffle, which costs a real
decoder almost nothing.

In software it costs verbosity instead, which is why every encoding in
`src/decoder.cpp` is written out explicitly with the spec's own bit-range
notation beside it. Guessing at these produces an emulator that runs most
programs and corrupts a few.

---

## What is still missing from phase 8

- **F and D** — floating point. `C.FLD`, `C.FSD`, `C.FLDSP` and `C.FSDSP` are
  valid compressed instructions that this phase deliberately leaves illegal,
  since the registers they address do not exist yet.
- **A device tree**, which Linux requires to discover the machine.
- **SBI**, the supervisor-mode interface a Linux kernel calls into.
