# Phase 7 — The PLIC, virtio-blk, and booting xv6

This is the phase the whole project was aimed at. At the end of phase 6 the
emulator had a complete RV64IMA core, three privilege modes, traps with
delegation, and Sv39 paging — everything a kernel needs to *run*. What it did
not have was a way for a kernel to be *interrupted by the outside world*, or
anywhere to keep a filesystem. Those two gaps are what this phase closes, and
closing them is what turns "executes instructions correctly" into "boots an
operating system".

The result:

```
xv6 kernel is booting

init: starting sh
$ ls
.              1 1 1024
..             1 1 1024
README         2 2 2441
cat            2 3 36728
...
$ cat README
xv6 is a re-implementation of Dennis Ritchie's and Ken Thompson's Unix
Version 6 (v6).  ...
```

Every byte of that `README` came off a virtio disk image, through a virtqueue,
through the page tables, into a user process, and back out over the UART.

---

## Part 1 — The PLIC

### Why a second interrupt controller

Phase 4 already added the CLINT, which delivers timer and software interrupts.
Both of those are things a hart raises *for itself*: the timer fires because
this hart's `mtimecmp` was reached. Neither says anything about the rest of the
machine.

External interrupts are different. A keystroke arrives at the UART; a disk
finishes a read. Many devices, one hart, and a single wire (`MEIP`/`SEIP`)
between them. Something has to sit in the middle and answer two questions:

1. Given several devices shouting at once, which one does the hart hear about?
2. Once the hart is listening, *which device was it?*

That is the PLIC — the Platform-Level Interrupt Controller — and its answer to
the second question is the **claim/complete handshake**.

### Claim and complete

The interrupt line into the hart carries no identity: `mip.SEIP` set means
"something external happened", nothing more. So the handler's first act is to
read the PLIC's *claim* register, which does two things at once:

- returns the highest-priority pending IRQ, and
- marks that IRQ **in service**, so the PLIC stops offering it.

When the handler is done it writes the same IRQ back to the same register. That
is *complete*, and it releases the in-service mark.

The in-service step is not bookkeeping — it is what makes the whole thing work.
The UART's line is **level-triggered**: it stays asserted for as long as a
character is sitting unread. If the PLIC kept offering an IRQ that a handler was
already inside, the hart would trap into that handler again on the very next
instruction, before the handler could get far enough to read the character and
quiet the line. The kernel would make no forward progress at all, and from the
outside it would look exactly like a hang.

### The bug the tests found

`Plic::claim()` clears the source's pending bit — correct, and the spec says so.
The first version of `complete()` did the obvious mirror image: clear the
in-service bit and stop there.

That is wrong, and `tests/test_interrupt_devices.cpp` caught it. Consider a
second keystroke arriving *while* the first is being handled. The line is still
asserted, but the pending bit was cleared by the claim, and nothing puts it
back. The interrupt is silently lost.

Real hardware does not have this problem because the pending bit is not the
device's line — it is what the PLIC's *gateway* has forwarded from that line.
The emulator now models both:

```cpp
u64 line_;      // what the devices are asserting right now
u64 pending_;   // what the gateway has forwarded and not yet had claimed
u64 claimed_;   // in service
```

`complete()` re-raises pending if the line is still up. The two bitmaps are
identical except in the window between a claim and its completion — which is
exactly the window where the bug lived. This is the kind of defect that would
have shown up as a rare, timing-dependent lost keystroke under load; far easier
to find with a unit test than in a running kernel.

### Contexts

A *context* is one (hart, privilege level) pair. Each has its own enable bitmap
and its own priority threshold, so machine-mode firmware and a supervisor kernel
can care about different devices. For hart 0, context 0 is machine mode and
context 1 is supervisor mode. xv6 uses context 1 exclusively — it delegates
everything to S-mode and never runs an M-mode handler at all.

A source reaches a context only if all three hold:

