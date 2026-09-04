# Phase 0 — Architecture

This phase adds no new instructions. It exists because the code as committed
could not have grown past ADDI without being restructured first, and because it
contained a memory-safety bug worth fixing before anything was built on top.

Read this as the "why" behind the file layout you now have.

---

## 1. What was wrong with the committed code

Four of these are correctness bugs; the fifth is structural, and it was the real
blocker.

### The execute() signature could not express most instructions

```cpp
void execute(uint32_t opcode, uint32_t rd, uint32_t rs1, int32_t imm)
```

There is no `rs2`, no `funct3`, no `funct7`. Consider what that rules out:

- `add x3, x1, x2` needs `rs2` — two source registers.
- `sd x5, 16(x2)` needs `rs2` — the value being stored.
- `beq x1, x2, label` needs `rs2` — the second value to compare.
- Every ALU operation needs `funct3` to know *which* operation it is.

So the signature admitted exactly one instruction shape: one source register,
one destination, one immediate. That is I-type and nothing else. No amount of
adding `if` branches inside `execute()` would have helped, because the
information simply was not passed in. This had to change before instruction
number two, which is why "just add more instructions" was not an option.

The fix is `DecodedInst` (`include/decoder.hpp`) — a struct carrying every
field, so `execute()` receives the whole instruction rather than four of its
parts.

### OP-IMM ignored funct3

```cpp
if (opcode == 0x13) {
    write_reg(rd, read_reg(rs1) + imm);
}
```

Opcode `0x13` is not ADDI. It is the whole OP-IMM *group*: ADDI, SLTI, SLTIU,
XORI, ORI, ANDI, SLLI, SRLI, SRAI. Nine instructions share that opcode and are
distinguished by `funct3`. The old code extracted `funct3`, printed it, and
then never used it — so eight of those nine instructions silently performed an
addition.

Silently is the important word. `andi x1, x2, 6` would not error; it would put
the wrong number in a register and keep going.

### The immediate was decoded before the format was known

```cpp
int32_t imm = static_cast<int32_t>(instruction) >> 20;
```

That is the I-type immediate, and it was computed for every instruction
regardless of opcode. RISC-V has six formats with six different immediate
encodings — some of them, as you will see below, quite scrambled. Applying the
I-type rule to an S-type or B-type instruction yields a meaningless number.

### The PC advanced even on unknown instructions

```cpp
else {
    std::cout << "Unknown opcode\n";
}
...
pc += 4;   // <- outside the if/else, runs unconditionally
```

An instruction the emulator did not understand printed a message and then
execution carried on to the next address. That is the single worst property the
old code had for the goal of booting an OS, and it is worth being precise about
why.

When a kernel misbehaves under an emulator, the usual cause is that *your
emulator* computed something wrong a few thousand instructions earlier — a
mis-decoded branch offset, say, sending the PC somewhere meaningless. What you
want at that moment is for the machine to stop, immediately, at the bad address.
What the old code did instead was step forward 4 bytes at a time through
whatever happened to be in memory, printing "Unknown opcode" for each, until
something eventually broke somewhere completely unrelated to the actual bug.

Real hardware does not do this. An undefined instruction raises an *illegal
instruction* exception, and the address of the offending instruction is recorded
so a handler can see it. Now the emulator does the same: `execute()` returns a
trap, and `step()` leaves `pc` on the faulting instruction.

### Tracing was unconditional

`decode()` printed seven lines to stdout for every instruction. Booting xv6
retires on the order of hundreds of millions of instructions; Linux, billions.
Tracing is genuinely useful when debugging, so it is still there — behind a
`--trace` flag, on stderr so it does not mix with the guest's own console
output.

---

## 2. Memory is not an array — it is a bus

The old model was a flat `std::array<uint8_t, 1MiB>` indexed from address 0.
Two separate problems with that.

### Problem one: no bounds checking

```cpp
uint32_t read32(uint64_t address) const {
    return memory[address] | (memory[address+1] << 8) | ...;
}
```

No check that `address` is in range. Any address ≥ 1 MiB read host memory beyond
the array. And because `Memory mem;` was a local in `main()`, that 1 MiB array
lived on the stack — so an out-of-range *write* corrupted the host process's own
stack directly.

Now every access is checked, including the subtle case where an access starts in
range but its width runs off the end (`Dram::in_range`, `src/dram.cpp`). Out of
range returns a trap to the guest instead of touching host memory.

### Problem two: there was nowhere to put devices

This is the deeper one. A CPU does not talk to RAM. It puts an address on a bus,
and *something* answers. On the machine we are building:

```
  0x0010_0000   syscon        poweroff / reboot            phase 4
  0x0200_0000   CLINT         timer + software interrupts  phase 4
  0x0C00_0000   PLIC          external interrupts          phase 7
  0x1000_0000   UART0         console                      phase 4
  0x1000_1000   virtio-mmio   block device                 phase 7
  0x8000_0000   DRAM          where the kernel loads       now
```

This is memory-mapped I/O: devices are addressed exactly like memory, and an
address decoder decides who responds. Writing a byte to `0x1000_0000` does not
store it anywhere — it prints a character.

With a flat array there is no decoder and therefore nowhere to attach a console.
No console means a guest OS cannot print, which means there is no way to tell
whether it booted. Every device phase from here on hangs off `Bus`
(`include/bus.hpp`) and the `Device` interface (`include/device.hpp`).

Note also that DRAM now starts at **0x8000_0000**, not 0. That is not a
stylistic choice: the low addresses are reserved for MMIO, and both xv6 and
Linux are linked expecting RAM there. Addresses claimed by no device return an
access fault rather than reading zero, so a kernel that jumps into the weeds
stops with a clear cause.

---

