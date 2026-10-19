//! The GDB server.
//!
//! Almost everything is tested by feeding handle_packet() the same strings a real gdb sends and
//! checking the reply, with a real program running underneath: a breakpoint is only a breakpoint if
//! the machine really stops on it. One test then connects an actual TCP client to check the framing
//! and the session loop.

mod common;

use std::cell::{Cell, RefCell};
use std::io::{Read, Write};
use std::net::TcpStream;
use std::rc::Rc;
use std::time::Duration;

use common::*;
use riscv_emu::bus::{Bus, Shared};
use riscv_emu::clint::{Clint, Clock};
use riscv_emu::csr::csr;
use riscv_emu::decoder::opcodes;
use riscv_emu::gdb::GdbStub;
use riscv_emu::mmu::pte;
use riscv_emu::platform::Platform;
use riscv_emu::plic::Plic;
use riscv_emu::syscon::Syscon;
use riscv_emu::types::*;
use riscv_emu::uart::Uart;

const DATA: u64 = DRAM_BASE + 0x10000;

fn hex64le(v: u64) -> String {
    v.to_le_bytes().iter().map(|b| format!("{b:02x}")).collect()
}

fn platform(program: &[u32], harts: u32) -> Platform {
    let mut bus = Bus::new(TEST_DRAM_SIZE);
    let (s, syscon) = Shared::new(Syscon::new());
    let (c, clint) = Shared::new(Clint::new(harts, Rc::new(Clock::new())));
    assert!(bus.attach(Box::new(s)) && bus.attach(Box::new(c)));
    let bytes: Vec<u8> = program.iter().flat_map(|w| w.to_le_bytes()).collect();
    bus.ram.load_image(DRAM_BASE, &bytes);

    let plic = Rc::new(RefCell::new(Plic::new(harts)));
    let uart = Rc::new(RefCell::new(Uart::new(Box::new(SharedBuf::new()))));
    let mut p = Platform::new(Rc::new(bus), harts);
    p.wire(clint, plic, uart, 0, syscon);
    for h in p.harts.iter_mut() {
        h.pc = DRAM_BASE;
    }
    p
}

/// A watchdog: a `continue` that nothing stops (because the feature under test is broken) must fail
/// the test, not hang the suite. Tests that want a different interrupt replace this.
fn arm_watchdog(stub: &mut GdbStub) {
    stub.interrupt_poll_interval = 64;
    let n = Cell::new(0);
    stub.set_interrupt_requested(Box::new(move || {
        n.set(n.get() + 1);
        n.get() > 2000
    }));
}

macro_rules! target {
    ($stub:ident, $program:expr) => {
        target!($stub, $program, 1)
    };
    ($stub:ident, $program:expr, $harts:expr) => {
        let mut platform_ = platform(&$program, $harts);
        let mut $stub = GdbStub::new(&mut platform_);
        arm_watchdog(&mut $stub);
    };
}

fn ask(stub: &mut GdbStub, packet: &str) -> String {
    stub.handle_packet(packet, None)
}

fn reg(stub: &mut GdbStub, n: usize) -> u64 {
    stub.platform_mut().harts[0].read_reg(n)
}

fn pc(stub: &GdbStub, hart: usize) -> u64 {
    stub.platform().harts[hart].pc
}

fn mem8(stub: &GdbStub, offset: u64) -> u8 {
    stub.platform().bus.ram.peek8(offset)
}

/// x5 counts 1, 2, 3, ... one instruction at a time, then spins.
fn counting_program(n: usize) -> Vec<u32> {
    let mut p = vec![addi(5, 0, 1)];
    for _ in 1..n {
        p.push(addi(5, 5, 1));
    }
    p.push(halt());
    p
}

// --- framing ---------------------------------------------------------------------------

#[test]
fn packets_are_framed_with_a_checksum() {
    assert_eq!(GdbStub::encode_packet("OK"), "$OK#9a"); // 'O'+'K' = 0x4f+0x4b = 0x9a
    assert_eq!(GdbStub::encode_packet(""), "$#00");
    assert_eq!(GdbStub::encode_packet("T05thread:1;").len(), 1 + 12 + 3);
}

