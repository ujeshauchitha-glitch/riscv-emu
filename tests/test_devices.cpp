#include <sstream>
#include <vector>

#include "clint.hpp"
#include "csr.hpp"
#include "elf_loader.hpp"
#include "machine.hpp"
#include "syscon.hpp"
#include "test_util.hpp"
#include "uart.hpp"

// ---------------------------------------------------------------------------
// Device tests: UART, CLINT, syscon, and the ELF loader.
// ---------------------------------------------------------------------------

using namespace rvt;

namespace {

// --- UART -------------------------------------------------------------------

void test_uart_writes_appear_on_the_output() {
    std::ostringstream out;
    Uart uart(out);
    for (char c : std::string("hi!")) uart.store(0, 1, static_cast<u64>(c));
    CHECK(out.str() == "hi!");
}

void test_uart_lsr_reports_transmitter_ready() {
    std::ostringstream out;
    Uart uart(out);
    auto lsr = uart.load(5, 1);
    CHECK(lsr);
    // Bit 5 (THR empty) is always set: a write completes immediately because
    // there is no real serial line to wait for.
    CHECK((lsr.value & (1 << 5)) != 0);
    // Bit 0 (data ready) is clear until something is queued.
    CHECK((lsr.value & 1) == 0);

    uart.feed_input("x");
    lsr = uart.load(5, 1);
    CHECK((lsr.value & 1) != 0);
}

void test_uart_reads_queued_input() {
    std::ostringstream out;
    Uart uart(out);
    uart.feed_input("ab");

    auto a = uart.load(0, 1);
    auto b = uart.load(0, 1);
    auto empty = uart.load(0, 1);
    CHECK_EQ_U(a.value, 'a');
    CHECK_EQ_U(b.value, 'b');
    CHECK_EQ_U(empty.value, 0);   // nothing left
}

void test_uart_iir_names_the_interrupt_cause() {
    // Bit 0 clear means an interrupt is pending, but that is not the whole
    // answer: bits 3:1 say *which* condition caused it, and a real driver
    // dispatches on that field rather than going looking.
    //
    //   0b001  transmitter holding register empty
    //   0b010  received data available
    //
    // Returning a bare "something happened" leaves the field at 000, which
    // means "modem status change" - so Linux's 8250 driver read the modem
    // status register, found nothing, and never touched the receive buffer.
    // The console printed perfectly and could not be typed at. xv6 never reads
    // IIR at all, which is why that went unnoticed for a whole phase.
    std::ostringstream out;
    Uart uart(out);

    // Nothing enabled, nothing waiting: bit 0 set means no interrupt.
    auto iir = uart.load(2, 1);
    CHECK(iir);
    CHECK((iir.value & 1) != 0);

    // A queued byte with receive interrupts enabled reports "data available".
    uart.store(1, 1, 0x01);          // IER: receive
    uart.feed_input("x");
    iir = uart.load(2, 1);
    CHECK(iir);
    CHECK((iir.value & 1) == 0);                 // pending
    CHECK_EQ_U((iir.value >> 1) & 0x7, 0x2);     // received data available

    // Reading IIR must not consume the byte - only reading RBR does that.
    auto rbr = uart.load(0, 1);
    CHECK(rbr);
    CHECK_EQ_U(rbr.value, 'x');

    // With the queue drained and transmit interrupts enabled, a sent byte
    // reports "transmitter empty" instead.
    uart.store(1, 1, 0x02);          // IER: transmit
    uart.store(0, 1, 'h');           // THR
    iir = uart.load(2, 1);
    CHECK(iir);
    CHECK((iir.value & 1) == 0);
    CHECK_EQ_U((iir.value >> 1) & 0x7, 0x1);     // THR empty

    // And reading IIR acknowledges that one, so it does not repeat.
    iir = uart.load(2, 1);
    CHECK(iir);
    CHECK((iir.value & 1) != 0);

    // Received data outranks a free transmitter: an unread byte is lost if the
    // next one arrives, while an idle transmitter will still be idle later.
    uart.store(1, 1, 0x03);          // IER: both
    uart.store(0, 1, 'h');           // make the transmitter empty again
    uart.feed_input("y");
    iir = uart.load(2, 1);
    CHECK(iir);
    CHECK_EQ_U((iir.value >> 1) & 0x7, 0x2);     // receive wins

    // Bits 7:6 advertise working FIFOs, which is what makes a driver identify
    // this as a 16550A rather than an earlier part.
    CHECK_EQ_U((iir.value >> 6) & 0x3, 0x3);
}

void test_uart_dlab_switches_register_bank() {
    // With DLAB set, registers 0 and 1 become the baud-rate divisor rather than
    // data and interrupt-enable. A UART that ignored this would print the
    // divisor bytes a real driver writes during initialisation.
    std::ostringstream out;
    Uart uart(out);

    uart.store(3, 1, 0x80);        // LCR: set DLAB
    uart.store(0, 1, 0x03);        // this is the divisor low byte, not a char
    uart.store(1, 1, 0x00);
    CHECK(out.str().empty());      // nothing was printed

    auto dll = uart.load(0, 1);
    CHECK_EQ_U(dll.value, 0x03);

    uart.store(3, 1, 0x03);        // clear DLAB, 8 data bits
    uart.store(0, 1, 'Z');         // now this really is a character
    CHECK(out.str() == "Z");
}

// --- CLINT ------------------------------------------------------------------

void test_clint_mtime_advances_with_instructions() {
    Clint clint;
    CHECK_EQ_U(clint.mtime(), 0);
    clint.tick();
    clint.tick();
    CHECK_EQ_U(clint.mtime(), 2);

    auto r = clint.load(0xbff8, 8);
    CHECK(r && r.value == 2);
}

void test_clint_raises_timer_interrupt_at_the_deadline() {
    Clint clint;
    CsrFile csrs;

    clint.store(0x4000, 8, 5);   // mtimecmp = 5
    clint.update(csrs);
    CHECK((csrs.read(csr::MIP) & csr::MIP_MTIP) == 0);   // not yet

    for (int i = 0; i < 5; ++i) clint.tick();
    clint.update(csrs);
    CHECK((csrs.read(csr::MIP) & csr::MIP_MTIP) != 0);   // mtime == mtimecmp

    // Moving the deadline forward is how a kernel acknowledges the interrupt -
    // it cannot clear MTIP by writing mip, because the device owns that bit.
    clint.store(0x4000, 8, 100);
    clint.update(csrs);
    CHECK((csrs.read(csr::MIP) & csr::MIP_MTIP) == 0);
}

void test_clint_software_interrupt() {
    Clint clint;
    CsrFile csrs;
    clint.store(0x4000, 8, ~0ull);   // push the timer far away

    clint.store(0, 4, 1);            // msip = 1
    clint.update(csrs);
    CHECK((csrs.read(csr::MIP) & csr::MIP_MSIP) != 0);

    clint.store(0, 4, 0);
    clint.update(csrs);
    CHECK((csrs.read(csr::MIP) & csr::MIP_MSIP) == 0);
}

void test_clint_mtimecmp_halves() {
    // 32-bit kernels write mtimecmp as two halves, so both widths must work.
    Clint clint;
    clint.store(0x4000, 4, 0xdeadbeef);
    clint.store(0x4004, 4, 0x12345678);
    auto r = clint.load(0x4000, 8);
    CHECK(r && r.value == 0x12345678deadbeefull);
}

// --- syscon -----------------------------------------------------------------

void test_syscon_poweroff_and_exit_code() {
    Syscon s;
    CHECK(!s.poweroff_requested());
    s.store(0, 4, Syscon::POWEROFF);
    CHECK(s.poweroff_requested());
    CHECK_EQ_U(s.exit_code(), 0);

    Syscon t;
    // riscv-tests packs a failure code into the upper bits.
    t.store(0, 4, (3ull << 16) | Syscon::POWEROFF);
    CHECK(t.poweroff_requested());
    CHECK_EQ_U(t.exit_code(), 3);

    Syscon u;
    u.store(0, 4, Syscon::REBOOT);
    CHECK(u.reboot_requested());
    CHECK(!u.poweroff_requested());
}

// --- integration: a guest program driving the devices -----------------------

// Builds a machine with all four devices, runs a program, returns UART output.
struct FullMachine {
    Bus                  bus;
    std::ostringstream   out;
    Dram*                dram = nullptr;
    Clint*               clint = nullptr;
    Syscon*              syscon = nullptr;
    std::unique_ptr<Cpu> cpu;

