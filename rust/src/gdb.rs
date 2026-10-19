//! A GDB remote-protocol stub, so a real `gdb` can debug the guest.
//!
//! With `--gdb PORT` the emulator builds the machine, then waits for a debugger before
//! executing a single instruction. gdb connects (`target remote :PORT`) and gets
//! breakpoints, single-stepping, register and memory access, write watchpoints, and a
//! thread per hart - everything a kernel developer wants and a hardware probe would
//! not give for free. Since the machine is deterministic, a bug found under the
//! debugger happens again, at the same instruction, on the next run.
//!
//! The protocol (gdb docs, "Remote Serial Protocol"): packets are `$payload#cc` where
//! `cc` is the two-hex-digit sum of the payload bytes modulo 256, each acknowledged
//! with `+` (or `-` to ask for a resend). A bare 0x03 byte, sent outside any packet,
//! interrupts a running target. Registers and memory travel as hex.
//!
//! Breakpoints are checked by comparing the pc before each instruction rather than by
//! patching an `ebreak` into guest memory: it leaves memory untouched, so reading
//! memory shows what the program really has and a breakpoint in ROM or in a page about
//! to be remapped just works.

use std::cell::RefCell;
use std::collections::BTreeSet;
use std::io::{ErrorKind, Read, Write};
use std::net::{Ipv4Addr, SocketAddrV4, TcpListener, TcpStream};
use std::rc::Rc;
use std::time::Duration;

use crate::cpu::Cpu;
use crate::device::AccessType;
use crate::platform::Platform;
use crate::trap::{Exception, Trap};
use crate::types::{privilege_name, NUM_REGS};

// --- hex ----------------------------------------------------------------------------

const HEX_DIGITS: &[u8; 16] = b"0123456789abcdef";

fn to_hex(bytes: &[u8]) -> String {
    let mut s = String::with_capacity(bytes.len() * 2);
    for &b in bytes {
        s.push(HEX_DIGITS[(b >> 4) as usize] as char);
        s.push(HEX_DIGITS[(b & 15) as usize] as char);
    }
    s
}

fn hex_value(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        b'A'..=b'F' => Some(c - b'A' + 10),
        _ => None,
    }
}

fn from_hex(s: &str) -> Option<Vec<u8>> {
    let b = s.as_bytes();
    if b.len() % 2 != 0 {
        return None;
    }
    b.chunks(2).map(|p| Some((hex_value(p[0])? << 4) | hex_value(p[1])?)).collect()
}

/// A register as little-endian bytes, which is how gdb wants a RISC-V register on the
/// wire.
fn reg_hex(value: u64, bytes: usize) -> String {
    to_hex(&value.to_le_bytes()[..bytes])
}

fn parse_u64(s: &str) -> Option<u64> {
    if s.is_empty() {
        return None;
    }
    u64::from_str_radix(s.strip_prefix("0x").unwrap_or(s), 16).ok()
}

// --- register numbering ---------------------------------------------------------------
//
// gdb's RISC-V register numbers: 0-31 the integer registers, 32 the pc, 33-64 the
// floating-point registers, and 65 + n for control register number n. (Machine state
// like mstatus is a "register" to a debugger; satp is 65 + 0x180.)
const REG_PC: u32 = 32;
const REG_FP_BASE: u32 = 33;
const REG_CSR_BASE: u32 = 65;

/// The control registers worth naming in the target description. gdb shows any the
/// description lists; the rest remain reachable through `monitor csr`.
const CSR_NAMES: &[(&str, u32)] = &[
    ("fflags", 0x001),
    ("frm", 0x002),
    ("fcsr", 0x003),
    ("sstatus", 0x100),
    ("sie", 0x104),
    ("stvec", 0x105),
    ("scounteren", 0x106),
    ("sscratch", 0x140),
    ("sepc", 0x141),
    ("scause", 0x142),
    ("stval", 0x143),
    ("sip", 0x144),
    ("satp", 0x180),
    ("mstatus", 0x300),
    ("misa", 0x301),
    ("medeleg", 0x302),
    ("mideleg", 0x303),
    ("mie", 0x304),
    ("mtvec", 0x305),
    ("mcounteren", 0x306),
    ("mscratch", 0x340),
    ("mepc", 0x341),
    ("mcause", 0x342),
    ("mtval", 0x343),
    ("mip", 0x344),
    ("cycle", 0xc00),
    ("time", 0xc01),
    ("instret", 0xc02),
    ("mhartid", 0xf14),
];

const GPR_NAMES: [&str; 32] = [
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "fp", "s1", "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7", "s2",
    "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
];