// --- reading state ------------------------------------------------------------------------

#[test]
fn stop_query_and_register_reads() {
    target!(t, counting_program(12));
    assert_eq!(ask(&mut t, "?"), "T05thread:1;");

    let g = ask(&mut t, "g");
    assert_eq!(g.len(), 33 * 16); // x0..x31 and pc
    assert_eq!(&g[..16], hex64le(0)); // x0 is zero
    assert_eq!(&g[32 * 16..33 * 16], hex64le(DRAM_BASE)); // pc

    assert_eq!(ask(&mut t, "p20"), hex64le(DRAM_BASE)); // register 0x20 is the pc
    assert_eq!(ask(&mut t, "p5"), hex64le(0));

    // A control register, by gdb's numbering: 65 + the CSR number. misa is 0x301.
    let misa = ask(&mut t, &format!("p{:x}", 65 + 0x301));
    assert_eq!(misa.len(), 16);
    assert_ne!(misa, hex64le(0));
    assert_eq!(ask(&mut t, &format!("p{:x}", 65 + 0xf14)), hex64le(0)); // mhartid

    assert_eq!(ask(&mut t, "pffff"), "E02"); // not a register
    assert_eq!(ask(&mut t, "pzz"), "E01"); // not even a number
}

#[test]
fn registers_can_be_written() {
    target!(t, counting_program(12));
    assert_eq!(ask(&mut t, &format!("P5={}", hex64le(0x1122_3344_5566_7788))), "OK");
    assert_eq!(reg(&mut t, 5), 0x1122_3344_5566_7788);
    assert_eq!(ask(&mut t, "p5"), hex64le(0x1122_3344_5566_7788));

    assert_eq!(ask(&mut t, &format!("P0={}", hex64le(99))), "OK"); // x0 stays zero
    assert_eq!(reg(&mut t, 0), 0);

    assert_eq!(ask(&mut t, &format!("P20={}", hex64le(DRAM_BASE + 8))), "OK"); // set the pc
    assert_eq!(pc(&t, 0), DRAM_BASE + 8);
}

#[test]
fn memory_reads_and_writes() {
    target!(t, counting_program(12));
    // The first instruction is addi x5,x0,1 = 0x00100293, little-endian in memory.
    assert_eq!(ask(&mut t, "m80000000,4"), "93021000");

    assert_eq!(ask(&mut t, "M80010000,4:01020304"), "OK");
    assert_eq!(ask(&mut t, "m80010000,4"), "01020304");
    assert_eq!(mem8(&t, 0x10000 + 3), 4);

    assert_eq!(ask(&mut t, "m0,4"), "E14"); // nothing there
    assert_eq!(ask(&mut t, "m10000000,1"), "E14"); // device registers are off limits
    assert_eq!(ask(&mut t, "m80000000"), "E01"); // malformed
}

#[test]
fn memory_is_read_through_the_guests_page_tables() {
    target!(t, counting_program(12));

    // Map virtual 0x4000_0000 (VPN[2] = 1) onto the 1 GiB at 0x8000_0000 (a superpage), and run the
    // hart in supervisor mode under Sv39.
    let root = DRAM_BASE + 0x20000;
    let leaf = ((DRAM_BASE >> 12) << 10) | pte::V | pte::R | pte::W | pte::X | pte::A | pte::D;
    {
        let p = t.platform_mut();
        assert!(p.bus.ram.store(root - DRAM_BASE + 8, 8, leaf));
        p.harts[0].csrs.write(csr::SATP, (8u64 << 60) | (root >> 12));
        p.harts[0].priv_ = PRIV_SUPERVISOR;
    }

    assert_eq!(ask(&mut t, "m40000000,4"), "93021000"); // the same instruction, via the mapping
    assert_eq!(ask(&mut t, "M40010000,2:abcd"), "OK");
    assert_eq!(mem8(&t, 0x10000), 0xab);

    assert_eq!(ask(&mut t, "m50000000,4"), "E14"); // unmapped: an error, not a guest fault
    assert_eq!(t.platform().harts[0].priv_, PRIV_SUPERVISOR); // and nothing was disturbed
}

