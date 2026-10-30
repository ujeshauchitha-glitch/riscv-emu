//! NS16550A UART - the console.
//!
//! This is the first device that makes the emulator observable from the outside. A
//! byte written to address 0x1000_0000 appears on the host's stdout; that is the
//! whole mechanism behind a kernel's boot messages.
//!
//! The NS16550A is a 1980s serial chip, and it is what QEMU's `virt` machine exposes,
//! so xv6 and Linux already have drivers for it. Eight byte-wide registers:
//!
//!   0  RBR/THR   receive buffer (read) / transmit holding (write)
//!   1  IER       interrupt enable
//!   2  IIR/FCR   interrupt identification (read) / FIFO control (write)
//!   3  LCR       line control - bit 7 is DLAB
//!   4  MCR       modem control
//!   5  LSR       line status - bit 0 "data ready", bit 5 "transmit ready"
//!   6  MSR       modem status
//!   7  SCR       scratch
//!
//! The DLAB bit in LCR is a bank-switch: when set, registers 0 and 1 become the low
//! and high halves of the baud-rate divisor instead. A driver sets DLAB, writes the
//! divisor, then clears it; a UART that ignored DLAB would take the divisor bytes as
//! characters to print.
//!
//! The host console is attached as the receive line. Reading stdin blocks, so a
//! background thread does it and hands bytes over a channel; the machine drains the
//! channel from its poll loop. Raw mode (no line buffering, no echo, Ctrl-C going to
//! the guest) is arranged per platform and undone on drop - it is a property of the
//! terminal, not the process, and outlives us otherwise.

use std::collections::VecDeque;
use std::io::{Read, Write};
use std::sync::mpsc::{channel, Receiver, TryRecvError};

use crate::device::Device;
use crate::trap::{Exception, Res, Trap};
use crate::types::UART0_BASE;

// Line Status Register bits.
const LSR_DATA_READY: u8 = 1 << 0;
const LSR_THR_EMPTY: u8 = 1 << 5;
const LSR_TRANSMIT_IDLE: u8 = 1 << 6;

// Interrupt Enable Register bits.
const IER_RX_READY: u8 = 1 << 0;
const IER_TX_EMPTY: u8 = 1 << 1;

const CTRL_A: u8 = 0x01;

/// How many fruitless LSR reads in a row count as "the guest is waiting for input".
const EMPTY_POLLS_BEFORE_YIELD: u32 = 64;

pub struct Uart {
    out: Box<dyn Write>,
    rx: VecDeque<u8>,
    ier: u8,
    lcr: u8,
    mcr: u8,
    scr: u8,
    dll: u8,
    dlm: u8,
    tx_irq: bool,
    /// Consecutive status-register reads that found nothing to receive (a guest busy-polling).
    empty_polls: u32,

    host_rx: Option<Receiver<Vec<u8>>>,
    host_eof: bool,
    escape_armed: bool,
    exit_requested: bool,
    console: Option<host::Console>,
}

impl Uart {
    /// Output goes to `out`; tests point this at a shared buffer.
    pub fn new(out: Box<dyn Write>) -> Uart {
        Uart {
            out,
            rx: VecDeque::new(),
            ier: 0,
            lcr: 0,
            mcr: 0,
            scr: 0,
            dll: 0,
            dlm: 0,
            tx_irq: false,
            empty_polls: 0,
            host_rx: None,
            host_eof: false,
            escape_armed: false,
            exit_requested: false,
            console: None,
        }
    }

    /// Queue bytes as though they had arrived on the serial line. How a test types at
    /// the guest.
    pub fn feed_input(&mut self, s: &[u8]) {
        self.rx.extend(s.iter().copied());
    }

    /// Make the host's stdin the receive line, putting the terminal into raw mode.
    ///
    /// When stdin is a pipe or file there is nothing to put into raw mode; reading
    /// still works, which is what lets `printf 'ls\n' | riscv_emu ...` drive the
    /// guest from a script.
    pub fn attach_host_stdin(&mut self) {
        if self.host_rx.is_some() {
            return;
        }
        self.console = host::Console::enter();
        let (tx, rx) = channel::<Vec<u8>>();
        std::thread::spawn(move || {
            let mut stdin = std::io::stdin();
            let mut buf = [0u8; 64];
            loop {
                match stdin.read(&mut buf) {
                    Ok(0) | Err(_) => break, // end of input: dropping tx says so
                    Ok(n) => {
                        if tx.send(buf[..n].to_vec()).is_err() {
                            break;
                        }
                    }
                }
            }
        });
        self.host_rx = Some(rx);
    }

    pub fn host_input_active(&self) -> bool {
        self.host_rx.is_some() && !self.host_eof
    }

