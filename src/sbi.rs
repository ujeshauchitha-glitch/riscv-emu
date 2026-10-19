//! The Supervisor Binary Interface: the contract between a supervisor-mode kernel and
//! the machine-mode firmware beneath it.
//!
//! A kernel running in S-mode cannot touch machine-mode resources - the timer
//! compare register, other harts' interrupt lines, the power button. It asks
//! firmware instead, with an `ecall`: extension id in a7, function id in a6,
//! arguments in a0-a5, and an error code / value pair coming back in a0 / a1.
//!
//! This emulator has no separate firmware image (it plays the OpenSBI role itself),
//! so an ecall from S-mode is answered here instead of being delivered as a trap.
//! That is only done when SBI is enabled (the Linux boot path); xv6 and the bare
//! test programs run their own M-mode code and see a normal ecall trap.

use std::io::Write;

use crate::cpu::Cpu;
use crate::csr::csr;

pub const EXT_SET_TIMER: u64 = 0x00;
pub const EXT_CONSOLE_PUTCHAR: u64 = 0x01;
pub const EXT_CONSOLE_GETCHAR: u64 = 0x02;
pub const EXT_SHUTDOWN: u64 = 0x08;

pub const EXT_BASE: u64 = 0x10; // "discover what is here"
pub const EXT_TIME: u64 = 0x5449_4d45; // "TIME"
pub const EXT_IPI: u64 = 0x73_5049; // "sPI"
pub const EXT_RFENCE: u64 = 0x5246_4e43; // "RFNC"
pub const EXT_HSM: u64 = 0x48_534d; // "HSM"
pub const EXT_SRST: u64 = 0x5352_5354; // "SRST"

pub const SUCCESS: i64 = 0;
pub const ERR_FAILED: i64 = -1;
pub const ERR_NOT_SUPPORTED: i64 = -2;
pub const ERR_INVALID_PARAM: i64 = -3;
pub const ERR_DENIED: i64 = -4;
pub const ERR_INVALID_ADDRESS: i64 = -5;
pub const ERR_ALREADY_AVAILABLE: i64 = -6;
pub const ERR_ALREADY_STARTED: i64 = -7;
pub const ERR_ALREADY_STOPPED: i64 = -8;

pub const HART_STARTED: u64 = 0;
pub const HART_STOPPED: u64 = 1;

// The SBI implementation this emulator reports. Nothing checks it, but a kernel that
// asks and gets nothing back may conclude SBI is absent.
const IMPL_ID: u64 = 5; // "unknown implementation"
const IMPL_VERSION: u64 = 1;
const SPEC_VERSION: u64 = 3; // v0.3

fn set_return(cpu: &mut Cpu, error: i64, value: u64) {
    cpu.write_reg(10, error as u64); // a0
    cpu.write_reg(11, value); // a1
}

/// Is this extension available? A kernel calls probe_extension before using one, and
/// an honest "no" makes it take a documented fallback path instead of executing
/// something that does not work.
fn have_extension(eid: u64) -> bool {
    matches!(
        eid,
        EXT_SET_TIMER
            | EXT_CONSOLE_PUTCHAR
            | EXT_CONSOLE_GETCHAR
            | EXT_SHUTDOWN
            | EXT_BASE
            | EXT_TIME
            | EXT_IPI
            | EXT_RFENCE
            | EXT_HSM
            | EXT_SRST
    )
}

/// The IPI and remote-fence calls name their targets as a bit mask relative to a base
/// hart id: bit i of `mask` selects hart `base + i`. A base of all-ones is the special
/// case "every hart", with the mask ignored. Returns the selected hart ids, or `None`
/// if the mask names a hart that does not exist.
fn targets(cpu: &Cpu, mask: u64, base: u64) -> Option<Vec<usize>> {
    let count = cpu.peers.len() as u64;
    if base == u64::MAX {
        return Some((0..count as usize).collect());
    }
    let mut out = Vec::new();
    let mut ok = true;
    for bit in 0..64u64 {
        if (mask >> bit) & 1 == 0 {
            continue;
        }
        let id = base.wrapping_add(bit);
        if id < count {
            out.push(id as usize);
        } else {
            ok = false;
        }
    }
    ok.then_some(out)
}