// --- running -------------------------------------------------------------------------------

#[test]
fn breakpoint_stops_the_machine_before_the_instruction() {
    target!(t, counting_program(12));
    assert_eq!(ask(&mut t, "Z0,80000014,4"), "OK"); // instruction 5
    assert_eq!(t.breakpoint_count(), 1);

    assert_eq!(ask(&mut t, "c"), "T05thread:1;");
    assert_eq!(pc(&t, 0), DRAM_BASE + 20);
    assert_eq!(reg(&mut t, 5), 5); // instructions 0-4 ran, 5 has not

    // Nothing in guest memory was patched to make that happen.
    assert_eq!(ask(&mut t, "m80000014,4"), "93821200"); // still addi x5,x5,1
}

#[test]
fn single_step_advances_one_instruction() {
    target!(t, counting_program(12));
    ask(&mut t, "Z0,80000014,4");
    ask(&mut t, "c");
    assert_eq!(ask(&mut t, "s"), "T05thread:1;");
    assert_eq!(pc(&t, 0), DRAM_BASE + 24);
    assert_eq!(reg(&mut t, 5), 6);
    ask(&mut t, "s");
    assert_eq!(reg(&mut t, 5), 7);
}

#[test]
fn continue_leaves_the_breakpoint_it_is_sitting_on() {
    target!(t, counting_program(12));
    ask(&mut t, "Z0,80000014,4");
    ask(&mut t, "c"); // stopped at the breakpoint
    assert_eq!(reg(&mut t, 5), 5);

    // Resuming must run the instruction under the breakpoint rather than stop at once; here it runs
    // on to the next breakpoint.
    ask(&mut t, "Z0,8000001c,4"); // instruction 7
    assert_eq!(ask(&mut t, "c"), "T05thread:1;");
    assert_eq!(pc(&t, 0), DRAM_BASE + 28);
    assert_eq!(reg(&mut t, 5), 7);
}

#[test]
fn removed_breakpoints_stop_stopping() {
    target!(t, counting_program(40));
    ask(&mut t, "Z0,80000014,4");
    assert_eq!(ask(&mut t, "z0,80000014,4"), "OK");
    assert_eq!(t.breakpoint_count(), 0);

    // Nothing to stop it; a stand-in for Ctrl-C ends the run.
    let polls = Cell::new(0);
    t.interrupt_poll_interval = 16;
    t.set_interrupt_requested(Box::new(move || {
        polls.set(polls.get() + 1);
        polls.get() >= 5
    }));
    assert_eq!(ask(&mut t, "c"), "T02thread:1;"); // SIGINT
    assert_eq!(reg(&mut t, 5), 40); // ran right through
}

#[test]
fn a_write_watchpoint_stops_when_memory_changes() {
    // A loop that increments a counter in memory.
    let mut p = Vec::new();
    load_imm64(&mut p, 6, DATA);
    let loop_ = p.len();
    p.push(i_type(opcodes::LOAD, 7, 0x3, 6, 0)); // ld   x7,0(x6)
    p.push(addi(7, 7, 1));
    p.push(s_type(opcodes::STORE, 0x3, 6, 7, 0)); // sd   x7,0(x6)
    p.push(j_type(opcodes::JAL, 0, -(((p.len() - loop_) * 4) as i32)));
    target!(t, p);

    assert_eq!(ask(&mut t, &format!("Z2,{DATA:x},8")), "OK");

    assert_eq!(ask(&mut t, "c"), format!("T05watch:{DATA:x};thread:1;"));
    assert_eq!(mem8(&t, 0x10000), 1); // the first store just landed

    ask(&mut t, "c");
    assert_eq!(mem8(&t, 0x10000), 2); // and it stops again on the next

    assert_eq!(ask(&mut t, &format!("z2,{DATA:x},8")), "OK");
    assert_eq!(ask(&mut t, "Z3,80010000,8"), ""); // read watchpoints: unsupported, said so
}

