# Phase 10: more than one hart

Until now there was one CPU. This phase gives the machine several, and — more
interesting than the count — changes who is in charge.

## Why a `Cpu` can't run a machine

A `Cpu` knew how to execute instructions, and for one hart that was enough: on
every instruction it ticked the timer, sampled the UART, asked the interrupt
controller what was pending, and then executed. With several harts that breaks
twice over.

- **The devices are shared.** There is one clock and one set of pending
  interrupts. If each of N harts ticked the timer on every instruction, time
  would run N times too fast, and the controller would be polled N times as
  often for no new information.
- **Somebody has to decide who runs.** The harts execute on one host thread, so
  they take turns.

The `Platform` ([`include/platform.hpp`](../include/platform.hpp)) is that
somebody. It owns the harts, schedules them, and polls the shared devices.

## Scheduling: deterministic quanta

Round-robin, in fixed *quanta*. Each runnable hart executes `--quantum`
instructions (256 by default), then the next one does. After each round the clock
advances by one quantum — the time the harts, running in parallel, would have
taken — and the devices are polled once, for all harts together.

This is **deterministic**: the same inputs interleave the same way every run.
That is the whole reason not to use host threads. A lost wake-up or a missing
memory barrier in a guest kernel shows up as a hang that happens one run in a
hundred on real hardware; here it happens every run, at the same instruction,
and can be debugged.

It is also *faster* than polling per instruction, because the devices are touched
once per quantum rather than once per step.

The price is that a device event can reach a hart up to a quantum late. That is a
few hundred instructions out of the ten million a timer tick spans, which no guest
can observe — with one exception, which turned out to matter.

### A guest's own device access must take effect immediately

`trap_selftest` writes the CLINT's `msip` register to raise a software interrupt
and expects it within a few instructions. With 256-instruction quanta it arrived
a round late, and the test saw different behaviour from the single-CPU emulator.
Real software relies on this: write a device register, and the effect is there
for the next instruction.

So the bus counts accesses to anything that isn't RAM (`Bus::mmio_epoch`), and a
hart ends its quantum early when the count moves. The platform polls the devices
and the hart carries on. Ordinary computation runs at full speed; a store to a
device register is a scheduling point.

The clock has the same problem in a different shape. `mtime` is advanced once per
round, so two back-to-back reads would return the same value — which no real
clock does, and which makes a `rdtime` delay loop spin until the round ends. So a
read of `mtime` (by MMIO or the `time` CSR) adds the instructions the *reading
hart* has retired since the round began (`Clint::mtime_now`).

## What each hart owns, and what is shared

| Per hart | Shared |
|---|---|
| registers, pc, privilege, CSRs (including `mhartid`) | RAM and every device |
| TLB and decoded-instruction cache | the CLINT's `mtime` |
| `mtimecmp` and `msip` (in the CLINT) | the PLIC's pending bits |
| two PLIC contexts: `2h` (M-mode) and `2h+1` (S-mode) | |
| LR/SC reservation | |

The device tree describes this: one `cpu@N` node per hart, each with its own
interrupt controller, and the CLINT and PLIC list one entry (CLINT) or two (PLIC)
per hart. **The position in the PLIC's `interrupts-extended` list *is* the
context number**, which is why the layout has to match the controller's
`2h` / `2h+1` numbering exactly.

## Atomics across harts

AMOs need no special handling: only one hart executes at a time, so they are
atomic by construction. **LR/SC is different.** A reservation must be cancelled
when *another* hart stores to the reserved location, or two harts can both
believe they won the same lock.

Every store a hart completes tells the others (`Cpu::note_store`); any hart
holding a reservation on that 8-byte granule loses it, so its SC fails. The
architecture allows a larger granule, which only ever makes an SC fail more
often — never less, and a spurious failure is always legal. The reservation is
tracked by *physical* address, because two harts can reach the same page through
different virtual addresses.

## Bringing up the other harts: SBI

On real hardware the secondary harts sit in firmware until the kernel releases
them. Here too: under `--linux`, hart 0 enters the kernel and the rest are
*parked*, and the kernel starts them with the SBI **HSM** extension:

- `hart_start(hartid, addr, opaque)` — supervisor mode, translation off,
  interrupts off, `a0` = hart id, `a1` = `opaque`.
- `hart_stop`, `hart_get_status`, and a retentive `hart_suspend`.

The other two multi-hart services:

- **IPI** (`sPI`): a supervisor can't write the CLINT's `msip` (machine-mode
  device), which is why this is an SBI call. It raises `SSIP` on each target.
- **Remote fences** (`RFNC`): flush the target hart's TLB and decoded-instruction
  cache. The harts take turns on one host thread, so the target is idle at that
  instant and the flush is immediate — the strongest ordering the call could
  promise.

Both take the SBI hart-mask convention: bit *i* of the mask names hart
`base + i`, and a base of all-ones means every hart.

## Idling

A hart executing `WFI` with nothing pending is skipped by the scheduler. When
*every* hart is idle, nothing can happen until the next timer deadline, so the
clock **jumps straight to it** rather than retiring millions of empty loop
iterations. A Linux that sleeps costs nothing.

Two safeguards, both found by thinking about what could go wrong:

- **Idling is opt-in.** The architecture lets `WFI` complete immediately, and
  software written against that has to keep working — the official `riscv-tests`
  suite executes a `WFI` with nothing pending and expects to carry on. Only a
  Linux boot turns idling on.
- **Don't skip past the outside world.** If a device is waiting on something real
  — a network reply — jumping the guest's clock forward would let the guest's own
  timeout expire in zero real seconds, before the answer could arrive. Devices
  report this through `io_pending()`, and an idle machine then lets real time
  pass instead.

If every hart is idle, there is no deadline, and nothing external could wake
them, the guest has deadlocked itself. The platform reports that
(`every hart is idle and nothing can wake them`) rather than hanging.

## Using it

```bash
./build/riscv_emu --smp 4 --linux --initrd ... Image   # four harts under Linux
./build/riscv_emu --smp 2 image.elf                    # bare metal: every hart starts at the entry point
```

On bare metal every hart comes out of reset at the same address, as on real
hardware, and sorts itself out by reading `mhartid`.

## Tests

[`tests/test_smp.cpp`](../tests/test_smp.cpp) runs real RISC-V code on several
harts: each hart reading its own id; an AMO counter that must be exact across four
harts interleaving every 7 instructions; LR/SC cancelled by another hart's store
(and *not* cancelled by a store elsewhere); a software interrupt sent through the
CLINT; the full SBI path — `hart_start`, `hart_get_status`, a second start refused,
and an IPI delivered to the started hart as a supervisor software interrupt; and a
WFI that sleeps a million ticks in a few hundred instructions.

Three of those were **mutation-checked**: with cross-hart reservation
cancellation, IPI delivery, or the idle clock-skip deliberately broken, the
corresponding test fails.
