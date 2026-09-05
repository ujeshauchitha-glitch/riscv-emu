# RISC-V Emulator — Phase Log

The goal: an RV64 emulator that boots a real operating system — **xv6-riscv**
first, then **Linux**.

Target ISA for xv6: `RV64IMA_Zicsr_Zifencei`, Sv39 paging, M/S/U privilege
modes. Linux additionally needs the C and F/D extensions, a device tree, and SBI.

| # | Phase | Status |
|---|-------|--------|
| 1 | Project setup | ✅ done |
| 2 | CPU state — registers, PC | ✅ done |
| 3 | Fetch / decode / ADDI | ✅ done |
| 0 | Foundation restructure + defect fixes | ✅ done |
| 1 | Complete RV64I | ✅ done |
| 2 | Zicsr + M-mode traps | ✅ done |
| 3 | M and A extensions | ✅ done |
| 4 | ELF loader, UART, CLINT — first output | ✅ done |
| 5 | riscv-tests + CI | ✅ done |
| 6 | S-mode + Sv39 MMU | ✅ done |
| 7 | PLIC + virtio-blk — **boot xv6** | ✅ done |
| 8 | Linux prerequisites (C, F/D, DTB, SBI) | ✅ done |
| 9 | **Boot Linux** | ✅ done |

The first three phases were numbered before the roadmap existed. Phase 0 is
numbered as it is because it is foundational work that logically precedes the
instruction set, even though it was done fourth.

---

## Phase 1: Project setup — done

- CMake build system.
- Folder structure: `src/`, `include/`, `docs/`.

*(An earlier version of this log claimed a `tests/` directory was created here.
It was not; it arrived in phase 0 below.)*

## Phase 2: CPU state — done

- 32 × 64-bit integer registers, `x0` hardwired to zero.
- Program counter.

## Phase 3: Fetch / decode / ADDI — done

- Instruction fetch from memory.
- Field extraction and a first executing instruction, ADDI.

## Phase 0: Foundation restructure + defect fixes — done

No new instructions. This phase fixed what was committed and put an architecture
in place capable of carrying the rest of the roadmap.

**Defects fixed**

- Unbounded array indexing in `Memory` — any out-of-range address read or wrote
  host memory outside the array, and since the array was stack-allocated an
  out-of-range write corrupted the host stack.
- OP-IMM dispatched on opcode alone, so `SLTI`, `SLTIU`, `XORI`, `ORI`, `ANDI`,
  `SLLI`, `SRLI` and `SRAI` all silently executed as `ADDI`.
- The PC advanced past unrecognised instructions instead of trapping.
- The I-type immediate was decoded unconditionally, before the instruction
  format was known.
- Seven lines of `stdout` tracing per instruction, unconditionally.

**Structural changes**

- `Memory` (flat array based at 0) replaced by `Bus` + `Device`, with DRAM at
  `0x8000_0000` and MMIO regions reserved for the devices to come.
- `execute(opcode, rd, rs1, imm)` replaced by `DecodedInst`, which carries every
  field. The old signature could not represent R-, S- or B-type instructions at
  all, so nothing beyond I-type could have been added.
- Traps modelled as return values (`Result<T>` / `Status`) rather than C++
  exceptions.
- Split into compilation units; core built as a static library so tests and the
  executable share it.
- `tests/` created for real, with 3 suites wired into CTest.

**Docs:** [`00-architecture.md`](00-architecture.md)

## Phase 1: Complete RV64I — done

Every base integer instruction is implemented and tested. The emulator now runs
real compiled code.

**Added:** `LUI`, `AUIPC`, `JAL`, `JALR`, all six branches, all seven loads, all
four stores, the ten register-register ALU ops, the nine RV64 `*W` forms,
`FENCE`/`FENCE.I`, `ECALL`, `EBREAK`.

**Fine print handled**

- `*W` results are sign-extended from 32 to 64 bits, and `*W` inputs ignore bits
  32–63. Zero-extending instead passes every small-number test and then breaks a
  kernel thousands of instructions after the actual mistake.
