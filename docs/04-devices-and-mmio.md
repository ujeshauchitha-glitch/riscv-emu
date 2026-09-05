# Phase 4 — Devices and memory-mapped I/O

The emulator can now be heard from. A byte written to address `0x1000_0000`
appears on the host's stdout, a timer can interrupt a running program, and a
guest can shut the machine down on its own terms.

That is a bigger change than it sounds. Every phase up to here could be checked
only by inspecting registers from outside. From now on a guest can *tell you
what it is doing*, which is the only practical way to debug a booting kernel.

```
$ ./build/riscv_emu
hello, RISC-V

machine powered off after 79 instruction(s)
```

---

## Memory-mapped I/O

There is no `out` instruction in RISC-V, and no separate I/O address space. A
device is reached by ordinary loads and stores to an address the bus routes to
it instead of to RAM. `sb t2, 0(t0)` with `t0 = 0x1000_0000` does not store
anything — it prints a character.

That is why phase 0 replaced the flat array with a `Bus`. This phase is the
payoff: three new `Device` implementations, registered at the addresses the QEMU
`virt` machine uses so that unmodified kernels find them where they expect.

| Address | Device |
|---|---|
| `0x0010_0000` | syscon — poweroff / reboot |
| `0x0200_0000` | CLINT — timer and software interrupts |
| `0x1000_0000` | UART0 — the console |
| `0x8000_0000` | DRAM |

## The UART, and the DLAB trap

The NS16550A is a serial chip from the early 1980s, and it is what `virt`
exposes — so xv6 and Linux already have drivers for it. Eight byte-wide
registers; the two that matter most are **THR** (write a byte, it is
transmitted) and **LSR** (line status: is there input waiting, is the
transmitter ready).

Our transmitter is always ready, because a write completes immediately — there
is no real serial line to drain. So `LSR` bit 5 is permanently set and a driver's
`while (!(lsr & TX_READY));` loop exits at once.

The detail worth knowing about is **DLAB**. Bit 7 of the LCR register is a
bank-switch: while it is set, registers 0 and 1 stop being data and
interrupt-enable and become the low and high halves of the baud-rate divisor. A
real driver's initialisation is:

```
set DLAB -> write divisor -> clear DLAB -> start sending
```

A UART that ignored DLAB would take those divisor bytes as characters and print
garbage before the first real output. It is a small thing that produces a
confusing symptom, so `tests/test_devices.cpp` checks it explicitly.

## The CLINT, and why the timer matters

The Core Local Interruptor provides two interrupt sources:

- **`msip`** — software interrupts, one hart poking another (or itself)
- **`mtime` / `mtimecmp`** — a free-running counter and a deadline; when
  `mtime >= mtimecmp`, a timer interrupt fires

The timer is what makes **preemptive multitasking possible**. A kernel sets
`mtimecmp` to "now plus one tick"; when the interrupt arrives it runs the
scheduler and sets the next deadline. Without it, a user process that never
makes a system call would keep the CPU forever.

### mtime advances with instructions, not wall-clock time

```cpp
void tick() { mtime_ += ticks_per_instruction; }
```

This is a deliberate choice. Wall-clock time would make every run different: the
timer would land on a different instruction each time, so a kernel that wedges
on its third context switch would wedge somewhere else on the next run — or not
at all. Tying the clock to retired instructions makes runs **reproducible**,
which matters enormously when the thing you are debugging happens ten million
instructions in.

`--timer-divisor` scales it when a guest wants slower ticks.

### The pending bits belong to the device, not to software

This phase changed a phase-2 behaviour, and the change is the more correct one.

`MTIP` and `MSIP` in `mip` are **read-only**. Hardware owns them. A kernel
acknowledges a timer interrupt by moving `mtimecmp` forward — not by clearing
`MTIP`, which it cannot do:

```asm
    li      t5, MTIMECMP
    li      t6, -1
    sd      t6, 0(t5)       # push the deadline out of reach; MTIP drops
```

Phase 2's trap self-test raised a software interrupt by writing `mip` directly,
which worked only because nothing was driving those bits. With a CLINT attached
it stopped working — the device recomputes them every step — and the fix was to
raise it properly through the CLINT's `msip` register. `CsrFile::write_mask` now
makes `mip`'s machine bits unwritable, so the mistake fails loudly rather than
looking like a phantom re-entry.

Note also that `mtimecmp` resets to 0, so `MTIP` asserts immediately from boot.
That is what real hardware does, and it is why every kernel sets `mtimecmp`
before enabling `MTIE`.

## syscon: a guest that can stop

One register. Writing `0x5555` powers the machine off; `0x7777` reboots it.

Until now the only way an emulated program could stop was to trap with no
handler installed — a debugging affordance, not something a guest can rely on.
Now there is a real exit path, and it carries a value: riscv-tests packs a
failure code into the upper bits of the poweroff word, which is how phase 5 will
learn *which* test failed rather than just that something did.

## The ELF loader

A kernel is not a flat blob. It is an ELF file describing where each piece of
itself belongs. Loading it means walking the program headers and copying each
`PT_LOAD` segment to the address it asks for, then starting at `e_entry`.

Two details matter for bare-metal images:

**Load at `p_paddr`, not `p_vaddr`.** A kernel is linked for the virtual
addresses it will use *after* it enables paging, but at boot there is no MMU, so
the physical address is the one that applies. Using `p_vaddr` puts a kernel
linked at `0xFFFF_FFFF_8000_0000` somewhere that does not exist yet.

**`p_memsz` may exceed `p_filesz`.** The difference is `.bss` — storage the
program expects to exist and to be zeroed, but which occupies no space in the
file. Skipping the zero-fill leaves a kernel's globals full of whatever was in
RAM, which produces failures far away from the cause. The test dirties the
region first so a missing zero-fill cannot pass by luck.

The format is detected from the magic number, so `riscv_emu image.elf` and
`riscv_emu image.bin` both work.

---

## Testing

**`tests/test_devices.cpp`** — 43 checks: UART registers including DLAB banking,
CLINT counting and interrupt assertion, `mtimecmp` written as 32-bit halves,
syscon exit codes, ELF loading with `.bss` zero-fill and rejection of malformed
images, plus two integration tests where a guest program prints and powers off,
and where a timer interrupt travels from the device through `mip` into the
guest's handler with nothing poking `mip` directly.

**`examples/device_selftest.S`** — five checks through the real assembler,
printing to the console, arming the timer, spinning until the interrupt lands,
then powering off. The test harness now also checks what the guest *printed*, so
a broken UART fails even if the arithmetic is right.

A note on the built-in demo: writing it by hand produced a nice illustration of
phase 1's fine print. `lui t1, 0x80000` gives `0xFFFFFFFF80000000` on RV64,
because LUI sign-extends from bit 31 — so the demo faulted on its own message
pointer. It uses `auipc` instead, which is what position-independent code does
anyway.

```bash
./build/riscv_emu                                   # the built-in demo
./build/riscv_emu build/device_selftest.bin         # or an ELF, or a flat binary
cd build && ctest --output-on-failure
```

---

**Next:** phase 5 brings in the official `riscv-tests` suite and CI. Everything
up to now has been checked against tests written alongside the code; from there
the correctness signal comes from the reference suite the RISC-V project itself
uses — which is what makes the MMU work in phase 6 tractable.
