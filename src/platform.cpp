#include "platform.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include "csr.hpp"

Platform::Platform(Bus& bus, u32 num_harts) : bus_(bus) {
    if (num_harts == 0) num_harts = 1;
    for (u32 i = 0; i < num_harts; ++i) {
        auto cpu = std::make_unique<Cpu>(bus);
        cpu->hartid = i;
        cpu->csrs.set_hartid(i);
        harts_.push_back(cpu.get());
        owned_.push_back(std::move(cpu));
    }

    // A single hart needs no peer list, and leaving it null keeps the
    // single-hart store path free of the reservation check.
    if (harts_.size() > 1) {
        for (Cpu* h : harts_) {
            h->harts       = &harts_;
            h->idle_on_wfi = true;
        }
    } else {
        // Idling pays even for one hart: a guest waiting in WFI lets the clock
        // jump to its next timer deadline.
        harts_[0]->idle_on_wfi = true;
    }
}

void Platform::wire(Clint* clint, Plic* plic, Uart* uart, u32 uart_irq, Syscon* syscon) {
    clint_ = clint;  plic_ = plic;  uart_ = uart;  syscon_ = syscon;
    uart_irq_ = uart_irq;
    for (Cpu* h : harts_) {
        h->clint    = clint;
        h->plic     = plic;
        h->uart     = uart;
        h->uart_irq = uart_irq;
        h->syscon   = syscon;
    }
}

void Platform::enter_supervisor(u64 entry, u64 dtb_addr) {
    for (Cpu* h : harts_) {
        h->sbi_enabled = true;

        // Delegate every trap to supervisor mode.
        //
        // This is firmware's job, and there is no firmware here to do it. A trap
        // taken in S-mode goes to M-mode unless the matching medeleg bit is set
        // - so without this, the kernel's very first page fault (the fetch
        // immediately after it enables paging, which is *designed* to fault so
        // that execution lands on the virtual-address continuation at stvec) is
        // delivered to mtvec instead. mtvec is zero, so the machine vectors to
        // address 0, faults on that fetch too, and loops forever with nothing
        // on the console.
        //
        // OpenSBI writes exactly this before its own mret into the kernel.
        h->csrs.write(csr::MEDELEG, 0xffff);
        h->csrs.write(csr::MIDELEG, 0xffff);

        // Let the supervisor see its own timer and external interrupts. mie is
        // machine-only, so a kernel cannot enable these for itself - firmware
        // does it on the way in.
        h->csrs.write(csr::MIE, csr::MIP_SEIP | csr::MIP_STIP | csr::MIP_SSIP);

        // Let supervisor and user code read the counters.
        //
        // `rdtime` is an ordinary instruction to a kernel - Linux's udelay() is
        // built on it - but reading the time CSR from a lower privilege level
        // is illegal unless the level above has enabled it. Nothing here has,
        // so udelay traps on its first instruction and the kernel spins in its
        // own illegal-instruction handler. Firmware enables them on the way in;
        // this does the same.
        h->csrs.write(csr::MCOUNTEREN, 0xffffffff);
        h->csrs.write(csr::SCOUNTEREN, 0xffffffff);

        // Linux installs stvec early but not instantly, and it delegates
        // nothing to itself - so the "no handler installed" guard, which is a
        // debugging aid rather than architectural behaviour, would fire on the
        // first page fault of the boot. A kernel with real firmware underneath
        // it would simply take the trap.
        h->trap_fatal_without_handler = false;
    }

    // Only the boot hart is released. The others wait in firmware until the
    // kernel starts them with SBI hart_start.
    Cpu& boot = boot_hart();
    boot.priv = PRIV_SUPERVISOR;
    boot.pc   = entry;
    boot.write_reg(10, 0);          // a0 = hartid
    boot.write_reg(11, dtb_addr);   // a1 = device tree
    for (u32 i = 1; i < num_harts(); ++i) harts_[i]->parked = true;
}

void Platform::poll_devices() {
    if (uart_ != nullptr && since_host_poll_ >= 4096) {
        since_host_poll_ = 0;
        uart_->poll_host_input();
    }

    // Devices with their own clocks or external inputs (a network card, say).
    for (const auto& dev : bus_.devices()) dev->poll();

    if (plic_ != nullptr && uart_ != nullptr) {
        plic_->set_pending(uart_irq_, uart_->interrupting());
    }

    for (Cpu* h : harts_) {
        if (clint_ != nullptr) clint_->update(h->csrs, h->hartid);
        if (plic_  != nullptr) plic_->update(h->csrs, h->hartid);
    }
}

bool Platform::check_stop() {
    for (Cpu* h : harts_) {
        if (h->halted) {
            halted_ = true;
            if (h->sbi_shutdown) sbi_shutdown_ = true;
        }
    }

    // A guest that writes the poweroff word to syscon has asked to stop.
    if (syscon_ && (syscon_->poweroff_requested() || syscon_->reboot_requested())) {
        halted_ = true;
    }

    // So has a user who pressed Ctrl-A X at the console. The guest is not
    // consulted: this is the human at the keyboard asking to leave, which is
    // the one request a running guest must not be able to refuse.
    if (uart_ && uart_->exit_requested()) {
        halted_ = true;
        user_quit_ = true;
    }

    // The riscv-tests suite stops by writing its result to `tohost` and then
    // spinning, so there is nothing to notice except the write itself.
    if (htif_tohost_addr != 0) {
        Result<u64> r = bus_.load(htif_tohost_addr, 8, AccessType::Load);
        if (r && r.value != 0) {
            htif_tohost_value = r.value;
            halted_ = true;
        }
    }

    // Mirror the verdict onto the boot hart, which is where single-hart code
    // (and the reporting in main) has always looked for it.
    if (halted_) {
        Cpu& b = boot_hart();
        b.halted = true;
        b.user_quit = user_quit_;
        b.sbi_shutdown = sbi_shutdown_;
        b.htif_tohost_value = htif_tohost_value;
    }
    return halted_;
}

