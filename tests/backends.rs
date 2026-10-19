//! The optional register-file and ALU backends.
//!
//! What is checked here is the *wiring*: that each instruction reaches the ALU backend with the
//! operation and operands it should, that a backend really does control the outcome, and that a
//! pass-through backend leaves a program's result identical to the plain core. Whether the plain ALU
//! itself is right is the job of the existing suites and riscv-tests; the default path runs the same
//! `alu_reference()` code the pass-through backend does.

mod common;

use std::cell::RefCell;
use std::rc::Rc;

use common::*;
use riscv_emu::backends::{alu_reference, AluBackend, AluOp, RegFileBackend};
use riscv_emu::decoder::opcodes;
use riscv_emu::types::{DRAM_BASE, NUM_REGS};

#[derive(Clone, Copy, Debug)]
struct Call {
    op: AluOp,
    a: u64,
    b: u64,
}

/// Records every call, and answers with the reference ALU.
struct RecordingAlu {
    log: Rc<RefCell<Vec<Call>>>,
}

impl AluBackend for RecordingAlu {
    fn exec(&mut self, op: AluOp, a: u64, b: u64) -> u64 {
        self.log.borrow_mut().push(Call { op, a, b });
        alu_reference(op, a, b)
    }
}

#[derive(Default)]
struct RegStats {
    bad_index: i32,
    reads: i64,
    writes: i64,
}

/// A register file that remembers every access and refuses x0.
struct CheckedRegFile {
    r: [u64; NUM_REGS],
    stats: Rc<RefCell<RegStats>>,
}

impl RegFileBackend for CheckedRegFile {
    fn read(&mut self, i: usize) -> u64 {
        let mut s = self.stats.borrow_mut();
        if i == 0 || i >= NUM_REGS {
            s.bad_index += 1;
        }
        s.reads += 1;
        self.r[i % NUM_REGS]
    }
    fn write(&mut self, i: usize, v: u64) {
        let mut s = self.stats.borrow_mut();
        if i == 0 || i >= NUM_REGS {
            s.bad_index += 1;
        }
        s.writes += 1;
        self.r[i % NUM_REGS] = v;
    }
}

const EDGES: [u64; 12] = [
    0,
    1,
    2,
    31,
    32,
    63,
    0x7fff_ffff,
    0x8000_0000,
    0xffff_ffff,
    0x7fff_ffff_ffff_ffff,
    0x8000_0000_0000_0000,
    0xffff_ffff_ffff_ffff,
];

fn pick(rng: &mut Rng) -> u64 {
    if rng.next() % 3 == 0 {
        EDGES[(rng.next() % EDGES.len() as u64) as usize]
    } else {
        rng.next()
    }
}

/// x1 = a, x2 = b, then `inst`, executing exactly that many steps.
struct OneShot {
    m: Machine,
    log: Rc<RefCell<Vec<Call>>>,
}

impl OneShot {
    fn new(a: u64, b: u64, inst: u32) -> OneShot {
        let mut p = Vec::new();
        load_imm64(&mut p, 1, a);
        load_imm64(&mut p, 2, b);
        p.push(inst);
        let mut m = Machine::new(&p);
        let log = Rc::new(RefCell::new(Vec::new()));
        m.cpu.alu_backend = Some(Box::new(RecordingAlu { log: log.clone() }));
        m.run(LOAD_IMM64_STEPS * 2 + 1);
        OneShot { m, log }
    }
    /// The operation the instruction under test sent to the ALU: the last call.
    fn last(&self) -> Call {
        *self.log.borrow().last().expect("no ALU call recorded")
    }
}

struct RForm {
    opcode: u32,
    f3: u32,
    f7: u32,
    op: AluOp,
}

const R_FORMS: [RForm; 15] = [
    RForm { opcode: opcodes::OP, f3: 0, f7: 0x00, op: AluOp::Add },
    RForm { opcode: opcodes::OP, f3: 0, f7: 0x20, op: AluOp::Sub },
    RForm { opcode: opcodes::OP, f3: 1, f7: 0x00, op: AluOp::Sll },
    RForm { opcode: opcodes::OP, f3: 2, f7: 0x00, op: AluOp::Slt },
    RForm { opcode: opcodes::OP, f3: 3, f7: 0x00, op: AluOp::Sltu },
    RForm { opcode: opcodes::OP, f3: 4, f7: 0x00, op: AluOp::Xor },
    RForm { opcode: opcodes::OP, f3: 5, f7: 0x00, op: AluOp::Srl },
    RForm { opcode: opcodes::OP, f3: 5, f7: 0x20, op: AluOp::Sra },
    RForm { opcode: opcodes::OP, f3: 6, f7: 0x00, op: AluOp::Or },
    RForm { opcode: opcodes::OP, f3: 7, f7: 0x00, op: AluOp::And },
    RForm { opcode: opcodes::OP_32, f3: 0, f7: 0x00, op: AluOp::AddW },
    RForm { opcode: opcodes::OP_32, f3: 0, f7: 0x20, op: AluOp::SubW },
    RForm { opcode: opcodes::OP_32, f3: 1, f7: 0x00, op: AluOp::SllW },
    RForm { opcode: opcodes::OP_32, f3: 5, f7: 0x00, op: AluOp::SrlW },
    RForm { opcode: opcodes::OP_32, f3: 5, f7: 0x20, op: AluOp::SraW },
];

