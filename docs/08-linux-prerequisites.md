# Phase 8 — What Linux needs that xv6 did not

xv6 boots on a machine that is barely more than a CPU with a UART. It is
written for exactly this hardware, with the console's address compiled into it,
its own machine-mode startup code, and no floating point anywhere.

Linux assumes none of that. The same kernel binary boots on a SiFive board, a
QEMU virt machine and this emulator, so it has to *discover* the hardware at
runtime. It runs entirely in supervisor mode and expects firmware underneath it
for the things supervisor mode cannot do. And it is built by a compiler that
uses the full instruction set, floating point included.

Four pieces, then. The C extension has [its own note](08-compressed-instructions.md);
this one covers the other three.

---

## Part 2 — F and D: floating point

### Why an emulator needs them even when the kernel does not

The Linux kernel itself avoids floating point almost entirely. But it has to
*save and restore* the floating-point registers across a context switch, because
user programs use them — and the C library uses them for `printf`. So the kernel
touches `mstatus.FS` and the `f` registers on every process switch, and a
userspace that prints anything at all needs the arithmetic to be right.

### Thirty-two more registers, and one awkward problem

The F and D extensions share one register file: 32 registers, 64 bits each.
Doubles live in them directly. Singles do not fit, and something has to happen
when a program stores a float and reads it back as a double.

The spec's answer is **NaN boxing**. A 32-bit value in a 64-bit register is
valid only if the upper half is all ones — which is, not coincidentally, the
encoding of a quiet NaN. Read that register as a double and you get a NaN: a
value that says "this is not a double" and propagates through arithmetic,
instead of silently producing a plausible-looking wrong answer.

```cpp
constexpr u64 nan_box(u32 single) { return 0xffffffff00000000ull | single; }

constexpr u32 nan_unbox(u64 reg) {
    return (reg >> 32) == 0xffffffffull ? static_cast<u32>(reg)
                                        : CANONICAL_NAN_F32;
}
```

Every instruction that writes a single boxes it; every instruction that reads
one checks the box. Skip either and a type error becomes a wrong number.

**With exactly one exception**, and it is the kind of thing that only a
reference suite finds. `FMV.X.W` moves the low 32 bits of a float register into
an integer register, sign-extended. It is a *raw bit move* — no conversion, no
rounding, no flags — and it does **not** apply the boxing rule. Unboxing there
would replace the bits software asked to see with a canonical NaN, defeating the
entire purpose of the instruction. `riscv-tests` `rv64ud/move` check 71 builds a
deliberately unboxed register and reads its low half back; the first
implementation here failed it.

### mstatus.FS: an enable disguised as an optimisation

Two bits in `mstatus` track the floating-point unit's state: Off, Initial,
Clean, Dirty. The obvious reading is that this is a context-switch
optimisation — a kernel only has to save 32 registers if the process actually
touched one, and Dirty is what tells it so.

It is also an *enable*. While FS is Off, every floating-point instruction and
every access to `fcsr` raises an illegal-instruction trap. That is not a detail:
it is how a kernel with no FPU support stops user code from quietly corrupting a
register file nobody is saving. Two processes would otherwise share it, and the
symptom would be one process's numbers appearing in another's.

So every FP instruction here begins with a check and ends, if it wrote anything,
with `mark_fpu_dirty()`. Forgetting the first lets user code use registers the
kernel is not saving; forgetting the second lets a context switch skip saving
registers that were modified. Both are silent.

`fcsr` is gated by the same bit, for the same reason — the rounding mode is as
much floating-point state as the registers are, and a process must not read the
one a *different* process left behind.

### Rounding, and the mode that does not exist

RISC-V carries a rounding mode in two places: each instruction has an `rm`
field, and `fcsr` holds a default that `rm = 7` (DYN) selects. Four of the five
modes map straight onto C's `fesetround`.

The fifth, RMM — round to nearest, ties **away from zero** — has no C equivalent
and no hardware support on x86 or ARM. It is also essentially unused: compilers
do not emit it and no libm depends on it. Rather than build a whole soft-float
path for it, this emulator maps it to round-to-nearest-even, which differs only
on an exact tie. **That is a real deviation** and is documented as one rather
than quietly absorbed.

Modes 5 and 6 are reserved, and an instruction asking for one is illegal rather
than defaulting to something plausible. Software that asks for a mode that does
not exist has a bug, and hiding it helps nobody.

### Where RISC-V and C disagree

Three places, each of which would be a silent wrong answer if you assumed the
host's behaviour:

**Out-of-range conversion.** C says float-to-integer conversion out of range is
undefined. RISC-V says it *saturates* to the largest or smallest representable
value and raises Invalid — and a NaN converts to the maximum **positive** value,
not to zero. Surprising, and specified.

**`std::nearbyint` versus `std::rint`.** Both apply the current rounding mode
and round identically. They differ in one respect: `nearbyint` is specified to
*suppress* the inexact exception. RISC-V requires `fcvt` to raise it, so
`nearbyint` gives the right number with the wrong flags. `rv64uf/fcvt_w` check 2
converts `-1.1` and expects `fflags = 0x01`; that is what caught it.

**FMIN/FMAX.** Neither C's `fmin`/`fmax` nor a comparison. A signalling NaN
raises Invalid; if one operand is NaN the result is the *other* operand; if both
are, it is the canonical NaN; and `-0.0` is defined to be less than `+0.0` —
which no comparison reports, since `-0.0 == +0.0` is true.

