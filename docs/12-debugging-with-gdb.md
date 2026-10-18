# Phase 12: debugging with gdb

When a guest kernel hangs, an instruction trace is a firehose: hundreds of millions
of lines. A debugger is the right tool — stop at the function you care about, look
around, step. The emulator speaks the **GDB remote serial protocol**, so a stock
`gdb` (or `gdb-multiarch`, or an IDE's debugger front end) attaches to it exactly
as it would to QEMU or to real hardware behind a debug probe.

```bash
./build/riscv_emu --gdb 1234 --linux Image        # waits for the debugger; nothing has run yet
```
```
$ gdb-multiarch vmlinux
(gdb) set architecture riscv:rv64
(gdb) target remote :1234
(gdb) break start_kernel
(gdb) continue
(gdb) info registers
(gdb) x/8i $pc
(gdb) watch jiffies
(gdb) info threads                # one thread per hart
(gdb) monitor csr satp            # anything gdb has no command for
```

`--gdb 0` picks a free port and prints it. Ctrl-C in gdb stops a running guest.
Detaching lets the guest run on; killing ends the emulator.

## What you get

| | |
|---|---|
| registers | the 32 integer registers and `pc`; `f0`–`f31`; and control registers by name (`mstatus`, `satp`, `sepc`, `scause`, ...) through a generated target description |
| memory | read **and write**, through the *guest's own page tables* — gdb speaks virtual addresses, so `x/s 0xffffffff80001000` works in a running kernel |
| breakpoints | `break`, `tbreak`, `hbreak` |
| watchpoints | `watch` (write watchpoints) |
| execution | `continue`, `step`, `stepi`, Ctrl-C |
| harts | every hart is a gdb *thread*; `thread 2` switches registers and memory to hart 1 |
| `monitor` | `harts`, `csr NAME`, `time` |

Not supported, and answered honestly with the protocol's "unsupported" reply: read
and access watchpoints (they would need a hook on every load), and reverse
debugging.

## How it works

A packet is `$` *payload* `#` and two hex digits of checksum (the payload bytes summed
mod 256); the receiver answers `+` or `-`. The payload is a command letter and its
arguments — `m1000,4` reads four bytes at `0x1000` — and the reply is another packet.
Stopping a running target is the one thing that is not a packet: gdb sends a bare
`0x03` byte.

### The handler touches no socket

`GdbStub::handle_packet()` takes a command string and returns a reply string. The
socket code is a thin shell around it. That makes nearly the whole server testable
without a debugger, a network, or a second process: the tests feed it the strings
gdb sends.

### Breakpoints belong to the emulator, not the guest

A conventional debug stub inserts an `EBREAK` instruction into the program. Here the
machine checks each hart's PC *before* it executes an instruction, so **nothing in
guest memory is patched**. Consequences:

- breakpoints work in ROM, in code about to be copied elsewhere, and in pages the
  guest is rewriting;
- they cannot corrupt a checksum the guest computes over its own text;
- a hart sitting on a breakpoint is allowed to leave it (otherwise the first step
  after a stop would stop again at once).

The price is speed. While a debugger is attached the platform drops from its
quantum-per-hart fast path to one instruction per turn, so it can look at every one
(`Platform::before_step`). It is the slow path, used only when someone is watching.

### Watchpoints compare values

A write watchpoint checks, before each instruction, whether the watched bytes have
changed. It therefore doesn't fire on a store of the *same* value, but it needs no
hook in the store path and works through any mechanism that changes the memory —
including a device's DMA.

### Reads that must not have side effects

Memory reads are served from RAM only. Device registers are refused: reading a
UART's receive register or the PLIC's claim register *changes the machine*, and a
debugger that merely looks must not.

### A program's own EBREAK

An `EBREAK` the guest executes with no trap handler installed (the end of a
bare-metal test, say) stops the machine as `SIGTRAP`. Resuming from it steps over the
instruction rather than hitting it forever.

## Tests

[`tests/test_gdbstub.cpp`](../tests/test_gdbstub.cpp), with real programs running
underneath: framing and checksums; register reads and writes (including the
little-endian encoding, and `x0` staying zero); memory through an Sv39 mapping
(and an unmapped address being an *error*, not a guest fault); a breakpoint stopping
*before* the instruction with guest memory unmodified; single-step; resuming off a
breakpoint; removing one; a write watchpoint stopping twice; machine exit reported as
`W00`; traps as `SIGTRAP`/`SIGILL`/`SIGSEGV`; threads and `vCont`; the target
description; and a real TCP session including a bad checksum being refused.

Breakpoints, watchpoints and the register encoding were mutation-checked, and a
watchdog makes a broken breakpoint fail a test instead of hanging the suite. The CLI
path was also driven against the real `riscv_emu` process by an independent client:
set a breakpoint, continue, step, run to the end, read `a0`.

**Not tested against a real gdb binary** — none for RISC-V is available on the
machine this was developed on. The protocol subset is the standard one QEMU's stub
also speaks (including the convention that `g` returns only the integer registers and
`pc`, with the rest fetched on demand), but a first attach is the real test.