#[test]
fn register_register_forms_reach_the_alu() {
    let mut rng = Rng(1);
    for f in &R_FORMS {
        for _ in 0..20 {
            let (a, b) = (pick(&mut rng), pick(&mut rng));
            let mut t = OneShot::new(a, b, r_type(f.opcode, 3, f.f3, 1, 2, f.f7));
            let last = t.last();
            assert_eq!(last.op, f.op);
            assert_eq!(last.a, a);
            assert_eq!(last.b, b);
            assert_eq!(t.m.reg(3), alu_reference(f.op, a, b));
        }
    }
}

#[test]
fn immediate_forms_reach_the_alu() {
    let mut rng = Rng(2);
    for _ in 0..40 {
        let a = pick(&mut rng);
        let imm = (rng.next() % 4096) as i32 - 2048;
        let sh6 = (rng.next() % 64) as u32;
        let sh5 = (rng.next() % 32) as u32;
        let sx = imm as i64 as u64;
        // (opcode, funct3, immediate field, expected op, expected b)
        let forms: [(u32, u32, i32, AluOp, u64); 13] = [
            (opcodes::OP_IMM, 0, imm, AluOp::Add, sx),
            (opcodes::OP_IMM, 2, imm, AluOp::Slt, sx),
            (opcodes::OP_IMM, 3, imm, AluOp::Sltu, sx),
            (opcodes::OP_IMM, 4, imm, AluOp::Xor, sx),
            (opcodes::OP_IMM, 6, imm, AluOp::Or, sx),
            (opcodes::OP_IMM, 7, imm, AluOp::And, sx),
            (opcodes::OP_IMM, 1, sh6 as i32, AluOp::Sll, sh6 as u64),
            (opcodes::OP_IMM, 5, sh6 as i32, AluOp::Srl, sh6 as u64),
            (opcodes::OP_IMM, 5, (0x400 | sh6) as i32, AluOp::Sra, sh6 as u64),
            (opcodes::OP_IMM_32, 0, imm, AluOp::AddW, sx),
            (opcodes::OP_IMM_32, 1, sh5 as i32, AluOp::SllW, sh5 as u64),
            (opcodes::OP_IMM_32, 5, sh5 as i32, AluOp::SrlW, sh5 as u64),
            (opcodes::OP_IMM_32, 5, (0x400 | sh5) as i32, AluOp::SraW, sh5 as u64),
        ];
        for (opcode, f3, field, op, expect_b) in forms {
            let mut t = OneShot::new(a, 0, i_type(opcode, 3, f3, 1, field));
            let last = t.last();
            assert_eq!(last.op, op);
            assert_eq!(last.a, a);
            assert_eq!(last.b, expect_b);
            assert_eq!(t.m.reg(3), alu_reference(op, a, expect_b));
        }
    }
}

#[test]
fn branches_compare_through_the_alu() {
    type Taken = fn(u64, u64) -> bool;
    let forms: [(u32, AluOp, Taken); 6] = [
        (0, AluOp::Xor, |a, b| a == b),
        (1, AluOp::Xor, |a, b| a != b),
        (4, AluOp::Slt, |a, b| (a as i64) < (b as i64)),
        (5, AluOp::Slt, |a, b| (a as i64) >= (b as i64)),
        (6, AluOp::Sltu, |a, b| a < b),
        (7, AluOp::Sltu, |a, b| a >= b),
    ];
    let mut rng = Rng(3);
    let pc_of_branch = DRAM_BASE + 4 * (LOAD_IMM64_STEPS * 2);
    for (f3, op, taken) in forms {
        for i in 0..40 {
            // Half the draws make the operands equal so BEQ/BNE see both outcomes.
            let a = pick(&mut rng);
            let b = if i % 2 == 1 { a } else { pick(&mut rng) };
            let t = OneShot::new(a, b, b_type(opcodes::BRANCH, f3, 1, 2, 8));
            let last = t.last();
            assert_eq!(last.op, op);
            assert_eq!(last.a, a);
            assert_eq!(last.b, b);
            assert_eq!(t.m.cpu.pc, pc_of_branch + if taken(a, b) { 8 } else { 4 });
        }
    }
}

