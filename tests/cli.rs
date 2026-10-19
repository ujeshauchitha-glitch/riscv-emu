//! The command line, exercised through the built binary: the built-in demo, option parsing and its
//! error messages, exit codes, the device tree dump, and the options that attach devices.

use std::io::Write;
use std::process::{Command, Output, Stdio};

const EMU: &str = env!("CARGO_BIN_EXE_riscv_emu");

fn emu(args: &[&str]) -> Output {
    Command::new(EMU).args(args).stdin(Stdio::null()).output().expect("cannot run the emulator")
}

fn stdout(o: &Output) -> String {
    String::from_utf8_lossy(&o.stdout).into_owned()
}
fn stderr(o: &Output) -> String {
    String::from_utf8_lossy(&o.stderr).into_owned()
}

fn temp(name: &str) -> std::path::PathBuf {
    std::env::temp_dir().join(format!("riscv_emu_cli_{}_{name}", std::process::id()))
}

#[test]
fn the_built_in_demo_prints_over_the_uart_and_powers_off() {
    let o = emu(&[]);
    assert!(o.status.success());
    assert!(stdout(&o).contains("hello, RISC-V"), "{}", stdout(&o));
    assert!(stderr(&o).contains("machine powered off"), "{}", stderr(&o));
}

#[test]
fn help_lists_the_options() {
    let o = emu(&["--help"]);
    assert!(o.status.success());
    let text = stderr(&o);
    for flag in ["--smp", "--net user", "--drive-snapshot", "--gdb PORT", "--linux", "--rng-seed", "--timer-divisor"] {
        assert!(text.contains(flag), "--help does not mention {flag}");
    }
}

#[test]
fn bad_options_are_refused_with_exit_code_2() {
    for args in [
        &["--nope"][..],
        &["--smp", "0"],
        &["--smp", "33"],
        &["--dram-size-mb", "0"],
        &["--quantum", "0"],
        &["--net", "bridge"],
        &["--net-dns"],
        &["--max-steps"],
        &["--disk", "a", "--drive", "b"],
        &["--net", "user", "--net-dns", "not-an-ip"],
    ] {
        let o = emu(args);
        assert_eq!(o.status.code(), Some(2), "{args:?}: {}", stderr(&o));
        assert!(stderr(&o).contains("error:"), "{args:?}: {}", stderr(&o));
    }
}

#[test]
fn a_missing_image_is_an_error_not_a_crash() {
    let o = emu(&["definitely-not-a-file.elf"]);
    assert_eq!(o.status.code(), Some(1));
    assert!(stderr(&o).contains("cannot read image"));
}

#[test]
fn the_step_budget_is_reported() {
    // An endless loop under a small budget.
    let image = temp("spin.bin");
    std::fs::write(&image, 0x0000_006fu32.to_le_bytes()).unwrap(); // j .
    let o = emu(&["--max-steps", "1000", image.to_str().unwrap()]);
    assert!(stderr(&o).contains("step budget exhausted after 1000 instruction(s)"), "{}", stderr(&o));
    assert_eq!(o.status.code(), Some(0));
    std::fs::remove_file(image).unwrap();
}

#[test]
fn a_fatal_trap_is_reported_with_its_cause_and_exit_code_1() {
    let image = temp("illegal.bin");
    std::fs::write(&image, 0u32.to_le_bytes()).unwrap(); // the all-zero word is illegal
    let o = emu(&[image.to_str().unwrap()]);
    assert_eq!(o.status.code(), Some(1));
    let e = stderr(&o);
    assert!(e.contains("illegal instruction") && e.contains("cause 2"), "{e}");
    std::fs::remove_file(image).unwrap();
}

#[test]
fn dump_prints_the_registers() {
    let o = emu(&["--dump"]);
    let text = stdout(&o);
    assert!(text.contains("=== register dump ==="));
    assert!(text.contains("x10 (a0)"));
}

