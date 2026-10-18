#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "clint.hpp"
#include "gdbstub.hpp"
#include "machine.hpp"
#include "platform.hpp"
#include "syscon.hpp"
#include "test_util.hpp"

// ---------------------------------------------------------------------------
// The GDB server.
//
// Almost everything is tested by feeding handle_packet() the same strings a real
// gdb sends and checking the reply, with a real program running underneath: a
// breakpoint is only a breakpoint if the machine really stops on it. One test
// then connects an actual TCP client to check the framing and the session loop.
// ---------------------------------------------------------------------------

using namespace rvt;

namespace {

constexpr u64 DATA = DRAM_BASE + 0x10000;

std::string hex64le(u64 v) {
    char buf[17];
    for (int i = 0; i < 8; ++i) std::snprintf(buf + 2 * i, 3, "%02x", static_cast<unsigned>((v >> (8 * i)) & 0xff));
    return buf;
}

struct Target {
    Bus bus;
    Dram* dram = nullptr;
    std::unique_ptr<Platform> platform;
    std::unique_ptr<GdbStub> stub;

    Target(const std::vector<u32>& program, u32 harts = 1) {
        auto d = std::make_unique<Dram>(kTestDramSize);
        dram = d.get();
        bus.attach(std::move(d));
        auto s = std::make_unique<Syscon>();
        Syscon* syscon = s.get();
        bus.attach(std::move(s));
        auto c = std::make_unique<Clint>(harts);
        Clint* clint = c.get();
        bus.attach(std::move(c));

        std::vector<u8> bytes;
        for (u32 w : program) for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<u8>(w >> (8 * b)));
        dram->load_image(DRAM_BASE, bytes);

        platform = std::make_unique<Platform>(bus, harts);
        platform->wire(clint, nullptr, nullptr, 0, syscon);
        for (u32 h = 0; h < harts; ++h) platform->hart(h).pc = DRAM_BASE;
        stub = std::make_unique<GdbStub>(*platform);

        // A watchdog: a `continue` that nothing stops (because the feature under
        // test is broken) must fail the test, not hang the suite. Tests that
        // want a different interrupt replace this.
        stub->interrupt_poll_interval = 64;
        stub->interrupt_requested = [n = 0]() mutable { return ++n > 2000; };
    }

    std::string ask(const std::string& packet) { return stub->handle_packet(packet); }
    u64 reg(u32 n) { return platform->hart(0).read_reg(n); }
};

// x5 counts 1, 2, 3, ... one instruction at a time, then spins.
std::vector<u32> counting_program(int n = 12) {
    std::vector<u32> p;
    p.push_back(ADDI(5, 0, 1));
    for (int i = 1; i < n; ++i) p.push_back(ADDI(5, 5, 1));
    p.push_back(HALT());
    return p;
}

// --- framing ---------------------------------------------------------------------------

void test_packets_are_framed_with_a_checksum() {
    CHECK(GdbStub::encode_packet("OK") == "$OK#9a");                    // 'O'+'K' = 0x4f+0x4b = 0x9a
    CHECK(GdbStub::encode_packet("") == "$#00");
    CHECK(GdbStub::encode_packet("T05thread:1;").size() == 1 + 12 + 3);
}

// --- reading state ------------------------------------------------------------------------

void test_stop_query_and_register_reads() {
    Target t(counting_program());
    CHECK(t.ask("?") == "T05thread:1;");

    const std::string g = t.ask("g");
    CHECK_EQ(g.size(), 33 * 16);                                        // x0..x31 and pc
    CHECK(g.substr(0, 16) == hex64le(0));                               // x0 is zero
    CHECK(g.substr(32 * 16, 16) == hex64le(DRAM_BASE));                 // pc

    CHECK(t.ask("p20") == hex64le(DRAM_BASE));                          // register 0x20 is the pc
    CHECK(t.ask("p5") == hex64le(0));

    // A control register, by gdb's numbering: 65 + the CSR number. misa is 0x301.
    char cmd[16];
    std::snprintf(cmd, sizeof cmd, "p%x", 65 + 0x301);
    const std::string misa = t.ask(cmd);
    CHECK_EQ(misa.size(), 16);
    CHECK(misa != hex64le(0));
    std::snprintf(cmd, sizeof cmd, "p%x", 65 + 0xf14);                  // mhartid
    CHECK(t.ask(cmd) == hex64le(0));

    CHECK(t.ask("pffff") == "E02");                                     // not a register
    CHECK(t.ask("pzz") == "E01");                                       // not even a number
}

