# Phase 9 — Booting Linux

```
[    0.000000] Linux version 6.6.0 (riscv64-linux-gnu-gcc 13.3.0) #1 SMP
[    0.000000] Machine model: riscv-emu,virt
[    0.000000] SBI specification v0.3 detected
[    0.000000] riscv: base ISA extensions acdfim
[    6.540482] 10000000.serial: ttyS0 at MMIO 0x10000000 (irq = 12) is a 16550A
[    7.138788] virtio_blk virtio0: 1/0/0 default/read/poll queues
[    9.934728] Freeing unused kernel image (initmem) memory: 2200K
[    9.936166] Run /init as init process

=====================================================
  Linux is running on the riscv-emu emulator.
=====================================================

--- /proc/cpuinfo ---
processor       : 0
hart            : 0
isa             : rv64imafdc_zicntr_zicsr_zifencei_zihpm
mmu             : sv39
```

An unmodified Linux 6.6 kernel, built with the stock `defconfig`, booting on an
emulator written from scratch — through SBI probing, memory setup, the device
tree, the console driver, virtio, and into a user process.

Every phase of this project appears in that log. `acdfim` is phases 1, 3 and 8.
`sv39` is phase 6. The `16550A` at `0x10000000` is phase 4, found through the
device tree from phase 8. `virtio0` is phase 7. The timestamps advance at all
because of the SBI timer.

---

## What booting Linux actually required

Phase 8 built the four things Linux needs that xv6 does not — compressed
instructions, floating point, a device tree, and SBI. With all of them in place
the kernel got exactly nowhere: not one character of output.

Five problems stood in the way, and the interesting thing is that **three of
them were not bugs in the emulator at all**. They were missing *firmware*.

### 1. Nobody had delegated the traps

Symptom: complete silence. The instruction trace stopped at the write to `satp`
and never advanced again.

That last detail is the clue. The tracer prints an instruction after it decodes
it, so a trace that *stops* means every subsequent **fetch** is faulting — the
machine is looping without retiring anything at all.

Instrumenting the trap path showed it exactly:

```
TRAP instruction page fault   pc=0x80201048  stvec=0xffffffff80001048  priv=1
TRAP instruction access fault pc=0x0         stvec=0xffffffff80001048  priv=3
TRAP instruction access fault pc=0x0         ...                       priv=3
```

The first trap is correct and expected. Linux enables paging by writing `satp`
and letting the *next fetch* fault: it has already pointed `stvec` at the
virtual address of the instruction after the write, so the fault lands there and
execution simply continues in virtual space. Elegant — and it depends entirely
on that fault reaching supervisor mode.

It did not. `medeleg` was zero, so the fault went to machine mode; `mtvec` was
zero, so the machine vectored to address 0; and the fetch there faulted too,
forever.

On real hardware OpenSBI sets `medeleg` and `mideleg` to all-ones before its
`mret` into the kernel. There was no OpenSBI here, and nothing doing it instead.

### 2. `rdtime` was illegal

With delegation fixed, the kernel reached `udelay` and span in its own
illegal-instruction handler:

```
TRAP illegal instruction pc=0xffffffff808cb00a  priv=1
```

Disassembling that address:

```asm
ffffffff808cb00a:  c01026f3   rdtime a3
```

Reading the `time` CSR from a lower privilege level is illegal unless the level
above enabled it in `mcounteren`. That is a real and useful protection — a
high-resolution clock is what side-channel attacks are built on — but firmware
grants it, and again there was no firmware.

### 3. The SBI timer expired into nothing

`mtimecmp` is a machine-mode register, so its expiry raises `MTIP` — a
*machine* timer interrupt. A supervisor cannot enable that: `mie` is
machine-only, and under `--linux` there is no M-mode software to enable it on
the kernel's behalf.

So the deadline expired into a bit nothing could receive. Jiffies would never
advance and every `msleep` in the kernel would hang forever — a failure that
looks exactly like the emulator being slow rather than being wrong, which is the
worst kind.

Real firmware takes the machine timer interrupt and posts a supervisor one in
its place. The CLINT now does the same, once a deadline has been armed through
SBI.

### 4. virtio was not a modern device

This one *was* an emulator bug, and a precise one: the device reported
`VERSION = 2`, meaning modern virtio-mmio, but never offered
`VIRTIO_F_VERSION_1` — the feature bit that says so. Linux refuses such a device
outright:

> device uses modern interface but does not have VIRTIO_F_VERSION_1

The bit is number 32, in the upper half of a 64-bit feature space that is read
through 32-bit registers, so it is reachable only if the device honours the
`DeviceFeaturesSel` register. Ours ignored it, which would have kept the bit
invisible even after setting it.

xv6 never checks this, which is why phase 7 worked without it — and is a neat
demonstration of why booting a second, stricter OS is worth the trouble.

### 5. The UART said *that* it interrupted, not *why*

With the kernel booted, the console printed perfectly and could not be typed
at. Instrumenting the path showed the PLIC delivering IRQ 10 and Linux claiming
it — and then nothing.

