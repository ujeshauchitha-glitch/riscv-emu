//! A throughput benchmark, not a correctness test: it reports how many guest instructions per second
//! the interpreter retires on a mixed workload.
//!
//! It exists so that speed work can be judged by a number rather than a feeling. Run it before and
//! after a change to the execution loop:
//!
//!   cargo run --release --example bench_cpu                       # default 40M instructions, bare hart
//!   cargo run --release --example bench_cpu -- 200000000
//!   cargo run --release --example bench_cpu -- 40000000 paged     # S-mode under Sv39: every access translates
//!   cargo run --release --example bench_cpu -- 40000000 platform  # through the scheduler (what riscv_emu does)
//!   cargo run --release --example bench_cpu -- 40000000 platform devices paged
//!
//! "paged" and "devices" matter because they are what a real boot looks like: a kernel runs with
//! translation on and the device polling live, and a bare-hart number alone flatters the interpreter.
//!
//! The guest program is a tight loop mixing the instruction classes a kernel spends its time on - ALU
//! ops, a load, a store, a taken branch - so a speed-up that only helps one class does not hide a
//! regression in another.

use std::cell::RefCell;
use std::rc::Rc;
use std::time::Instant;

use riscv_emu::bus::{Bus, Shared};
use riscv_emu::clint::{Clint, Clock};
use riscv_emu::cpu::Cpu;
use riscv_emu::csr::csr;
use riscv_emu::mmu::pte;
use riscv_emu::platform::Platform;
use riscv_emu::plic::Plic;
use riscv_emu::syscon::Syscon;
use riscv_emu::types::*;
use riscv_emu::uart::Uart;