/// Answer an ecall from the supervisor. Returns true if it was handled here.
pub fn handle_ecall(cpu: &mut Cpu) -> bool {
    let eid = cpu.read_reg(17); // a7
    let fid = cpu.read_reg(16); // a6
    let a0 = cpu.read_reg(10);
    let me = cpu.hartid as usize;

    match eid {
        // --- legacy v0.1 calls --------------------------------------------------
        EXT_SET_TIMER => {
            // The legacy form takes the deadline in a0 and returns nothing - not even
            // an error code, which is why a0 is left alone. The timer is per-hart:
            // each core arms its own deadline, and this sets the calling hart's.
            if let Some(c) = cpu.clint.clone() {
                c.borrow_mut().set_timer(a0, &mut cpu.csrs, cpu.hartid as u32);
            }
            true
        }
        EXT_CONSOLE_PUTCHAR => {
            // Straight to the host console. Linux uses this for its earliest output,
            // before it has probed the device tree and found a real UART driver -
            // which is exactly when you most want to see something, because a failure
            // before that point is otherwise completely silent.
            let mut out = std::io::stdout();
            let _ = out.write_all(&[a0 as u8]);
            let _ = out.flush();
            true
        }
        EXT_CONSOLE_GETCHAR => {
            let c = cpu.uart.as_ref().map_or(-1, |u| u.borrow_mut().take_input_byte());
            cpu.write_reg(10, c as i64 as u64);
            true
        }
        EXT_SHUTDOWN => {
            cpu.halted = true;
            cpu.sbi_shutdown = true;
            true
        }

        // --- v0.2 base extension: discovery -------------------------------------
        EXT_BASE => {
            match fid {
                0 => set_return(cpu, SUCCESS, SPEC_VERSION),
                1 => set_return(cpu, SUCCESS, IMPL_ID),
                2 => set_return(cpu, SUCCESS, IMPL_VERSION),
                3 => set_return(cpu, SUCCESS, have_extension(a0) as u64),
                4..=6 => set_return(cpu, SUCCESS, 0), // mvendorid, marchid, mimpid
                _ => set_return(cpu, ERR_NOT_SUPPORTED, 0),
            }
            true
        }

        // --- v0.2 timer ---------------------------------------------------------
        EXT_TIME => {
            if fid == 0 {
                if let Some(c) = cpu.clint.clone() {
                    c.borrow_mut().set_timer(a0, &mut cpu.csrs, cpu.hartid as u32);
                }
                set_return(cpu, SUCCESS, 0);
            } else {
                set_return(cpu, ERR_NOT_SUPPORTED, 0);
            }
            true
        }

        // --- v0.2 inter-processor interrupts ------------------------------------
        EXT_IPI => {
            if fid != 0 {
                set_return(cpu, ERR_NOT_SUPPORTED, 0);
                return true;
            }
            // send_ipi(hart_mask, hart_mask_base): raise the *supervisor* software
            // interrupt on each target. A supervisor cannot write the CLINT's msip
            // (that is a machine-mode device), which is the whole reason this is an
            // SBI call; firmware turns it into SSIP.
            let a1 = cpu.read_reg(11);
            match targets(cpu, a0, a1) {
                Some(ids) => {
                    for id in ids {
                        if id == me {
                            cpu.csrs.raise_interrupt(csr::MIP_SSIP);
                        } else {
                            cpu.peers[id].send_ipi();
                        }
                    }
                    set_return(cpu, SUCCESS, 0);
                }
                None => set_return(cpu, ERR_INVALID_PARAM, 0),
            }
            true
        }

        // --- v0.2 remote fences -------------------------------------------------
        EXT_RFENCE => {
            // remote_fence_i (0), remote_sfence_vma (1), remote_sfence_vma_asid (2):
            // ask other harts to drop what they have cached about translations and
            // code. The target does it before it next runs; the harts take turns on
            // one host thread, so it is idle at this instant and the flush is
            // immediate in effect - the strongest ordering the call could promise.
            if fid > 2 {
                set_return(cpu, ERR_NOT_SUPPORTED, 0);
                return true;
            }
            let a1 = cpu.read_reg(11);
            match targets(cpu, a0, a1) {
                Some(ids) => {
                    for id in ids {
                        if id == me {
                            cpu.remote_fence();
                        } else {
                            cpu.peers[id].send_fence();
                        }
                    }
                    set_return(cpu, SUCCESS, 0);
                }
                None => set_return(cpu, ERR_INVALID_PARAM, 0),
            }
            true
        }

        // --- v0.2 hart state management -----------------------------------------
        EXT_HSM => {
            match fid {
                0 => {
                    // hart_start(hartid, start_addr, opaque)
                    let (addr, opaque) = (cpu.read_reg(11), cpu.read_reg(12));
                    if a0 >= cpu.peers.len() as u64 {
                        set_return(cpu, ERR_INVALID_PARAM, 0);
                    } else if !cpu.peers[a0 as usize].parked.get() {
                        set_return(cpu, ERR_ALREADY_AVAILABLE, 0);
                    } else {
                        cpu.peers[a0 as usize].send_start(addr, opaque);
                        set_return(cpu, SUCCESS, 0);
                    }
                }
                1 => {
                    // hart_stop(): the calling hart stops itself. The scheduler notices
                    // `parked` and never runs this hart again until another hart
                    // restarts it.
                    cpu.mb.parked.set(true);
                    set_return(cpu, SUCCESS, 0);
                }
                2 => {
                    // hart_get_status(hartid)
                    if a0 >= cpu.peers.len() as u64 {
                        set_return(cpu, ERR_INVALID_PARAM, 0);
                    } else {
                        let stopped = cpu.peers[a0 as usize].parked.get();
                        set_return(cpu, SUCCESS, if stopped { HART_STOPPED } else { HART_STARTED });
                    }
                }
                3 => {
                    // hart_suspend(type, resume_addr, opaque). Only the retentive default
                    // suspend (type bit 31 clear) is offered: the hart idles exactly as
                    // WFI does and carries on after the ecall when something wakes it. A
                    // non-retentive suspend would have to resume at resume_addr with the
                    // hart's state gone, which nothing here needs.
                    if a0 & 0x8000_0000 != 0 {
                        set_return(cpu, ERR_NOT_SUPPORTED, 0);
                    } else {
                        cpu.waiting_for_interrupt = cpu.idle_on_wfi;
                        set_return(cpu, SUCCESS, 0);
                    }
                }
                _ => set_return(cpu, ERR_NOT_SUPPORTED, 0),
            }
            true
        }

        // --- v0.2 system reset --------------------------------------------------
        EXT_SRST => {
            if fid == 0 {
                cpu.halted = true;
                cpu.sbi_shutdown = true;
                set_return(cpu, SUCCESS, 0);
            } else {
                set_return(cpu, ERR_NOT_SUPPORTED, 0);
            }
            true
        }

        // An unknown extension gets a clean "not supported" rather than a trap. A
        // kernel probing for something optional must be able to ask and be told no.
        _ => {
            set_return(cpu, ERR_NOT_SUPPORTED, 0);
            true
        }
    }
}
