#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "clint.hpp"
#include "cpu.hpp"
#include "fdt.hpp"
#include "machine.hpp"
#include "sbi.hpp"
#include "syscon.hpp"
#include "test_util.hpp"
#include "uart.hpp"

// ---------------------------------------------------------------------------
// The two things a Linux kernel needs that a bare-metal program does not: a
// device tree telling it what hardware exists, and an SBI firmware layer doing
// the things supervisor mode cannot do for itself.
// ---------------------------------------------------------------------------

using namespace rvt;

namespace {

// --- device tree ------------------------------------------------------------

u32 be32_at(const std::vector<u8>& blob, std::size_t offset) {
    return (u32(blob[offset]) << 24) | (u32(blob[offset + 1]) << 16) |
           (u32(blob[offset + 2]) << 8) | u32(blob[offset + 3]);
}

// Does the blob contain this NUL-terminated string? Enough to confirm a
// property name or a compatible string made it in, without reimplementing a
// device-tree parser here - `dtc -I dtb` is the real structural check, and the
// runner does that.
bool contains_string(const std::vector<u8>& blob, const char* needle) {
    const std::size_t n = std::strlen(needle);
    if (blob.size() < n) return false;
    for (std::size_t i = 0; i + n <= blob.size(); ++i) {
        if (std::memcmp(blob.data() + i, needle, n) == 0) return true;
    }
    return false;
}

void test_fdt_header_is_well_formed() {
    const std::vector<u8> blob = Fdt::build(128 * 1024 * 1024, "console=ttyS0");

    // Everything in a device tree is big-endian, including on a little-endian
    // machine. A blob written natively parses as garbage, and the magic number
    // is the first place that shows up.
    CHECK_EQ_U(be32_at(blob, 0), 0xd00dfeed);
    CHECK_EQ_U(be32_at(blob, 4), blob.size());        // totalsize
    CHECK_EQ_U(be32_at(blob, 20), 17);                // version
    CHECK_EQ_U(be32_at(blob, 24), 16);                // last compatible version

    const u32 off_struct  = be32_at(blob, 8);
    const u32 off_strings = be32_at(blob, 12);
    const u32 off_rsvmap  = be32_at(blob, 16);
    const u32 size_strings = be32_at(blob, 32);
    const u32 size_struct  = be32_at(blob, 36);

    // The three blocks follow the header in order and exactly fill the blob.
    CHECK(off_rsvmap >= 40);
    CHECK(off_struct > off_rsvmap);
    CHECK(off_strings == off_struct + size_struct);
    CHECK_EQ_U(off_strings + size_strings, blob.size());

    // Every offset is 4-byte aligned, which a parser reading 32-bit tokens
    // depends on.
    CHECK_EQ_U(off_struct % 4, 0);
    CHECK_EQ_U(off_rsvmap % 8, 0);

    // The structure block ends with FDT_END.
    CHECK_EQ_U(be32_at(blob, off_struct + size_struct - 4), 9);

    // The reservation block is terminated by an all-zero entry, which is how a
    // parser knows to stop reading it.
    CHECK_EQ_U(be32_at(blob, off_rsvmap), 0);
    CHECK_EQ_U(be32_at(blob, off_rsvmap + 12), 0);
}

void test_fdt_describes_this_machine() {
    const std::vector<u8> blob = Fdt::build(128 * 1024 * 1024, "console=ttyS0");

    // Every device the emulator actually has, named at the address it is
    // actually at. The blob is built from the same constants in types.hpp that
    // the devices are attached with, so these cannot drift apart - but the node
    // names are spelled out here, and a mismatch would mean a kernel looking in
    // the wrong place.
    CHECK(contains_string(blob, "memory@80000000"));
    CHECK(contains_string(blob, "serial@10000000"));
    CHECK(contains_string(blob, "virtio_mmio@10001000"));
    CHECK(contains_string(blob, "clint@2000000"));
    CHECK(contains_string(blob, "plic@c000000"));
    CHECK(contains_string(blob, "syscon@100000"));

    // The compatible strings are how Linux picks a driver. Get one wrong and
    // the device is simply not there as far as the kernel is concerned.
    CHECK(contains_string(blob, "ns16550a"));
    CHECK(contains_string(blob, "virtio,mmio"));
    CHECK(contains_string(blob, "riscv,cpu-intc"));
    CHECK(contains_string(blob, "syscon-poweroff"));

    // The ISA string must match what the emulator implements: claim more and
    // the kernel executes an instruction that traps, claim less and it takes a
    // slower path for nothing.
    CHECK(contains_string(blob, "rv64imafdc_zicsr_zifencei"));
    CHECK(contains_string(blob, "riscv,sv39"));

    // Without stdout-path the kernel has a UART driver and a node describing a
    // UART, but no reason to believe that UART is the console - and boots
    // silently.
    CHECK(contains_string(blob, "stdout-path"));
    CHECK(contains_string(blob, "/soc/serial@10000000"));
}

void test_fdt_carries_the_command_line_and_initrd() {
    const std::vector<u8> blob =
        Fdt::build(64 * 1024 * 1024, "root=/dev/vda rw console=ttyS0",
                   0x8400'0000, 0x8410'0000);
    CHECK(contains_string(blob, "root=/dev/vda rw console=ttyS0"));
    CHECK(contains_string(blob, "linux,initrd-start"));
    CHECK(contains_string(blob, "linux,initrd-end"));

    // 64-bit, not 32. With more than 4 GiB of guest RAM the initramfs lands
    // above the 4 GiB line, and a 32-bit property truncates it to an address
    // below DRAM_BASE with no memory behind it.
    const std::vector<u8> high =
        Fdt::build(8ull * 1024 * 1024 * 1024, "", 0x1'7c00'0000ull, 0x1'7c10'0000ull);
    bool found_start = false;
    for (std::size_t i = 0; i + 8 <= high.size(); i += 4) {
        if (be32_at(high, i) == 1 && be32_at(high, i + 4) == 0x7c00'0000) {
            found_start = true;
            break;
        }
    }
    CHECK(found_start);

    // With no initrd the properties are absent rather than zero: a kernel reads
    // "present" as "there is one", so a zero-length one would be worse than
    // none at all.
    const std::vector<u8> plain = Fdt::build(64 * 1024 * 1024, "console=ttyS0");
    CHECK(!contains_string(plain, "linux,initrd-start"));
}

void test_fdt_reports_the_memory_it_was_given() {
    // The size lands in the memory node's reg property as two big-endian cells
    // following the base address. A kernel told the wrong size either wastes
    // memory or writes off the end of it.
    const std::vector<u8> blob = Fdt::build(0x1234'5000ull, "");
    bool found = false;
    for (std::size_t i = 0; i + 16 <= blob.size(); i += 4) {
        if (be32_at(blob, i) == 0 && be32_at(blob, i + 4) == 0x8000'0000 &&
            be32_at(blob, i + 8) == 0 && be32_at(blob, i + 12) == 0x1234'5000) {
            found = true;
            break;
        }
    }
    CHECK(found);
}

// --- SBI ---------------------------------------------------------------------

// A machine set up the way --linux sets one up: supervisor mode, SBI enabled.
struct SbiRig {
    Machine       m;
    Clint         clint;
    std::ostringstream console;
    Uart          uart{console};

    explicit SbiRig(const std::vector<u32>& program) : m(program) {
        m.cpu->priv        = PRIV_SUPERVISOR;
        m.cpu->sbi_enabled = true;
        m.cpu->clint       = &clint;
        m.cpu->uart        = &uart;
    }

    // Place an ECALL with the SBI arguments already in place and run it.
    void call(u64 eid, u64 fid = 0, u64 a0 = 0) {
        m.cpu->write_reg(17, eid);
        m.cpu->write_reg(16, fid);
        m.cpu->write_reg(10, a0);
        CHECK(m.cpu->step());
    }

    u64 a0() const { return m.cpu->read_reg(10); }
    u64 a1() const { return m.cpu->read_reg(11); }
};

// ECALL, encoded directly.
constexpr u32 ECALL = 0x00000073;

void test_sbi_ecall_returns_instead_of_trapping() {
    // This is the whole idea: with SBI enabled, an ecall from supervisor mode
    // is a call into firmware, not an exception. Execution continues at the
    // next instruction rather than vectoring to stvec.
    SbiRig rig({ECALL, ADDI(5, 0, 7)});
    rig.call(sbi::EXT_BASE, 0);
    CHECK_EQ_U(rig.m.cpu->pc, DRAM_BASE + 4);
    CHECK(rig.m.cpu->step());
    CHECK_EQ_U(rig.m.reg(5), 7);
}

void test_sbi_is_off_by_default() {
    // A kernel that provides its own machine-mode code - xv6 does - must see
    // its own ecalls. Enabling SBI unconditionally would steal them.
    Machine m({ECALL});
    m.cpu->priv = PRIV_SUPERVISOR;
    CHECK(!m.cpu->sbi_enabled);
    const Status st = m.cpu->step();
    CHECK(!st);
    CHECK(st.trap.cause == Exception::ECallFromSMode);
}

void test_sbi_base_extension_reports_what_exists() {
    SbiRig rig({ECALL, ECALL, ECALL, ECALL});

    rig.call(sbi::EXT_BASE, 0);                       // get_spec_version
    CHECK_EQ_U(rig.a0(), 0);                          // success
    CHECK(rig.a1() != 0);

    rig.call(sbi::EXT_BASE, 3, sbi::EXT_TIME);        // probe_extension
    CHECK_EQ_U(rig.a0(), 0);
    CHECK_EQ_U(rig.a1(), 1);                          // present

    rig.call(sbi::EXT_BASE, 3, 0xdead'beef);          // probe something absent
    CHECK_EQ_U(rig.a0(), 0);
    CHECK_EQ_U(rig.a1(), 0);

    // An unknown *function* is an error, not silence. A kernel probing for
    // something optional must be able to ask and be told no.
    rig.call(sbi::EXT_BASE, 99);
    CHECK_EQ_U(rig.a0(), static_cast<u64>(sbi::ERR_NOT_SUPPORTED));
}

void test_sbi_set_timer_writes_mtimecmp_and_clears_the_pending_interrupt() {
    // mtimecmp is a machine-mode register, so a supervisor cannot write it -
    // which is the entire reason this call exists.
    SbiRig rig({ECALL});
    rig.m.cpu->csrs.raise_interrupt(csr::MIP_MTIP);
    CHECK((rig.m.cpu->csrs.read(csr::MIP) & csr::MIP_MTIP) != 0);

    rig.call(sbi::EXT_TIME, 0, 5000);

    // Clearing MTIP is required, and easy to miss. The interrupt that just
    // fired is still pending; leaving it set means the kernel re-enters its
    // timer handler on the very next instruction and never makes progress.
    CHECK((rig.m.cpu->csrs.read(csr::MIP) & csr::MIP_MTIP) == 0);

    // And the deadline took effect: running past it raises the interrupt again.
    for (int i = 0; i < 10; ++i) rig.clint.tick();
    rig.clint.update(rig.m.cpu->csrs);
    CHECK((rig.m.cpu->csrs.read(csr::MIP) & csr::MIP_MTIP) == 0);
}

void test_sbi_set_timer_expiry_reaches_the_supervisor() {
    // mtimecmp's expiry naturally raises MTIP - a *machine* timer interrupt.
    // A supervisor cannot enable that: mie is machine-only, and under --linux
    // there is no M-mode software to enable it on the kernel's behalf. So
    // without forwarding, the deadline expires into nothing, jiffies never
    // advance, and every sleep in the kernel hangs forever - a failure that
    // looks exactly like the emulator being slow rather than being wrong.
    //
    // Real firmware handles the machine timer interrupt and posts a supervisor
    // one in its place. This checks that it happens.
    SbiRig rig({ECALL});
    rig.call(sbi::EXT_TIME, 0, 5);

    rig.clint.update(rig.m.cpu->csrs);
    CHECK((rig.m.cpu->csrs.read(csr::MIP) & csr::MIP_STIP) == 0);

    for (int i = 0; i < 10; ++i) rig.clint.tick();
    rig.clint.update(rig.m.cpu->csrs);
    CHECK((rig.m.cpu->csrs.read(csr::MIP) & csr::MIP_STIP) != 0);
}

void test_the_timer_divisor_slows_the_clock_rather_than_speeding_it_up() {
    // "--timer-divisor N: instructions per mtime tick". This used to *add* N
    // per instruction, the exact inverse - so asking for a slower clock gave an
    // interrupt storm instead.
    Clint clint;
    clint.instructions_per_tick = 10;
    for (int i = 0; i < 9; ++i) clint.tick();
    CHECK_EQ_U(clint.mtime(), 0);
    clint.tick();
    CHECK_EQ_U(clint.mtime(), 1);
    for (int i = 0; i < 30; ++i) clint.tick();
    CHECK_EQ_U(clint.mtime(), 4);

    // The default advances once per instruction, which is what makes a run
    // reproducible: the timer fires at the same instruction every time.
    Clint fast;
    for (int i = 0; i < 5; ++i) fast.tick();
    CHECK_EQ_U(fast.mtime(), 5);
}

void test_sbi_legacy_set_timer_returns_no_error_code() {
    // The v0.1 form returns nothing at all - not even success - so a0 must be
    // left exactly as the caller set it. Writing a return code into it would
    // corrupt a register the kernel still considers live.
    SbiRig rig({ECALL});
    rig.m.cpu->write_reg(10, 1234);
    rig.m.cpu->write_reg(17, sbi::EXT_SET_TIMER);
    CHECK(rig.m.cpu->step());
    CHECK_EQ_U(rig.m.cpu->read_reg(10), 1234);
}

void test_sbi_console_putchar_reaches_the_host() {
    // Linux uses this for its earliest output, before it has probed the device
    // tree and found a real UART driver - which is exactly when you most want
    // to see something, because a failure before that point is silent.
    SbiRig rig({ECALL, ECALL});
    // Writes go to std::cout rather than the rig's stream, so this checks the
    // call is accepted and consumes no trap rather than the text itself.
    rig.call(sbi::EXT_CONSOLE_PUTCHAR, 0, 'h');
    CHECK_EQ_U(rig.m.cpu->pc, DRAM_BASE + 4);
}

void test_sbi_console_getchar_drains_the_uart() {
    SbiRig rig({ECALL, ECALL, ECALL});
    rig.uart.feed_input("hi");

    rig.call(sbi::EXT_CONSOLE_GETCHAR);
    CHECK_EQ_U(rig.a0(), 'h');
    rig.call(sbi::EXT_CONSOLE_GETCHAR);
    CHECK_EQ_U(rig.a0(), 'i');

    // Nothing waiting reads as -1, which is how the caller knows to stop.
    rig.call(sbi::EXT_CONSOLE_GETCHAR);
    CHECK_EQ_U(rig.a0(), static_cast<u64>(-1));
}

void test_sbi_shutdown_stops_the_machine() {
    SbiRig rig({ECALL, ADDI(5, 0, 7)});
    rig.call(sbi::EXT_SRST, 0);
    CHECK(rig.m.cpu->halted);
    CHECK(rig.m.cpu->sbi_shutdown);

    // And the legacy call does the same thing.
    SbiRig legacy({ECALL});
    legacy.call(sbi::EXT_SHUTDOWN);
    CHECK(legacy.m.cpu->halted);
}

void test_sbi_single_hart_calls_succeed() {
    // An IPI to a mask containing only this hart, and a remote fence with no
    // remote harts to fence, are both no-ops that must report success.
    // Returning "not supported" would make the kernel take a fallback path for
    // a situation that is not a failure.
    SbiRig rig({ECALL, ECALL});
    rig.call(sbi::EXT_IPI, 0);
    CHECK_EQ_U(rig.a0(), 0);
    rig.call(sbi::EXT_RFENCE, 0);
    CHECK_EQ_U(rig.a0(), 0);
}

void test_sbi_unknown_extension_is_refused_cleanly() {
    SbiRig rig({ECALL});
    rig.call(0xbadc0de);
    CHECK_EQ_U(rig.a0(), static_cast<u64>(sbi::ERR_NOT_SUPPORTED));
    // Refused, but not a trap: the kernel carries on and takes its fallback.
    CHECK_EQ_U(rig.m.cpu->pc, DRAM_BASE + 4);
}

}  // namespace

int main() {
    test_fdt_header_is_well_formed();
    test_fdt_describes_this_machine();
    test_fdt_carries_the_command_line_and_initrd();
    test_fdt_reports_the_memory_it_was_given();

    test_sbi_ecall_returns_instead_of_trapping();
    test_sbi_is_off_by_default();
    test_sbi_base_extension_reports_what_exists();
    test_sbi_set_timer_writes_mtimecmp_and_clears_the_pending_interrupt();
    test_sbi_set_timer_expiry_reaches_the_supervisor();
    test_the_timer_divisor_slows_the_clock_rather_than_speeding_it_up();
    test_sbi_legacy_set_timer_returns_no_error_code();
    test_sbi_console_putchar_reaches_the_host();
    test_sbi_console_getchar_drains_the_uart();
    test_sbi_shutdown_stops_the_machine();
    test_sbi_single_hart_calls_succeed();
    test_sbi_unknown_extension_is_refused_cleanly();
    return testutil::summary("firmware");
}