bool Platform::io_pending() const {
    for (const auto& dev : bus_.devices()) {
        if (dev->io_pending()) return true;
    }
    return false;
}

bool Platform::fast_forward_idle() {
    if (clint_ == nullptr) return false;

    const u64 now = clint_->mtime();
    // Deadlines further away than this are "disarmed" for practical purposes: a
    // kernel with nothing scheduled writes all-ones, and jumping the clock there
    // would wrap it.
    constexpr u64 HORIZON = 1ull << 36;
    u64 best = ~0ull;

    for (Cpu* h : harts_) {
        if (h->parked) continue;
        auto consider = [&](u64 deadline) {
            if (deadline > now && deadline - now <= HORIZON) best = std::min(best, deadline);
        };
        consider(clint_->mtimecmp(h->hartid));
        if (h->csrs.sstc_enabled()) consider(h->csrs.read(csr::STIMECMP));
    }
    if (best == ~0ull) return false;
    clint_->advance_to(best);
    return true;
}

Status Platform::run(u64 max_steps, u64* steps_out) {
    u64 consecutive_idle = 0;

    // Idle rounds retire nothing, so they would never use up `max_steps` by
    // themselves. This bounds them anyway: a clock that failed to move, or
    // devices that keep waking a hart which then immediately sleeps again, must
    // end the run rather than spin the host forever.
    u64 idle_rounds = 0;
    hook_stopped_ = false;

    while (retired_ < max_steps) {
        u64  round_max = 0;
        bool any_ran   = false;

        for (Cpu* h : harts_) {
            if (!h->runnable()) continue;
            any_ran = true;
            h->round_start_instret = h->instret;
            if (clint_ != nullptr) clint_->set_live_source(&h->instret, &h->round_start_instret);

            // Run this hart's quantum. If the guest touches a device partway
            // through, bring the devices up to date and carry on with the rest
            // of the quantum, so that a write to a timer or interrupt register
            // takes effect at the next instruction rather than a round later.
            u64 budget = std::min(quantum, max_steps - retired_);
            u64 used   = 0;
            while (budget > 0) {
                // A debugger gets to look at the hart before every instruction.
                if (before_step && before_step(*h)) {
                    hook_stopped_ = true;
                    hook_hart_    = h->hartid;
                    if (steps_out) *steps_out = retired_;
                    return Status::good();
                }
                u64 done = 0;
                Status st = h->run_core(before_step ? 1 : budget, &done);
                retired_ += done;
                used     += done;
                budget   -= std::min(budget, done);
                since_host_poll_ += done;

                if (!st) {
                    fault_hart_ = h->hartid;
                    if (steps_out) *steps_out = retired_;
                    return st;
                }
                if (h->halted || h->parked || !h->device_touched) break;
                poll_devices();
            }
            round_max = std::max(round_max, used);
            if (retired_ >= max_steps) break;
        }

        if (any_ran) {
            consecutive_idle = 0;
            // The harts ran side by side, so the clock moves by the longest of
            // them, not the sum.
            if (clint_ != nullptr) clint_->tick(round_max);
        } else {
            // Nothing can execute. Skip to the next deadline if there is one;
            // otherwise the only thing that can still wake the machine is the
            // outside world typing at the console.
            if (++idle_rounds > max_steps) break;
            // An idle round executes nothing, so it would never accumulate toward
            // the next host-keyboard poll on its own.
            since_host_poll_ += 64;

            if (io_pending()) {
                // Something outside the machine is still expected to answer. Let
                // real time pass - a couple of hundred microseconds a round, the
                // guest clock keeping pace at its nominal 10 MHz - rather than
                // skipping to the next deadline and timing the request out
                // before the reply could possibly arrive.
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                if (clint_ != nullptr) clint_->tick(2000);
                consecutive_idle = 0;
            } else if (!fast_forward_idle()) {
                if (uart_ != nullptr && uart_->host_input_active()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    since_host_poll_ = 4096;
                } else if (++consecutive_idle > 4) {
                    stalled_ = true;
                    if (steps_out) *steps_out = retired_;
                    return Status::good();
                }
            }
        }

        poll_devices();
        if (check_stop()) {
            if (steps_out) *steps_out = retired_;
            return Status::good();
        }
    }

    if (steps_out) *steps_out = retired_;
    return Status::good();
}

Status Platform::step_hart(u32 hart) {
    if (hart >= num_harts()) return Status::good();
    Cpu* h = harts_[hart];
    if (!h->runnable()) return Status::good();

    h->round_start_instret = h->instret;
    if (clint_ != nullptr) clint_->set_live_source(&h->instret, &h->round_start_instret);
    u64 done = 0;
    Status st = h->run_core(1, &done);
    retired_ += done;
    if (!st) {
        fault_hart_ = hart;
        return st;
    }
    if (clint_ != nullptr) clint_->tick(done);
    poll_devices();
    check_stop();
    return Status::good();
}