const FP_NAMES: [&str; 32] = [
    "ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6", "ft7", "fs0", "fs1", "fa0", "fa1", "fa2", "fa3", "fa4", "fa5",
    "fa6", "fa7", "fs2", "fs3", "fs4", "fs5", "fs6", "fs7", "fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11",
];

/// The three floating-point control registers are only 32 bits wide in gdb's
/// description.
fn csr_is_narrow(csr: u32) -> bool {
    (0x001..=0x003).contains(&csr)
}

fn signal_for(e: Exception) -> u32 {
    match e {
        Exception::Breakpoint | Exception::ECallFromUMode | Exception::ECallFromSMode | Exception::ECallFromMMode => 5, // SIGTRAP
        Exception::IllegalInstruction => 4, // SIGILL
        Exception::InstructionAddressMisaligned
        | Exception::LoadAddressMisaligned
        | Exception::StoreAMOAddressMisaligned => 7, // SIGBUS
        _ => 11, // SIGSEGV
    }
}

// --- guest memory as the debugger sees it ------------------------------------------------

const PAGE_SIZE: u64 = 4096;

/// gdb speaks virtual addresses. Resolve one the way the hart itself would, but report
/// a failure instead of raising the page fault into the guest.
fn translate(cpu: &mut Cpu, vaddr: u64, write: bool) -> Option<u64> {
    let primary = if write { AccessType::Store } else { AccessType::Load };
    let mut pa = cpu.mmu.translate(&cpu.bus, &cpu.csrs, vaddr, primary, cpu.priv_);
    // A debugger wants to read code that is execute-only, which a load refuses.
    if pa.is_err() && !write {
        pa = cpu.mmu.translate(&cpu.bus, &cpu.csrs, vaddr, AccessType::Instruction, cpu.priv_);
    }
    pa.ok()
}

/// Read guest memory. Only RAM: device registers are refused, because reading some of
/// them (a UART's receive register, the PLIC's claim register) changes the machine, and
/// a debugger that merely looks must not.
fn read_bytes(cpu: &mut Cpu, vaddr: u64, len: u64) -> Option<Vec<u8>> {
    let mut out = vec![0u8; len as usize];
    let mut i = 0u64;
    while i < len {
        let va = vaddr.wrapping_add(i);
        let pa = translate(cpu, va, false)?;
        // Stay inside one page per translation.
        let chunk = (len - i).min(PAGE_SIZE - (va & (PAGE_SIZE - 1)));
        if !cpu.bus.ram.read_bytes(pa, &mut out[i as usize..(i + chunk) as usize]) {
            return None;
        }
        i += chunk;
    }
    Some(out)
}

fn write_bytes(cpu: &mut Cpu, vaddr: u64, data: &[u8]) -> bool {
    let mut i = 0usize;
    while i < data.len() {
        let va = vaddr.wrapping_add(i as u64);
        let Some(pa) = translate(cpu, va, true) else {
            return false;
        };
        let chunk = (data.len() - i).min((PAGE_SIZE - (va & (PAGE_SIZE - 1))) as usize);
        if !cpu.bus.ram.write_bytes(pa, &data[i..i + chunk]) {
            return false;
        }
        i += chunk;
    }
    true
}

// --- state shared with the per-instruction hook -------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Stop {
    None,
    Step,
    Breakpoint,
    Watch,
    Interrupt,
    Halted,
    Trap,
}

struct Watch {
    addr: u64,
    len: u64,
    last: Vec<u8>,
}

struct State {
    client: Option<TcpStream>,
    inbuf: Vec<u8>,
    last_sent: String,

    breakpoints: BTreeSet<u64>, // by virtual address
    watches: Vec<Watch>,
    general_hart: u32, // `Hg`: whose registers and memory
    detached: bool,
    killed: bool,

    stop: Stop,
    stop_hart: u32,
    stop_addr: u64, // watchpoint address
    signal: u32,    // SIGTRAP

    interrupt_requested: Option<Box<dyn FnMut() -> bool>>,
}

impl State {
    /// While the target runs, the only thing gdb sends is a bare 0x03 to stop it.
    fn poll_interrupt_byte(&mut self) -> bool {
        let Some(c) = self.client.as_mut() else {
            return false;
        };
        let mut buf = [0u8; 256];
        match c.read(&mut buf) {
            Ok(0) => true, // the debugger went away: stop
            Ok(n) => {
                let mut interrupted = false;
                for &b in &buf[..n] {
                    if b == 0x03 {
                        interrupted = true;
                    } else {
                        self.inbuf.push(b);
                    }
                }
                interrupted
            }
            Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::Interrupted) => false,
            Err(_) => true,
        }
    }
}

pub struct GdbStub<'p> {
    platform: &'p mut Platform,
    st: Rc<RefCell<State>>,
    listener: Option<TcpListener>,
    /// How often (in instructions) a running target checks whether gdb sent Ctrl-C.
    pub interrupt_poll_interval: u64,
}