- 64-bit shifts mask the shift amount to 6 bits and select on `funct6`; 32-bit
  shifts mask to 5 and select on `funct7`. Masking is required, not optional —
  an unmasked shift by ≥ 64 is undefined behaviour in C++.
- `JALR` clears bit 0 *after* the addition, and computes its target before
  writing `rd` (they may be the same register).
- A misaligned jump target traps on the *jump*, not on the target, so the PC
  left behind points at the instruction that caused it.
- A faulting load leaves its destination register untouched.
- `funct7 == 0x01` (M extension) and `funct3 != 0` on SYSTEM (CSR ops) trap as
  illegal rather than being mistaken for `ADD` or `ECALL`.

**Testing:** 95 unit-test checks, plus a 14-check bare-metal self-test in
`examples/rv64i_selftest.S` assembled by the real GNU assembler and run under
CTest — validating the decoder against an independent implementation rather than
against our own encoders.

**Docs:** [`01-rv64i.md`](01-rv64i.md)

## Phase 2: Zicsr + M-mode traps — done

A trap now does what hardware does: records why it happened, saves enough state
to return, and jumps to a handler the guest installed. `ECALL` behaves like a
system call instead of halting the machine.

**Added:** the CSR file (`mstatus`, `mtvec`, `mepc`, `mcause`, `mtval`,
`mie`/`mip`, `misa`, `mhartid`, `mscratch`, `medeleg`/`mideleg`, counters), the
six CSR instructions, `MRET`, `WFI`, trap entry and dispatch, and interrupt
delivery with spec priority ordering.

**Fine print handled**

- `mepc` points *at* a faulting instruction (so a page fault can retry) but at
  the *not-yet-run* instruction for an interrupt. A syscall handler's
  `addi mepc, mepc, 4` is the mechanism, not a quirk.
- Interrupts are checked before the fetch, because an interrupt happens between
  instructions rather than being caused by one.
- In vectored `mtvec` mode only *interrupts* are vectored; exceptions still go
  to the base address.
- `CSRRW` with `rd == x0` suppresses the read; `CSRRS`/`CSRRC` with a zero
  source suppress the write. Some CSRs have access side effects, so a
  suppressed access that happened anyway would consume events. This is also why
  `csrr` works on read-only CSRs.
- Writes are masked per register: `mstatus` keeps only implemented bits,
  `mepc` has its low bits hardwired to zero, `misa` ignores writes, `mtvec`
  coerces a reserved mode.
- Unimplemented CSRs trap — that is how software probes for features.
- While `mtvec` is zero a trap stops the emulator rather than looping to
  address 0 forever. A debugging affordance, flagged as such.

**Testing:** 50 unit-test checks, plus a 12-check trap self-test in
`examples/trap_selftest.S` with a real handler, assembled by GNU `as`.

**Docs:** [`02-csrs-and-traps.md`](02-csrs-and-traps.md)

## Phase 3: M and A extensions — done

`misa` now advertises RV64IMA. Both extensions are prerequisites for xv6.

**Added:** `MUL`, `MULH`, `MULHSU`, `MULHU`, `DIV`, `DIVU`, `REM`, `REMU` and
their `*W` forms; `LR`/`SC` in `.W`/`.D`; all nine AMOs in `.W`/`.D`.

**Fine print handled**

- RISC-V division *never traps*. Divide-by-zero and signed overflow have
  specified results. `INT64_MIN / -1` is the case C++ cannot express — it is
  undefined behaviour and raises SIGFPE on x86 — so it is special-cased before
  the division happens.
- `MULHSU` exists for multi-word arithmetic, where a bignum's limbs are unsigned
  but the top one carries the sign.
- A trap clears the LR/SC reservation. On a single hart nothing can break a
  reservation, so every SC would otherwise succeed — but a trap is a context
  switch, and a thread must not inherit another's reservation. Tested with a
  control that isolates the trap as the cause.
