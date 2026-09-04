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
| 1 | Complete RV64I | ⬜ next |
| 2 | Zicsr + M-mode traps | ⬜ |
| 3 | M and A extensions | ⬜ |
| 4 | ELF loader, UART, CLINT — first output | ⬜ |
| 5 | riscv-tests + CI | ⬜ |
| 6 | S-mode + Sv39 MMU | ⬜ |
| 7 | PLIC + virtio-blk — **boot xv6** | ⬜ |
| 8 | Linux prerequisites (C, F/D, DTB, SBI) | ⬜ |
| 9 | **Boot Linux** | ⬜ |

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