#[test]
fn address_generation_uses_the_alu_add() {
    let base = DRAM_BASE + 0x1000;
    {
        let t = OneShot::new(base, 0, i_type(opcodes::LOAD, 3, 3, 1, -16)); // LD x3, -16(x1)
        let last = t.last();
        assert_eq!(last.op, AluOp::Add);
        assert_eq!(last.a, base);
        assert_eq!(last.b, (-16i64) as u64);
    }
    {
        let t = OneShot::new(base, 0x1122, s_type(opcodes::STORE, 3, 1, 2, 24)); // SD x2, 24(x1)
        let last = t.last();
        assert_eq!(last.op, AluOp::Add);
        assert_eq!(last.a, base);
        assert_eq!(last.b, 24);
    }
    {
        let t = OneShot::new(base, 0, i_type(opcodes::JALR, 3, 0, 1, 5)); // JALR x3, 5(x1)
        let last = t.last();
        assert_eq!(last.op, AluOp::Add);
        assert_eq!(last.a, base);
        assert_eq!(last.b, 5);
        assert_eq!(t.m.cpu.pc, (base + 5) & !1); // bit 0 still cleared after the add
    }
}

/// A backend decides the result: an ALU that corrupts bit 0 of every Add must change what ADDI
/// leaves in the register.
#[test]
fn backend_controls_the_outcome() {
    struct Flip;
    impl AluBackend for Flip {
        fn exec(&mut self, op: AluOp, a: u64, b: u64) -> u64 {
            let r = alu_reference(op, a, b);
            if op == AluOp::Add {
                r ^ 1
            } else {
                r
            }
        }
    }
    // One instruction only: two flipped adds would cancel each other out.
    let p = [addi(1, 0, 100)];
    let mut plain = Machine::new(&p);
    let mut bent = Machine::new(&p);
    bent.cpu.alu_backend = Some(Box::new(Flip));
    plain.run(1);
    bent.run(1);
    assert_eq!(plain.reg(1), 100);
    assert_eq!(bent.reg(1), 101);
}

/// A random straight-line program over x1..x7.
fn random_program(rng: &mut Rng, n: usize) -> Vec<u32> {
    let mut p = Vec::new();
    for r in 1..=7 {
        load_imm64(&mut p, r, pick(rng));
    }
    for _ in 0..n {
        let rd = 1 + (rng.next() % 7) as u32;
        let rs1 = 1 + (rng.next() % 7) as u32;
        let rs2 = 1 + (rng.next() % 7) as u32;
        if rng.next() % 2 == 1 {
            let f = &R_FORMS[(rng.next() % R_FORMS.len() as u64) as usize];
            p.push(r_type(f.opcode, rd, f.f3, rs1, rs2, f.f7));
        } else {
            let imm = (rng.next() % 4096) as i32 - 2048;
            let f3s = [0, 2, 3, 4, 6, 7];
            p.push(i_type(opcodes::OP_IMM, rd, f3s[(rng.next() % 6) as usize], rs1, imm));
        }
    }
    p
}

#[test]
fn passthrough_backends_match_the_plain_core() {
    let mut rng = Rng(4);
    for _ in 0..50 {
        let prog = random_program(&mut rng, 200);
        let steps = prog.len() as u64;

        let mut plain = Machine::new(&prog);
        let mut backed = Machine::new(&prog);
        let log = Rc::new(RefCell::new(Vec::new()));
        let stats = Rc::new(RefCell::new(RegStats::default()));
        backed.cpu.alu_backend = Some(Box::new(RecordingAlu { log: log.clone() }));
        backed.cpu.regfile = Some(Box::new(CheckedRegFile { r: [0; NUM_REGS], stats: stats.clone() }));

        plain.run(steps);
        backed.run(steps);

        for r in 0..NUM_REGS {
            assert_eq!(backed.reg(r), plain.reg(r), "x{r}");
        }
        assert_eq!(backed.cpu.pc, plain.cpu.pc);
        let s = stats.borrow();
        assert_eq!(s.bad_index, 0); // the backend never sees x0 or an out-of-range index
        assert!(s.reads > 0 && s.writes > 0);
        assert!(!log.borrow().is_empty());
        // While a backend is installed the CPU's own array is not touched.
        for r in 0..NUM_REGS {
            assert_eq!(backed.cpu.regs[r], 0);
        }
    }
}

#[test]
fn x0_never_reaches_the_register_file_backend() {
    let mut m = Machine::new(&[addi(0, 0, 77), addi(1, 0, 5)]);
    let stats = Rc::new(RefCell::new(RegStats::default()));
    m.cpu.regfile = Some(Box::new(CheckedRegFile { r: [0; NUM_REGS], stats: stats.clone() }));
    m.run(2);
    assert_eq!(stats.borrow().bad_index, 0);
    assert_eq!(m.reg(0), 0);
    assert_eq!(m.reg(1), 5);
}
