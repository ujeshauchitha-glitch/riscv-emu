#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "bus.hpp"
#include "clint.hpp"
#include "cpu.hpp"
#include "dram.hpp"
#include "elf_loader.hpp"
#include "plic.hpp"
#include "syscon.hpp"
#include "types.hpp"
#include "fdt.hpp"
#include "uart.hpp"
#include "virtio.hpp"

namespace {

void print_usage(const char* argv0) {
    std::cerr
        << "usage: " << argv0 << " [options] <image>\n"
        << "\n"
        << "  Runs an ELF64 RISC-V image, or a flat binary loaded at 0x"
        << std::hex << DRAM_BASE << std::dec << ".\n"
        << "  The format is detected from the file's magic number.\n"
        << "  With no image, runs a built-in demo that prints over the UART.\n"
        << "\n"
        << "options:\n"
        << "  --trace              print one line per retired instruction (stderr)\n"
        << "  --max-steps N        stop after N instructions (default 100000000)\n"
        << "  --dram-size-mb N     guest RAM size in MiB (default 128)\n"
        << "  --timer-divisor N    instructions per mtime tick (default 1)\n"
        << "  --dump               dump registers when execution stops\n"
        << "  --disk FILE          back the virtio block device with FILE\n"
        << "\n"
        << "  Booting Linux (see docs/09-booting-linux.md):\n"
        << "  --linux              start in supervisor mode with a generated device\n"
        << "                       tree in a1 and SBI firmware services enabled,\n"
        << "                       which is what a real bootloader hands a kernel\n"
        << "  --bootargs STR       the kernel command line placed in /chosen\n"
        << "  --initrd FILE        load an initramfs and point the kernel at it\n"
        << "  --dump-dtb FILE      write the generated device tree out and exit\n"
        << "  -h, --help           show this message\n"
        << "\n"
        << "  To boot xv6, use scripts/boot-xv6.sh - it fetches and builds it\n"
        << "  for you. Note that a real kernel needs a much larger --max-steps\n"
        << "  than the default: xv6 reaches its shell at around 500 million.\n";
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

// A built-in demo, used when no image is given: write "hello, RISC-V\n" to the
// UART one byte at a time, then power off through syscon. This is the smallest
// program that demonstrates what phase 4 added - a guest that can be heard from
// and can stop on its own terms.
//
//   la   t0, UART0        (lui + addi)
//   la   t1, message
//   loop: lbu t2, 0(t1); beqz t2 -> done; sb t2, 0(t0); addi t1,t1,1; j loop
//   done: la t0, SYSCON; li t1, 0x5555; sw t1, 0(t0)
std::vector<u32> demo_program() {
    return {
        //  0
        0x100002b7,  // lui   t0, 0x10000    -> t0 = UART0_BASE (0x1000_0000)
        // Note this uses auipc rather than `lui t1, 0x80000`. On RV64 LUI
        // sign-extends from bit 31, so that would give 0xFFFFFFFF80000000, not
        // 0x0000000080000000. auipc sidesteps it and is what real
        // position-independent code uses anyway.
        0x00000317,  // auipc t1, 0          -> t1 = address of this instruction
        0x03c30313,  // addi  t1, t1, 60     -> the message, at byte offset 64
        // loop (index 3):
        0x00034383,  // lbu   t2, 0(t1)
        0x00038863,  // beqz  t2, done       -> +16, to index 8
        0x00728023,  // sb    t2, 0(t0)      -> the byte appears on the console
        0x00130313,  // addi  t1, t1, 1
        0xff1ff06f,  // j     loop           -> -16, back to index 3
        // done (index 8):
        0x001002b7,  // lui   t0, 0x100      -> t0 = SYSCON_BASE (0x0010_0000)
        0x000053b7,  // lui   t2, 0x5        -> 0x5000
        0x5553839b,  // addiw t2, t2, 0x555  -> 0x5555, the poweroff word
        0x0072a023,  // sw    t2, 0(t0)      -> stop the machine
        0x0000006f,  // j     .              (never reached)
    };
}

const char* kDemoMessage = "hello, RISC-V\n";

}  // namespace

int main(int argc, char** argv) {
    std::string image_path;
    std::string disk_path;
    bool        trace         = false;
    bool        dump          = false;
    u64         max_steps     = 100'000'000;
    u64         dram_size_mb  = 128;
    u64         timer_divisor = 1;
    bool        linux_boot    = false;
    std::string bootargs      = "console=ttyS0 earlycon=sbi";
    std::string initrd_path;
    std::string dtb_out_path;

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

        if (arg == "-h" || arg == "--help") { print_usage(argv[0]); return 0; }
        else if (arg == "--trace") trace = true;
        else if (arg == "--dump")  dump = true;
        else if (arg == "--disk") {
            if (i + 1 >= argc) { std::cerr << "error: --disk requires a path\n"; return 2; }
            disk_path = argv[++i];
        }
        else if (arg == "--linux") linux_boot = true;
        else if (arg == "--bootargs") {
            if (i + 1 >= argc) { std::cerr << "error: --bootargs requires a value\n"; return 2; }
            bootargs = argv[++i];
        }
        else if (arg == "--initrd") {
            if (i + 1 >= argc) { std::cerr << "error: --initrd requires a path\n"; return 2; }
            initrd_path = argv[++i];
        }
        else if (arg == "--dump-dtb") {
            if (i + 1 >= argc) { std::cerr << "error: --dump-dtb requires a path\n"; return 2; }
            dtb_out_path = argv[++i];
        }
        else if (arg == "--max-steps")     { if (!next_value(max_steps)) return 2; }
        else if (arg == "--dram-size-mb")  { if (!next_value(dram_size_mb)) return 2; }
        else if (arg == "--timer-divisor") { if (!next_value(timer_divisor)) return 2; }
        else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option " << arg << "\n";
            print_usage(argv[0]);
            return 2;
        } else image_path = arg;
    }

    if (dram_size_mb == 0 || timer_divisor == 0) {
        std::cerr << "error: --dram-size-mb and --timer-divisor must be at least 1\n";
        return 2;
    }

    // --- build the machine ---
    Bus bus;

    auto  dram_owned = std::make_unique<Dram>(dram_size_mb * 1024 * 1024);
    Dram* dram = dram_owned.get();
    auto  uart_owned = std::make_unique<Uart>(std::cout);
    auto  clint_owned = std::make_unique<Clint>();
    Clint* clint = clint_owned.get();
    auto  syscon_owned = std::make_unique<Syscon>();
    Syscon* syscon = syscon_owned.get();
    auto  plic_owned = std::make_unique<Plic>();
    Plic* plic = plic_owned.get();
    auto  virtio_owned = std::make_unique<VirtioBlk>();
    VirtioBlk* virtio = virtio_owned.get();

    // Keep a borrowed pointer to the UART before the bus takes ownership: the
    // CPU samples its interrupt line every step.
    Uart* uart = uart_owned.get();

    if (!bus.attach(std::move(dram_owned)) || !bus.attach(std::move(uart_owned)) ||
        !bus.attach(std::move(clint_owned)) || !bus.attach(std::move(syscon_owned)) ||
        !bus.attach(std::move(plic_owned)) || !bus.attach(std::move(virtio_owned))) {
        std::cerr << "error: failed to build the machine's address map\n";
        return 1;
    }
    clint->ticks_per_instruction = 1;

    // The block device is a bus master - it reads and writes guest memory
    // itself - and raises its interrupt through the PLIC.
    virtio->attach(&bus, plic, VIRTIO0_IRQ);

    if (!disk_path.empty()) {
        if (!virtio->load_image(disk_path)) {
            std::cerr << "error: cannot read disk image '" << disk_path << "'\n";
            return 1;
        }
        std::cerr << "disk: " << disk_path << " (" << virtio->sectors()
                  << " sectors)\n";
    }

    // --- load the image ---
    u64 entry = DRAM_BASE;
    u64 tohost = 0;
    if (image_path.empty()) {
        std::vector<u8> bytes;
        for (u32 w : demo_program()) {
            for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<u8>((w >> (8 * b)) & 0xff));
        }
        bytes.resize(64, 0);   // the message lives at offset 64
        for (const char* p = kDemoMessage; *p; ++p) bytes.push_back(static_cast<u8>(*p));
        bytes.push_back(0);
        dram->load_image(DRAM_BASE, bytes);
        std::cerr << "no image given; running the built-in UART demo\n";
    } else {
        std::vector<u8> bytes;
        if (!read_file(image_path, bytes)) {
            std::cerr << "error: cannot read image '" << image_path << "'\n";
            return 1;
        }
        if (is_elf(bytes)) {
            const LoadedImage img = load_elf(bytes, bus);
            if (!img.ok) {
                std::cerr << "error: " << img.error << "\n";
                return 1;
            }
            entry = img.entry;
            tohost = img.tohost;
        } else {
            // A flat binary normally goes at the start of DRAM. A Linux kernel
            // Image is the exception: it carries a header saying where it wants
            // to be.
            //
            //   offset 0x00  code0, code1   a jump, so the header is executable
            //   offset 0x08  text_offset    where to load it, relative to the
            //                               2 MiB-aligned start of memory
            //   offset 0x38  magic2         "RSC\x05"
            //
            // text_offset is 2 MiB on RV64, because that is where firmware
            // conventionally ends. Loading the kernel at the start of DRAM
            // instead puts every symbol 2 MiB away from where it was linked,
            // and it dies immediately on its first absolute reference.
            u64 load_at = DRAM_BASE;
            if (bytes.size() >= 0x40) {
                u32 magic2 = 0;
                for (int b = 0; b < 4; ++b) {
                    magic2 |= static_cast<u32>(bytes[0x38 + b]) << (8 * b);
                }
                if (magic2 == 0x0543'5352) {   // "RSC\x05"
                    u64 text_offset = 0;
                    for (int b = 0; b < 8; ++b) {
                        text_offset |= static_cast<u64>(bytes[8 + b]) << (8 * b);
                    }
                    load_at = DRAM_BASE + text_offset;
                    entry   = load_at;
                    std::cerr << "linux kernel image, text_offset 0x" << std::hex
                              << text_offset << " -> loading at 0x" << load_at
                              << std::dec << "\n";
                }
            }
            if (!dram->load_image(load_at, bytes)) {
                std::cerr << "error: image (" << bytes.size() << " bytes) does not fit in "
                          << dram_size_mb << " MiB of DRAM\n";
                return 1;
            }
        }
    }

    // --- Linux boot: device tree, initramfs, and supervisor mode ---
    //
    // A RISC-V kernel expects to be entered by firmware with a0 = the hart ID
    // and a1 = the address of a device tree. Everything it knows about the
    // machine comes from that blob, so it is built here from the same constants
    // the devices were attached with and cannot drift out of agreement.
    u64 dtb_addr = 0;
    u64 initrd_start = 0, initrd_end = 0;
    const u64 dram_bytes = dram_size_mb * 1024 * 1024;

    if (linux_boot || !dtb_out_path.empty()) {
        // The initramfs goes high in memory, well clear of the kernel, and the
        // device tree just below it. Putting them at the top rather than
        // immediately after the kernel means their placement does not depend on
        // how big the kernel turned out to be.
        if (!initrd_path.empty()) {
            std::vector<u8> initrd;
            if (!read_file(initrd_path, initrd)) {
                std::cerr << "error: cannot read initrd '" << initrd_path << "'\n";
                return 1;
            }
            initrd_start = DRAM_BASE + dram_bytes - 64 * 1024 * 1024;
            initrd_end   = initrd_start + initrd.size();
            if (!dram->load_image(initrd_start, initrd)) {
                std::cerr << "error: initrd does not fit in DRAM\n";
                return 1;
            }
            std::cerr << "initrd: " << initrd_path << " (" << initrd.size()
                      << " bytes at 0x" << std::hex << initrd_start << std::dec << ")\n";
        }

        const std::vector<u8> dtb =
            Fdt::build(dram_bytes, bootargs, initrd_start, initrd_end);

        if (!dtb_out_path.empty()) {
            std::ofstream out(dtb_out_path, std::ios::binary);
            if (!out) {
                std::cerr << "error: cannot write '" << dtb_out_path << "'\n";
                return 1;
            }
            out.write(reinterpret_cast<const char*>(dtb.data()),
                      static_cast<std::streamsize>(dtb.size()));
            std::cerr << "device tree written to " << dtb_out_path << " ("
                      << dtb.size() << " bytes)\n";
            if (!linux_boot) return 0;
        }

        // Two megabytes below the initramfs, or below the top of memory when
        // there is none.
        const u64 top = initrd_start != 0 ? initrd_start : DRAM_BASE + dram_bytes;
        dtb_addr = (top - 2 * 1024 * 1024) & ~0xfffull;
        if (!dram->load_image(dtb_addr, dtb)) {
            std::cerr << "error: device tree does not fit in DRAM\n";
            return 1;
        }
    }

    Cpu cpu(bus);
    cpu.trace  = trace;
    cpu.pc     = entry;
    cpu.clint  = clint;
    cpu.syscon = syscon;
    cpu.htif_tohost_addr = tohost;
    cpu.plic     = plic;
    cpu.uart     = uart;
    cpu.uart_irq = UART0_IRQ;

    if (linux_boot) {
        // Enter the kernel the way OpenSBI would: in supervisor mode, with the
        // hart ID in a0 and the device tree in a1, and with SBI available for
        // the things supervisor mode cannot do for itself.
        cpu.priv = PRIV_SUPERVISOR;
        cpu.write_reg(10, 0);          // a0 = hartid
        cpu.write_reg(11, dtb_addr);   // a1 = device tree
        cpu.sbi_enabled = true;

        // Linux installs stvec early but not instantly, and it delegates
        // nothing to itself - so the "no handler installed" guard, which is a
        // debugging aid rather than architectural behaviour, would fire on the
        // first page fault of the boot. A kernel with real firmware underneath
        // it would simply take the trap.
        cpu.trap_fatal_without_handler = false;

        std::cerr << "device tree at 0x" << std::hex << dtb_addr << std::dec
                  << ", entering supervisor mode\n\n";
    }

    // The console becomes bidirectional here. Everything before this point
    // could print; from now on a guest shell can also be typed at.
    uart->attach_host_stdin();
    clint->ticks_per_instruction = timer_divisor;

    u64    retired = 0;
    Status st      = cpu.run(max_steps, &retired);

    if (cpu.htif_tohost_value != 0) {
        // The HTIF result convention used by riscv-tests: bit 0 marks the word
        // as valid, and the rest is the failing check's number (0 = all passed).
        const u64 code = cpu.htif_tohost_value >> 1;
        if (code == 0) {
            std::cerr << "\nPASS after " << retired << " instruction(s)\n";
        } else {
            std::cerr << "\nFAIL: test " << code << " failed, after " << retired
                      << " instruction(s)\n";
        }
    } else if (cpu.sbi_shutdown) {
        std::cerr << "\nguest requested shutdown through SBI, after " << retired
                  << " instruction(s)\n";
    } else if (cpu.user_quit) {
        std::cerr << "\nleft the machine after " << retired << " instruction(s)\n";
    } else if (cpu.halted) {
        std::cerr << "\nmachine powered off after " << retired << " instruction(s)";
        if (syscon->exit_code() != 0) std::cerr << " (exit code " << syscon->exit_code() << ")";
        std::cerr << "\n";
    } else if (!st) {
        std::cerr << "\nstopped after " << retired << " instruction(s): "
                  << exception_name(st.trap.cause)
                  << " (cause " << st.trap.cause_code()
                  << ", tval 0x" << std::hex << st.trap.tval << std::dec << ")"
                  << " at pc 0x" << std::hex << cpu.pc << std::dec << "\n";
    } else {
        std::cerr << "\nstep budget exhausted after " << retired << " instruction(s)\n";
    }

    if (dump) cpu.dump_registers(std::cout);

    if (cpu.htif_tohost_value != 0) {
        return (cpu.htif_tohost_value >> 1) == 0 ? 0 : 1;
    }
    if (cpu.user_quit) return 0;
    if (cpu.halted) return static_cast<int>(syscon->exit_code());
    return st ? 0 : 1;
}