#[test]
fn the_syscon_exit_code_becomes_the_process_exit_code() {
    // Power off with exit code 3: the value written is (3 << 16) | 0x5555 = 0x35555.
    let words: [u32; 5] = [
        0x0010_02b7, // lui   t0, 0x100
        0x0003_5337, // lui   t1, 0x35        (0x35000)
        0x5553_0313, // addi  t1, t1, 0x555   (0x35555)
        0x0062_a023, // sw    t1, 0(t0)
        0x0000_006f, // j .
    ];
    let image = temp("exit3.bin");
    let bytes: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
    std::fs::write(&image, bytes).unwrap();
    let o = emu(&[image.to_str().unwrap()]);
    assert_eq!(o.status.code(), Some(3), "{}", stderr(&o));
    assert!(stderr(&o).contains("exit code 3"));
    std::fs::remove_file(image).unwrap();
}

#[test]
fn dump_dtb_writes_a_valid_device_tree_and_exits() {
    let dtb = temp("machine.dtb");
    let o = emu(&["--dump-dtb", dtb.to_str().unwrap(), "--smp", "2", "--net", "loopback", "--rng", "--rtc"]);
    assert!(o.status.success(), "{}", stderr(&o));
    let blob = std::fs::read(&dtb).unwrap();
    assert_eq!(&blob[..4], &0xd00d_feedu32.to_be_bytes());
    let has = |s: &str| blob.windows(s.len()).any(|w| w == s.as_bytes());
    for needle in ["cpu@1", "virtio_mmio@10002000", "virtio_mmio@10003000", "rtc@101000", "rng-seed"] {
        assert!(has(needle), "{needle}");
    }
    std::fs::remove_file(dtb).unwrap();
}

#[test]
fn the_same_rng_seed_gives_the_same_device_tree() {
    let (a, b, c) = (temp("a.dtb"), temp("b.dtb"), temp("c.dtb"));
    emu(&["--dump-dtb", a.to_str().unwrap(), "--rng-seed", "42"]);
    emu(&["--dump-dtb", b.to_str().unwrap(), "--rng-seed", "42"]);
    emu(&["--dump-dtb", c.to_str().unwrap(), "--rng-seed", "43"]);
    let (a, b, c) = (std::fs::read(&a).unwrap(), std::fs::read(&b).unwrap(), std::fs::read(&c).unwrap());
    assert_eq!(a, b);
    assert_ne!(a, c);
}

#[test]
fn guest_input_reaches_the_uart_through_stdin() {
    // A program that echoes one received byte back, then powers off: polls LSR.DR, reads RBR, writes
    // THR, and writes the poweroff word.
    let words: [u32; 11] = [
        0x1000_02b7, // lui   t0, 0x10000          (UART)
        // wait: lbu t1, 5(t0) ; andi t1,t1,1 ; beqz t1, wait
        0x0052_c303, // lbu   t1, 5(t0)
        0x0013_7313, // andi  t1, t1, 1
        0xfe03_0ce3, // beqz  t1, -8
        0x0002_c303, // lbu   t1, 0(t0)            (the byte)
        0x0062_8023, // sb    t1, 0(t0)            (echo it)
        0x0010_03b7, // lui   t2, 0x100            (syscon)
        0x0000_5e37, // lui   t3, 0x5
        0x555e_0e1b, // addiw t3, t3, 0x555
        0x01c3_a023, // sw    t3, 0(t2)
        0x0000_006f, // j .
    ];
    let image = temp("echo.bin");
    let bytes: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
    std::fs::write(&image, bytes).unwrap();

    let mut child = Command::new(EMU)
        .arg(image.to_str().unwrap())
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    child.stdin.take().unwrap().write_all(b"Z").unwrap();
    let o = child.wait_with_output().unwrap();
    assert!(stdout(&o).contains('Z'), "stdout {:?} stderr {}", stdout(&o), stderr(&o));
    std::fs::remove_file(image).unwrap();
}