- The reservation is consumed by SC whether or not it succeeded.
- Atomics must be naturally aligned, unlike ordinary loads and stores.
- Signed and unsigned min/max AMOs disagree on the same bits.

**Testing:** unit tests for every edge case above, plus a 15-check self-test in
`examples/muldiv_atomic_selftest.S` ending with a working LR/SC spinlock.

**Docs:** [`03-m-and-a.md`](03-m-and-a.md)

## Phase 4: ELF loader, UART, CLINT — done

The emulator can be heard from. A guest prints to the console, gets interrupted
by a timer, and shuts the machine down on its own terms.

**Added:** NS16550A UART at `0x1000_0000`, CLINT at `0x0200_0000`, syscon at
`0x0010_0000`, an ELF64 loader, and a machine assembled from all of them in
`main.cpp`.

**Fine print handled**

- The UART's DLAB bit banks registers 0 and 1 over to the baud-rate divisor. A
  driver sets it during init, so ignoring it prints the divisor bytes as text.
- `mtime` advances with retired instructions, not wall-clock time, so runs are
  reproducible — a timer lands on the same instruction every run.
- `MTIP` and `MSIP` are read-only in `mip`: hardware owns them. A kernel
  acknowledges the timer by moving `mtimecmp`, not by clearing the bit. This
  corrected a phase-2 behaviour and broke the phase-2 self-test, which now
  raises its software interrupt through the CLINT as real code would.
- `mtimecmp` resets to 0, so `MTIP` asserts from boot — which is why kernels set
  it before enabling `MTIE`.
- The ELF loader uses `p_paddr`, not `p_vaddr`: at boot there is no MMU yet.
- `p_memsz > p_filesz` means `.bss` and must be zero-filled.

**Testing:** 43 unit-test checks, plus a 5-check device self-test that prints,
takes a timer interrupt and powers off. The self-test harness now also verifies
console output.

**Docs:** [`04-devices-and-mmio.md`](04-devices-and-mmio.md)

## Phase 5: riscv-tests + CI — done

Correctness no longer rests on tests written alongside the emulator. The suite
the RISC-V project maintains now runs against it: **96/96 passing**, 3 excluded
for features not implemented yet.

| Suite | Result |
|---|---|
| `rv64ui` base integer | 54/54 |
| `rv64um` multiply/divide | 13/13 |
| `rv64ua` atomics | 19/19 |
| `rv64mi` machine mode | 10/10 |

**Added:** `tests/riscv-tests-env/` (a self-contained replacement for the
upstream `riscv-test-env` submodule, which was unreachable here), HTIF `tohost`
support in the emulator — ELF symbol lookup plus polling — a runner script, and
GitHub Actions CI. The repository had no CI before.

**The bug it found.** `rv64mi-instret_overflow` writes 0 to `minstret` and reads
it straight back. `step()` was *assigning* the counter from the emulator's own
instruction count, so any guest write was discarded on the next instruction.
Counters now increment, and an instruction that writes one does not also
increment it — so the value read back is exactly what was written.

**Excluded, with reasons:** `illegal` needs supervisor mode (phase 6, worth
re-running then); `breakpoint` needs the debug trigger module; `pmpaddr` needs
PMP. Both of the latter are optional and not required by xv6 or Linux. They are
listed explicitly in the runner rather than dropped.

**Docs:** [`05-testing.md`](05-testing.md)

## Phase 6: S-mode + Sv39 MMU — done

Three privilege levels and virtual memory. **101/101 riscv-tests pass**,
including `rv64mi/illegal` which phase 5 had to exclude, and the whole `rv64si`
supervisor suite.

**Added:** U/S/M privilege enforcement; the supervisor CSRs (`sstatus`, `stvec`,
`sepc`, `scause`, `stval`, `sie`, `sip`, `sscratch`, `satp`); trap delegation via
`medeleg`/`mideleg`; `SRET`; `SFENCE.VMA`; a full Sv39 walker with superpages,
`A`/`D` updates and a TLB; `mstatus.MPRV`, `SUM`, `MXR`, `TVM`, `TSR`, `TW`.

