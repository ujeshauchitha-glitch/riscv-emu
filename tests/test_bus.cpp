#include <memory>

#include "bus.hpp"
#include "dram.hpp"
#include "test_util.hpp"

// ---------------------------------------------------------------------------
// Bus and DRAM tests.
//
// The headline case is the memory-safety bug this phase fixes: the previous
// Memory class indexed a fixed 1 MiB std::array with no bounds check at all, so
// any wrong address read or wrote host memory outside the array. Because that
// array was a stack-allocated member, an out-of-range store corrupted the
// host's stack directly. Here every such access must come back as a trap.
// ---------------------------------------------------------------------------

namespace {

constexpr u64 kTestDramSize = 1024 * 1024;  // 1 MiB, plenty for these tests

std::unique_ptr<Bus> make_bus(Dram** dram_out) {
    auto  bus  = std::make_unique<Bus>();
    auto  dram = std::make_unique<Dram>(kTestDramSize);
    *dram_out  = dram.get();
    CHECK(bus->attach(std::move(dram)));
    return bus;
}

void test_dram_roundtrip() {
    Dram* dram = nullptr;
    auto  bus  = make_bus(&dram);

    // Every access width, written and read back.
    CHECK(bus->store(DRAM_BASE + 0x100, 1, 0xab));
    CHECK(bus->store(DRAM_BASE + 0x200, 2, 0xbeef));
    CHECK(bus->store(DRAM_BASE + 0x300, 4, 0xdead'beef));
    CHECK(bus->store(DRAM_BASE + 0x400, 8, 0x0123'4567'89ab'cdefull));

    auto r1 = bus->load(DRAM_BASE + 0x100, 1, AccessType::Load);
    auto r2 = bus->load(DRAM_BASE + 0x200, 2, AccessType::Load);
    auto r4 = bus->load(DRAM_BASE + 0x300, 4, AccessType::Load);
    auto r8 = bus->load(DRAM_BASE + 0x400, 8, AccessType::Load);

    CHECK(r1 && r2 && r4 && r8);
    CHECK_EQ_U(r1.value, 0xabull);
    CHECK_EQ_U(r2.value, 0xbeefull);
    CHECK_EQ_U(r4.value, 0xdead'beefull);
    CHECK_EQ_U(r8.value, 0x0123'4567'89ab'cdefull);
}

void test_little_endian() {
    Dram* dram = nullptr;
    auto  bus  = make_bus(&dram);

    // RISC-V is little-endian: the least significant byte lives at the lowest
    // address. Storing a word and reading its bytes back must show that order.
    CHECK(bus->store(DRAM_BASE + 0x10, 4, 0x1122'3344));

    auto b0 = bus->load(DRAM_BASE + 0x10, 1, AccessType::Load);
    auto b1 = bus->load(DRAM_BASE + 0x11, 1, AccessType::Load);
    auto b2 = bus->load(DRAM_BASE + 0x12, 1, AccessType::Load);
    auto b3 = bus->load(DRAM_BASE + 0x13, 1, AccessType::Load);

    CHECK_EQ_U(b0.value, 0x44);
    CHECK_EQ_U(b1.value, 0x33);
    CHECK_EQ_U(b2.value, 0x22);
    CHECK_EQ_U(b3.value, 0x11);
}

void test_dram_is_based_at_0x80000000() {
    Dram* dram = nullptr;
    auto  bus  = make_bus(&dram);

    // Address 0 is *not* RAM. On a real machine the low addresses are reserved
    // for MMIO, and every kernel we care about is linked for RAM at
    // 0x8000_0000. The old flat-array-from-zero model could not represent this.
    auto low = bus->load(0, 4, AccessType::Load);
    CHECK(!low);
    CHECK(low.trap.cause == Exception::LoadAccessFault);

    // The first byte of DRAM is at 0x8000_0000 and maps to array element 0.
    CHECK(bus->store(DRAM_BASE, 1, 0x5a));
    CHECK_EQ_U(dram->bytes()[0], 0x5a);
}

void test_out_of_range_traps_instead_of_corrupting_memory() {
    Dram* dram = nullptr;
    auto  bus  = make_bus(&dram);

    const u64 last_byte = DRAM_BASE + kTestDramSize - 1;

    // A single byte at the very last address is fine.
    CHECK(bus->store(last_byte, 1, 0x77));
    auto ok = bus->load(last_byte, 1, AccessType::Load);
    CHECK(ok && ok.value == 0x77);

    // A 4-byte access *starting* at the last byte begins in range but runs
    // three bytes past the end. This is precisely the case the old read32 got
    // wrong — it would have read host memory beyond the array.
    auto over = bus->load(last_byte, 4, AccessType::Load);
    CHECK(!over);
    CHECK(over.trap.cause == Exception::LoadAccessFault);

    auto over_store = bus->store(last_byte, 8, 0xffff'ffff'ffff'ffffull);
    CHECK(!over_store);
    CHECK(over_store.trap.cause == Exception::StoreAMOAccessFault);

    // Comfortably past the end.
    auto way_over = bus->load(DRAM_BASE + kTestDramSize + 0x1000, 4, AccessType::Load);
    CHECK(!way_over);
}

void test_unmapped_addresses_report_the_right_cause() {
    Dram* dram = nullptr;
    auto  bus  = make_bus(&dram);

    // 0x1000_0000 is where the UART will live, but nothing is attached there
    // yet. The same unmapped address yields a different trap cause depending on
    // why we touched it, which is what AccessType is for.
    auto as_fetch = bus->load(UART0_BASE, 4, AccessType::Instruction);
    CHECK(!as_fetch);
    CHECK(as_fetch.trap.cause == Exception::InstructionAccessFault);
    CHECK_EQ_U(as_fetch.trap.tval, UART0_BASE);

    auto as_load = bus->load(UART0_BASE, 4, AccessType::Load);
    CHECK(!as_load);
    CHECK(as_load.trap.cause == Exception::LoadAccessFault);

    auto as_store = bus->store(UART0_BASE, 4, 0);
    CHECK(!as_store);
    CHECK(as_store.trap.cause == Exception::StoreAMOAccessFault);
}

void test_overlapping_devices_are_rejected() {
    Bus  bus;
    auto a = std::make_unique<Dram>(kTestDramSize);
    CHECK(bus.attach(std::move(a)));

    // A second DRAM at the same base would silently shadow the first. Catching
    // it at attach time is far cheaper than debugging why a UART write lands in
    // RAM and nothing prints.
    auto b = std::make_unique<Dram>(kTestDramSize);
    CHECK(!bus.attach(std::move(b)));
    CHECK_EQ(bus.devices().size(), 1u);
}

void test_load_image() {
    Dram dram(kTestDramSize);

    const std::vector<u8> blob = {0xde, 0xad, 0xbe, 0xef};
    CHECK(dram.load_image(DRAM_BASE, blob));
    auto r = dram.load(0, 4);
    CHECK(r && r.value == 0xefbe'addeull);

    // An image that does not fit must be refused, not truncated or written past
    // the end of the buffer.
    const std::vector<u8> too_big(kTestDramSize + 1, 0);
    CHECK(!dram.load_image(DRAM_BASE, too_big));

    // An address below DRAM_BASE is not a valid load target.
    CHECK(!dram.load_image(0, blob));
}

}  // namespace

int main() {
    test_dram_roundtrip();
    test_little_endian();
    test_dram_is_based_at_0x80000000();
    test_out_of_range_traps_instead_of_corrupting_memory();
    test_unmapped_addresses_report_the_right_cause();
    test_overlapping_devices_are_rejected();
    test_load_image();
    return testutil::summary("bus");
}