void test_registers_can_be_written() {
    Target t(counting_program());
    CHECK(t.ask("P5=" + hex64le(0x1122334455667788ull)) == "OK");
    CHECK_EQ_U(t.reg(5), 0x1122334455667788ull);
    CHECK(t.ask("p5") == hex64le(0x1122334455667788ull));

    CHECK(t.ask("P0=" + hex64le(99)) == "OK");                          // x0 stays zero
    CHECK_EQ_U(t.reg(0), 0);

    CHECK(t.ask("P20=" + hex64le(DRAM_BASE + 8)) == "OK");              // set the pc
    CHECK_EQ_U(t.platform->hart(0).pc, DRAM_BASE + 8);
}

void test_memory_reads_and_writes() {
    Target t(counting_program());
    // The first instruction is addi x5,x0,1 = 0x00100293, little-endian in memory.
    CHECK(t.ask("m80000000,4") == "93021000");

    CHECK(t.ask("M80010000,4:01020304") == "OK");
    CHECK(t.ask("m80010000,4") == "01020304");
    CHECK_EQ_U(t.dram->bytes()[0x10000 + 3], 4);

    CHECK(t.ask("m0,4") == "E14");                                      // nothing there
    CHECK(t.ask("m10000000,1") == "E14");                               // device registers are off limits
    CHECK(t.ask("m80000000") == "E01");                                 // malformed
}

void test_memory_is_read_through_the_guests_page_tables() {
    Target t(counting_program());
    Cpu& cpu = t.platform->hart(0);

    // Map virtual 0x4000_0000 (VPN[2] = 1) onto the 1 GiB at 0x8000_0000 (a
    // superpage), and run the hart in supervisor mode under Sv39.
    const u64 root = DRAM_BASE + 0x20000;
    const u64 leaf = ((DRAM_BASE >> 12) << 10) | pte::V | pte::R | pte::W | pte::X | pte::A | pte::D;
    t.dram->store(root - DRAM_BASE + 1 * 8, 8, leaf);
    cpu.csrs.write(csr::SATP, (8ull << 60) | (root >> 12));
    cpu.priv = PRIV_SUPERVISOR;

    CHECK(t.ask("m40000000,4") == "93021000");                          // the same instruction, via the mapping
    CHECK(t.ask("M40010000,2:abcd") == "OK");
    CHECK_EQ_U(t.dram->bytes()[0x10000], 0xab);

    CHECK(t.ask("m50000000,4") == "E14");                               // unmapped: an error, not a guest fault
    CHECK_EQ_U(cpu.priv, PRIV_SUPERVISOR);                              // and nothing was disturbed
}

// --- running -------------------------------------------------------------------------------

void test_breakpoint_stops_the_machine_before_the_instruction() {
    Target t(counting_program());
    CHECK(t.ask("Z0,80000014,4") == "OK");                              // instruction 5
    CHECK_EQ(t.stub->breakpoint_count(), 1);

    CHECK(t.ask("c") == "T05thread:1;");
    CHECK_EQ_U(t.platform->hart(0).pc, DRAM_BASE + 20);
    CHECK_EQ_U(t.reg(5), 5);                                            // instructions 0-4 ran, 5 has not

    // Nothing in guest memory was patched to make that happen.
    CHECK(t.ask("m80000014,4") == "93821200");                          // still addi x5,x5,1
}

void test_single_step_advances_one_instruction() {
    Target t(counting_program());
    t.ask("Z0,80000014,4");
    t.ask("c");
    CHECK(t.ask("s") == "T05thread:1;");
    CHECK_EQ_U(t.platform->hart(0).pc, DRAM_BASE + 24);
    CHECK_EQ_U(t.reg(5), 6);
    t.ask("s");
    CHECK_EQ_U(t.reg(5), 7);
}

void test_continue_leaves_the_breakpoint_it_is_sitting_on() {
    Target t(counting_program());
    t.ask("Z0,80000014,4");
    t.ask("c");                                                         // stopped at the breakpoint
    CHECK_EQ_U(t.reg(5), 5);

    // Resuming must run the instruction under the breakpoint rather than stop
    // at once; here it runs on to the next breakpoint.
    t.ask("Z0,8000001c,4");                                             // instruction 7
    CHECK(t.ask("c") == "T05thread:1;");
    CHECK_EQ_U(t.platform->hart(0).pc, DRAM_BASE + 28);
    CHECK_EQ_U(t.reg(5), 7);
}