`IIR` was returning a bare `0xc0` for "an interrupt is pending". Bit 0 clear
does mean that, and the inverted sense is the part everyone remembers. But bits
3:1 say **which** condition caused it, and a real driver dispatches on that
field:

```
0b000  modem status change
0b001  transmitter holding register empty
0b010  received data available
0b011  receiver line status
0b110  character timeout
```

`0xc0` leaves that field at `000`. So Linux's 8250 driver, told a modem status
change, read the modem status register, found nothing to do, and returned
without ever touching the receive buffer.

xv6 never reads IIR at all — it checks LSR and drains — which is why this was
invisible for a whole phase. The same pattern as virtio: the first OS was not
strict enough to notice.

With that fixed the console is fully interactive:

```
# 9.96 0.00

#            CPU0
 11:       2494  RISC-V INTC   5 Edge      riscv-timer
 12:         10  SiFive PLIC  10 Edge      ttyS0
 13:          0  SiFive PLIC   1 Edge      virtio0

# powering off
[   10.134004] reboot: Power down

guest requested shutdown through SBI, after 120467864 instruction(s)
```

That `/proc/interrupts` is worth reading closely, because it is the machine
reporting on itself: 2494 timer interrupts arriving through the SBI forwarding
described above, 10 UART interrupts arriving through the PLIC, and virtio
registered on IRQ 1. `uptime` proves the clock advances. `poweroff` leaves
through SBI's system-reset call and stops the emulator.

---

## The shape of the lesson

Three of those four were **absent firmware**, not incorrect emulation. The
emulator was behaving exactly as the specification says a bare hart should: an
undelegated trap goes to machine mode, an unauthorised counter read is illegal,
a machine timer raises a machine interrupt. Every one of those is right.

What was missing is the thing that normally sits underneath a kernel and makes
those arrangements on its behalf. Implementing SBI's *function calls* was phase
8; what phase 9 needed was SBI's *setup* — the register writes OpenSBI performs
before it ever hands control over:

```cpp
cpu.csrs.write(csr::MEDELEG, 0xffff);           // every exception to S-mode
cpu.csrs.write(csr::MIDELEG, 0xffff);           // every interrupt too
cpu.csrs.write(csr::MIE, SEIP | STIP | SSIP);
cpu.csrs.write(csr::MCOUNTEREN, 0xffffffff);
cpu.csrs.write(csr::SCOUNTEREN, 0xffffffff);
```

Five lines, and the difference between total silence and a booting kernel.

---

## Running it

```bash
./scripts/boot-linux.sh
```

Fetches Linux 6.6, builds it with the stock `defconfig`, builds a static `init`
into a cpio initramfs, and boots the result. The first run spends several
minutes compiling the kernel; after that it goes straight to booting.

The prompt is interactive — `cpuinfo`, `meminfo`, `interrupts`, `uptime`,
`poweroff`. `Ctrl-A` then `x` leaves the emulator.

### The initramfs

The kernel starts exactly one process, `/init`, and panics if it exits. Ours is
a single static binary — no shell, no libraries, no dynamic loader — the
smallest thing that proves the boot worked and then stays alive to be typed at.
Busybox would give a real shell and is a much larger build.

### Speed

The emulator runs at roughly 15 million instructions a second and a defconfig
Linux boot is a few billion of them, so the prompt takes a couple of minutes.
The kernel's own timestamps say about ten seconds, because the guest clock is
derived from the instruction count rather than from the wall clock.

That is a deliberate trade made back in phase 4: an instruction-counted clock
makes a run reproducible, so a bug ten million instructions in happens at
instruction ten million every single time. It costs realistic timing and buys
debuggability, which for an emulator is the right way round.

---

## What Linux confirms

Each line of the boot log is a different part of the emulator being verified
independently, by software that has never heard of it:

| Line | What it proves |
|---|---|
| `SBI specification v0.3 detected` | the firmware interface answers correctly |
| `Machine model: riscv-emu,virt` | the generated device tree parses |
| `base ISA extensions acdfim` | `riscv,isa` matches what is implemented |
| `mmu: sv39` | the page-table format was accepted |
| `ttyS0 at MMIO 0x10000000 ... is a 16550A` | the UART is register-accurate enough for the real driver to identify the chip |
| `virtio_blk virtio0` | the modern virtio handshake completed |
| `Freeing unused kernel image (initmem)` | the whole of `start_kernel` ran |
| `Run /init as init process` | user mode, page tables, ELF loading |

The `16550A` line deserves a moment. That is Linux's own 8250 driver probing
registers and identifying the chip — the same driver that runs on real
hardware, reaching the same conclusion about a device that is 200 lines of C++.

---

## Limitations

- **One hart.** `CONFIG_SMP` is on and the device tree describes a single CPU,
  so Linux boots uniprocessor. The CLINT and PLIC both have the shape to grow
  more contexts.
- **The virtio disk reports zero capacity** unless `--disk` is given, so `vda`
  exists but is empty; the root filesystem is the initramfs.
- **PMP is stored, not enforced** — as it has been since phase 7.
- **RMM rounding maps to round-to-nearest-even**, which differs only on an exact
  tie. Documented in phase 8.

None of these stop the boot. All of them are where a more complete emulator
would go next.