**Fine print handled**

- `sstatus`/`sie`/`sip` are *views* onto `mstatus`/`mie`/`mip`, not copies. A
  copy would drift, and a kernel clearing `sstatus.SIE` would keep taking
  interrupts inside its critical section. The masking is also a boundary: a
  supervisor writing all-ones must not reach `MIE` or `MPP`.
- Traps taken in machine mode are never delegated — there is nothing more
  privileged to delegate from.
- `SUM` lets a supervisor read user pages but *never* execute them.
- The sign-extension rule on bits 63:39 is what creates the unaddressable hole
  between user and kernel space.
- `W` without `R` is reserved, not write-only. A misaligned superpage faults.
- `SFENCE.VMA` exists because nothing about writing a PTE tells the hardware to
  drop its cached translation.

**Two bugs found.** `mstatus.TVM`/`TSR` were checked by `SFENCE.VMA` and `SRET`
but were not writable, so those checks were dead code that read correctly —
`rv64mi/illegal` exposed it. And `rv64si/ma_fetch` failed on what looked like a
wrong `sepc`; the emulator was right, and the fault was the test environment's
trap entry clobbering `t0` before the handler could read it.

**Docs:** [`06-privilege-and-paging.md`](06-privilege-and-paging.md)

## Phase 7: PLIC + virtio-blk — xv6 boots — done

**The milestone.** xv6 boots to a shell prompt, the prompt can be typed at, and
`ls` and `cat` read real data off a virtio disk image. `scripts/boot-xv6.sh`
does the whole thing — fetch, build, verify, boot — in one command.

```
xv6 kernel is booting

init: starting sh
$ ls
.              1 1 1024
..             1 1 1024
README         2 2 2441
cat            2 3 36728
...
```

**Added:** the PLIC (priorities, per-context enables and thresholds,
claim/complete, driving `MEIP`/`SEIP`); a virtio-mmio **version 2** block device
with the full descriptor/available/used ring protocol, acting as a bus master;
host stdin as the UART's receive line, in raw non-blocking mode with the
terminal restored on exit; the `--disk` option; and the CSRs xv6 needs that
earlier phases had not implemented — `pmpcfg0`/`pmpaddr0` (stored, **not
enforced**), `menvcfg`, and `stimecmp` for the Sstc supervisor timer.

**Two bugs, both in earlier phases' code, neither caught by riscv-tests**

- **A UART interrupt storm.** `interrupting()` reported
  transmit-holding-register-empty as a standing level. Our transmitter finishes
  instantly, so it was true forever — the moment xv6 enabled the TX interrupt
  the line asserted and never dropped, and the kernel re-entered its console
  handler every instruction. It printed one line and then made no progress past
  three billion instructions. THRE is an *edge* on real hardware; it is a latch
  here now, set on a THR write and cleared when the driver reads IIR.
- **A fatal-trap check that ignored delegation.** Traps are reported rather than
  dispatched while `mtvec` is zero, so an early fault does not vanish into an
  invisible loop. But xv6 delegates every exception to S-mode and never writes
  `mtvec` at all — so the check fired on `initcode`'s very first `ecall`, after
  420 million instructions of correct execution. It now follows the same
  delegation decision `enter_trap()` makes, and looks at `stvec` when the trap
  is bound for supervisor mode.

**A third, found by the new tests.** `Plic::complete()` cleared the in-service
bit but did not re-raise a source whose line was still asserted, so a second
interrupt arriving during a handler was silently lost. The PLIC now keeps the
device's line and the gateway's pending bit as separate bitmaps, which is what
real hardware does and what makes the re-raise natural.

**Speed.** The CPU consults the PLIC once per instruction, and arbitration is a
scan over every source. Caching the result behind a dirty flag took the emulator
from 7M to **15.4M instructions per second** — 2.4×, and the difference between
a boot measured in seconds and one measured in minutes.