    explicit FullMachine(const std::vector<u32>& program) {
        auto d = std::make_unique<Dram>(1024 * 1024);
        dram = d.get();
        auto u = std::make_unique<Uart>(out);
        auto c = std::make_unique<Clint>();
        clint = c.get();
        auto s = std::make_unique<Syscon>();
        syscon = s.get();
        bus.attach(std::move(d));
        bus.attach(std::move(u));
        bus.attach(std::move(c));
        bus.attach(std::move(s));

        std::vector<u8> bytes;
        for (u32 w : program)
            for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<u8>((w >> (8 * b)) & 0xff));
        dram->load_image(DRAM_BASE, bytes);

        cpu = std::make_unique<Cpu>(bus);
        cpu->clint  = clint;
        cpu->syscon = syscon;
    }
};

void test_guest_can_print_and_power_off() {
    std::vector<u32> p;
    load_imm64(p, 1, UART0_BASE);
    load_imm64(p, 2, 'O');
    p.push_back(s_type(opcodes::STORE, 0x0, 1, 2, 0));   // sb 'O' -> console
    load_imm64(p, 2, 'K');
    p.push_back(s_type(opcodes::STORE, 0x0, 1, 2, 0));
    load_imm64(p, 1, SYSCON_BASE);
    load_imm64(p, 2, Syscon::POWEROFF);
    p.push_back(s_type(opcodes::STORE, 0x2, 1, 2, 0));   // sw poweroff
    p.push_back(HALT());                                 // must not be reached

    FullMachine m(p);
    const Status st = m.cpu->run(500, nullptr);

    CHECK(st);                       // powering off is not a failure
    CHECK(m.cpu->halted);
    CHECK(m.out.str() == "OK");
}

