use std::cell::RefCell;
use std::rc::Rc;

use riscv_emu::bus::{Bus, Shared};
use riscv_emu::clint::Clint;
use riscv_emu::elf;
use riscv_emu::plic::Plic;
use riscv_emu::platform::Platform;
use riscv_emu::syscon::Syscon;
use riscv_emu::types::*;
use riscv_emu::uart::Uart;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut image = None;
    let mut max_steps = 100_000_000u64;
    let mut trace = false;
    let mut dump = false;
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--trace" => trace = true,
            "--dump" => dump = true,
            "--max-steps" => {
                i += 1;
                max_steps = args[i].parse().unwrap();
            }
            a => image = Some(a.to_string()),
        }
        i += 1;
    }

    let mut bus = Bus::new(128 << 20);
    let (u, uart) = Shared::new(Uart::new(Box::new(std::io::stdout())));
    let clock = Rc::new(riscv_emu::clint::Clock::new());
    let (c, clint) = Shared::new(Clint::new(1, clock));
    let (s, syscon) = Shared::new(Syscon::new());
    let (p, plic) = Shared::new(Plic::new(1));
    assert!(bus.attach(Box::new(u)));
    assert!(bus.attach(Box::new(c)));
    assert!(bus.attach(Box::new(s)));
    assert!(bus.attach(Box::new(p)));
    let _ = RefCell::new(0);

    let bytes = std::fs::read(image.expect("image")).unwrap();
    let img = elf::load_elf(&bytes, &bus).unwrap();
    let mut plat = Platform::new(Rc::new(bus), 1);
    plat.wire(clint, plic, uart, UART0_IRQ, syscon.clone());
    plat.htif_tohost_addr = img.tohost;
    plat.harts[0].pc = img.entry;
    plat.harts[0].trace = trace;
    let r = plat.run(max_steps);
    if plat.htif_tohost_value != 0 {
        let code = plat.htif_tohost_value >> 1;
        if code == 0 {
            eprintln!("\nPASS after {} instruction(s)", plat.retired());
        } else {
            eprintln!("\nFAIL: test {} failed, after {} instruction(s)", code, plat.retired());
        }
        std::process::exit((code != 0) as i32);
    }
    eprintln!("\nstopped: {:?} halted={}", r.err(), plat.halted());
    if dump {
        let mut so = std::io::stdout();
        plat.harts[0].dump_registers(&mut so);
    }
}
