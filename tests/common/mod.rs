//! Shared test scaffolding: a small machine, plus instruction encoders.
//!
//! The encoders are written independently of `src/decoder.rs` on purpose. A test that
//! used the emulator's own decoder to build its inputs would agree with itself no
//! matter how wrong both were; two independent implementations that agree is real
//! evidence.

#![allow(dead_code)]

use std::cell::RefCell;
use std::io::Write;
use std::rc::Rc;

use riscv_emu::bus::Bus;
use riscv_emu::cpu::Cpu;
use riscv_emu::decoder::opcodes;
use riscv_emu::trap::Trap;
use riscv_emu::types::DRAM_BASE;

pub const TEST_DRAM_SIZE: u64 = 1024 * 1024;

pub struct Machine {
    pub bus: Rc<Bus>,
    pub cpu: Cpu,
}

impl Machine {
    pub fn new(program: &[u32]) -> Machine {
        let bus = Rc::new(Bus::new(TEST_DRAM_SIZE));
        let bytes: Vec<u8> = program.iter().flat_map(|w| w.to_le_bytes()).collect();
        assert!(bus.ram.load_image(DRAM_BASE, &bytes));
        let cpu = Cpu::new(bus.clone());
        Machine { bus, cpu }
    }

    /// Run up to `steps` instructions; the trap that stopped it, if any.
    pub fn run(&mut self, steps: u64) -> Option<Trap> {
        self.cpu.run(steps).1
    }

    pub fn reg(&mut self, n: usize) -> u64 {
        self.cpu.read_reg(n)
    }
}

/// Build a machine, run it, and hand it back.
pub fn run(prog: &[u32], steps: u64) -> Machine {
    let mut m = Machine::new(prog);
    m.run(steps);
    m
}

// --- instruction encoders ---------------------------------------------------

pub fn r_type(opcode: u32, rd: u32, funct3: u32, rs1: u32, rs2: u32, funct7: u32) -> u32 {
    ((funct7 & 0x7f) << 25)
        | ((rs2 & 0x1f) << 20)
        | ((rs1 & 0x1f) << 15)
        | ((funct3 & 0x7) << 12)
        | ((rd & 0x1f) << 7)
        | (opcode & 0x7f)
}

pub fn i_type(opcode: u32, rd: u32, funct3: u32, rs1: u32, imm: i32) -> u32 {
    (((imm as u32) & 0xfff) << 20)
        | ((rs1 & 0x1f) << 15)
        | ((funct3 & 0x7) << 12)
        | ((rd & 0x1f) << 7)
        | (opcode & 0x7f)
}

pub fn s_type(opcode: u32, funct3: u32, rs1: u32, rs2: u32, imm: i32) -> u32 {
    let i = imm as u32;
    (((i >> 5) & 0x7f) << 25)
        | ((rs2 & 0x1f) << 20)
        | ((rs1 & 0x1f) << 15)
        | ((funct3 & 0x7) << 12)
        | ((i & 0x1f) << 7)
        | (opcode & 0x7f)
}

pub fn b_type(opcode: u32, funct3: u32, rs1: u32, rs2: u32, imm: i32) -> u32 {
    let i = imm as u32;
    (((i >> 12) & 0x1) << 31)
        | (((i >> 5) & 0x3f) << 25)
        | ((rs2 & 0x1f) << 20)
        | ((rs1 & 0x1f) << 15)
        | ((funct3 & 0x7) << 12)
        | (((i >> 1) & 0xf) << 8)
        | (((i >> 11) & 0x1) << 7)
        | (opcode & 0x7f)
}

pub fn u_type(opcode: u32, rd: u32, imm_hi20: u32) -> u32 {
    ((imm_hi20 & 0xfffff) << 12) | ((rd & 0x1f) << 7) | (opcode & 0x7f)
}

pub fn j_type(opcode: u32, rd: u32, imm: i32) -> u32 {
    let i = imm as u32;
    (((i >> 20) & 0x1) << 31)
        | (((i >> 1) & 0x3ff) << 21)
        | (((i >> 11) & 0x1) << 20)
        | (((i >> 12) & 0xff) << 12)
        | ((rd & 0x1f) << 7)
        | (opcode & 0x7f)
}

// --- convenience mnemonics --------------------------------------------------

pub fn addi(rd: u32, rs1: u32, imm: i32) -> u32 {
    i_type(opcodes::OP_IMM, rd, 0x0, rs1, imm)
}
pub fn lui(rd: u32, imm20: u32) -> u32 {
    u_type(opcodes::LUI, rd, imm20)
}
pub fn auipc(rd: u32, imm20: u32) -> u32 {
    u_type(opcodes::AUIPC, rd, imm20)
}
pub fn jal(rd: u32, imm: i32) -> u32 {
    j_type(opcodes::JAL, rd, imm)
}
pub fn jalr(rd: u32, rs1: u32, imm: i32) -> u32 {
    i_type(opcodes::JALR, rd, 0x0, rs1, imm)
}
pub fn nop() -> u32 {
    addi(0, 0, 0)
}

/// End a test program without trapping: jump to self.
///
/// EBREAK is the obvious way to stop, but it only halts the machine while no trap
/// handler is installed. Once mtvec points somewhere, EBREAK vectors into the handler
/// like any other trap - and if the handler itself ends in EBREAK it re-enters
/// forever. A self-loop stops making progress without disturbing any architectural
/// state, so the test runs a fixed budget and then inspects the machine.
pub fn halt() -> u32 {
    j_type(opcodes::JAL, 0, 0)
}

/// Load a full 64-bit constant into rd, for setting up test inputs. Built from a
/// shift-and-or chain of OP-IMM instructions, which the first tests verify, so setup
/// never depends on the instruction a given test is trying to check.
pub fn load_imm64(out: &mut Vec<u32>, rd: u32, value: u64) {
    out.push(addi(rd, 0, 0)); // rd = 0
    for shift in (0..=56).rev().step_by(8) {
        let byte = ((value >> shift) & 0xff) as u32;
        out.push(i_type(opcodes::OP_IMM, rd, 0x1, rd, 8)); // rd <<= 8
        // rd |= byte. 0..255 fits the positive half of a 12-bit immediate, so no
        // sign-extension surprises.
        out.push(i_type(opcodes::OP_IMM, rd, 0x6, rd, byte as i32));
    }
}

pub const LOAD_IMM64_STEPS: u64 = 1 + 8 * 2;

/// A small deterministic generator (splitmix64) so randomised tests are reproducible without a
/// dependency.
pub struct Rng(pub u64);

impl Rng {
    pub fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9e37_79b9_7f4a_7c15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        z ^ (z >> 31)
    }
}

/// A clonable in-memory writer, so a test can read back what a device printed.
#[derive(Clone, Default)]
pub struct SharedBuf(pub Rc<RefCell<Vec<u8>>>);

impl SharedBuf {
    pub fn new() -> SharedBuf {
        SharedBuf::default()
    }
    pub fn text(&self) -> String {
        String::from_utf8_lossy(&self.0.borrow()).into_owned()
    }
}

impl Write for SharedBuf {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        self.0.borrow_mut().extend_from_slice(buf);
        Ok(buf.len())
    }
    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}
