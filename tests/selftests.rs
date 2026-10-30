//! The bare-metal self-tests in examples/*.S, assembled with a real RISC-V assembler and run under the
//! built emulator binary.
//!
//! Each program leaves a bitmask in a0 - one bit per passing sub-test, so a clear bit in a failure
//! message names exactly which check did not pass - and then stops on an EBREAK or by writing the
//! poweroff word to syscon.
//!
//! These need a RISC-V toolchain (riscv64-unknown-elf-, riscv64-elf-, riscv-none-elf- or
//! riscv64-linux-gnu-). Without one they are skipped, loudly; set RISCV_EMU_REQUIRE_TOOLCHAIN=1 (as CI
//! does) to make a missing toolchain a failure instead.

use std::path::{Path, PathBuf};
use std::process::Command;

const EMU: &str = env!("CARGO_BIN_EXE_riscv_emu");

fn find_toolchain() -> Option<String> {
    ["riscv64-unknown-elf-", "riscv64-elf-", "riscv-none-elf-", "riscv64-linux-gnu-"]
        .iter()
        .find(|p| Command::new(format!("{p}as")).arg("--version").output().map_or(false, |o| o.status.success()))
        .map(|p| p.to_string())
}

fn run(cmd: &mut Command) -> Result<(), String> {
    let out = cmd.output().map_err(|e| format!("cannot run {cmd:?}: {e}"))?;
    if out.status.success() {
        Ok(())
    } else {
        Err(format!("{cmd:?} failed:\n{}", String::from_utf8_lossy(&out.stderr)))
    }
}

/// Assemble and link one self-test into a flat binary.
fn build(prefix: &str, name: &str, arch: &str, work: &Path) -> Result<PathBuf, String> {
    let root = Path::new(env!("CARGO_MANIFEST_DIR"));
    let (obj, elf, bin) = (work.join(format!("{name}.o")), work.join(format!("{name}.elf")), work.join(format!("{name}.bin")));
    run(Command::new(format!("{prefix}as"))
        .args([&format!("-march={arch}"), "-mabi=lp64", "-o"])
        .arg(&obj)
        .arg(root.join("examples").join(format!("{name}.S"))))?;
    run(Command::new(format!("{prefix}ld"))
        .args(["-m", "elf64lriscv", "--no-warn-rwx-segments", "-T"])
        .arg(root.join("examples/link.ld"))
        .arg("-o")
        .arg(&elf)
        .arg(&obj))?;
    run(Command::new(format!("{prefix}objcopy")).args(["-O", "binary"]).arg(&elf).arg(&bin))?;
    Ok(bin)
}

fn selftest(name: &str, arch: &str, expect_a0: u64, expect_output: Option<&str>) {
    let Some(prefix) = find_toolchain() else {
        if std::env::var_os("RISCV_EMU_REQUIRE_TOOLCHAIN").is_some() {
            panic!("no RISC-V toolchain found, and RISCV_EMU_REQUIRE_TOOLCHAIN is set");
        }
        eprintln!("SKIPPED {name}: no RISC-V toolchain found (install gcc-riscv64-unknown-elf)");
        return;
    };
    let work = std::env::temp_dir().join(format!("riscv_emu_selftest_{}_{name}", std::process::id()));
    std::fs::create_dir_all(&work).unwrap();
    let bin = build(&prefix, name, arch, &work).unwrap_or_else(|e| panic!("{e}"));

    let out = Command::new(EMU).arg("--dump").arg(&bin).output().expect("cannot run the emulator");
    let (stdout, stderr) = (String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr));
    let all = format!("{stdout}{stderr}");

    // A program stops either on its EBREAK (reported as a trap) or by writing the poweroff word to
    // syscon. Anything else - a step-budget exhaustion, an unexpected fault - means it did not finish.
    assert!(all.contains("breakpoint") || all.contains("powered off"), "program did not stop cleanly.\n{all}");

    if let Some(text) = expect_output {
        assert!(stdout.contains(text), "expected console output containing {text:?}.\n{all}");
    }

    let a0 = stdout
        .lines()
        .find(|l| l.starts_with("x10 (a0)"))
        .and_then(|l| l.split(':').nth(1))
        .and_then(|v| u64::from_str_radix(v.trim().trim_start_matches("0x"), 16).ok())
        .unwrap_or_else(|| panic!("could not find a0 in the register dump.\n{all}"));
    assert_eq!(
        a0, expect_a0,
        "self-test failed: a0 = {a0:#x}, expected {expect_a0:#x}. Each bit is one sub-test; a clear bit identifies which one failed.\n{all}"
    );
    let _ = std::fs::remove_dir_all(&work);
}

#[test]
fn rv64i_selftest() {
    selftest("rv64i_selftest", "rv64i", 0x3fff, None);
}

#[test]
fn trap_selftest() {
    selftest("trap_selftest", "rv64i_zicsr", 0xfff, None);
}

#[test]
fn muldiv_atomic_selftest() {
    selftest("muldiv_atomic_selftest", "rv64ima_zicsr", 0x7fff, None);
}

#[test]
fn device_selftest() {
    selftest("device_selftest", "rv64ima_zicsr", 0x1f, Some("uart works"));
}