impl<'p> GdbStub<'p> {
    pub fn new(platform: &'p mut Platform) -> GdbStub<'p> {
        GdbStub {
            platform,
            st: Rc::new(RefCell::new(State {
                client: None,
                inbuf: Vec::new(),
                last_sent: String::new(),
                breakpoints: BTreeSet::new(),
                watches: Vec::new(),
                general_hart: 0,
                detached: false,
                killed: false,
                stop: Stop::None,
                stop_hart: 0,
                stop_addr: 0,
                signal: 5,
                interrupt_requested: None,
            })),
            listener: None,
            interrupt_poll_interval: 4096,
        }
    }

    /// Start listening on `port` (0 picks one) on the loopback address. Returns the port,
    /// or 0 on failure.
    pub fn listen(&mut self, port: u16) -> u16 {
        match TcpListener::bind(SocketAddrV4::new(Ipv4Addr::LOCALHOST, port)) {
            Ok(l) => {
                let bound = l.local_addr().map_or(0, |a| a.port());
                let _ = l.set_nonblocking(true);
                self.listener = Some(l);
                bound
            }
            Err(_) => 0,
        }
    }

    /// Block until a debugger connects.
    pub fn accept_client(&mut self) -> bool {
        let Some(l) = &self.listener else {
            return false;
        };
        loop {
            match l.accept() {
                Ok((s, _)) => {
                    let _ = s.set_nonblocking(true);
                    let _ = s.set_nodelay(true);
                    self.st.borrow_mut().client = Some(s);
                    return true;
                }
                Err(e) if e.kind() == ErrorKind::WouldBlock => std::thread::sleep(Duration::from_millis(5)),
                Err(_) => return false,
            }
        }
    }

    /// Install a replacement for "did gdb send Ctrl-C?" (tests use this to stop a
    /// running guest without a socket).
    pub fn set_interrupt_requested(&mut self, f: Box<dyn FnMut() -> bool>) {
        self.st.borrow_mut().interrupt_requested = Some(f);
    }

    /// The machine being debugged.
    pub fn platform(&self) -> &Platform {
        self.platform
    }

    pub fn platform_mut(&mut self) -> &mut Platform {
        self.platform
    }

    pub fn detached(&self) -> bool {
        self.st.borrow().detached
    }
    pub fn killed(&self) -> bool {
        self.st.borrow().killed
    }
    pub fn breakpoint_count(&self) -> usize {
        self.st.borrow().breakpoints.len()
    }

    // --- packet framing -----------------------------------------------------------------

    pub fn encode_packet(payload: &str) -> String {
        let sum = payload.bytes().fold(0u32, |a, b| a + b as u32);
        format!("${payload}#{:02x}", sum & 0xff)
    }

    fn write_client(st: &mut State, data: &[u8]) -> bool {
        let Some(c) = st.client.as_mut() else {
            return false;
        };
        let mut sent = 0;
        while sent < data.len() {
            match c.write(&data[sent..]) {
                Ok(0) => return false,
                Ok(n) => sent += n,
                Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::Interrupted) => {
                    std::thread::sleep(Duration::from_millis(1))
                }
                Err(_) => return false,
            }
        }
        true
    }

    fn send_packet(&self, payload: &str) -> bool {
        let mut st = self.st.borrow_mut();
        st.last_sent = Self::encode_packet(payload);
        let data = st.last_sent.clone();
        Self::write_client(&mut st, data.as_bytes())
    }

