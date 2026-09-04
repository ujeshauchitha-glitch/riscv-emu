#pragma once

#include <array>
#include <iosfwd>

#include "bus.hpp"
#include "decoder.hpp"
#include "result.hpp"
#include "types.hpp"

// ABI register names (zero, ra, sp, ...) indexed by register number.
extern const char* const REG_ABI_NAMES[NUM_REGS];

// ---------------------------------------------------------------------------
// The CPU core.
//
// The execution loop is the classic fetch / decode / execute cycle, with one
// important structural detail: **the PC is only advanced on success.**
//
// The previous version did `pc += 4` unconditionally at the end of execute(),
// including on the path that printed "Unknown opcode". That meant a bad decode
// silently skipped the instruction and kept going. When you are bringing up a
// kernel, a wrong jump would send the PC into garbage and the emulator would
// happily grind through megabytes of zeros rather than stopping at the point of
// the mistake. Debugging that is close to impossible.
//
// Here, execute() writes to `next_pc`, and step() commits it only if no trap
// occurred. On a trap the PC is left pointing at the faulting instruction,
// which is exactly what a real hart does (it stashes that address in mepc) and
// exactly what you want to see in a register dump.
// ---------------------------------------------------------------------------
class Cpu {
public:
    explicit Cpu(Bus& bus);

    // Architectural state.
    std::array<u64, NUM_REGS> regs{};
    u64                       pc = DRAM_BASE;

    // x0 is hardwired to zero: reads always yield 0 and writes are discarded.
    // Compilers rely on this constantly (`add x0, x0, x0` is the canonical NOP,
    // and any instruction whose result is unwanted just targets x0).
    u64  read_reg(u32 index) const;
    void write_reg(u32 index, u64 value);

    // Run one instruction. Returns OK, or the trap the instruction raised.
    Status step();

    // Run until a trap occurs or `max_steps` instructions have retired.
    // Returns the trap that stopped execution, or OK if the step budget ran
    // out. `steps_out`, when non-null, receives the number of instructions
    // actually retired.
    Status run(u64 max_steps, u64* steps_out = nullptr);

    // Fetch the 32-bit instruction word at the current PC.
    Result<u32> fetch() const;

    // Execute an already-decoded instruction. `next_pc_` is pre-set to pc + 4
    // by step(); control-transfer instructions overwrite it.
    Status execute(const DecodedInst& inst);

    void dump_registers(std::ostream& os) const;

    // Tracing is opt-in and off by default. The previous version printed seven
    // lines to stdout for every instruction executed. Booting even a small
    // kernel retires hundreds of millions of instructions, so unconditional
    // tracing is not merely noisy — it makes the emulator unusable.
    bool          trace          = false;
    std::ostream* trace_stream   = nullptr;  // defaults to std::cerr
    u64           instret        = 0;        // instructions retired

private:
    Bus& bus_;
    u64  next_pc_ = 0;

    Status execute_op_imm(const DecodedInst& inst);
    void   trace_inst(const DecodedInst& inst) const;
};
