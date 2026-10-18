#include "sbi.hpp"

#include <iostream>
#include <vector>

#include "clint.hpp"
#include "cpu.hpp"
#include "syscon.hpp"
#include "uart.hpp"

namespace sbi {

namespace {

// The SBI implementation version this emulator reports. Nothing checks it, but
// a kernel that asks and gets nothing back may conclude SBI is absent.
constexpr u64 IMPL_ID      = 5;        // "unknown implementation"
constexpr u64 IMPL_VERSION = 1;
constexpr u64 SPEC_VERSION = (0 << 24) | 3;   // v0.3

void set_return(Cpu& cpu, i64 error, u64 value = 0) {
    cpu.write_reg(10, static_cast<u64>(error));   // a0
    cpu.write_reg(11, value);                     // a1
}

// Is this extension available? A kernel calls probe_extension before using one,
// and an honest "no" makes it take a documented fallback path instead of
// executing something that does not work.
bool have_extension(u64 eid) {
    switch (eid) {
        case EXT_SET_TIMER:
        case EXT_CONSOLE_PUTCHAR:
        case EXT_CONSOLE_GETCHAR:
        case EXT_SHUTDOWN:
        case EXT_BASE:
        case EXT_TIME:
        case EXT_IPI:
        case EXT_RFENCE:
        case EXT_HSM:
        case EXT_SRST:
            return true;
        default:
            return false;
    }
}

// --- addressing other harts ---------------------------------------------------

u64 hart_count(const Cpu& cpu) {
    return cpu.harts ? cpu.harts->size() : 1;
}

// The hart with this id, or null if there is no such hart. On a single-hart
// machine hart 0 is the caller.
Cpu* hart_at(Cpu& cpu, u64 id) {
    if (cpu.harts == nullptr) return id == 0 ? &cpu : nullptr;
    return id < cpu.harts->size() ? (*cpu.harts)[static_cast<std::size_t>(id)] : nullptr;
}

// The IPI and remote-fence calls name their targets as a bit mask relative to a
// base hart id: bit i of `mask` selects hart `base + i`. A base of all-ones is
// the special case "every hart", with the mask ignored. Calls `fn` on each
// selected hart and returns false if the mask names a hart that does not exist.
template <typename Fn>
bool for_each_target(Cpu& cpu, u64 mask, u64 base, Fn&& fn) {
    if (base == ~0ull) {
        for (u64 id = 0; id < hart_count(cpu); ++id) fn(*hart_at(cpu, id));
        return true;
    }
    bool ok = true;
    for (unsigned bit = 0; bit < 64; ++bit) {
        if (((mask >> bit) & 1) == 0) continue;
        Cpu* target = hart_at(cpu, base + bit);
        if (target == nullptr) { ok = false; continue; }
        fn(*target);
    }
    return ok;
}

}  // namespace

bool handle_ecall(Cpu& cpu) {
    const u64 eid = cpu.read_reg(17);   // a7
    const u64 fid = cpu.read_reg(16);   // a6
    const u64 a0  = cpu.read_reg(10);

    switch (eid) {
        // --- legacy v0.1 calls ------------------------------------------------
        case EXT_SET_TIMER:
            // The legacy form takes the deadline in a0 and returns nothing -
            // not even an error code, which is why a0 is left alone here.
            //
            // The timer is per-hart: each core arms its own deadline, and this
            // sets the calling hart's.
            if (cpu.clint) cpu.clint->set_timer(a0, cpu.csrs, cpu.hartid);
            return true;

        case EXT_CONSOLE_PUTCHAR:
            // Straight to the host console. Linux uses this for its earliest
            // output, before it has probed the device tree and found a real
            // UART driver - which is exactly when you most want to see
            // something, because a failure before that point is otherwise
            // completely silent.
            std::cout.put(static_cast<char>(a0));
            std::cout.flush();
            return true;

        case EXT_CONSOLE_GETCHAR: {
            const int c = cpu.uart ? cpu.uart->take_input_byte() : -1;
            cpu.write_reg(10, static_cast<u64>(static_cast<i64>(c)));
            return true;
        }

        case EXT_SHUTDOWN:
            cpu.halted = true;
            cpu.sbi_shutdown = true;
            return true;

        // --- v0.2 base extension: discovery -----------------------------------
        case EXT_BASE:
            switch (fid) {
                case 0: set_return(cpu, SUCCESS, SPEC_VERSION); return true;
                case 1: set_return(cpu, SUCCESS, IMPL_ID);      return true;
                case 2: set_return(cpu, SUCCESS, IMPL_VERSION); return true;
                case 3: set_return(cpu, SUCCESS, have_extension(a0) ? 1 : 0);
                        return true;
                case 4: set_return(cpu, SUCCESS, 0);            return true;  // mvendorid
                case 5: set_return(cpu, SUCCESS, 0);            return true;  // marchid
                case 6: set_return(cpu, SUCCESS, 0);            return true;  // mimpid
                default: set_return(cpu, ERR_NOT_SUPPORTED);    return true;
            }

        // --- v0.2 timer -------------------------------------------------------
        case EXT_TIME:
            if (fid == 0) {
                if (cpu.clint) cpu.clint->set_timer(a0, cpu.csrs, cpu.hartid);
                set_return(cpu, SUCCESS);
            } else {
                set_return(cpu, ERR_NOT_SUPPORTED);
            }
            return true;

        // --- v0.2 inter-processor interrupts ----------------------------------
        case EXT_IPI: {
            if (fid != 0) { set_return(cpu, ERR_NOT_SUPPORTED); return true; }
            // send_ipi(hart_mask, hart_mask_base): raise the *supervisor*
            // software interrupt on each target. A supervisor cannot write the
            // CLINT's msip (that is a machine-mode device), which is the whole
            // reason this is an SBI call; firmware turns it into SSIP.
            //
            // On a single-hart machine the only possible target is the caller,
            // and an interrupt to yourself is simply delivered like any other.
            const u64 a1 = cpu.read_reg(11);
            const bool ok = for_each_target(cpu, a0, a1, [](Cpu& target) {
                target.csrs.raise_interrupt(csr::MIP_SSIP);
            });
            set_return(cpu, ok ? SUCCESS : ERR_INVALID_PARAM);
            return true;
        }

        // --- v0.2 remote fences -----------------------------------------------
        case EXT_RFENCE: {
            // remote_fence_i (0), remote_sfence_vma (1), remote_sfence_vma_asid
            // (2): ask other harts to drop what they have cached about
            // translations and code. Each target's TLB and decoded-instruction
            // cache are flushed directly. A real hart would do it on receiving
            // the IPI; here the harts take turns on one host thread, so the
            // target is idle at this instant and the flush is immediate - which
            // is the strongest ordering the call could promise.
            if (fid > 2) { set_return(cpu, ERR_NOT_SUPPORTED); return true; }
            const u64 a1 = cpu.read_reg(11);
            const bool ok = for_each_target(cpu, a0, a1,
                                            [](Cpu& target) { target.remote_fence(); });
            set_return(cpu, ok ? SUCCESS : ERR_INVALID_PARAM);
            return true;
        }

        // --- v0.2 hart state management ---------------------------------------
        case EXT_HSM:
            switch (fid) {
                case 0: {   // hart_start(hartid, start_addr, opaque)
                    Cpu* target = hart_at(cpu, a0);
                    if (target == nullptr) { set_return(cpu, ERR_INVALID_PARAM); return true; }
                    if (!target->parked) { set_return(cpu, ERR_ALREADY_AVAILABLE); return true; }
                    target->start_at(cpu.read_reg(11), cpu.read_reg(12));
                    set_return(cpu, SUCCESS);
                    return true;
                }
                case 1:     // hart_stop(): the calling hart stops itself
                    // Only the caller can be stopped. The scheduler notices
                    // `parked` and never runs this hart again until another
                    // hart restarts it.
                    cpu.parked = true;
                    set_return(cpu, SUCCESS);
                    return true;
                case 2: {   // hart_get_status(hartid)
                    Cpu* target = hart_at(cpu, a0);
                    if (target == nullptr) { set_return(cpu, ERR_INVALID_PARAM); return true; }
                    set_return(cpu, SUCCESS, target->parked ? HART_STOPPED : HART_STARTED);
                    return true;
                }
                case 3:     // hart_suspend(type, resume_addr, opaque)
                    // Only the retentive default suspend (type bit 31 clear) is
                    // offered: the hart idles exactly as WFI does and carries on
                    // after the ecall when something wakes it. A non-retentive
                    // suspend would have to resume at resume_addr with the
                    // hart's state gone, which nothing here needs.
                    if ((a0 & 0x80000000ull) != 0) {
                        set_return(cpu, ERR_NOT_SUPPORTED);
                    } else {
                        cpu.waiting_for_interrupt = cpu.idle_on_wfi;
                        set_return(cpu, SUCCESS);
                    }
                    return true;
                default:
                    set_return(cpu, ERR_NOT_SUPPORTED);
                    return true;
            }

        // --- v0.2 system reset -------------------------------------------------
        case EXT_SRST:
            if (fid == 0) {
                cpu.halted = true;
                cpu.sbi_shutdown = true;
                set_return(cpu, SUCCESS);
            } else {
                set_return(cpu, ERR_NOT_SUPPORTED);
            }
            return true;

        default:
            // An unknown extension gets a clean "not supported" rather than a
            // trap. A kernel probing for something optional must be able to ask
            // and be told no.
            set_return(cpu, ERR_NOT_SUPPORTED);
            return true;
    }
}

}  // namespace sbi
