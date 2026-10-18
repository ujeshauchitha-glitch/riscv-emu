#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "bus.hpp"
#include "clint.hpp"
#include "cpu.hpp"
#include "plic.hpp"
#include "syscon.hpp"
#include "uart.hpp"
#include "types.hpp"

// ---------------------------------------------------------------------------
// The machine: one or more harts sharing a bus and a set of devices.
//
// A `Cpu` knows how to execute instructions. It does not know what else is in
// the machine, and for one hart that was enough: the CPU polled the timer and
// the interrupt controller itself, once per instruction. With several harts that
// design breaks down in two ways.
//
//   - The devices are shared. The CLINT has one clock and the PLIC one set of
//     pending interrupts, so polling them once per instruction per hart would
//     advance time N times too fast and do N times the work.
//   - Somebody has to decide who runs. Harts execute on one host thread, taking
//     turns.
//
// The Platform is that somebody.
//
// **Scheduling.** Round-robin in fixed quanta: each runnable hart executes
// `quantum` instructions, then the next one does. After every round the clock
// advances by one quantum - the time the harts, running in parallel, would have
// taken - and the devices are polled once, for all harts together. This is
// deterministic: the same inputs interleave the same way every run, which is
// what makes a lost-wakeup or a missing barrier reproducible rather than a
// once-a-week ghost. It is also faster than polling per instruction, since the
// devices are touched once per quantum instead of once per step.
//
// The price is that a device event reaches the guest up to one quantum late.
// With the default quantum that is a few hundred instructions out of the ten
// million per second a timer tick spans, which no guest can observe.
//
// **Idling.** A hart in WFI with nothing pending is skipped. When *every* hart
// is idle, nothing can happen until the next timer deadline, so the clock jumps
// straight to it instead of retiring millions of empty instructions. That is
// the difference between a Linux boot that finishes its sleeps instantly and
// one that waits them out at emulation speed.
// ---------------------------------------------------------------------------
class Platform {
public:
    explicit Platform(Bus& bus, u32 num_harts = 1);

    u32  num_harts() const { return static_cast<u32>(harts_.size()); }
    Bus& bus() { return bus_; }
    Cpu& hart(u32 i) { return *harts_[i]; }
    Cpu& boot_hart() { return *harts_[0]; }

    // Connect the shared devices. Every hart is given the pointers (so the
    // single-hart `Cpu::step()` path and the `time` CSR keep working) and the
    // platform keeps its own for the polling it does between rounds.
    void wire(Clint* clint, Plic* plic, Uart* uart, u32 uart_irq, Syscon* syscon);

    // Put the machine in the state a bootloader plus SBI firmware leaves it in
    // when it hands control to a supervisor-mode kernel: the boot hart in
    // supervisor mode with a0 = hartid and a1 = the device tree, all traps
    // delegated, counters readable, and SBI services answering ecalls. The other
    // harts are parked - they sit in firmware, and the kernel releases them one
    // at a time with SBI's hart_start.
    void enter_supervisor(u64 entry, u64 dtb_addr);

    // Instructions each hart runs per scheduling round.
    u64 quantum = 256;

    // Run until the machine halts, a hart hits a fatal trap, or `max_steps`
    // instructions have retired across all harts. Returns the trap that stopped
    // a hart (see fault_hart()), or OK.
    Status run(u64 max_steps, u64* steps_out = nullptr);

    // Run exactly one instruction on one hart, polling devices first. For a
    // debugger: it is the slow, precise path.
    Status step_hart(u32 hart);

    // --- debugger support -------------------------------------------------------
    //
    // When set, this is called before every instruction with the hart about to
    // run it; returning true stops the run before that instruction executes.
    // Setting it also drops the scheduler to one instruction per turn so that
    // nothing slips past: it is the slow path, used only while a debugger is
    // attached. run() then returns normally and hook_stopped() says why.
    std::function<bool(Cpu&)> before_step;
    bool hook_stopped() const { return hook_stopped_; }
    u32  hook_hart() const { return hook_hart_; }

    // Bring every hart's interrupt-pending bits up to date with the devices.
    void poll_devices();

    // --- why did run() return? ------------------------------------------------
    bool halted() const { return halted_; }
    bool user_quit() const { return user_quit_; }
    bool sbi_shutdown() const { return sbi_shutdown_; }
    // True if every hart went idle with no deadline and no input source that
    // could ever wake one: the guest has deadlocked itself.
    bool stalled() const { return stalled_; }
    u32  fault_hart() const { return fault_hart_; }

    // HTIF: the riscv-tests suite reports its result by writing a word to
    // `tohost` and then spinning. Set the address to watch, or leave it 0.
    u64 htif_tohost_addr  = 0;
    u64 htif_tohost_value = 0;

    // Total instructions retired by all harts.
    u64 retired() const { return retired_; }

private:
    Bus&                              bus_;
    std::vector<std::unique_ptr<Cpu>> owned_;
    std::vector<Cpu*>                 harts_;

    Clint*  clint_  = nullptr;
    Plic*   plic_   = nullptr;
    Uart*   uart_   = nullptr;
    Syscon* syscon_ = nullptr;
    u32     uart_irq_ = 0;

    bool hook_stopped_ = false;
    u32  hook_hart_    = 0;
    bool halted_       = false;
    bool user_quit_    = false;
    bool sbi_shutdown_ = false;
    bool stalled_      = false;
    u32  fault_hart_   = 0;
    u64  retired_      = 0;

    // Host keystrokes are collected only every so many instructions: a read()
    // syscall per scheduling round would cost more than the instructions it
    // sits between.
    u64 since_host_poll_ = 0;

    // Check for the ways a run can end that do not come from the instruction
    // stream: a syscon poweroff, the user's Ctrl-A X, an HTIF result, a hart
    // that called SBI shutdown.
    bool check_stop();

    // Every hart is idle: skip the clock to the next deadline. Returns false if
    // there is no deadline to skip to.
    bool fast_forward_idle();

    // Is some device waiting on the outside world? If so an idle machine lets
    // real time pass instead of skipping ahead.
    bool io_pending() const;
};
