//! Zba, Zbb, Zbc, Zbs and Zicond.
//!
//! Two layers of checking, because each catches what the other cannot:
//!
//!   1. Hand-computed cases with real assembler encodings. These pin the meaning of a few
//!      instructions against numbers worked out on paper and against encodings taken from actual
//!      toolchain output, so a mistake in how the decoder reads an encoding cannot hide behind a
//!      model that shares it.
//!
//!   2. A differential run against tests/gen_bitmanip_vectors.py, an independent model (in another
//!      language, written from the specification tables) that produces tens of thousands of random
//!      and edge-case instruction/operand/result triples. The vectors are generated when the test
//!      runs; set BITMANIP_VECTORS to use an existing file instead.

mod common;

use common::*;
use riscv_emu::bitmanip::*;
use riscv_emu::csr::csr;
use riscv_emu::trap::Trap;
use riscv_emu::types::DRAM_BASE;

/// Execute one instruction with a0..: x11 = rs1, x12 = rs2, result in x10.
struct Exec {
    m: Machine,
}

impl Exec {
    fn new() -> Exec {
        Exec { m: Machine::new(&[halt()]) }
    }

    fn run(&mut self, insn: u32, rs1: u64, rs2: u64) -> (Result<(), Trap>, u64) {
        // Writing the instruction into DRAM also exercises decode-cache invalidation on code writes.
        assert!(self.m.bus.ram.store(0, 4, insn as u64));
        self.m.cpu.pc = DRAM_BASE;
        self.m.cpu.write_reg(10, 0xdead_beef_cafe_f00d); // so an instruction that forgets to write rd is noticed
        self.m.cpu.write_reg(11, rs1);
        self.m.cpu.write_reg(12, rs2);
        let r = self.m.cpu.step();
        (r, self.m.cpu.read_reg(10))
    }

    /// Run and require no trap; returns rd.
    fn ok(&mut self, insn: u32, rs1: u64, rs2: u64) -> u64 {
        let (r, rd) = self.run(insn, rs1, rs2);
        assert!(r.is_ok(), "{insn:#010x} trapped: {r:?}");
        rd
    }
}

// --- known encodings ---------------------------------------------------------------------

#[test]
fn encodings_match_real_assembler_output() {
    let mut e = Exec::new();

    // These words are what a RISC-V assembler emits for the named instruction with a0 as
    // destination and a1, a2 as sources.
    assert_eq!(e.ok(0x20c5a533, 5, 100), (5 << 1) + 100); // sh1add
    assert_eq!(e.ok(0x20c5c533, 5, 100), (5 << 2) + 100); // sh2add
    assert_eq!(e.ok(0x20c5e533, 5, 100), (5 << 3) + 100); // sh3add
    assert_eq!(e.ok(0x08c5853b, 0xffff_ffff_0000_0007, 1), 8); // add.uw drops rs1's upper half: 7 + 1
    assert_eq!(e.ok(0x60059513, 0x0000_0000_00f0_0000, 0), 40); // clz
    assert_eq!(e.ok(0x6b85d513, 0x0102_0304_0506_0708, 0), 0x0807_0605_0403_0201); // rev8
    assert_eq!(e.ok(0x0ec5d533, 77, 0), 0); // czero.eqz, condition zero
    assert_eq!(e.ok(0x0ec5d533, 77, 1), 77); // condition non-zero
}

// --- hand-computed arithmetic ----------------------------------------------------------------

#[test]
fn counting_and_byte_operations() {
    assert_eq!(clz64(0), 64);
    assert_eq!(clz64(1), 63);
    assert_eq!(clz64(!0), 0);
    assert_eq!(ctz64(0), 64);
    assert_eq!(ctz64(0x8000_0000_0000_0000), 63);
    assert_eq!(cpop64(0xff00_ff00_ff00_ff00), 32);
    // The 32-bit forms count within the low word only.
    assert_eq!(clz32(0xffff_ffff_0000_0001), 31);
    assert_eq!(ctz32(0xffff_ffff_0000_0000), 32);
    assert_eq!(cpop32(0xffff_ffff_0000_000f), 4);

    assert_eq!(rev8(0x0102_0304_0506_0708), 0x0807_0605_0403_0201);
    assert_eq!(orc_b(0x0100_ff00_0080_0000), 0xff00_ff00_00ff_0000);
    assert_eq!(orc_b(0), 0);
}

#[test]
fn carryless_multiply() {
    // (x + 1) * (x + 1) = x^2 + 1 over GF(2): no carry, no middle term.
    assert_eq!(clmul(3, 3), 5);
    assert_eq!(clmul(0, 0xdead_beef), 0);
    assert_eq!(clmul(1, 0xdead_beef_cafe_f00d), 0xdead_beef_cafe_f00d); // multiplying by 1 is the identity
    // x^63 * x^63 = x^126: bit 62 of the high half, and bit 63 of the "reversed" product.
    assert_eq!(clmulh(1 << 63, 1 << 63), 1 << 62);
    assert_eq!(clmulr(1 << 63, 1 << 63), 1 << 63);
    assert_eq!(clmul(1 << 63, 1 << 63), 0);
}