fn i_type(opcode: u32, rd: u32, f3: u32, rs1: u32, imm: i32) -> u32 {
    (((imm as u32) & 0xfff) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | opcode
}
fn r_type(opcode: u32, rd: u32, f3: u32, rs1: u32, rs2: u32, f7: u32) -> u32 {
    (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | opcode
}
fn s_type(opcode: u32, f3: u32, rs1: u32, rs2: u32, imm: i32) -> u32 {
    let i = imm as u32;
    (((i >> 5) & 0x7f) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | ((i & 0x1f) << 7) | opcode
}
fn b_type(opcode: u32, f3: u32, rs1: u32, rs2: u32, imm: i32) -> u32 {
    let i = imm as u32;
    (((i >> 12) & 1) << 31)
        | (((i >> 5) & 0x3f) << 25)
        | (rs2 << 20)
        | (rs1 << 15)
        | (f3 << 12)
        | (((i >> 1) & 0xf) << 8)
        | (((i >> 11) & 1) << 7)
        | opcode
}

fn program() -> Vec<u32> {
    const OP_IMM: u32 = 0x13;
    const OP: u32 = 0x33;
    const LOAD: u32 = 0x03;
    const STORE: u32 = 0x23;
    const BRANCH: u32 = 0x63;
    const JAL: u32 = 0x6f;

    // x5 = scratch address (a few KiB into DRAM, clear of the code), x6 = counter.
    //   loop: addi x6,x6,1 ; add x7,x6,x6 ; xor x8,x7,x6 ; slli x9,x8,3
    //         sd x9,0(x5) ; ld x10,0(x5) ; add x11,x10,x9 ; andi x12,x11,0xff
    //         bne x12,x0,loop        (almost always taken)
    //         j loop
    let mut p = vec![
        (0x80010 << 12) | (5 << 7) | 0x37,       // lui  x5,0x80010 (sign-extended)
        i_type(OP_IMM, 5, 0x1, 5, 32),           // slli x5,x5,32
        i_type(OP_IMM, 5, 0x5, 5, 32),           // srli x5,x5,32
    ];
    let loop_ = p.len();
    p.push(i_type(OP_IMM, 6, 0, 6, 1)); // addi x6,x6,1
    p.push(r_type(OP, 7, 0x0, 6, 6, 0x00)); // add  x7,x6,x6
    p.push(r_type(OP, 8, 0x4, 7, 6, 0x00)); // xor  x8,x7,x6
    p.push(i_type(OP_IMM, 9, 0x1, 8, 3)); // slli x9,x8,3
    p.push(s_type(STORE, 0x3, 5, 9, 0)); // sd   x9,0(x5)
    p.push(i_type(LOAD, 10, 0x3, 5, 0)); // ld   x10,0(x5)
    p.push(r_type(OP, 11, 0x0, 10, 9, 0x00)); // add  x11,x10,x9
    p.push(i_type(OP_IMM, 12, 0x7, 11, 0xff)); // andi x12,x11,255
    let back = -(((p.len() - loop_) * 4) as i32);
    p.push(b_type(BRANCH, 0x1, 12, 0, back)); // bne  x12,x0,loop
    let jump_back = -(((p.len() - loop_) * 4) as i32) as u32;
    // jal x0, back
    let imm = jump_back;
    p.push(
        (((imm >> 20) & 1) << 31) | (((imm >> 1) & 0x3ff) << 21) | (((imm >> 11) & 1) << 20) | (((imm >> 12) & 0xff) << 12) | JAL,
    );
    p
}

fn map_superpage(bus: &Bus) -> u64 {
    // One 1 GiB identity-mapped superpage covering DRAM: VPN[2] = 2 maps 0x8000_0000..0xBFFF_FFFF. The
    // root table sits in DRAM, clear of the code and the scratch word.
    let root = DRAM_BASE + 0x20000;
    let leaf = ((DRAM_BASE >> 12) << 10) | pte::V | pte::R | pte::W | pte::X | pte::A | pte::D;
    bus.ram.store(root - DRAM_BASE + 2 * 8, 8, leaf);
    (8u64 << 60) | (root >> 12)
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let steps: u64 = args.first().and_then(|s| s.parse().ok()).unwrap_or(40_000_000);
    let has = |w: &str| args.iter().skip(1).any(|a| a == w);
    let (paged, devices, use_platform) = (has("paged"), has("devices"), has("platform"));

    let mut bus = Bus::new(1024 * 1024);
    let bytes: Vec<u8> = program().iter().flat_map(|w| w.to_le_bytes()).collect();
    bus.ram.load_image(DRAM_BASE, &bytes);

    let (clint_dev, clint) = Shared::new(Clint::new(1, Rc::new(Clock::new())));
    let (plic_dev, plic) = Shared::new(Plic::new(1));
    let (uart_dev, uart) = Shared::new(Uart::new(Box::new(std::io::stdout())));
    let (syscon_dev, syscon) = Shared::new(Syscon::new());
    if devices {
        assert!(bus.attach(Box::new(clint_dev)));
        assert!(bus.attach(Box::new(plic_dev)));
        assert!(bus.attach(Box::new(uart_dev)));
        assert!(bus.attach(Box::new(syscon_dev)));
    }
    let satp = map_superpage(&bus);
    let bus = Rc::new(bus);

    let label = format!("{}{}{}", if use_platform { " [platform]" } else { "" }, if paged { " [paged]" } else { "" }, if devices { " [devices]" } else { "" });

    let (retired, secs, ok) = if use_platform {
        let mut platform = Platform::new(bus, 1);
        platform.wire(clint, plic, uart, UART0_IRQ, syscon);
        if paged {
            let h = platform.boot_hart();
            h.csrs.write(csr::SATP, satp);
            h.priv_ = PRIV_SUPERVISOR;
            h.trap_fatal_without_handler = false;
        }
        let t0 = Instant::now();
        let r = platform.run(steps);
        (platform.retired(), t0.elapsed().as_secs_f64(), r.is_ok())
    } else {
        let mut cpu = Cpu::new(bus);
        if paged {
            cpu.csrs.write(csr::SATP, satp);
            cpu.priv_ = PRIV_SUPERVISOR;
            cpu.trap_fatal_without_handler = false;
        }
        let _keep: (Rc<RefCell<Clint>>, Rc<RefCell<Plic>>, Rc<RefCell<Uart>>, Rc<RefCell<Syscon>>) = (clint, plic, uart, syscon);
        let t0 = Instant::now();
        let (n, trap) = cpu.run(steps);
        (n, t0.elapsed().as_secs_f64(), trap.is_none())
    };
    println!(
        "{}{label}: {retired} instructions in {secs:.3} s = {:.1} MIPS",
        if ok { "ok" } else { "TRAPPED" },
        retired as f64 / secs / 1e6
    );
    if !ok {
        std::process::exit(1);
    }
}