void test_removed_breakpoints_stop_stopping() {
    Target t(counting_program(40));
    t.ask("Z0,80000014,4");
    CHECK(t.ask("z0,80000014,4") == "OK");
    CHECK_EQ(t.stub->breakpoint_count(), 0);

    // Nothing to stop it; a stand-in for Ctrl-C ends the run.
    int polls = 0;
    t.stub->interrupt_poll_interval = 16;
    t.stub->interrupt_requested = [&] { return ++polls >= 5; };
    CHECK(t.ask("c") == "T02thread:1;");                                // SIGINT
    CHECK_EQ_U(t.reg(5), 40);                                           // ran right through
}

void test_a_write_watchpoint_stops_when_memory_changes() {
    // A loop that increments a counter in memory.
    std::vector<u32> p;
    load_imm64(p, 6, DATA);
    const std::size_t loop = p.size();
    p.push_back(i_type(opcodes::LOAD, 7, 0x3, 6, 0));                   // ld   x7,0(x6)
    p.push_back(ADDI(7, 7, 1));
    p.push_back(s_type(opcodes::STORE, 0x3, 6, 7, 0));                  // sd   x7,0(x6)
    p.push_back(j_type(opcodes::JAL, 0, -static_cast<i32>((p.size() - loop) * 4)));
    Target t(p);

    char cmd[48];
    std::snprintf(cmd, sizeof cmd, "Z2,%llx,8", static_cast<unsigned long long>(DATA));
    CHECK(t.ask(cmd) == "OK");

    const std::string reply = t.ask("c");
    char want[64];
    std::snprintf(want, sizeof want, "T05watch:%llx;thread:1;", static_cast<unsigned long long>(DATA));
    CHECK(reply == want);
    CHECK_EQ_U(t.dram->bytes()[0x10000], 1);                            // the first store just landed

    t.ask("c");
    CHECK_EQ_U(t.dram->bytes()[0x10000], 2);                            // and it stops again on the next

    std::snprintf(cmd, sizeof cmd, "z2,%llx,8", static_cast<unsigned long long>(DATA));
    CHECK(t.ask(cmd) == "OK");
    CHECK(t.ask("Z3,80010000,8") == "");                                // read watchpoints: unsupported, said so
}

void test_machine_exit_is_reported_as_w00() {
    std::vector<u32> p;
    load_imm64(p, 5, SYSCON_BASE);
    load_imm64(p, 6, 0x5555);
    p.push_back(s_type(opcodes::STORE, 0x2, 5, 6, 0));                  // sw x6,0(x5): power off
    p.push_back(HALT());
    Target t(p);
    CHECK(t.ask("c") == "W00");
}

void test_traps_become_signals() {
    // An EBREAK with no handler is SIGTRAP, and resuming steps over it.
    {
        std::vector<u32> p = {ADDI(5, 0, 7), 0x00100073, ADDI(5, 5, 1), HALT()};
        Target t(p);
        CHECK(t.ask("c") == "T05thread:1;");
        CHECK_EQ_U(t.platform->hart(0).pc, DRAM_BASE + 4);
        t.stub->interrupt_poll_interval = 8;
        int polls = 0;
        t.stub->interrupt_requested = [&] { return ++polls >= 3; };
        t.ask("c");
        CHECK_EQ_U(t.reg(5), 8);                                        // carried on past the ebreak
    }
    // An illegal instruction is SIGILL.
    {
        Target t({ADDI(5, 0, 1), 0x00000000});
        CHECK(t.ask("c") == "T04thread:1;");
    }
    // A wild jump is SIGSEGV.
    {
        std::vector<u32> p;
        load_imm64(p, 5, 0x1000);
        p.push_back(JALR(0, 5, 0));
        Target t(p);
        CHECK(t.ask("c") == "T0bthread:1;");
    }
}

// --- harts as threads ---------------------------------------------------------------------------

void test_every_hart_is_a_thread() {
    Target t(counting_program(), 2);
    CHECK(t.ask("qfThreadInfo") == "m1,2");
    CHECK(t.ask("qsThreadInfo") == "l");
    CHECK(t.ask("qC") == "QC1");
    CHECK(t.ask("T2") == "OK");
    CHECK(t.ask("T3") == "E01");

    // Give hart 1 a distinct pc and select it for register access.
    t.platform->hart(1).pc = DRAM_BASE + 8;
    CHECK(t.ask("Hg2") == "OK");
    CHECK(t.ask("qC") == "QC2");
    CHECK(t.ask("p20") == hex64le(DRAM_BASE + 8));
    CHECK(t.ask("Hg1") == "OK");
    CHECK(t.ask("p20") == hex64le(DRAM_BASE));

    // vCont;s:2 steps only the second hart.
    CHECK(t.ask("vCont;s:2") == "T05thread:2;");
    CHECK_EQ_U(t.platform->hart(1).pc, DRAM_BASE + 12);
    CHECK_EQ_U(t.platform->hart(0).pc, DRAM_BASE);

    std::vector<std::string> console;
    CHECK(t.stub->handle_packet("qRcmd," + std::string("6861727473"), &console) == "OK");   // "harts"
    CHECK(console.size() == 2);
    CHECK(console[0].find("hart 0") != std::string::npos);
}