#[test]
fn machine_exit_is_reported_as_w00() {
    let mut p = Vec::new();
    load_imm64(&mut p, 5, SYSCON_BASE);
    load_imm64(&mut p, 6, 0x5555);
    p.push(s_type(opcodes::STORE, 0x2, 5, 6, 0)); // sw x6,0(x5): power off
    p.push(halt());
    target!(t, p);
    assert_eq!(ask(&mut t, "c"), "W00");
}

#[test]
fn ebreak_is_sigtrap_and_resuming_steps_over_it() {
    target!(t, [addi(5, 0, 7), 0x0010_0073, addi(5, 5, 1), halt()]);
    assert_eq!(ask(&mut t, "c"), "T05thread:1;");
    assert_eq!(pc(&t, 0), DRAM_BASE + 4);
    t.interrupt_poll_interval = 8;
    let polls = Cell::new(0);
    t.set_interrupt_requested(Box::new(move || {
        polls.set(polls.get() + 1);
        polls.get() >= 3
    }));
    ask(&mut t, "c");
    assert_eq!(reg(&mut t, 5), 8); // carried on past the ebreak
}

#[test]
fn an_illegal_instruction_is_sigill() {
    target!(t, [addi(5, 0, 1), 0x0000_0000]);
    assert_eq!(ask(&mut t, "c"), "T04thread:1;");
}

#[test]
fn a_wild_jump_is_sigsegv() {
    let mut p = Vec::new();
    load_imm64(&mut p, 5, 0x1000);
    p.push(jalr(0, 5, 0));
    target!(t, p);
    assert_eq!(ask(&mut t, "c"), "T0bthread:1;");
}

// --- harts as threads ---------------------------------------------------------------------------

#[test]
fn every_hart_is_a_thread() {
    target!(t, counting_program(12), 2);
    assert_eq!(ask(&mut t, "qfThreadInfo"), "m1,2");
    assert_eq!(ask(&mut t, "qsThreadInfo"), "l");
    assert_eq!(ask(&mut t, "qC"), "QC1");
    assert_eq!(ask(&mut t, "T2"), "OK");
    assert_eq!(ask(&mut t, "T3"), "E01");

    // Give hart 1 a distinct pc and select it for register access.
    t.platform_mut().harts[1].pc = DRAM_BASE + 8;
    assert_eq!(ask(&mut t, "Hg2"), "OK");
    assert_eq!(ask(&mut t, "qC"), "QC2");
    assert_eq!(ask(&mut t, "p20"), hex64le(DRAM_BASE + 8));
    assert_eq!(ask(&mut t, "Hg1"), "OK");
    assert_eq!(ask(&mut t, "p20"), hex64le(DRAM_BASE));

    // vCont;s:2 steps only the second hart.
    assert_eq!(ask(&mut t, "vCont;s:2"), "T05thread:2;");
    assert_eq!(pc(&t, 1), DRAM_BASE + 12);
    assert_eq!(pc(&t, 0), DRAM_BASE);

    let mut console = Vec::new();
    assert_eq!(t.handle_packet("qRcmd,6861727473", Some(&mut console)), "OK"); // "harts"
    assert_eq!(console.len(), 2);
    assert!(console[0].contains("hart 0"));
}

#[test]
fn the_target_description_names_the_architecture() {
    target!(t, counting_program(12));
    assert!(ask(&mut t, "qSupported:multiprocess+").starts_with("PacketSize="));
    assert!(ask(&mut t, "qXfer:features:read:target.xml:0,fff").starts_with('m')); // more to come
    let mut xml = String::new();
    let mut off = 0;
    loop {
        let chunk = ask(&mut t, &format!("qXfer:features:read:target.xml:{off:x},400"));
        xml += &chunk[1..];
        if chunk.starts_with('l') {
            break;
        }
        off += 0x400;
    }
    assert!(xml.contains("riscv:rv64"));
    assert!(xml.contains("org.gnu.gdb.riscv.cpu"));
    assert!(xml.contains("name=\"satp\""));
    assert!(xml.contains("name=\"fa0\""));
    assert_eq!(ask(&mut t, "vMustReplyEmpty"), "");
    assert_eq!(ask(&mut t, "Xnonsense"), ""); // unsupported commands answer empty
}