- its priority is non-zero (priority 0 means "never interrupt"),
- its enable bit is set for that context, and
- its priority is **strictly greater** than that context's threshold.

### Making it fast

The CPU consults the PLIC once per instruction, to sample the UART line and let
the PLIC drive `mip`. Arbitration is a scan over all 64 sources, so doing it
per instruction cost more than emulating the instruction did.

The answer only changes when interrupt state changes, so it is cached and
invalidated on every write:

```cpp
mutable std::array<u32, NUM_CONTEXTS> best_;
mutable bool dirty_ = true;
```

That single change took the emulator from **7M to 15.4M instructions per
second** — a 2.4× speedup, and the difference between an xv6 boot that takes
half a minute and one that takes a minute and a half.

---

## Part 2 — virtio-blk

### Paravirtualisation

The UART is a faithful emulation of a real 1980s chip, register for register.
A disk could be done the same way — emulate an IDE or AHCI controller — but
there is no reason to. The guest knows it is virtualised; the two sides can
simply agree on a protocol designed for the purpose. That is virtio, and its
central idea is the **virtqueue**.

A virtqueue is three arrays in *guest* memory:

| | Written by | Contents |
|---|---|---|
| descriptor table | driver | a pool of `{addr, len, flags, next}` entries |
| available ring | driver | "here are chain heads I want processed" |
| used ring | device | "here are the ones I have finished" |

The driver fills in descriptors, appends the head index to the available ring,
bumps `avail.idx`, and writes `QueueNotify`. The device walks the chains, does
the work, appends to the used ring, bumps `used.idx`, and raises an interrupt.
Neither side ever blocks on the other, and the only registers involved are the
notify and the interrupt — everything else is shared memory.

### A block request

Three descriptors, chained by `next`:

```
[0] header   {u32 type; u32 reserved; u64 sector;}   device reads
[1] data     512 bytes per sector                    device reads or writes
[2] status   one byte, 0 = OK                        device writes
```

`type` is 0 for a read and 1 for a write. Descriptor [1] carries the
`VIRTQ_DESC_F_WRITE` flag on a read — the flag describes what the *device* does
to the buffer, which reads backwards until you internalise it.

### The device is a bus master

Every other device in this emulator is a *target*: the CPU addresses it, and it
responds. The virtio device is the first that reaches into guest memory on its
own — it has to, because the descriptors, the rings and the data buffers all
live there. That is why it holds a `Bus*`:

```cpp
void attach(Bus* bus, Plic* plic, u32 irq);
```

Which is also a reminder that the addresses in a descriptor are *physical*. The
driver translates before it fills them in; the device never sees a virtual
address and never consults `satp`.

### Version 2, not legacy

virtio-mmio has a legacy layout and a modern one, and they differ in how the
queue's location is communicated: legacy uses a single page-frame number,
modern uses three separate 64-bit addresses (`QueueDescLow/High`,
`QueueDriverLow/High`, `QueueDeviceLow/High`). This emulator implements
**version 2**, the modern one, which is what current xv6 drives.

---

## Part 3 — The console becomes bidirectional

Until this phase the console was half a console. A guest could print, but
nothing could be typed at it — so xv6 would reach a shell prompt that could not
be used, which is a strange place to stop.

The UART now takes the host's standard input as its receive line. Three things
have to be arranged, and leaving out any one of them breaks the shell in a
different way:

- **Raw mode.** A terminal in its normal (canonical) mode buffers a whole line
  and only hands it over on Enter, and interprets keys like Ctrl-C itself. The
  guest shell wants each keystroke as it is struck, and it wants Ctrl-C
  delivered to the *guest*, not to the emulator.
- **Non-blocking.** The poll runs from the CPU's inner loop. A blocking read
  with nothing typed would stop the machine dead.
- **Restore on exit.** Raw mode is a property of the terminal, which outlives
  the process. Not putting it back leaves the user's shell with no echo.