**Tests:** `tests/test_interrupt_devices.cpp`, 728 checks over both devices —
arbitration by priority rather than IRQ number (verified in both directions),
the full claim/complete cycle, context independence, sector-accurate reads and
writes, interrupt delivery and acknowledgement, consecutive requests, and a
request past the end of the disk being refused rather than reading out of
bounds.

**Known limitation:** PMP registers are stored but not enforced. Enough to boot;
not enough to isolate.

**Docs:** [`07-booting-xv6.md`](07-booting-xv6.md)

## Phase 8: Linux prerequisites — in progress

### Part 1: the C (compressed) extension — done

**Stock, unmodified xv6 now boots.** Phase 7 could only run it after patching
its Makefile off `-march=rv64gc`; the emulator now runs the 5,253 compressed
instructions in its stock kernel directly. `scripts/boot-xv6.sh` no longer
patches anything.

**Approach:** every compressed instruction is *defined by the spec* as being
equivalent to exactly one 32-bit base instruction, so the extension is
implemented as a translation - `u32 decompress(u16)` - with the existing decoder
and every execute path untouched. A parallel 16-bit execute path would have been
a second implementation of instructions that already work, and every bug fixed
in one would have to be found again in the other.

**Where it reaches the CPU:** three places, and no more. Fetch reads a halfword
first, because instruction length is not known until the low two bits have been
read - and because a 32-bit instruction may straddle a page boundary, which is
only reported correctly if each halfword gets its own translation. The PC
advances by `inst.length`. And IALIGN drops from 32 to 16, which incidentally
makes a misaligned jump impossible to express at all: JAL and the branches have
bit 0 of their immediate hardwired to zero, and JALR clears it.

**Two bugs, neither caught by the new unit tests** - both in code around the
decompressor that had quietly assumed every instruction is four bytes:

- `riscv-tests` `rv64uc/rvc` check 36 caught JALR linking `pc + 4`. `C.JALR` is
  two bytes and must link `pc + 2`; a compiler calling through it would get a
  return address one instruction too far and skip whatever followed the call.
- Booting stock xv6 caught `mepc` masking bit 1 as well as bit 0. `main` lands
  at `0x8000_0dee` - 2-byte aligned, ordinary with C - and `mret` returned two
  bytes early into the tail of the function before it. The comment above that
  mask *said* it was assuming no C extension and *named* phase 8 as the phase
  that would invalidate it. It still broke: a note in a comment is not a
  mechanism.

**Tests:** `tests/test_compressed.cpp`, 101 checks - including 57 rows of
encodings produced by the real GNU assembler, each pairing a compressed
instruction with the assembler's encoding of its documented expansion, with
immediates at both extremes of every field's range. Plus the official `rv64uc`
suite in the runner: **102/102 riscv-tests pass**.

**Docs:** [`08-compressed-instructions.md`](08-compressed-instructions.md)

### Part 2: F and D — done

Thirty-two more registers, shared between the two precisions - so a single is
**NaN-boxed**, stored with its upper half all ones, which is the encoding of a
quiet NaN. Read it as a double and you get a NaN rather than a
plausible-looking wrong number. Every instruction that writes a single boxes it
and every one that reads one checks the box, with exactly one exception:
`FMV.X.W` is a raw bit move and must *not* unbox, since unboxing would replace
the bits software asked to see. `rv64ud/move` check 71 caught that.

`mstatus.FS` is an enable, not just a context-switch optimisation: while it is
Off, every floating-point instruction and every `fcsr` access traps. That is how
a kernel with no FPU support stops user code from corrupting a register file
nobody is saving.

**Three places RISC-V and C disagree**, each a silent wrong answer if you assume
the host: out-of-range conversion saturates rather than being undefined;
`std::nearbyint` suppresses the inexact flag that `fcvt` must raise (caught by
`rv64uf/fcvt_w` check 2 - right number, wrong flags); and FMIN/FMAX are neither
`fmin`/`fmax` nor a comparison, with `-0.0` defined as less than `+0.0`.