    /// Move whatever the host has typed into the receive queue. Ctrl-A is an escape
    /// prefix: Ctrl-A X asks the emulator to exit (the guest cannot be relied on to
    /// let go of a raw terminal), Ctrl-A Ctrl-A sends one literal Ctrl-A.
    pub fn poll_host_input(&mut self) {
        if self.host_eof {
            return;
        }
        let Some(rx) = &self.host_rx else {
            return;
        };
        loop {
            match rx.try_recv() {
                Ok(chunk) => {
                    for c in chunk {
                        if self.escape_armed {
                            self.escape_armed = false;
                            if c == b'x' || c == b'X' {
                                self.exit_requested = true;
                                return;
                            }
                            if c == CTRL_A {
                                self.rx.push_back(CTRL_A);
                                continue;
                            }
                            // Not a sequence we know. Give the guest both bytes rather
                            // than swallowing the prefix.
                            self.rx.push_back(CTRL_A);
                            self.rx.push_back(c);
                            continue;
                        }
                        if c == CTRL_A {
                            self.escape_armed = true;
                            continue;
                        }
                        self.rx.push_back(c);
                    }
                }
                Err(TryRecvError::Empty) => return,
                Err(TryRecvError::Disconnected) => {
                    // End of input. Stop polling.
                    self.host_eof = true;
                    return;
                }
            }
        }
    }

    /// True once the guest has polled the line-status register many times in a row with nothing to read.
    /// A guest spinning on the UART would otherwise pin a host core at full speed waiting for a key;
    /// the machine uses this to sleep briefly while it waits for real input. Resets the count.
    pub fn take_yield_request(&mut self) -> bool {
        if self.empty_polls >= EMPTY_POLLS_BEFORE_YIELD {
            self.empty_polls = 0;
            return true;
        }
        false
    }

    pub fn exit_requested(&self) -> bool {
        self.exit_requested
    }

    /// Take the next received byte for the SBI legacy console, or -1 if none.
    pub fn take_input_byte(&mut self) -> i32 {
        self.rx.pop_front().map_or(-1, |c| c as i32)
    }

    /// Is the receive/transmit interrupt condition asserted?
    pub fn interrupting(&self) -> bool {
        (self.ier & IER_RX_READY != 0 && !self.rx.is_empty()) || (self.ier & IER_TX_EMPTY != 0 && self.tx_irq)
    }

    fn dlab(&self) -> bool {
        self.lcr & 0x80 != 0
    }

    /// Write a byte straight to the console (used by SBI's legacy putchar).
    pub fn put_byte(&mut self, b: u8) {
        let _ = self.out.write_all(&[b]);
        let _ = self.out.flush();
    }
}

impl Device for Uart {
    fn base(&self) -> u64 {
        UART0_BASE
    }
    fn size(&self) -> u64 {
        0x100
    }
    fn name(&self) -> &'static str {
        "uart0"
    }

    fn load(&mut self, offset: u64, size: usize) -> Res<u64> {
        // Every UART register is one byte wide. A driver reads them with lb.
        if size != 1 {
            return Err(Trap::new(Exception::LoadAccessFault, UART0_BASE + offset));
        }
        Ok(match offset & 0x7 {
            0 => {
                if self.dlab() {
                    self.dll as u64
                } else {
                    // RBR: take the next queued byte, or zero if nothing is waiting.
                    self.empty_polls = 0;
                    self.rx.pop_front().unwrap_or(0) as u64
                }
            }
            1 => (if self.dlab() { self.dlm } else { self.ier }) as u64,
            2 => {
                // IIR - "why did you interrupt me?" Bit 0 clear means an interrupt is
                // pending (the sense is inverted), and bits 3:1 say which condition
                // caused it; a real driver dispatches on that field rather than going
                // looking:
                //
                //   0b001  transmitter holding register empty
                //   0b010  received data available
                //
                // Linux's 8250 driver reads this. Returning a bare "something
                // happened" gave it the modem-status ID, it found nothing to do, and
                // the console printed perfectly and could not be typed at. (xv6 never
                // reads IIR, which is why it worked with this wrong.)
                //
                // Received data outranks a free transmitter: an unread byte is lost
                // if the next arrives; an idle transmitter will still be idle later.
                let cause = if self.ier & IER_RX_READY != 0 && !self.rx.is_empty() {
                    0x04
                } else if self.ier & IER_TX_EMPTY != 0 && self.tx_irq {
                    // Reading IIR acknowledges *this* interrupt, and only when it is
                    // the one being reported - which is what makes THRE an edge
                    // rather than a level that never falls.
                    self.tx_irq = false;
                    0x02
                } else {
                    0x01 // bit 0 set: nothing pending
                };
                // Bits 7:6 advertise working FIFOs: a 16550A, not an earlier part.
                (0xc0 | cause) as u64
            }
            3 => self.lcr as u64,
            4 => self.mcr as u64,
            5 => {
                // The transmitter is always ready: a write to THR completes at once.
                let mut lsr = LSR_THR_EMPTY | LSR_TRANSMIT_IDLE;
                if !self.rx.is_empty() {
                    lsr |= LSR_DATA_READY;
                    self.empty_polls = 0;
                } else {
                    self.empty_polls = self.empty_polls.saturating_add(1);
                }
                lsr as u64
            }
            6 => 0, // modem status: nothing connected
            _ => self.scr as u64,
        })
    }

    fn store(&mut self, offset: u64, size: usize, value: u64) -> Res<()> {
        if size != 1 {
            return Err(Trap::new(Exception::StoreAMOAccessFault, UART0_BASE + offset));
        }
        let v = value as u8;
        match offset & 0x7 {
            0 => {
                if self.dlab() {
                    self.dll = v;
                } else {
                    // THR: this is the line that makes a kernel's printf visible.
                    self.put_byte(v);
                    // The byte is sent instantly, so the holding register becomes
                    // empty again right away - the edge THRE reports.
                    self.tx_irq = true;
                }
            }
            1 => {
                if self.dlab() {
                    self.dlm = v;
                } else {
                    // Enabling the transmit interrupt does not by itself assert it:
                    // that needs a byte to have been sent.
                    self.ier = v;
                }
            }
            3 => self.lcr = v,
            4 => self.mcr = v,
            7 => self.scr = v,
            _ => {} // FCR (no FIFOs to configure), LSR and MSR (read-only)
        }
        Ok(())
    }
}

