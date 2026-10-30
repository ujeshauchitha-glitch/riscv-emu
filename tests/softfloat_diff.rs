//! Differential test of the soft-float unit against an exact rational reference.
//!
//! tests/gen_float_vectors.py computes every result with Python's exact `Fraction` arithmetic and
//! rounds it by the textbook definition, for all five RISC-V rounding modes on binary32 and binary64,
//! with the five exception flags, tininess detected after rounding, subnormals in and out, the sign
//! of an exact zero, and saturating conversion to integers. The vectors are generated when the test
//! runs; set FLOAT_VECTORS to use an existing file instead.
//!
//! In round-to-nearest-even the host FPU is a second, independent reference, so those cases are also
//! checked against Rust's own `f32`/`f64` arithmetic over random bit patterns.

mod common;

use common::Rng;
use riscv_emu::softfloat::{self as sf, Fmt, Rm, F32, F64};

fn vector_file() -> std::path::PathBuf {
    if let Ok(p) = std::env::var("FLOAT_VECTORS") {
        return p.into();
    }
    let out = std::env::temp_dir().join("riscv_emu_float_vectors.txt");
    let script = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/gen_float_vectors.py");
    for python in ["python3", "python", "py"] {
        let ok = std::process::Command::new(python)
            .arg(script)
            .arg(&out)
            .arg("300")
            .status()
            .map_or(false, |s| s.success());
        if ok {
            return out;
        }
    }
    panic!("could not generate float vectors: no python found (set FLOAT_VECTORS to a prebuilt file)");
}

fn hex(s: &str) -> u64 {
    u64::from_str_radix(s, 16).unwrap()
}

fn fmt_of(c: char) -> Fmt {
    if c == 's' {
        F32
    } else {
        F64
    }
}

#[test]
fn matches_the_exact_rational_reference() {
    let text = std::fs::read_to_string(vector_file()).unwrap();
    let (mut run, mut bad) = (0usize, 0usize);
    let mut kinds = std::collections::BTreeMap::<String, usize>::new();

    for line in text.lines() {
        let w: Vec<&str> = line.split_whitespace().collect();
        if w.len() != 8 {
            continue;
        }
        let (op, kind) = (w[0], w[1]);
        let rm = Rm::from_bits(w[2].parse().unwrap()).unwrap();
        let (a, b, c) = (hex(w[3]), hex(w[4]), hex(w[5]));
        let (want, want_flags) = (hex(w[6]), hex(w[7]));

        let (got, got_flags) = match op {
            "add" => sf::add(a, b, fmt_of(kind.chars().next().unwrap()), rm),
            "sub" => sf::sub(a, b, fmt_of(kind.chars().next().unwrap()), rm),
            "mul" => sf::mul(a, b, fmt_of(kind.chars().next().unwrap()), rm),
            "div" => sf::div(a, b, fmt_of(kind.chars().next().unwrap()), rm),
            "sqrt" => sf::sqrt(a, fmt_of(kind.chars().next().unwrap()), rm),
            "fma" => sf::fma(a, b, c, fmt_of(kind.chars().next().unwrap()), rm),
            "cvt" => {
                let (from, to) = if kind == "d2s" { (F64, F32) } else { (F32, F64) };
                sf::convert(a, from, to, rm)
            }
            "i2f" => {
                // kind is e.g. "i32s": integer type, then the destination format
                let signed = kind.starts_with('i');
                let bits: u32 = kind[1..3].parse().unwrap();
                let dst = fmt_of(kind.chars().last().unwrap());
                let (neg, mag) = if signed {
                    let v = if bits == 32 { a as u32 as i32 as i64 } else { a as i64 };
                    (v < 0, v.unsigned_abs())
                } else {
                    (false, a)
                };
                sf::from_int(neg, mag, dst, rm)
            }
            "f2i" => {
                let signed = kind.starts_with('i');
                let bits: u32 = kind[1..3].parse().unwrap();
                let src = fmt_of(kind.chars().last().unwrap());
                let (v, fl) = sf::to_int(a, src, rm, bits, signed);
                // 32-bit results are sign-extended into the 64-bit register, even the unsigned one.
                (if bits == 32 { v as u32 as i32 as i64 as u64 } else { v }, fl)
            }
            other => panic!("unknown op {other}"),
        };
        run += 1;
        *kinds.entry(format!("{op}-{kind}")).or_default() += 1;
        if got != want || got_flags != want_flags {
            bad += 1;
            if bad <= 25 {
                eprintln!(
                    "MISMATCH {line}\n   got {got:x} flags {got_flags:x}, want {want:x} flags {want_flags:x}"
                );
            }
        }
    }
    eprintln!("softfloat: {run} vectors across {} operation kinds, {bad} mismatches", kinds.len());
    assert_eq!(bad, 0, "differential mismatches");
    assert!(run > 20_000);
    assert!(kinds.len() >= 30);
}