#[test]
fn rotates_and_word_forms() {
    assert_eq!(rol64(0x8000_0000_0000_0001, 1), 3);
    assert_eq!(ror64(1, 1), 0x8000_0000_0000_0000);
    assert_eq!(rol64(0x123, 64), 0x123); // a rotate by the width is no rotate (and no overflow panic)
    assert_eq!(rol32(0x8000_0001, 1), 3);
    assert_eq!(ror32(1, 1), 0x8000_0000);

    // ROLW/RORW sign-extend their 32-bit result.
    let mut e = Exec::new();
    assert_eq!(e.ok(0x60c5953b, 0x8000_0001, 1), 3); // rolw a0,a1,a2
    assert_eq!(e.ok(0x60c5d53b, 1, 1), 0xffff_ffff_8000_0000); // rorw a0,a1,a2
    // The upper half of rs1 is ignored by the word forms.
    assert_eq!(e.ok(0x60c5953b, 0xdead_beef_8000_0001, 1), 3);
}

#[test]
fn extensions_and_zeroing() {
    let mut e = Exec::new();
    assert_eq!(e.ok(0x60459513, 0x80, 0), 0xffff_ffff_ffff_ff80); // sext.b
    assert_eq!(e.ok(0x60559513, 0x8000, 0), 0xffff_ffff_ffff_8000); // sext.h
    assert_eq!(e.ok(0x0805c53b, 0x1234_5678_abcd, 0), 0xabcd); // zext.h a0,a1
    // czero.nez is the mirror image.
    assert_eq!(e.ok(0x0ec5f533, 77, 0), 77);
    assert_eq!(e.ok(0x0ec5f533, 77, 5), 0);
}

#[test]
fn reserved_neighbours_still_trap() {
    // Encodings next to the new ones, which no extension defines, must still be illegal: adding
    // instructions must not turn unallocated space into NOPs.
    let mut e = Exec::new();
    // OP-IMM funct3=1 with imm12 = 0x603 (between cpop and sext.b): reserved.
    assert!(e.run(0x60359513, 1, 0).0.is_err());
    // A shift-immediate with a reserved function code: funct6 = 0x2a on SLLI's funct3.
    assert!(e.run(0xa0059513, 1, 0).0.is_err());
    // OP with funct7 = 0x07 (Zicond's) but funct3 = 4: not an instruction.
    assert!(e.run(0x0ec5c533, 1, 1).0.is_err());
    // ZEXT.H's encoding with a non-zero rs2 is PACK (Zbkb), which is not implemented.
    assert!(e.run(0x08c5c53b, 1, 1).0.is_err());
}

#[test]
fn misa_advertises_the_b_extension() {
    let m = Machine::new(&[halt()]);
    assert_ne!(m.cpu.csrs.read(csr::MISA) & (1 << (b'B' - b'A')), 0);
}

// --- the differential run --------------------------------------------------------------------

fn vector_file() -> Option<std::path::PathBuf> {
    if let Ok(p) = std::env::var("BITMANIP_VECTORS") {
        return Some(p.into());
    }
    let out = std::env::temp_dir().join("riscv_emu_bitmanip_vectors.txt");
    let script = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/gen_bitmanip_vectors.py");
    for python in ["python3", "python", "py"] {
        let ok = std::process::Command::new(python)
            .arg(script)
            .arg(&out)
            .status()
            .map_or(false, |s| s.success());
        if ok {
            return Some(out);
        }
    }
    None
}

#[test]
fn differential_run_against_the_python_model() {
    let Some(path) = vector_file() else {
        panic!("could not generate bitmanip vectors: no python found (set BITMANIP_VECTORS to a prebuilt file)");
    };
    let text = std::fs::read_to_string(&path).expect("cannot read the vector file");

    let mut e = Exec::new();
    let (mut run, mut bad) = (0usize, 0usize);
    let mut names = std::collections::BTreeMap::<String, usize>::new();
    for line in text.lines() {
        let mut w = line.split_whitespace();
        let (Some(i), Some(a), Some(b), Some(want), Some(name)) = (w.next(), w.next(), w.next(), w.next(), w.next())
        else {
            continue;
        };
        let insn = u32::from_str_radix(i, 16).unwrap();
        let rs1 = u64::from_str_radix(a, 16).unwrap();
        let rs2 = u64::from_str_radix(b, 16).unwrap();
        let want = u64::from_str_radix(want, 16).unwrap();

        let (r, got) = e.run(insn, rs1, rs2);
        run += 1;
        *names.entry(name.to_string()).or_default() += 1;

        // The instruction must also leave its sources alone.
        let intact = e.m.cpu.read_reg(11) == rs1 && e.m.cpu.read_reg(12) == rs2;
        if r.is_err() || got != want || !intact {
            bad += 1;
            if bad <= 20 {
                eprintln!(
                    "MISMATCH {name:<10} insn={insn:08x} rs1={rs1:016x} rs2={rs2:016x}: got {got:016x} want {want:016x}{}{}",
                    if r.is_ok() { "" } else { " (trapped)" },
                    if intact { "" } else { " (clobbered a source)" }
                );
            }
        }
    }
    eprintln!("bitmanip: {run} vectors across {} instructions, {bad} mismatches", names.len());
    assert_eq!(bad, 0, "differential mismatches");
    // Guard against the generator silently producing nothing.
    assert!(run > 10000);
    assert!(names.len() >= 40);
}