void test_timer_interrupt_reaches_the_guest_handler() {
    // The full path: the CLINT's counter passes mtimecmp, the device asserts
    // MTIP, the CPU notices it before the next fetch, and the guest's handler
    // runs. Nothing here writes mip directly.
    const u64 K = kLoadImm64Steps;
    const u64 handler = DRAM_BASE + (4 * K + 5) * 4;

    std::vector<u32> p;
    load_imm64(p, 1, handler);                                    // [0,K)
    load_imm64(p, 2, CLINT_BASE + 0x4000);                        // [K,2K)
    load_imm64(p, 3, 50);                                         // [2K,3K)  deadline
    load_imm64(p, 4, csr::MIE_MTIE);                              // [3K,4K)
    p.push_back(i_type(opcodes::SYSTEM, 0, 0x1, 1, csr::MTVEC));  // 4K   mtvec
    p.push_back(s_type(opcodes::STORE, 0x3, 2, 3, 0));            // 4K+1 mtimecmp = 50
    p.push_back(i_type(opcodes::SYSTEM, 0, 0x1, 4, csr::MIE));    // 4K+2 mie = MTIE
    p.push_back(i_type(opcodes::SYSTEM, 0, 0x6, 8, csr::MSTATUS));// 4K+3 mstatus.MIE
    p.push_back(HALT());                                          // 4K+4 spin
    p.push_back(ADDI(9, 0, 123));                                 // 4K+5 handler
    p.push_back(HALT());                                          // 4K+6

    FullMachine m(p);
    m.cpu->run(500, nullptr);

    CHECK_EQ_U(m.cpu->read_reg(9), 123);
}

// --- ELF loader -------------------------------------------------------------

// Builds a minimal ELF64 RISC-V file with one PT_LOAD segment.
std::vector<u8> make_elf(u64 entry, u64 paddr, const std::vector<u8>& payload,
                         u64 memsz_extra = 0) {
    std::vector<u8> e(64 + 56, 0);
    auto put16 = [&](u64 o, u16 v) { e[o] = v & 0xff; e[o+1] = v >> 8; };
    auto put32 = [&](u64 o, u32 v) { for (int i=0;i<4;i++) e[o+i] = (v >> (8*i)) & 0xff; };
    auto put64 = [&](u64 o, u64 v) { for (int i=0;i<8;i++) e[o+i] = (v >> (8*i)) & 0xff; };

    e[0]=0x7f; e[1]='E'; e[2]='L'; e[3]='F';
    e[4]=2;    // ELFCLASS64
    e[5]=1;    // little-endian
    e[6]=1;    // version
    put16(16, 2);        // ET_EXEC
    put16(18, 243);      // EM_RISCV
    put32(20, 1);
    put64(24, entry);
    put64(32, 64);       // e_phoff
    put16(52, 64);       // e_ehsize
    put16(54, 56);       // e_phentsize
    put16(56, 1);        // e_phnum

    const u64 ph = 64;
    put32(ph + 0, 1);                       // PT_LOAD
    put32(ph + 4, 5);                       // flags: R+X
    put64(ph + 8, 120);                     // p_offset (payload follows)
    put64(ph + 16, paddr);                  // p_vaddr
    put64(ph + 24, paddr);                  // p_paddr
    put64(ph + 32, payload.size());         // p_filesz
    put64(ph + 40, payload.size() + memsz_extra);  // p_memsz
    put64(ph + 48, 8);                      // p_align

    e.insert(e.end(), payload.begin(), payload.end());
    return e;
}