The poll runs every 4096th instruction rather than every instruction — a
`read()` syscall per emulated instruction would cost far more than the
instruction itself. At 15M instructions a second that is still a poll every
quarter of a millisecond, which no typist will ever notice.

When stdin is a pipe rather than a terminal there is nothing to put into raw
mode, and the non-blocking read still works. That is what makes this possible:

```bash
printf 'ls\n' | ./build/riscv_emu --disk fs.img kernel
```

---

## Part 4 — Two bugs that stood between "kernel runs" and "xv6 boots"

Both were in code written in earlier phases and passing every test in the
repository. Neither was found by riscv-tests. Real software found them, which
is the argument for booting a real OS in the first place.

### The interrupt storm

Symptom: xv6 printed `xv6 kernel is booting` and then made no progress
whatsoever, still spinning after three billion instructions.

Cause: `Uart::interrupting()` reported the transmit-holding-register-empty
condition as a *level*:

```cpp
// wrong
if (ier_ & IER_TX_EMPTY) return true;   // "the transmitter is never busy"
```

Which is true, and useless. Our transmitter completes a write instantly, so the
register *is* always empty — so the moment xv6 enabled the TX interrupt (it
enables TX and RX together), the line asserted and never dropped. Every
instruction trapped into the console handler.

Real hardware asserts THRE when the register *becomes* empty, and clears it when
the driver reads IIR. It is an edge, not a level. The fix is one latch:

```cpp
bool tx_irq_ = false;               // set when a byte is written to THR
                                    // cleared when the driver reads IIR
```

### The fatal-trap check that ignored delegation

Symptom, after the UART fix: xv6 got all the way to user mode — 420 million
instructions, through `kinit`, paging, `procinit`, `plicinit`, `virtio_disk_init`
and `userinit` — and then stopped with:

```
ecall from U-mode (cause 8, tval 0x0) at pc 0x568
```

`pc 0x568` is a user virtual address, and the cause is `initcode` making its
very first system call. Everything had worked; the emulator stopped on the trap
the kernel was waiting for.

Cause: a debugging affordance from phase 2. A hart out of reset has `mtvec = 0`,
so an early trap would vector to address 0, fault on the fetch, and vector to 0
again — an infinite loop that looks exactly like a hang. So the emulator stops
and reports instead, while `mtvec` is still zero.

The check tested `mtvec` and nothing else. xv6 delegates every exception to
supervisor mode (`medeleg = 0xffff`), installs `stvec`, and **never writes
`mtvec` at all** — so `mtvec == 0` is entirely legitimate, and the check fired
on a trap that was about to be delegated to a handler that existed.

The fix is to ask the question the check was always meant to ask: does the mode
that will *actually receive* this trap have a vector installed?

```cpp
bool Cpu::handler_installed_for(const Trap& trap) const {
    const bool to_supervisor = (priv <= PRIV_SUPERVISOR) &&
                               csrs.delegated_exception(trap.cause_code());
    return csrs.read(to_supervisor ? csr::STVEC : csr::MTVEC) != 0;
}
```

Same delegation decision `enter_trap()` makes, one line earlier.

---

## Building and booting xv6

xv6's stock build targets `-march=rv64gc`, and `g` implies `f`/`d` (floating
point) while `c` means compressed 16-bit instructions. Its kernel contains
**5,253 compressed instructions**, none of which this emulator can decode yet —
that is phase 8. Rebuilding with the extensions this emulator implements yields
zero:

```bash
git clone https://github.com/mit-pdos/xv6-riscv
cd xv6-riscv
make CFLAGS_EXTRA='-march=rv64ima_zicsr_zifencei -mabi=lp64' \
     kernel/kernel fs.img
```

Then:

```bash
./build/riscv_emu --disk path/to/fs.img path/to/kernel/kernel
```

`docs/RUNNING.md` has the full recipe, including how to check that a build
really is free of compressed instructions before you try to boot it.

### CSRs xv6 needed that earlier phases had not implemented

Three, all found by booting and hitting an illegal-instruction trap at a precise
address:

- **PMP** (`pmpcfg0`, `pmpaddr0`). xv6's `start()` configures a
  physical-memory-protection region covering all of memory before dropping to
  supervisor mode. These are **stored but not enforced** — a real limitation,
  documented as such. A guest that relied on PMP for isolation would not get it.
- **`menvcfg`** (0x30a), whose `STCE` bit enables the next one.
- **`stimecmp`** (0x14d) — the **Sstc** extension, which lets a supervisor set
  its own timer deadline directly instead of calling into M-mode firmware for
  every tick. xv6 uses it, so the CLINT now drives `STIP` from `stimecmp`
  whenever `menvcfg.STCE` is set.

---

## What the boot actually does

Worth following once, because every phase of this project appears in it:

1. **`_entry`** (`entry.S`), in machine mode at `0x8000_0000`. Sets up a stack
   per hart. *(Phase 1.)*
2. **`start()`** (`start.c`). Still in M-mode. Sets `mstatus.MPP` to supervisor,
   points `mepc` at `main`, clears `satp`, delegates every exception and
   interrupt to S-mode (`medeleg`/`mideleg` = `0xffff`), opens PMP, enables the
   Sstc timer, and executes `mret`. *(Phases 2 and 6.)*
3. **`main()`**, now in supervisor mode. `kinit` builds the page allocator;
   `kvminit` builds the kernel page table and `kvminithart` writes `satp` —
   **paging is on from this instruction onward**. *(Phase 6.)*
4. `plicinit`/`plicinithart` set the UART and virtio priorities and enable them
   for supervisor context 1. *(This phase.)*
5. `binit`, `iinit`, `fileinit`, `virtio_disk_init` — the buffer cache, the
   inode layer, and the disk. *(This phase.)*
6. `userinit` creates the first process from `initcode`, and `scheduler()` runs
   it. `initcode` calls `exec("/init")`, whose text is read from the virtio disk
   through the buffer cache.
7. `/init` opens the console, forks, and executes `sh`. The prompt appears.

Every layer has to be right for step 7 to happen, which is why booting an OS is
such a good test: `ls` printing a directory means the decoder, the MMU, the
trap machinery, the PLIC and the disk are all simultaneously correct.

---

## Tests

`tests/test_interrupt_devices.cpp` covers both new devices — 728 checks:

**PLIC.** That an idle controller reports nothing and claims IRQ 0; that a
pending source drives `SEIP` and dropping the line drops it; that priority 0, a
cleared enable bit, and a too-high threshold each suppress a source; the full
claim/complete cycle including the re-raise; that arbitration picks by priority
and not by IRQ number (checked in both directions, since the naive
lowest-number scan would agree by accident in one of them); and that the M and S
contexts are genuinely independent.

**virtio-blk.** That the device identifies itself as modern virtio-mmio block;
that config space reports the capacity; that a read delivers the right sector's
bytes and a write reaches the disk without spilling into its neighbour; that
completion raises an interrupt through the PLIC and acknowledging it drops the
line; that consecutive requests are all noticed (which only works if
`last_avail_` advances); and that a request past the end of the disk is refused
with a non-zero status rather than reading out of bounds.

The disk is filled with a pattern where byte *i* of sector *s* is *s + i*, so a
wrong sector and a wrong offset each produce a distinguishable failure.

---

## What this phase does not do

- **PMP is stored, not enforced.** Enough to boot; not enough to isolate.
- **One hart.** xv6 is built for multiprocessing, but this emulator has a single
  hart, so `hart 0` does all the work. The CLINT and PLIC both have the shape to
  grow more contexts.
- **`mtime` counts instructions, not nanoseconds.** Deterministic, which is what
  a debuggable emulator wants, but a guest that calibrates a delay loop against
  wall-clock time will find the clock strange.
- **No compressed instructions.** xv6 has to be rebuilt without `c`. Removing
  that requirement is the first item of phase 8.
