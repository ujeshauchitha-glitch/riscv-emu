#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "bus.hpp"
#include "cpu.hpp"
#include "dram.hpp"
#include "types.hpp"

namespace {

void print_usage(const char* argv0) {
    std::cerr
        << "usage: " << argv0 << " [options] [image.bin]\n"
        << "\n"
        << "  Runs a flat binary image loaded at 0x" << std::hex << DRAM_BASE << std::dec << ".\n"
        << "  With no image, runs a small built-in demo exercising the OP-IMM instructions.\n"
        << "\n"
        << "options:\n"
        << "  --trace              print one line per retired instruction (stderr)\n"
        << "  --max-steps N        stop after N instructions (default 1000000)\n"
        << "  --dram-size-mb N     guest RAM size in MiB (default 128)\n"
        << "  --dump               dump registers when execution stops\n"
        << "  -h, --help           show this message\n";
}

bool read_file(const std::string& path, std::vector<u8>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;

    const std::streamsize len = f.tellg();
    if (len < 0) return false;
    f.seekg(0, std::ios::beg);

    out.resize(static_cast<std::size_t>(len));
    if (len > 0 && !f.read(reinterpret_cast<char*>(out.data()), len)) return false;
    return true;
}

// A short program used when no image is supplied. It exercises every OP-IMM
// form, which is exactly the set this phase implements — and in particular the
// seven instructions that the previous code silently executed as ADDI.
//
//   addi  t0, zero, 5        t0 = 5
//   addi  t1, zero, -3       t1 = -3          (sign-extended immediate)
//   slti  t2, t1, 0          t2 = 1           (signed: -3 < 0)
//   sltiu s0, t1, 0          s0 = 0           (unsigned: huge, not < 0)
//   xori  s1, t0, 0xf        s1 = 5 ^ 15 = 10
//   ori   a0, t0, 0x8        a0 = 5 | 8  = 13
//   andi  a1, t0, 0x6        a1 = 5 & 6  = 4
//   slli  a2, t0, 4          a2 = 5 << 4 = 80
//   srli  a3, t1, 60         a3 = 0xF          (logical: zeros shifted in)
//   srai  a4, t1, 60         a4 = -1           (arithmetic: sign shifted in)
//   ebreak                   stop (traps; not implemented until phase 2)
const std::vector<u32> kDemoProgram = {
    0x00500293,  // addi  t0, zero, 5
    0xffd00313,  // addi  t1, zero, -3
    0x00032393,  // slti  t2, t1, 0
    0x00033413,  // sltiu s0, t1, 0
    0x00f2c493,  // xori  s1, t0, 15
    0x0082e513,  // ori   a0, t0, 8
    0x0062f593,  // andi  a1, t0, 6
    0x00429613,  // slli  a2, t0, 4
    0x03c35693,  // srli  a3, t1, 60
    0x43c35713,  // srai  a4, t1, 60
    0x00100073,  // ebreak
};

std::vector<u8> encode_program(const std::vector<u32>& words) {
    std::vector<u8> bytes;
    bytes.reserve(words.size() * 4);
    for (u32 w : words) {
        bytes.push_back(static_cast<u8>(w & 0xff));
        bytes.push_back(static_cast<u8>((w >> 8) & 0xff));
        bytes.push_back(static_cast<u8>((w >> 16) & 0xff));
        bytes.push_back(static_cast<u8>((w >> 24) & 0xff));
    }
    return bytes;
}

}  // namespace

int main(int argc, char** argv) {
    std::string image_path;
    bool        trace        = false;
    bool        dump         = false;
    u64         max_steps    = 1'000'000;
    u64         dram_size_mb = 128;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        auto next_value = [&](u64& out) -> bool {
            if (i + 1 >= argc) {
                std::cerr << "error: " << arg << " requires a value\n";
                return false;
            }
            out = std::strtoull(argv[++i], nullptr, 0);
            return true;
        };

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--trace") {
            trace = true;
        } else if (arg == "--dump") {
            dump = true;
        } else if (arg == "--max-steps") {
            if (!next_value(max_steps)) return 2;
        } else if (arg == "--dram-size-mb") {
            if (!next_value(dram_size_mb)) return 2;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option " << arg << "\n";
            print_usage(argv[0]);
            return 2;
        } else {
            image_path = arg;
        }
    }

    if (dram_size_mb == 0) {
        std::cerr << "error: --dram-size-mb must be at least 1\n";
        return 2;
    }

    // Build the machine: a bus with DRAM attached at 0x8000_0000.
    Bus  bus;
    auto dram = std::make_unique<Dram>(dram_size_mb * 1024 * 1024);
    Dram* dram_ptr = dram.get();
    if (!bus.attach(std::move(dram))) {
        std::cerr << "error: failed to attach DRAM to the bus\n";
        return 1;
    }

    std::vector<u8> image;
    if (image_path.empty()) {
        image = encode_program(kDemoProgram);
        std::cerr << "no image given; running built-in OP-IMM demo\n";
    } else if (!read_file(image_path, image)) {
        std::cerr << "error: cannot read image '" << image_path << "'\n";
        return 1;
    }

    if (!dram_ptr->load_image(DRAM_BASE, image)) {
        std::cerr << "error: image (" << image.size() << " bytes) does not fit in "
                  << dram_size_mb << " MiB of DRAM\n";
        return 1;
    }

    Cpu cpu(bus);
    cpu.trace = trace;
    cpu.pc    = DRAM_BASE;

    u64    retired = 0;
    Status st      = cpu.run(max_steps, &retired);

    if (!st) {
        std::cerr << "\nstopped after " << retired << " instruction(s): "
                  << exception_name(st.trap.cause)
                  << " (cause " << st.trap.cause_code()
                  << ", tval 0x" << std::hex << st.trap.tval << std::dec << ")"
                  << " at pc 0x" << std::hex << cpu.pc << std::dec << "\n";
    } else {
        std::cerr << "\nstep budget exhausted after " << retired << " instruction(s)\n";
    }

    // Until phase 2 gives us real trap handling, any trap ends the run, so a
    // register dump is the only way to see what happened. Show it by default
    // for the built-in demo.
    if (dump || image_path.empty()) {
        cpu.dump_registers(std::cout);
    }

    return st ? 0 : 1;
}