### Fused multiply-add

The point of `FMADD` is that the product is **not rounded** before the addition:
the whole `a*b+c` is computed once and rounded once. `std::fma` is exactly this;
writing `a * b + c` instead rounds twice and differs in the last bit.

That difference is precisely what numerical code uses `fma` to avoid, so it is
not something to wave away. `test_fma_rounds_once` pins it with
`(1 + 2⁻³⁰)² − 1`, where the unfused answer is exactly `2⁻²⁹` and the fused one
keeps the `2⁻⁶⁰` term. The test marks its operands `volatile`, because otherwise
the compiler contracts `a * b + c` into an `fma` itself and both sides agree for
the wrong reason.

---

## Part 3 — The device tree

### The problem it solves

How does a kernel that has never seen this machine find the console?

xv6 does not have this problem: `UART0` is `#define`d in its source. Linux
cannot work that way — one binary, many machines. So the bootloader leaves a
**device tree** in memory and passes its address in `a1`, and everything the
kernel knows about the hardware comes from that blob.

The format is deliberately austere, because it is parsed by a kernel with no
allocator, no console, and no idea where anything is:

```
header      magic, sizes, offsets to the three blocks below
memory      reservation map (regions the kernel must not use)
structure   a token stream: BEGIN_NODE, PROP, END_NODE, END
strings     every property name, once, referenced by offset
```

Property names are pooled because real trees repeat `compatible` and `reg`
constantly. **Everything is big-endian**, including on a little-endian machine,
which is a standing trap for anyone writing one by hand.

### Generated, not shelled out to `dtc`

`src/fdt.cpp` builds the blob directly. That keeps the emulator dependency-free,
but the better reason is that the tree is generated from **the same constants in
`types.hpp` that the devices are attached with**. A `.dts` file maintained
alongside the code is a second description of the machine that can drift out of
agreement with the first; this one cannot.

The runner checks it the honest way: `dtc -I dtb -O dts` parses the generated
blob back, so the structure is validated by the reference implementation rather
than by the code that produced it.

### The properties that matter most

Two are easy to leave out and produce failures that look like something else:

**`stdout-path`.** Without it Linux has a UART driver *and* a device tree node
describing the UART, and still boots silently — nothing told it that particular
UART is the console.

**`riscv,isa`.** How Linux discovers what it may use. It must match what the
emulator actually implements: claim more and the kernel executes an instruction
that traps; claim less and it takes a slower path for nothing.

And `timebase-frequency` is a claim about how fast the emulator runs. Our
`mtime` advances once per instruction, so the number is an estimate, and the
guest's idea of wall-clock time is only as good as it.

---

## Part 4 — SBI

### Firmware, from the kernel's point of view

Linux runs in supervisor mode, and there are things supervisor mode cannot do.
`mtimecmp` is a machine-mode register, so a kernel cannot set its own timer.
Nothing in S-mode can send an interrupt to another hart or turn the machine off.

On real hardware a firmware layer — OpenSBI, almost always — sits in machine
mode and provides these, and the kernel reaches it with `ecall`. SBI is a
syscall interface with the *kernel* in the position userspace normally occupies:

```
a7   extension ID       a6   function ID
a0.. arguments          a0   error code on return, a1 the value
```

### Implemented here, rather than loading OpenSBI

Running real firmware would be more faithful. It would also be a second binary
to build, a second thing to debug, and it would hide the boot behind 100 KB of
code that is not the subject of this project.

Implementing the interface directly means the kernel takes **exactly the same
path it would take on real hardware** — the same `ecall`, the same register
convention, the same return codes — and the thing it calls into is fifty lines
you can read. `src/sbi.cpp` is shorter than the section of this document
describing it.

With `--linux`, the emulator enters the kernel the way a bootloader would: in
supervisor mode, `a0` = hart ID, `a1` = device tree, SBI available.

### Three details worth stating

**Clearing MTIP in `set_timer` is required and easy to miss.** The interrupt
that just fired is still pending. Set a new deadline without clearing it and the
kernel re-enters its timer handler on the very next instruction, forever. It
looks exactly like a hang.

**The legacy `set_timer` returns nothing at all** — not even a success code — so
`a0` must be left exactly as the caller set it. Writing a return code there
corrupts a register the kernel still considers live.

**Single-hart IPI and remote-fence calls return success, not "unsupported".**
There is nobody to send an IPI to and no remote TLB to fence, so both are
genuinely no-ops — and reporting failure would send the kernel down a fallback
path for a situation that is not a failure.

SBI is off by default, because a kernel that provides its own machine-mode code
must be allowed to see its own `ecall`s. xv6 does exactly that.

---

## Testing

| | |
|---|---|
| `rv64uf` 11/11, `rv64ud` 12/12 | the official floating-point suites |
| `tests/test_float.cpp` | 124 checks: boxing, the FS enable, rounding modes, the RISC-V/C disagreements, fused multiply-add |
| `tests/test_firmware.cpp` | 76 checks: device-tree structure and content, every SBI call, and that SBI is off unless asked for |
| `dtc -I dtb` | the generated device tree, parsed back by the reference implementation |

**125/125 riscv-tests pass.**

---

## What this adds up to

The emulator now presents a machine a general-purpose kernel can boot on: a full
RV64GC instruction set, three privilege levels, Sv39 paging, interrupt
controllers, a disk, a description of itself, and firmware underneath.

[Phase 9](09-booting-linux.md) is what happens when you point Linux at it.
