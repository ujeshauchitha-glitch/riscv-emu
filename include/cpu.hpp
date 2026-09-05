#pragma once

#include <array>
#include <iosfwd>

#include "bus.hpp"
#include "clint.hpp"
#include "csr.hpp"
#include "decoder.hpp"
#include "mmu.hpp"
#include "plic.hpp"
#include "uart.hpp"
#include "syscon.hpp"
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
// Here, execute() writes to `next_pc_`, and step() commits it only if no trap
// occurred. On a trap the PC is left pointing at the faulting instruction,
// which is exactly what a real hart does (it stashes that address in mepc) and
// exactly what you want to see in a register dump.
// ---------------------------------------------------------------------------
class Cpu {
public:
    explicit Cpu(Bus& bus);

    // Architectural state.
    std::array<u64, NUM_REGS> regs{};

    // The floating-point register file, separate from the integer one. Each is
    // 64 bits; a single-precision value lives NaN-boxed in the low half. Unlike
    // x0, f0 is an ordinary register with no special behaviour.
    std::array<u64, NUM_REGS> fregs{};
    u64                       pc = DRAM_BASE;
    CsrFile                   csrs;

    // Current privilege level. A hart comes out of reset in machine mode.
    // Nothing lowers it until phase 6 adds MRET-to-supervisor and user mode.
    u32 priv = PRIV_MACHINE;

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

    // Fetch and decode the instruction at the current PC, which may be 16 or
    // 32 bits wide. Not const: an instruction fetch can fill the TLB.
    Result<DecodedInst> fetch_inst();

    // The raw 32-bit instruction word at the current PC - the *expansion*, if
    // the instruction there is compressed. Kept for tests, which want to check
    // what a given address decodes to without running it.
    Result<u32> fetch();

    // Virtual memory. Translation is a no-op in machine mode and while satp
    // says Bare, so this costs nothing until a kernel turns paging on.
    Mmu mmu;

    // Translated memory access, used by loads, stores and the fetch path.
    Result<u64> mem_load(u64 vaddr, unsigned size, AccessType type);
    Status      mem_store(u64 vaddr, unsigned size, u64 value);

    // Does this access span two pages, and so need two translations? RISC-V
    // permits misaligned accesses, so ordinary guest code reaches this.
    static bool crosses_page(u64 vaddr, unsigned size);
    Result<u64> load_across_pages(u64 vaddr, unsigned size, AccessType type);
    Status      store_across_pages(u64 vaddr, unsigned size, u64 value);

    // Execute an already-decoded instruction. `next_pc_` is pre-set to the next
    // instruction (pc + 2 or pc + 4)
    // by step(); control-transfer instructions overwrite it.
    Status execute(const DecodedInst& inst);

    // Enter a trap handler: record the cause in the CSRs and vector to mtvec.
    // This is what turns a trap from "the emulator stops" into "the guest's
    // handler runs", which is the whole point of this phase.
    void take_trap(const Trap& trap);
    void take_interrupt(Interrupt intr);

    // The highest-priority interrupt that is pending, enabled, and permitted by
    // mstatus.MIE - or false if none is ready to fire.
    bool next_interrupt(Interrupt& out) const;

    // Traps are fatal while mtvec is still zero.
    //
    // A hart out of reset has mtvec = 0, so a trap would vector to address 0,
    // fault on the unmapped fetch, and vector to 0 again - an infinite loop
    // that looks exactly like a hang. Real hardware does precisely this, but it
    // makes for a terrible debugging experience, so until a guest installs a
    // handler we stop and report instead. Once mtvec is set, traps dispatch
    // normally. Phase 4's syscon device gives guests a real way to exit.
    bool trap_fatal_without_handler = true;

    // The trap that stopped execution, when step() or run() returns a failure.
    Trap last_trap{};

    // Optional devices the CPU has to consult every step rather than only when
    // the guest addresses them. The bus routes loads and stores; these two need
    // the CPU to advance the clock and to notice a poweroff request.
    Clint*        clint  = nullptr;
    const Syscon* syscon = nullptr;

    // External interrupts. The UART's line is level-triggered - it stays
    // asserted while a character is waiting - so the CPU samples it each step
    // and tells the PLIC, rather than the UART pushing an edge.
    // Not const: the console is polled for host input from the step loop, so
    // that a guest shell can be typed at.
    Plic* plic     = nullptr;
    Uart* uart     = nullptr;
    u32   uart_irq = 0;

    // Set when a syscon poweroff or reboot stops the run.
    bool halted = false;

    // SBI: firmware services for a supervisor.
    //
    // When this is on, an ECALL from supervisor mode is handled by the emulator
    // as an SBI call rather than being delivered as a trap. That is the role a
    // machine-mode firmware like OpenSBI plays on real hardware, and Linux
    // cannot boot without it - setting the timer needs a machine-mode register
    // a kernel has no access to.
    //
    // It is off by default, because a kernel that provides its own machine-mode
    // code (xv6 does) must be allowed to see its own ecalls.
    bool sbi_enabled  = false;
    bool sbi_shutdown = false;   // set when the guest asked SBI to power off

    // Set when the run stopped because the user pressed Ctrl-A X, rather than
    // because the guest asked to power off. Both set `halted`; only this
    // distinguishes "you quit" from "the machine shut down".
    bool user_quit = false;

    // HTIF: the address of a `tohost` word to watch, or 0 to watch nothing.
    //
    // The riscv-tests suite reports its result by writing here and spinning,
    // rather than by powering the machine off - so the emulator has to poll it.
    // Polling costs a bus lookup per instruction, which is why it is off unless
    // an image actually declares the symbol.
    u64 htif_tohost_addr = 0;

