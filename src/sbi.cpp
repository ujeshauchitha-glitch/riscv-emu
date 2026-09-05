#include "sbi.hpp"

#include <iostream>

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
        case EXT_SRST:
            return true;
        default:
            return false;
    }
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
            if (cpu.clint) cpu.clint->set_timer(a0, cpu.csrs);
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
                if (cpu.clint) cpu.clint->set_timer(a0, cpu.csrs);
                set_return(cpu, SUCCESS);
            } else {
                set_return(cpu, ERR_NOT_SUPPORTED);
            }
            return true;

        // --- v0.2 inter-processor interrupts ----------------------------------
        case EXT_IPI:
            // One hart, so there is nobody to send an IPI to. Reporting success
            // is correct rather than lazy: the kernel asked for every hart in a
            // mask that contains only itself to be interrupted, and an
            // interrupt to yourself that you would immediately handle and
            // discard is indistinguishable from nothing happening.
            set_return(cpu, fid == 0 ? SUCCESS : ERR_NOT_SUPPORTED);
            return true;

        // --- v0.2 remote fences -----------------------------------------------
        case EXT_RFENCE:
            // Same reasoning. A remote fence asks other harts to invalidate
            // their TLBs; with one hart there are no other TLBs, and this
            // hart's own invalidation is done by the SFENCE.VMA the kernel
            // executes itself.
            set_return(cpu, SUCCESS);
            return true;

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
