// A throughput benchmark, not a correctness test: it reports how many guest
// instructions per second the interpreter retires on a mixed workload.
//
// It exists so that speed work can be judged by a number rather than a feeling.
// Run it before and after a change to the execution loop:
//
//   ./build/bench_cpu                    # default 40M instructions, bare machine
//   ./build/bench_cpu 200000000
//   ./build/bench_cpu 40000000 paged     # S-mode under Sv39, so every access translates
//   ./build/bench_cpu 40000000 devices   # CLINT + PLIC + UART attached, as in a real boot
//   ./build/bench_cpu 40000000 paged devices
//
// "paged" and "devices" matter because they are what a real boot looks like: a
// kernel runs with translation on and the per-step device polling live, and a
// bare-machine number alone flatters the interpreter.
//
// The guest program is a tight loop mixing the instruction classes a kernel
// spends its time on - ALU ops, a load, a store, a taken branch - so a
// speed-up that only helps one class does not hide a regression in another.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#include "clint.hpp"
#include "csr.hpp"
#include "machine.hpp"
#include "mmu.hpp"
#include "plic.hpp"
#include "uart.hpp"

using namespace rvt;

int main(int argc, char** argv) {
    const u64 steps = argc > 1 ? std::strtoull(argv[1], nullptr, 0) : 40'000'000;
    bool paged = false, devices = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "paged")) paged = true;
        else if (!std::strcmp(argv[i], "devices")) devices = true;
    }

    // x5 = scratch address (a few KiB into DRAM, clear of the code), x6 = counter.
    //   loop: addi x6,x6,1 ; add x7,x6,x6 ; xor x8,x7,x6 ; slli x9,x8,3
    //         sd x9,0(x5) ; ld x10,0(x5) ; add x11,x10,x9 ; andi x12,x11,0xff
    //         bne x12,x0,loop        (almost always taken)
    //         j loop
    std::vector<u32> prog;
    prog.push_back(LUI(5, 0x80010));             // x5 = 0x80010000 (sign-extended)
    prog.push_back(i_type(opcodes::OP_IMM, 5, 0x1, 5, 32));   // slli x5,x5,32
    prog.push_back(i_type(opcodes::OP_IMM, 5, 0x5, 5, 32));   // srli x5,x5,32
    const std::size_t loop = prog.size();
    prog.push_back(ADDI(6, 6, 1));
    prog.push_back(r_type(opcodes::OP, 7, 0x0, 6, 6, 0x00));  // add  x7,x6,x6
    prog.push_back(r_type(opcodes::OP, 8, 0x4, 7, 6, 0x00));  // xor  x8,x7,x6
    prog.push_back(i_type(opcodes::OP_IMM, 9, 0x1, 8, 3));    // slli x9,x8,3
    prog.push_back(s_type(opcodes::STORE, 0x3, 5, 9, 0));     // sd   x9,0(x5)
    prog.push_back(i_type(opcodes::LOAD, 10, 0x3, 5, 0));     // ld   x10,0(x5)
    prog.push_back(r_type(opcodes::OP, 11, 0x0, 10, 9, 0x00));// add  x11,x10,x9
    prog.push_back(i_type(opcodes::OP_IMM, 12, 0x7, 11, 0xff)); // andi x12,x11,255
    const i32 back = -static_cast<i32>((prog.size() - loop) * 4);
    prog.push_back(b_type(opcodes::BRANCH, 0x1, 12, 0, back)); // bne x12,x0,loop
    prog.push_back(JAL(0, -static_cast<i32>((prog.size() - loop) * 4)));

    Machine m(prog);

    if (devices) {
        auto clint = std::make_unique<Clint>();
        auto plic  = std::make_unique<Plic>();
        auto uart  = std::make_unique<Uart>(std::cout);
        m.cpu->clint = clint.get();
        m.cpu->plic  = plic.get();
        m.cpu->uart  = uart.get();
        m.cpu->uart_irq = UART0_IRQ;
        m.bus.attach(std::move(clint));
        m.bus.attach(std::move(plic));
        m.bus.attach(std::move(uart));
    }

    if (paged) {
        // One 1 GiB identity-mapped superpage covering DRAM: VPN[2] = 2 maps
        // 0x8000_0000..0xBFFF_FFFF. The root table sits in DRAM, clear of the
        // code and the scratch word.
        const u64 root = DRAM_BASE + 0x20000;
        const u64 leaf = ((DRAM_BASE >> 12) << 10) | pte::V | pte::R | pte::W | pte::X |
                         pte::A | pte::D;
        m.dram->store(root - DRAM_BASE + 2 * 8, 8, leaf);
        m.cpu->csrs.write(csr::SATP, (8ull << 60) | (root >> 12));
        m.cpu->priv = PRIV_SUPERVISOR;
        m.cpu->trap_fatal_without_handler = false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    u64 retired = 0;
    Status st = m.cpu->run(steps, &retired);
    const auto t1 = std::chrono::steady_clock::now();

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    std::printf("%s%s%s: %llu instructions in %.3f s = %.1f MIPS\n",
                st ? "ok" : "TRAPPED", paged ? " [paged]" : "", devices ? " [devices]" : "",
                static_cast<unsigned long long>(retired), secs,
                static_cast<double>(retired) / secs / 1e6);
    return st ? 0 : 1;
}