    fn read_packet(&self, block: bool) -> Option<String> {
        loop {
            {
                let mut st = self.st.borrow_mut();
                // Anything already buffered first.
                while !st.inbuf.is_empty() {
                    let c = st.inbuf[0];
                    if c == b'+' {
                        st.inbuf.remove(0); // an acknowledgment
                        continue;
                    }
                    if c == b'-' {
                        st.inbuf.remove(0);
                        let again = st.last_sent.clone();
                        Self::write_client(&mut st, again.as_bytes());
                        continue;
                    }
                    if c != b'$' {
                        st.inbuf.remove(0); // noise, or a stray Ctrl-C
                        continue;
                    }
                    let Some(hash) = st.inbuf.iter().position(|&b| b == b'#') else {
                        break;
                    };
                    if st.inbuf.len() < hash + 3 {
                        break; // incomplete
                    }
                    let payload: Vec<u8> = st.inbuf[1..hash].to_vec();
                    let (hi, lo) = (hex_value(st.inbuf[hash + 1]), hex_value(st.inbuf[hash + 2]));
                    st.inbuf.drain(..hash + 3);
                    let sum = payload.iter().fold(0u32, |a, &b| a + b as u32);
                    match (hi, lo) {
                        (Some(h), Some(l)) if ((h << 4) | l) as u32 == sum & 0xff => {
                            Self::write_client(&mut st, b"+");
                            return Some(String::from_utf8_lossy(&payload).into_owned());
                        }
                        _ => {
                            Self::write_client(&mut st, b"-");
                            continue;
                        }
                    }
                }

                let mut buf = [0u8; 4096];
                let Some(c) = st.client.as_mut() else {
                    return None;
                };
                match c.read(&mut buf) {
                    Ok(0) => return None, // closed
                    Ok(n) => {
                        st.inbuf.extend_from_slice(&buf[..n]);
                        continue;
                    }
                    Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::Interrupted) => {}
                    Err(_) => return None,
                }
            }
            if !block {
                return None;
            }
            std::thread::sleep(Duration::from_millis(1));
        }
    }

    /// Serve the debugger until it detaches (returns true: the guest should run on) or
    /// the session ends (false).
    pub fn serve(&mut self) -> bool {
        loop {
            let Some(payload) = self.read_packet(true) else {
                self.st.borrow_mut().killed = true;
                return false;
            };
            let mut console = Vec::new();
            let reply = self.handle_packet(&payload, Some(&mut console));
            for line in &console {
                self.send_packet(&format!("O{}", to_hex(line.as_bytes())));
            }
            if self.st.borrow().killed {
                return false;
            }
            if !self.send_packet(&reply) {
                self.st.borrow_mut().killed = true;
                return false;
            }
            if self.st.borrow().detached {
                return true; // let the guest run on
            }
            if reply.starts_with('W') {
                return false; // the machine exited
            }
        }
    }

    // --- helpers --------------------------------------------------------------------------

    /// gdb thread ids are hart id + 1.
    fn hart_for(&self, thread_id: u64) -> Option<usize> {
        (thread_id >= 1 && thread_id <= self.platform.num_harts() as u64).then(|| (thread_id - 1) as usize)
    }

    fn general(&self) -> Option<usize> {
        self.hart_for(self.st.borrow().general_hart as u64 + 1)
    }

    fn read_registers(&mut self) -> String {
        let Some(h) = self.general() else {
            return "E01".into();
        };
        let cpu = &mut self.platform.harts[h];
        // Only the 32 integer registers and the pc, exactly as QEMU's stub does. gdb
        // fetches the floating-point and control registers one at a time (`p`) when it
        // needs them, which keeps this reply small enough to be sent on every stop.
        let mut out = String::new();
        for i in 0..NUM_REGS {
            out += &reg_hex(cpu.read_reg(i), 8);
        }
        out += &reg_hex(cpu.pc, 8);
        out
    }

    fn read_register(&mut self, regnum: u32) -> String {
        let Some(h) = self.general() else {
            return "E01".into();
        };
        let cpu = &mut self.platform.harts[h];
        if (regnum as usize) < NUM_REGS {
            return reg_hex(cpu.read_reg(regnum as usize), 8);
        }
        if regnum == REG_PC {
            return reg_hex(cpu.pc, 8);
        }
        if (REG_FP_BASE..REG_FP_BASE + 32).contains(&regnum) {
            return reg_hex(cpu.fregs[(regnum - REG_FP_BASE) as usize], 8);
        }
        if (REG_CSR_BASE..REG_CSR_BASE + 4096).contains(&regnum) {
            let c = regnum - REG_CSR_BASE;
            if !cpu.csrs.exists(c) {
                return "E02".into();
            }
            return reg_hex(cpu.csrs.read(c), if csr_is_narrow(c) { 4 } else { 8 });
        }
        "E02".into()
    }

    fn write_register(&mut self, regnum: u32, hexvalue: &str) -> String {
        let Some(h) = self.general() else {
            return "E01".into();
        };
        let Some(bytes) = from_hex(hexvalue).filter(|b| !b.is_empty() && b.len() <= 8) else {
            return "E03".into();
        };
        let mut value = 0u64;
        for (i, b) in bytes.iter().enumerate() {
            value |= (*b as u64) << (8 * i);
        }
        let cpu = &mut self.platform.harts[h];
        if (regnum as usize) < NUM_REGS {
            cpu.write_reg(regnum as usize, value);
            return "OK".into();
        }
        if regnum == REG_PC {
            cpu.pc = value;
            return "OK".into();
        }
        if (REG_FP_BASE..REG_FP_BASE + 32).contains(&regnum) {
            cpu.fregs[(regnum - REG_FP_BASE) as usize] = value;
            return "OK".into();
        }
        if (REG_CSR_BASE..REG_CSR_BASE + 4096).contains(&regnum) {
            let c = regnum - REG_CSR_BASE;
            if !cpu.csrs.exists(c) {
                return "E02".into();
            }
            cpu.csrs.write(c, value);
            return "OK".into();
        }
        "E02".into()
    }

    fn read_memory(&mut self, addr: u64, len: u64) -> String {
        let Some(h) = self.general() else {
            return "E01".into();
        };
        match read_bytes(&mut self.platform.harts[h], addr, len.min(0x10000)) {
            Some(d) => to_hex(&d),
            None => "E14".into(),
        }
    }

    fn write_memory(&mut self, addr: u64, hexdata: &str) -> String {
        let Some(h) = self.general() else {
            return "E01".into();
        };
        let Some(data) = from_hex(hexdata) else {
            return "E03".into();
        };
        if write_bytes(&mut self.platform.harts[h], addr, &data) {
            "OK".into()
        } else {
            "E14".into()
        }
    }

    // --- breakpoints and watchpoints ------------------------------------------------------

    fn breakpoint_command(&mut self, insert: bool, args: &str) -> String {
        // "type,addr,kind" - kind is the instruction size for a breakpoint, the byte
        // length for a watchpoint.
        let f: Vec<&str> = args.split(',').collect();
        if f.len() < 3 || f[0].len() != 1 {
            return "E01".into();
        }
        let (Some(addr), Some(kind)) = (parse_u64(f[1]), parse_u64(f[2].split(';').next().unwrap_or(""))) else {
            return "E01".into();
        };
        match f[0] {
            // software breakpoint; hardware breakpoint - to an emulator, the same thing
            "0" | "1" => {
                let mut st = self.st.borrow_mut();
                if insert {
                    st.breakpoints.insert(addr);
                } else {
                    st.breakpoints.remove(&addr);
                }
                "OK".into()
            }
            "2" => {
                // write watchpoint
                if kind == 0 || kind > 64 {
                    return "E01".into();
                }
                if insert {
                    let Some(h) = self.general() else {
                        return "E14".into();
                    };
                    let Some(last) = read_bytes(&mut self.platform.harts[h], addr, kind) else {
                        return "E14".into();
                    };
                    self.st.borrow_mut().watches.push(Watch { addr, len: kind, last });
                } else {
                    self.st.borrow_mut().watches.retain(|w| !(w.addr == addr && w.len == kind));
                }
                "OK".into()
            }
            // read and access watchpoints would need a hook on every load
            _ => String::new(),
        }
    }

    fn refresh_watches(&mut self) {
        let Some(h) = self.general() else {
            return;
        };
        let mut st = self.st.borrow_mut();
        for w in st.watches.iter_mut() {
            if let Some(d) = read_bytes(&mut self.platform.harts[h], w.addr, w.len) {
                w.last = d;
            }
        }
    }

    // --- running ------------------------------------------------------------------------------

    fn classify_trap(&mut self, trap: &Trap) {
        let mut st = self.st.borrow_mut();
        st.stop = Stop::Trap;
        st.signal = signal_for(trap.cause);
    }

    /// An EBREAK the program put there itself (the end of a test, say) is a trap the
    /// emulator reports as a stop with nowhere to go. Resuming from it should move on
    /// rather than hit the same instruction forever.
    fn skip_ebreak_if_stopped_on_one(&mut self, hart: usize) {
        if self.st.borrow().stop != Stop::Trap {
            return;
        }
        let cpu = &mut self.platform.harts[hart];
        let pc = cpu.pc;
        let Some(insn) = read_bytes(cpu, pc, 4) else {
            return;
        };
        let word = u32::from_le_bytes([insn[0], insn[1], insn[2], insn[3]]);
        if word == 0x0010_0073 {
            cpu.pc += 4; // ebreak
        } else if word & 0xffff == 0x9002 {
            cpu.pc += 2; // c.ebreak
        }
    }

    fn run_until_stop(&mut self, only_hart: Option<u32>, single_step: bool) -> bool {
        if self.platform.halted() {
            self.st.borrow_mut().stop = Stop::Halted;
            return false;
        }

        if single_step {
            let h = only_hart.unwrap_or(self.st.borrow().general_hart);
            self.skip_ebreak_if_stopped_on_one(h as usize);
            let r = self.platform.step_hart(h);
            self.st.borrow_mut().stop_hart = h;
            match r {
                Err(t) => {
                    self.classify_trap(&t);
                    self.st.borrow_mut().stop_hart = self.platform.fault_hart();
                }
                Ok(()) if self.platform.halted() => self.st.borrow_mut().stop = Stop::Halted,
                Ok(()) => {
                    let mut st = self.st.borrow_mut();
                    st.stop = Stop::Step;
                    st.signal = 5;
                }
            }
            return true;
        }

        for h in 0..self.platform.harts.len() {
            self.skip_ebreak_if_stopped_on_one(h);
        }
        self.refresh_watches();

        // A hart sitting on a breakpoint must be allowed to leave it: the check runs
        // before the instruction, so without this the first step would stop at once.
        let mut leaving: BTreeSet<u32> = BTreeSet::new();
        {
            let st = self.st.borrow();
            for (h, cpu) in self.platform.harts.iter().enumerate() {
                if st.breakpoints.contains(&cpu.pc) {
                    leaving.insert(h as u32);
                }
            }
        }

        let st_rc = self.st.clone();
        let interval = self.interrupt_poll_interval.max(1);
        let mut calls = 0u64;
        self.platform.before_step = Some(Box::new(move |c: &mut Cpu| -> bool {
            let mut st = st_rc.borrow_mut();
            calls += 1;
            if calls % interval == 0 {
                let stop = match st.interrupt_requested.as_mut() {
                    Some(f) => f(),
                    None => st.poll_interrupt_byte(),
                };
                if stop {
                    st.stop = Stop::Interrupt;
                    st.stop_hart = c.hartid;
                    st.signal = 2;
                    return true;
                }
            }
            let mut hit = None;
            for w in st.watches.iter_mut() {
                if let Some(now) = read_bytes(c, w.addr, w.len) {
                    if now != w.last {
                        w.last = now;
                        hit = Some(w.addr);
                        break;
                    }
                }
            }
            if let Some(addr) = hit {
                st.stop = Stop::Watch;
                st.stop_hart = c.hartid;
                st.stop_addr = addr;
                st.signal = 5;
                return true;
            }
            if !leaving.remove(&c.hartid) && st.breakpoints.contains(&c.pc) {
                st.stop = Stop::Breakpoint;
                st.stop_hart = c.hartid;
                st.signal = 5;
                return true;
            }
            false
        }));

        let budget = self.platform.retired() + (1u64 << 60);
        let r = self.platform.run(budget);
        self.platform.before_step = None;

        if self.platform.hook_stopped() {
            return true;
        }
        if let Err(t) = r {
            self.classify_trap(&t);
            self.st.borrow_mut().stop_hart = self.platform.fault_hart();
            return true;
        }
        if self.platform.halted() {
            self.st.borrow_mut().stop = Stop::Halted;
            return false;
        }
        // ran out of budget: treat as a stop
        let mut st = self.st.borrow_mut();
        st.stop = Stop::Interrupt;
        st.signal = 2;
        true
    }

    fn resume(&mut self, step: bool, hart: Option<u32>) -> String {
        self.run_until_stop(hart, step);
        self.stop_reply()
    }

    fn vcont(&mut self, args: &str) -> String {
        // vCont;a1[:tid];a2[:tid]... Each action is c, s (optionally with a signal
        // number we ignore) and applies to the named thread, or to the rest.
        let mut step_hart = None;
        let mut step = false;
        for action in args.split(';') {
            let Some(op) = action.chars().next() else {
                continue;
            };
            if op != 's' && op != 'S' {
                continue;
            }
            step = true;
            if let Some((_, t)) = action.split_once(':') {
                let t = if t.starts_with('p') { t.split_once('.').map_or(t, |(_, x)| x) } else { t };
                if let Some(tid) = parse_u64(t).filter(|&t| t >= 1) {
                    step_hart = Some((tid - 1) as u32);
                }
            }
        }
        self.resume(step, step_hart)
    }

    pub fn stop_reply(&self) -> String {
        let st = self.st.borrow();
        match st.stop {
            Stop::Halted => "W00".into(),
            Stop::Watch => format!("T05watch:{:x};thread:{:x};", st.stop_addr, st.stop_hart + 1),
            Stop::None => format!("T05thread:{:x};", st.general_hart + 1),
            _ => format!("T{:02x}thread:{:x};", st.signal, st.stop_hart + 1),
        }
    }

    // --- monitor commands -----------------------------------------------------------------

    fn monitor(&mut self, command: &str, console: Option<&mut Vec<String>>) -> String {
        let mut lines = Vec::new();
        let mut words = command.split_whitespace();
        let verb = words.next().unwrap_or("");

        match verb {
            "" | "help" => {
                lines.push("monitor harts            list the harts and what each is doing".to_string());
                lines.push("monitor csr NAME|NUMBER  print a control register of the selected hart".to_string());
                lines.push("monitor time             print the machine clock".to_string());
            }
            "harts" => {
                let selected = self.st.borrow().general_hart;
                for (h, c) in self.platform.harts.iter().enumerate() {
                    lines.push(format!(
                        "hart {}: pc={:#x} priv={}{}{}{}",
                        h,
                        c.pc,
                        privilege_name(c.priv_),
                        if c.parked() { " (parked)" } else { "" },
                        if c.waiting_for_interrupt { " (wfi)" } else { "" },
                        if h as u32 == selected { "  <- selected" } else { "" },
                    ));
                }
            }
            "csr" => {
                let which = words.next().unwrap_or("");
                let number = CSR_NAMES.iter().find(|(n, _)| *n == which).map(|&(_, n)| n).or_else(|| {
                    let parsed = match which.strip_prefix("0x") {
                        Some(h) => u32::from_str_radix(h, 16).ok(),
                        None => which.parse::<u32>().ok(),
                    };
                    parsed.filter(|&n| n < 4096)
                });
                let cpu = self.general().map(|h| &self.platform.harts[h]);
                match (number, cpu) {
                    (Some(n), Some(c)) if c.csrs.exists(n) => {
                        lines.push(format!("{which} ({n:#x}) = {:#x}", c.csrs.read(n)));
                    }
                    _ => lines.push(format!("no such control register: {which}")),
                }
            }
            "time" => lines.push(format!("{}", self.platform.clock.mtime())),
            _ => lines.push(format!("unknown monitor command '{verb}' (try: monitor help)")),
        }
        if let Some(c) = console {
            c.extend(lines.into_iter().map(|l| l + "\n"));
        }
        "OK".into()
    }

    // --- the target description -----------------------------------------------------------------

    fn target_xml(&self) -> String {
        let mut x = String::from(
            "<?xml version=\"1.0\"?>\n<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n<target version=\"1.0\">\n  \
             <architecture>riscv:rv64</architecture>\n  <feature name=\"org.gnu.gdb.riscv.cpu\">\n",
        );
        for i in 0..32 {
            let ty = match i {
                1 => "code_ptr",
                2 | 3 | 4 | 8 => "data_ptr",
                _ => "int",
            };
            x += &format!("    <reg name=\"{}\" bitsize=\"64\" type=\"{ty}\" regnum=\"{i}\"/>\n", GPR_NAMES[i]);
        }
        x += "    <reg name=\"pc\" bitsize=\"64\" type=\"code_ptr\" regnum=\"32\"/>\n  </feature>\n";

        x += "  <feature name=\"org.gnu.gdb.riscv.fpu\">\n";
        for i in 0..32u32 {
            x += &format!(
                "    <reg name=\"{}\" bitsize=\"64\" type=\"ieee_double\" regnum=\"{}\"/>\n",
                FP_NAMES[i as usize],
                REG_FP_BASE + i
            );
        }
        x += "    <reg name=\"fflags\" bitsize=\"32\" type=\"int\" regnum=\"66\"/>\n\
              \x20   <reg name=\"frm\" bitsize=\"32\" type=\"int\" regnum=\"67\"/>\n\
              \x20   <reg name=\"fcsr\" bitsize=\"32\" type=\"int\" regnum=\"68\"/>\n  </feature>\n";

        x += "  <feature name=\"org.gnu.gdb.riscv.csr\">\n";
        for &(name, number) in CSR_NAMES {
            if csr_is_narrow(number) {
                continue; // declared with the fpu above
            }
            x += &format!("    <reg name=\"{name}\" bitsize=\"64\" type=\"int\" regnum=\"{}\"/>\n", REG_CSR_BASE + number);
        }
        x += "  </feature>\n</target>\n";
        x
    }

    // --- the dispatcher -------------------------------------------------------------------------

    /// Answer one packet. (Public so the protocol can be tested without a socket.)
    pub fn handle_packet(&mut self, p: &str, console: Option<&mut Vec<String>>) -> String {
        let Some(cmd) = p.chars().next() else {
            return String::new();
        };
        let rest = &p[cmd.len_utf8()..];

        match cmd {
            '?' => self.stop_reply(),

            'g' => self.read_registers(),
            'G' => {
                let Some(h) = self.general() else {
                    return "E03".into();
                };
                let Some(bytes) = from_hex(rest).filter(|b| b.len() >= 33 * 8) else {
                    return "E03".into();
                };
                let cpu = &mut self.platform.harts[h];
                for i in 0..33 {
                    let v = u64::from_le_bytes(bytes[i * 8..i * 8 + 8].try_into().unwrap());
                    if i < 32 {
                        cpu.write_reg(i, v);
                    } else {
                        cpu.pc = v;
                    }
                }
                "OK".into()
            }
            'p' => match parse_u64(rest) {
                Some(n) => self.read_register(n as u32),
                None => "E01".into(),
            },
            'P' => {
                let Some((n, v)) = rest.split_once('=') else {
                    return "E01".into();
                };
                match parse_u64(n) {
                    Some(n) => self.write_register(n as u32, v),
                    None => "E01".into(),
                }
            }

            'm' => {
                let f: Vec<&str> = rest.split(',').collect();
                match (f.len(), f.first().and_then(|a| parse_u64(a)), f.get(1).and_then(|l| parse_u64(l))) {
                    (2, Some(addr), Some(len)) => self.read_memory(addr, len),
                    _ => "E01".into(),
                }
            }
            'M' => {
                let Some((head, data)) = rest.split_once(':') else {
                    return "E01".into();
                };
                let f: Vec<&str> = head.split(',').collect();
                match (f.len(), f.first().and_then(|a| parse_u64(a)), f.get(1).and_then(|l| parse_u64(l))) {
                    (2, Some(addr), Some(_)) => self.write_memory(addr, data),
                    _ => "E01".into(),
                }
            }

            'Z' => self.breakpoint_command(true, rest),
            'z' => self.breakpoint_command(false, rest),

            'c' | 's' => {
                if let Some(addr) = parse_u64(rest) {
                    let g = self.st.borrow().general_hart as usize;
                    self.platform.harts[g].pc = addr;
                }
                self.resume(cmd == 's', None)
            }

            'D' => {
                self.st.borrow_mut().detached = true;
                "OK".into()
            }
            'k' => {
                self.st.borrow_mut().killed = true;
                String::new()
            }

            'H' => {
                // Hg<tid> picks the thread for register and memory access; Hc<tid> for
                // execution, which this all-stop stub does not distinguish.
                let Some(op) = rest.chars().next() else {
                    return "E01".into();
                };
                if op == 'g' {
                    let t = &rest[1..];
                    if t == "-1" || t == "0" {
                        return "OK".into();
                    }
                    match parse_u64(t).and_then(|tid| self.hart_for(tid)) {
                        Some(h) => self.st.borrow_mut().general_hart = h as u32,
                        None => return "E01".into(),
                    }
                }
                "OK".into()
            }
            'T' => {
                if parse_u64(rest).and_then(|t| self.hart_for(t)).is_some() {
                    "OK".into()
                } else {
                    "E01".into()
                }
            }

            'v' => {
                if p == "vCont?" {
                    "vCont;c;C;s;S".into()
                } else if let Some(a) = p.strip_prefix("vCont;") {
                    self.vcont(a)
                } else {
                    String::new() // vMustReplyEmpty and anything unknown
                }
            }

            'q' => self.handle_query(p, console),

            _ => String::new(), // unsupported: an empty reply says so
        }
    }

    fn handle_query(&mut self, p: &str, console: Option<&mut Vec<String>>) -> String {
        if p.starts_with("qSupported") {
            return "PacketSize=8000;qXfer:features:read+;swbreak+;hwbreak+;vContSupported+".into();
        }
        match p {
            "qAttached" => return "1".into(),
            "qC" => return format!("QC{:x}", self.st.borrow().general_hart + 1),
            "qfThreadInfo" => {
                let ids: Vec<String> = (0..self.platform.num_harts()).map(|h| format!("{:x}", h + 1)).collect();
                return format!("m{}", ids.join(","));
            }
            "qsThreadInfo" => return "l".into(),
            _ => {}
        }
        if p.starts_with("qThreadExtraInfo") {
            let tid = p.split_once(',').and_then(|(_, t)| parse_u64(t)).unwrap_or(0);
            let Some(h) = self.hart_for(tid) else {
                return "E01".into();
            };
            let c = &self.platform.harts[h];
            let text = format!(
                "hart {} ({}){}",
                c.hartid,
                privilege_name(c.priv_),
                if c.parked() { ", parked" } else { "" }
            );
            return to_hex(text.as_bytes());
        }
        if let Some(hex) = p.strip_prefix("qRcmd,") {
            let Some(bytes) = from_hex(hex) else {
                return "E01".into();
            };
            return self.monitor(&String::from_utf8_lossy(&bytes), console);
        }
        if p.starts_with("qXfer:features:read:target.xml") {
            // qXfer:features:read:target.xml:OFFSET,LENGTH
            let tail = p.rsplit(':').next().unwrap_or("");
            let ol: Vec<&str> = tail.split(',').collect();
            let (Some(off), Some(len)) =
                (ol.first().and_then(|a| parse_u64(a)), ol.get(1).and_then(|l| parse_u64(l)))
            else {
                return "E01".into();
            };
            let xml = self.target_xml();
            if off >= xml.len() as u64 {
                return "l".into();
            }
            let end = (off.saturating_add(len)).min(xml.len() as u64);
            let chunk = &xml[off as usize..end as usize];
            return format!("{}{}", if off + len >= xml.len() as u64 { "l" } else { "m" }, chunk);
        }
        String::new() // qOffsets, qSymbol, qTStatus, ...
    }
}