#[cfg(windows)]
mod host {
    //! Windows console: switch input to raw VT mode so keys (arrows included) arrive
    //! as the escape sequences a Linux terminal would send, and switch output to
    //! understand ANSI and UTF-8 - the guest speaks both.

    type Handle = *mut core::ffi::c_void;

    extern "system" {
        fn GetStdHandle(which: u32) -> Handle;
        fn GetConsoleMode(h: Handle, mode: *mut u32) -> i32;
        fn SetConsoleMode(h: Handle, mode: u32) -> i32;
        fn GetConsoleOutputCP() -> u32;
        fn SetConsoleOutputCP(cp: u32) -> i32;
    }

    const STD_INPUT: u32 = -10i32 as u32;
    const STD_OUTPUT: u32 = -11i32 as u32;
    const ENABLE_PROCESSED_OUTPUT: u32 = 0x0001;
    const ENABLE_VIRTUAL_TERMINAL_PROCESSING: u32 = 0x0004;
    const ENABLE_VIRTUAL_TERMINAL_INPUT: u32 = 0x0200;

    pub struct Console {
        input: Handle,
        saved_in: Option<u32>,
        output: Handle,
        saved_out: Option<(u32, u32)>,
    }

    impl Console {
        pub fn enter() -> Option<Console> {
            // SAFETY: plain Win32 console calls on the process's own standard handles.
            unsafe {
                let input = GetStdHandle(STD_INPUT);
                let output = GetStdHandle(STD_OUTPUT);
                let mut c = Console { input, saved_in: None, output, saved_out: None };
                let mut mode = 0u32;
                if GetConsoleMode(input, &mut mode) != 0 {
                    c.saved_in = Some(mode);
                    // Everything the console does for itself - line editing, echo,
                    // Ctrl-C handling - off, so each keystroke reaches the guest as
                    // struck. (Ctrl-A X is how you leave.)
                    SetConsoleMode(input, ENABLE_VIRTUAL_TERMINAL_INPUT);
                }
                if GetConsoleMode(output, &mut mode) != 0 {
                    c.saved_out = Some((mode, GetConsoleOutputCP()));
                    SetConsoleMode(output, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
                    SetConsoleOutputCP(65001);
                }
                Some(c)
            }
        }
    }

    impl Drop for Console {
        fn drop(&mut self) {
            // SAFETY: restoring modes captured in enter().
            unsafe {
                if let Some(m) = self.saved_in {
                    SetConsoleMode(self.input, m);
                }
                if let Some((m, cp)) = self.saved_out {
                    SetConsoleMode(self.output, m);
                    SetConsoleOutputCP(cp);
                }
            }
        }
    }
}

#[cfg(all(unix, not(windows)))]
mod host {
    //! Unix terminal: raw mode through `stty`, which keeps the crate free of a libc
    //! dependency. Only attempted when stdin really is a terminal; the saved settings
    //! (`stty -g`) are restored on drop.

    use std::io::IsTerminal;
    use std::process::{Command, Stdio};

    pub struct Console {
        saved: String,
    }

    impl Console {
        pub fn enter() -> Option<Console> {
            if !std::io::stdin().is_terminal() {
                return None;
            }
            let out = Command::new("stty").arg("-g").stdin(Stdio::inherit()).output().ok()?;
            if !out.status.success() {
                return None;
            }
            let saved = String::from_utf8_lossy(&out.stdout).trim().to_string();
            let ok = Command::new("stty")
                .args(["raw", "-echo"])
                .stdin(Stdio::inherit())
                .status()
                .map_or(false, |s| s.success());
            ok.then_some(Console { saved })
        }
    }

    impl Drop for Console {
        fn drop(&mut self) {
            let _ = Command::new("stty").arg(&self.saved).stdin(Stdio::inherit()).status();
        }
    }
}

#[cfg(not(any(windows, unix)))]
mod host {
    pub struct Console;
    impl Console {
        pub fn enter() -> Option<Console> {
            None
        }
    }
}