// --- the host FPU as a second reference, round-to-nearest-even only ----------------------------

fn canon32(x: u32) -> u32 {
    if f32::from_bits(x).is_nan() {
        0x7fc0_0000
    } else {
        x
    }
}
fn canon64(x: u64) -> u64 {
    if f64::from_bits(x).is_nan() {
        0x7ff8_0000_0000_0000
    } else {
        x
    }
}

fn random_f64(rng: &mut Rng) -> u64 {
    let r = rng.next();
    match r % 8 {
        0 => rng.next() & 0x800f_ffff_ffff_ffff, // subnormal or zero
        1 => (rng.next() & 0x800f_ffff_ffff_ffff) | (0x7fe << 52), // huge
        2 => (rng.next() & 0x800f_ffff_ffff_ffff) | ((1023 + (rng.next() % 5) - 2) << 52), // near one
        _ => rng.next(),
    }
}

fn random_f32(rng: &mut Rng) -> u32 {
    let r = rng.next();
    match r % 8 {
        0 => (rng.next() as u32) & 0x807f_ffff,
        1 => ((rng.next() as u32) & 0x807f_ffff) | (0xfe << 23),
        2 => ((rng.next() as u32) & 0x807f_ffff) | ((127 + (rng.next() % 5) as u32 - 2) << 23),
        _ => rng.next() as u32,
    }
}

#[test]
fn round_to_nearest_even_agrees_with_the_host_fpu() {
    let mut rng = Rng(0xfeed);
    for _ in 0..300_000 {
        let (a, b, c) = (random_f64(&mut rng), random_f64(&mut rng), random_f64(&mut rng));
        let (x, y, z) = (f64::from_bits(a), f64::from_bits(b), f64::from_bits(c));
        assert_eq!(sf::add(a, b, F64, Rm::Rne).0, canon64((x + y).to_bits()), "{x:e}+{y:e}");
        assert_eq!(sf::sub(a, b, F64, Rm::Rne).0, canon64((x - y).to_bits()), "{x:e}-{y:e}");
        assert_eq!(sf::mul(a, b, F64, Rm::Rne).0, canon64((x * y).to_bits()), "{x:e}*{y:e}");
        assert_eq!(sf::div(a, b, F64, Rm::Rne).0, canon64((x / y).to_bits()), "{x:e}/{y:e}");
        assert_eq!(sf::sqrt(a, F64, Rm::Rne).0, canon64(x.sqrt().to_bits()), "sqrt {x:e}");
        assert_eq!(sf::fma(a, b, c, F64, Rm::Rne).0, canon64(x.mul_add(y, z).to_bits()), "fma {x:e} {y:e} {z:e}");

        let (a, b, c) = (random_f32(&mut rng), random_f32(&mut rng), random_f32(&mut rng));
        let (x, y, z) = (f32::from_bits(a), f32::from_bits(b), f32::from_bits(c));
        let (a, b, c) = (a as u64, b as u64, c as u64);
        assert_eq!(sf::add(a, b, F32, Rm::Rne).0 as u32, canon32((x + y).to_bits()), "{x:e}+{y:e}");
        assert_eq!(sf::mul(a, b, F32, Rm::Rne).0 as u32, canon32((x * y).to_bits()), "{x:e}*{y:e}");
        assert_eq!(sf::div(a, b, F32, Rm::Rne).0 as u32, canon32((x / y).to_bits()), "{x:e}/{y:e}");
        assert_eq!(sf::sqrt(a, F32, Rm::Rne).0 as u32, canon32(x.sqrt().to_bits()), "sqrt {x:e}");
        assert_eq!(sf::fma(a, b, c, F32, Rm::Rne).0 as u32, canon32(x.mul_add(y, z).to_bits()), "fma {x:e} {y:e} {z:e}");

        // Conversions between the precisions, and from integers.
        let d = random_f64(&mut rng);
        assert_eq!(sf::convert(d, F64, F32, Rm::Rne).0 as u32, canon32((f64::from_bits(d) as f32).to_bits()));
        let s = random_f32(&mut rng);
        assert_eq!(sf::convert(s as u64, F32, F64, Rm::Rne).0, canon64((f32::from_bits(s) as f64).to_bits()));
        let i = rng.next() as i64;
        assert_eq!(sf::from_int(i < 0, i.unsigned_abs(), F64, Rm::Rne).0, (i as f64).to_bits());
        assert_eq!(sf::from_int(i < 0, i.unsigned_abs(), F32, Rm::Rne).0 as u32, (i as f32).to_bits());
        let u = rng.next();
        assert_eq!(sf::from_int(false, u, F64, Rm::Rne).0, (u as f64).to_bits());
        assert_eq!(sf::from_int(false, u, F32, Rm::Rne).0 as u32, (u as f32).to_bits());
    }
}