#[test]
fn monitor_commands_answer_on_the_console() {
    target!(t, counting_program(12));
    let mut console = Vec::new();
    // "csr misa"
    assert_eq!(t.handle_packet("qRcmd,63737220 6d697361".replace(' ', "").as_str(), Some(&mut console)), "OK");
    assert!(console.iter().any(|l| l.contains("misa")), "{console:?}");
    let mut console = Vec::new();
    // "time"
    assert_eq!(t.handle_packet("qRcmd,74696d65", Some(&mut console)), "OK");
    assert_eq!(console.len(), 1);
}

// --- over a real socket ---------------------------------------------------------------------------

/// What the debugger side of the conversation saw.
struct Transcript {
    query: String,
    reg_pc: String,
    nak: String,
    detach: String,
}

fn client(port: u16) -> Transcript {
    let mut c = {
        let mut tries = 0;
        loop {
            match TcpStream::connect(("127.0.0.1", port)) {
                Ok(s) => break s,
                Err(_) if tries < 2000 => {
                    tries += 1;
                    std::thread::sleep(Duration::from_millis(1));
                }
                Err(e) => panic!("cannot connect: {e}"),
            }
        }
    };
    c.set_read_timeout(Some(Duration::from_secs(3))).unwrap();

    let mut exchange = |payload: &str| -> String {
        c.write_all(GdbStub::encode_packet(payload).as_bytes()).unwrap();
        let mut got = String::new();
        let mut buf = [0u8; 512];
        for _ in 0..3000 {
            if let Ok(n) = c.read(&mut buf) {
                got.push_str(&String::from_utf8_lossy(&buf[..n]));
            }
            if let Some(hash) = got.find('#') {
                if got.len() >= hash + 3 && got.contains('$') {
                    break;
                }
            }
        }
        got
    };

    let query = exchange("?");
    let reg_pc = exchange("p20");

    // A packet whose checksum is wrong is refused with '-', not acted on.
    c.write_all(b"$p20#00").unwrap();
    let mut nak = String::new();
    let mut b = [0u8; 8];
    for _ in 0..2000 {
        if let Ok(n) = c.read(&mut b) {
            if n > 0 {
                nak = String::from_utf8_lossy(&b[..n]).into_owned();
                break;
            }
        }
    }

    // Detaching ends the session and tells the emulator to carry on.
    c.write_all(GdbStub::encode_packet("D").as_bytes()).unwrap();
    let mut detach = String::new();
    let mut buf = [0u8; 64];
    while !detach.contains('#') {
        match c.read(&mut buf) {
            Ok(0) | Err(_) => break,
            Ok(n) => detach.push_str(&String::from_utf8_lossy(&buf[..n])),
        }
    }
    Transcript { query, reg_pc, nak, detach: detach.chars().take(7).collect() }
}

#[test]
fn a_session_over_tcp() {
    target!(t, counting_program(12));
    let port = t.listen(0);
    assert_ne!(port, 0);

    // The stub keeps its machine on this thread (a machine is not Send), so the debugger is the one
    // that runs on another.
    let debugger = std::thread::spawn(move || client(port));
    assert!(t.accept_client());
    let carry_on = t.serve();
    let seen = debugger.join().unwrap();

    // The server acknowledges the packet with '+' and replies with a framed packet.
    assert_eq!(seen.query, format!("+{}", GdbStub::encode_packet("T05thread:1;")));
    assert_eq!(seen.reg_pc, format!("+{}", GdbStub::encode_packet(&hex64le(DRAM_BASE))));
    assert_eq!(seen.nak, "-");
    assert_eq!(seen.detach, format!("+{}", GdbStub::encode_packet("OK")).chars().take(7).collect::<String>());
    assert!(carry_on);
    assert!(t.detached());
}