void test_elf_detection() {
    CHECK(is_elf({0x7f, 'E', 'L', 'F', 0}));
    CHECK(!is_elf({0x00, 0x50, 0x02, 0x93}));   // a plain instruction word
    CHECK(!is_elf({}));
}

void test_elf_loads_segment_at_paddr_and_reports_entry() {
    Bus bus;
    auto d = std::make_unique<Dram>(1024 * 1024);
    Dram* dram = d.get();
    bus.attach(std::move(d));

    const std::vector<u8> payload = {0xde, 0xad, 0xbe, 0xef};
    const auto elf = make_elf(DRAM_BASE + 0x100, DRAM_BASE + 0x200, payload);

    const LoadedImage img = load_elf(elf, bus);
    CHECK(img.ok);
    CHECK_EQ_U(img.entry, DRAM_BASE + 0x100);

    auto r = bus.load(DRAM_BASE + 0x200, 4, AccessType::Load);
    CHECK(r && r.value == 0xefbeaddeull);
    (void)dram;
}

void test_elf_zero_fills_bss() {
    // p_memsz > p_filesz means .bss: storage the program expects to exist and
    // be zero, but which takes no space in the file. Skipping the zero-fill
    // leaves a kernel's globals full of whatever was in RAM.
    Bus bus;
    auto d = std::make_unique<Dram>(1024 * 1024);
    Dram* dram = d.get();
    bus.attach(std::move(d));

    // Dirty the region first so a missing zero-fill would be visible.
    for (int i = 0; i < 16; ++i) bus.store(DRAM_BASE + 0x300 + i, 1, 0xff);

    const std::vector<u8> payload = {1, 2, 3, 4};
    const auto elf = make_elf(DRAM_BASE, DRAM_BASE + 0x300, payload, /*memsz_extra=*/8);
    CHECK(load_elf(elf, bus).ok);

    auto tail = bus.load(DRAM_BASE + 0x304, 8, AccessType::Load);
    CHECK(tail && tail.value == 0);
    (void)dram;
}

void test_elf_rejects_bad_images() {
    Bus bus;
    bus.attach(std::make_unique<Dram>(1024 * 1024));

    CHECK(!load_elf({1, 2, 3}, bus).ok);                    // not ELF
    auto e32 = make_elf(DRAM_BASE, DRAM_BASE, {1});
    e32[4] = 1;                                             // ELFCLASS32
    CHECK(!load_elf(e32, bus).ok);
    auto ex86 = make_elf(DRAM_BASE, DRAM_BASE, {1});
    ex86[18] = 62;                                          // EM_X86_64
    CHECK(!load_elf(ex86, bus).ok);
    // A segment aimed at unmapped memory must be refused, not silently dropped.
    CHECK(!load_elf(make_elf(0, 0x1000, {1, 2, 3, 4}), bus).ok);
}

}  // namespace

int main() {
    test_uart_writes_appear_on_the_output();
    test_uart_lsr_reports_transmitter_ready();
    test_uart_reads_queued_input();
    test_uart_iir_names_the_interrupt_cause();
    test_uart_dlab_switches_register_bank();
    test_clint_mtime_advances_with_instructions();
    test_clint_raises_timer_interrupt_at_the_deadline();
    test_clint_software_interrupt();
    test_clint_mtimecmp_halves();
    test_syscon_poweroff_and_exit_code();
    test_guest_can_print_and_power_off();
    test_timer_interrupt_reaches_the_guest_handler();
    test_elf_detection();
    test_elf_loads_segment_at_paddr_and_reports_entry();
    test_elf_zero_fills_bss();
    test_elf_rejects_bad_images();
    return testutil::summary("devices");
}