void test_the_target_description_names_the_architecture() {
    Target t(counting_program());
    CHECK(t.ask("qSupported:multiprocess+").find("PacketSize=") == 0);
    CHECK(t.ask("qXfer:features:read:target.xml:0,fff").front() == 'm');   // more to come
    std::string xml, chunk;
    for (std::size_t off = 0;; off += 0x400) {
        char cmd[80];
        std::snprintf(cmd, sizeof cmd, "qXfer:features:read:target.xml:%zx,400", off);
        chunk = t.ask(cmd);
        xml += chunk.substr(1);
        if (chunk[0] == 'l') break;
    }
    CHECK(xml.find("riscv:rv64") != std::string::npos);
    CHECK(xml.find("org.gnu.gdb.riscv.cpu") != std::string::npos);
    CHECK(xml.find("name=\"satp\"") != std::string::npos);
    CHECK(xml.find("name=\"fa0\"") != std::string::npos);
    CHECK(t.ask("vMustReplyEmpty") == "");
    CHECK(t.ask("Xnonsense") == "");                                    // unsupported commands answer empty
}

// --- over a real socket ---------------------------------------------------------------------------

void test_a_session_over_tcp() {
    Target t(counting_program());
    const u16 port = t.stub->listen(0);
    CHECK(port != 0);

    bool detached = false;
    std::thread server([&] {
        if (t.stub->accept_client()) detached = t.stub->serve();
    });

    sock_t c = hostnet::tcp_socket();
    int rc = hostnet::connect_ipv4(c, 0x7f000001, port);
    for (int i = 0; i < 2000 && rc == 0; ++i) {
        rc = hostnet::connect_result(c);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(rc == 1);

    auto exchange = [&](const std::string& payload) {
        const std::string wire = GdbStub::encode_packet(payload);
        hostnet::send_data(c, reinterpret_cast<const u8*>(wire.data()), wire.size());
        std::string got;
        for (int i = 0; i < 3000; ++i) {
            u8 buf[512];
            const long n = hostnet::recv_data(c, buf, sizeof buf);
            if (n > 0) got.append(reinterpret_cast<char*>(buf), static_cast<std::size_t>(n));
            const std::size_t hash = got.find('#');
            if (hash != std::string::npos && got.size() >= hash + 3 && got.find('$') != std::string::npos) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return got;
    };

    // The server acknowledges the packet with '+' and replies with a framed packet.
    CHECK(exchange("?") == "+" + GdbStub::encode_packet("T05thread:1;"));
    CHECK(exchange("p20") == "+" + GdbStub::encode_packet(hex64le(DRAM_BASE)));

    // A packet whose checksum is wrong is refused with '-', not acted on.
    const std::string bad = "$p20#00";
    hostnet::send_data(c, reinterpret_cast<const u8*>(bad.data()), bad.size());
    std::string nak;
    for (int i = 0; i < 2000 && nak.empty(); ++i) {
        u8 b[8];
        const long n = hostnet::recv_data(c, b, sizeof b);
        if (n > 0) nak.assign(reinterpret_cast<char*>(b), static_cast<std::size_t>(n));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(nak == "-");

    // Detaching ends the session and tells the emulator to carry on.
    CHECK(exchange("D") == "+" + GdbStub::encode_packet("OK"));
    server.join();
    CHECK(detached);
    CHECK(t.stub->detached());
    hostnet::close_socket(c);
}

}  // namespace

int main() {
    test_packets_are_framed_with_a_checksum();
    test_stop_query_and_register_reads();
    test_registers_can_be_written();
    test_memory_reads_and_writes();
    test_memory_is_read_through_the_guests_page_tables();
    test_breakpoint_stops_the_machine_before_the_instruction();
    test_single_step_advances_one_instruction();
    test_continue_leaves_the_breakpoint_it_is_sitting_on();
    test_removed_breakpoints_stop_stopping();
    test_a_write_watchpoint_stops_when_memory_changes();
    test_machine_exit_is_reported_as_w00();
    test_traps_become_signals();
    test_every_hart_is_a_thread();
    test_the_target_description_names_the_architecture();
    test_a_session_over_tcp();
    return testutil::summary("gdbstub");
}