**One documented deviation:** RMM (ties away from zero) has no C equivalent and
no hardware support on x86 or ARM. It maps to round-to-nearest-even, which
differs only on an exact tie.

### Part 3: the device tree — done

Linux gets everything it knows about the machine from a blob left in memory.
`src/fdt.cpp` generates it directly rather than shelling out to `dtc` - not for
dependency reasons but because it is then built from the same constants in
`types.hpp` the devices are attached with, so it cannot drift out of agreement
with them. The runner validates it by parsing it back with `dtc`.

Two properties are easy to omit and fail confusingly: without `stdout-path`
Linux has a UART driver *and* a node describing the UART and still boots
silently; `riscv,isa` must match what the emulator implements exactly.

### Part 4: SBI — done

`mtimecmp` is a machine-mode register, so a supervisor cannot set its own timer.
Implemented directly rather than by loading OpenSBI - real firmware would be
more faithful but is a second binary to build and hides the boot behind 100 KB
of code that is not the subject of this project. The kernel takes exactly the
path it would on real hardware.

Clearing MTIP in `set_timer` is required and easy to miss: the interrupt that
just fired is still pending, and leaving it set makes the kernel re-enter its
timer handler every instruction - which looks exactly like a hang.

**Tests:** `tests/test_float.cpp` (124 checks), `tests/test_firmware.cpp` (76).
**125/125 riscv-tests**, now including `rv64uf` 11/11 and `rv64ud` 12/12.

**Docs:** [`08-linux-prerequisites.md`](08-linux-prerequisites.md)

## Phase 9: Linux boots — done

An unmodified Linux 6.6 kernel, stock `defconfig`, boots to a user process.

```
[    0.000000] SBI specification v0.3 detected
[    0.000000] riscv: base ISA extensions acdfim
[    6.540482] 10000000.serial: ttyS0 at MMIO 0x10000000 (irq = 12) is a 16550A
[    7.138788] virtio_blk virtio0: 1/0/0 default/read/poll queues
[    9.936166] Run /init as init process

  Linux is running on the riscv-emu emulator.
```

**Four problems stood between phase 8 and this, and three of them were not
emulator bugs at all** - they were missing firmware. The emulator was behaving
exactly as the spec says a bare hart should; what was absent was the layer that
normally sits underneath a kernel and arranges things on its behalf.

- **No `medeleg`/`mideleg`.** Linux enables paging by writing `satp` and letting
  the *next fetch* fault onto a virtual-address continuation it has already
  installed in `stvec`. That depends on the fault reaching supervisor mode; with
  no delegation it went to `mtvec` = 0 and looped forever. The symptom was an
  instruction trace that stopped at the `satp` write and never advanced - which
  is itself the clue, since a trace only stops when every *fetch* is faulting.
- **No `mcounteren`.** `rdtime` is an ordinary instruction to a kernel (`udelay`
  is built on it) but reading a counter from a lower privilege level is illegal
  unless the level above enables it.
- **The SBI timer expired into nothing.** `mtimecmp` raises MTIP, a *machine*
  interrupt, which a supervisor cannot enable - so jiffies never advanced and
  every sleep would have hung forever. Real firmware posts STIP in its place.
- **virtio was not a modern device.** It reported `VERSION = 2` but never
  offered `VIRTIO_F_VERSION_1`, which Linux refuses outright. The bit is number
  32, so it is only reachable if the device honours `DeviceFeaturesSel` - which
  it did not. xv6 never checks, which is why phase 7 was unaffected, and why
  booting a second stricter OS was worth the trouble.

**Added:** `scripts/boot-linux.sh` (fetch, build, initramfs, boot in one
command), `examples/initramfs/init.c`, and RISC-V `Image` header parsing so a
flat kernel is placed at `DRAM_BASE + text_offset` rather than at the start of
DRAM.

**Docs:** [`09-booting-linux.md`](09-booting-linux.md)