    // The value the guest wrote to tohost, once it wrote a non-zero one.
    u64 htif_tohost_value = 0;

    void dump_registers(std::ostream& os) const;

    // Tracing is opt-in and off by default. The previous version printed seven
    // lines to stdout for every instruction executed. Booting even a small
    // kernel retires hundreds of millions of instructions, so unconditional
    // tracing is not merely noisy - it makes the emulator unusable.
    bool          trace        = false;
    std::ostream* trace_stream = nullptr;  // defaults to std::cerr
    u64           instret      = 0;        // instructions retired

    // Clear any outstanding LR/SC reservation. Called on trap entry, because a
    // context switch between an LR and its SC must make the SC fail - otherwise
    // two threads could both believe they won the same lock.
    void clear_reservation() { reservation_valid_ = false; }

private:
    Bus& bus_;
    u64  next_pc_ = 0;

    // The LR/SC reservation set. Real hardware reserves a cache-line-sized
    // "granule"; a single-hart emulator only needs the address, since nothing
    // else can write memory behind our back.
    bool reservation_valid_ = false;
    u64  reservation_addr_  = 0;

    // One handler per major opcode group.
    Status execute_op_imm(const DecodedInst& inst);     // ADDI, SLTI, ... SRAI
    Status execute_op_imm_32(const DecodedInst& inst);  // ADDIW, SLLIW, ...
    Status execute_op(const DecodedInst& inst);         // ADD, SUB, SLL, ...
    Status execute_op_32(const DecodedInst& inst);      // ADDW, SUBW, SLLW, ...
    Status execute_load(const DecodedInst& inst);       // LB .. LWU
    Status execute_store(const DecodedInst& inst);      // SB .. SD
    Status execute_branch(const DecodedInst& inst);     // BEQ .. BGEU
    Status execute_jal(const DecodedInst& inst);
    Status execute_jalr(const DecodedInst& inst);
    Status execute_system(const DecodedInst& inst);     // ECALL, EBREAK, CSRs
    Status execute_mul_div(const DecodedInst& inst);    // M: MUL, DIV, REM
    Status execute_mul_div_32(const DecodedInst& inst); // M: MULW, DIVW, REMW
    Status execute_amo(const DecodedInst& inst);        // A: LR/SC and the AMOs
    Status execute_csr(const DecodedInst& inst);        // CSRRW/S/C and imm forms
    Status execute_mret(const DecodedInst& inst);
    Status execute_sret(const DecodedInst& inst);
    Status execute_sfence_vma(const DecodedInst& inst);

    // F and D. Loads and stores are separate from the integer ones because
    // they write the other register file and must NaN-box a single.
    Status execute_load_fp(const DecodedInst& inst);    // FLW, FLD
    Status execute_store_fp(const DecodedInst& inst);   // FSW, FSD
    Status execute_op_fp(const DecodedInst& inst);      // everything else
    Status execute_fused_madd(const DecodedInst& inst); // FMADD .. FNMADD

    // Every floating-point instruction begins by checking that the FPU is on -
    // mstatus.FS != Off - and ends by marking the register file dirty if it
    // wrote one. Forgetting either is a silent bug: the first lets user code
    // use registers the kernel is not saving, the second lets a context switch
    // skip saving registers that were modified.
    Status require_fpu() const;
    void   write_freg(u32 index, u64 value);

    // Run one host floating-point operation with RISC-V's rounding mode and
    // exception reporting: set the mode from the instruction's rm field (or
    // fcsr when it says DYN), clear the host flags, call `op`, then accumulate
    // whatever it raised into fflags.
    template <typename Op>
    Status with_rounding(const DecodedInst& inst, Op&& op);

    // The privilege a load or store runs at.
    //
    // Normally the current mode, but mstatus.MPRV makes machine-mode data
    // accesses use MPP's privilege instead - which is how M-mode firmware
    // reaches a supervisor's address space to copy arguments in and out. It
    // deliberately does not affect instruction fetch.
    u32 data_privilege() const;

    // True if mstatus.TVM makes this CSR access illegal.
    bool tvm_blocks(u32 addr) const;

    // Read/write a CSR with the architectural access checks applied:
    // unimplemented address, write to a read-only address, insufficient
    // privilege. Each of those is an illegal-instruction trap.
    Result<u64> csr_read(u32 addr) const;
    Status      csr_write(u32 addr, u64 value);

    // Set when the instruction being executed wrote minstret or mcycle. The
    // spec says such a write suppresses that instruction's own increment, so
    // reading the counter back immediately yields exactly what was written.
    bool counter_written_ = false;

    // Shared trap-entry sequence for both exceptions and interrupts. They
    // differ only in the cause code, the resume address, and whether vectored
    // mtvec applies.
    void enter_trap(u64 cause_code, u64 tval, u64 epc, bool is_interrupt);

    // Dispatch a trap to the guest handler, or stop when none is installed.
    Status handle_trap_or_stop(const Trap& trap);

    // True when the mode that will actually receive this trap has a vector
    // installed. Delegation decides which vector that is - see the definition.
    bool handler_installed_for(const Trap& trap) const;

    // Redirect control flow to `target`, or raise InstructionAddressMisaligned.
    //
    // The spec is specific about where this trap is reported: on the *jump or
    // branch* instruction, not on the target. So the check happens here, before
    // next_pc_ is committed, rather than at fetch time - that way the PC left
    // behind (and later mepc) points at the instruction that did the jumping,
    // which is the one you need to look at.
    Status set_branch_target(u64 target);

    void trace_inst(const DecodedInst& inst) const;
};