## 3. Traps are return values, not exceptions

Every memory access and every instruction can fail. The natural C++ instinct is
`throw`. That instinct is wrong here, and it is worth understanding why.

In an operating system, "failure" means the guest takes a trap — and traps are
not rare. They are the normal mechanism by which an OS works:

- Every system call is an `ECALL` exception.
- Demand paging works *by* taking page faults and fixing them up.
- Every timer tick is an interrupt.

These are hot-path control flow, not exceptional conditions. So instead:

```cpp
Result<u64> load(...);   // either a value, or a Trap
Status      store(...);  // either OK, or a Trap
```

This is the shape of Rust's `Result<T, E>`, which is what most RISC-V emulators
use. It keeps the trap path visible in the type signature — you cannot forget to
handle it — and avoids stack unwinding on a path that executes constantly.
(`std::expected` would be the natural fit but it is C++23; `include/result.hpp`
is a small stand-in.)

The `AccessType` enum (`include/device.hpp`) is a related detail worth noticing:
the *same* failure produces a different trap cause depending on why memory was
touched. Reading an unmapped address during instruction fetch is an
`InstructionAccessFault`; reading it for a load is a `LoadAccessFault`. That
enum also becomes the read/write/execute permission check when the MMU lands in
phase 6.

---

## 4. Instruction formats, and why B and J look insane

There are six formats. The important consequence is that the immediate is
assembled differently in each:

```
 R   funct7 | rs2 | rs1 | funct3 | rd      | opcode     no immediate
 I   imm[11:0]    | rs1 | funct3 | rd      | opcode     12-bit signed
 S   imm[11:5]|rs2| rs1 | funct3 |imm[4:0] | opcode     12-bit signed, split
 B   imm[12|10:5] | ... | imm[4:1|11]      | opcode     13-bit signed, scrambled
 U   imm[31:12]                  | rd      | opcode     upper 20 bits
 J   imm[20|10:1|11|19:12]       | rd      | opcode     21-bit signed, scrambled
```

The B and J encodings look deranged. Compare the S and B immediates: they are
almost identical, except bit 11 and bit 12 swap places. Why not lay the bits out
in order?

Because the encoding is chosen so that each immediate bit sits in the *same
physical instruction bit position* across as many formats as possible. A
hardware decoder can then wire those bits straight through to the immediate with
almost no multiplexing, which is real gates saved on real silicon. RISC-V pushes
that cost onto the assembler and onto emulator authors — a few lines of shifting
in `src/decoder.cpp` — rather than onto every chip that implements it.

Two further details that are easy to get wrong:

- **B and J immediates are always even.** Bit 0 is not stored, because branch
  and jump targets are at least 2-byte aligned. That is how a 12-bit stored
  field covers a 13-bit range.
- **U-type immediates are sign-extended from bit 31.** `lui x1, 0x80000`
  produces a *negative* 64-bit value on RV64, not `0x0000000080000000`.

`sign_extend()` in `include/types.hpp` does the shift-left-then-arithmetic-
shift-right trick that all of these rely on.

---

## 5. RV64 shifts use funct6, not funct7

A detail that bites people. On RV32, `SLLI`/`SRLI`/`SRAI` take a 5-bit shift
amount and `funct7` distinguishes logical from arithmetic. On RV64 the shift
amount needs 6 bits (you can shift by up to 63), and it steals that bit from
`funct7` — leaving `funct6` in bits [31:26] as the selector.

So dispatching a shift on `funct7` decodes `SRAI` as `SRLI`, and truncating the
shift amount to 5 bits makes `slli x1, x1, 40` shift by 8. Hence
`DecodedInst::funct6()` and `shamt6()` in `include/decoder.hpp`, and the tests
in `tests/test_decoder.cpp` that specifically shift by more than 31.

---

## 6. The resulting layout

```
include/                    src/
  types.hpp     widths, memory map, sign_extend
  trap.hpp      exception/interrupt cause codes   trap.cpp
  result.hpp    Result<T> / Status
  device.hpp    the Device interface, AccessType
  bus.hpp       address decoding                  bus.cpp
  dram.hpp      guest RAM at 0x8000_0000          dram.cpp
  decoder.hpp   u32 -> DecodedInst                decoder.cpp
  cpu.hpp       registers, fetch/decode/execute   cpu.cpp
                CLI                               main.cpp
tests/
  test_decoder.cpp   immediate encodings, field extraction, funct6
  test_bus.cpp       bounds checking, endianness, address decoding
  test_cpu.cpp       OP-IMM semantics, trap behaviour, PC-on-trap
```

The core builds as a static library (`riscv_core`) so the executable and the
tests link the same code. CMake now defaults to a Release build — the inner loop
runs billions of times when booting an OS, and an unoptimised build turns a
seconds-long boot into a minutes-long one.

## 7. Try it

```bash
cmake -S . -B build && cmake --build build
cd build && ctest --output-on-failure
```

`test_decoder`, `test_bus` and `test_cpu` are the suites this phase added, and
they still run unchanged: the immediate encodings for all six formats, the bus's
bounds checking and address decoding, and OP-IMM dispatching on `funct3` rather
than executing everything as ADDI.

> **A note on reading this document.** Each phase doc describes the state of the
> project at the end of that phase, and is left as written. Later phases change
> some of what is described here — the built-in demo in `src/main.cpp` is now a
> UART program rather than the OP-IMM sequence phase 0 shipped, and `ebreak` no
> longer traps as an illegal instruction because phase 2 implemented the SYSTEM
> instructions. `README.md` always reflects the current state.

---

**Next:** phase 1 fills in the rest of RV64I — loads, stores, branches, jumps,
register-register ALU ops, and the RV64 `*W` variants.
