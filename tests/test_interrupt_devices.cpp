#include <vector>

#include "bus.hpp"
#include "csr.hpp"
#include "dram.hpp"
#include "plic.hpp"
#include "test_util.hpp"
#include "virtio.hpp"

// ---------------------------------------------------------------------------
// The two devices phase 7 added: the PLIC and the virtio block device.
//
// Both are harder to test than the UART or the CLINT, because neither is a
// simple register file. The PLIC's behaviour is a protocol - claim, then
// complete - and the virtio device's behaviour is a data structure it walks in
// guest memory. So these tests drive them the way a driver would: they build a
// virtqueue by hand and check that the disk contents come back.
// ---------------------------------------------------------------------------

namespace {

// PLIC register offsets, from the SiFive layout that QEMU's virt machine uses.
constexpr u64 PLIC_PRIORITY  = 0x000000;   // + irq * 4
constexpr u64 PLIC_PENDING   = 0x001000;
constexpr u64 PLIC_ENABLE    = 0x002000;   // + context * 0x80
constexpr u64 PLIC_CONTEXT   = 0x200000;   // + context * 0x1000
constexpr u64 PLIC_THRESHOLD = 0x0;        // within a context block
constexpr u64 PLIC_CLAIM     = 0x4;

constexpr u32 CTX_M = 0;
constexpr u32 CTX_S = 1;

u64 ctx_off(u32 context, u64 reg) {
    return PLIC_CONTEXT + context * 0x1000 + reg;
}

// Give a context an interest in one IRQ: non-zero priority, enabled, and a
// threshold low enough to let it through.
void enable_irq(Plic& plic, u32 context, u32 irq, u32 priority) {
    plic.store(PLIC_PRIORITY + irq * 4, 4, priority);
    // Read-modify-write: the enable register is a bitmap of 32 sources, so
    // storing a bare `1 << irq` would disable everything else in the same word.
    const u64 at = PLIC_ENABLE + context * 0x80 + (irq / 32) * 4;
    auto cur = plic.load(at, 4);
    CHECK(cur);
    plic.store(at, 4, cur.value | (1u << (irq % 32)));
    plic.store(ctx_off(context, PLIC_THRESHOLD), 4, 0);
}

u32 read32(Plic& plic, u64 offset) {
    auto r = plic.load(offset, 4);
    CHECK(r);
    return static_cast<u32>(r.value);
}

// --- PLIC -------------------------------------------------------------------

void test_plic_reports_nothing_when_idle() {
    Plic plic;
    CsrFile csrs;
    plic.update(csrs);
    CHECK((csrs.read(csr::MIP) & csr::MIP_SEIP) == 0);
    // Claiming with nothing pending yields IRQ 0, the reserved "none" value.
    CHECK(read32(plic, ctx_off(CTX_S, PLIC_CLAIM)) == 0);
}

void test_plic_pending_irq_drives_seip() {
    Plic plic;
    CsrFile csrs;
    enable_irq(plic, CTX_S, UART0_IRQ, 1);

    plic.set_pending(UART0_IRQ, true);
    plic.update(csrs);
    CHECK((csrs.read(csr::MIP) & csr::MIP_SEIP) != 0);

    // Dropping the line drops the interrupt: the PLIC is level-triggered, so a
    // source that goes quiet before the handler runs simply disappears.
    plic.set_pending(UART0_IRQ, false);
    plic.update(csrs);
    CHECK((csrs.read(csr::MIP) & csr::MIP_SEIP) == 0);
}

void test_plic_ignores_a_disabled_or_zero_priority_source() {
    Plic plic;
    CsrFile csrs;

    // Enabled, but priority 0 - which the spec defines as "never interrupt".
    plic.store(PLIC_ENABLE + CTX_S * 0x80, 4, 1u << UART0_IRQ);
    plic.set_pending(UART0_IRQ, true);
    plic.update(csrs);
    CHECK((csrs.read(csr::MIP) & csr::MIP_SEIP) == 0);

    // Priority set, but below the context's threshold. A threshold of N means
    // "only tell me about priorities strictly greater than N".
    plic.store(PLIC_PRIORITY + UART0_IRQ * 4, 4, 3);
    plic.store(ctx_off(CTX_S, PLIC_THRESHOLD), 4, 3);
    plic.update(csrs);
    CHECK((csrs.read(csr::MIP) & csr::MIP_SEIP) == 0);

    plic.store(ctx_off(CTX_S, PLIC_THRESHOLD), 4, 2);
    plic.update(csrs);
    CHECK((csrs.read(csr::MIP) & csr::MIP_SEIP) != 0);
}

void test_plic_claim_and_complete() {
    Plic plic;
    CsrFile csrs;
    enable_irq(plic, CTX_S, UART0_IRQ, 1);
    plic.set_pending(UART0_IRQ, true);

    CHECK(read32(plic, PLIC_PENDING) == (1u << UART0_IRQ));

    // Claim: the handler learns which source fired.
    CHECK(read32(plic, ctx_off(CTX_S, PLIC_CLAIM)) == UART0_IRQ);

    // While it is in service the same source must not be offered again, even
    // though its line is still asserted. This is the property that stops a
    // level-triggered device from re-entering its own handler forever.
    plic.update(csrs);
    CHECK((csrs.read(csr::MIP) & csr::MIP_SEIP) == 0);
    CHECK(read32(plic, ctx_off(CTX_S, PLIC_CLAIM)) == 0);

    // Complete, with the line still up: it becomes claimable again.
    plic.store(ctx_off(CTX_S, PLIC_CLAIM), 4, UART0_IRQ);
    plic.update(csrs);
    CHECK((csrs.read(csr::MIP) & csr::MIP_SEIP) != 0);
    CHECK(read32(plic, ctx_off(CTX_S, PLIC_CLAIM)) == UART0_IRQ);
}

void test_plic_picks_the_highest_priority_source() {
    Plic plic;
    enable_irq(plic, CTX_S, UART0_IRQ, 1);
    enable_irq(plic, CTX_S, VIRTIO0_IRQ, 7);

    plic.set_pending(UART0_IRQ, true);
    plic.set_pending(VIRTIO0_IRQ, true);

    // Higher priority wins regardless of IRQ number - virtio is IRQ 1 here and
    // the UART is IRQ 10, so a scan that returned the lowest number would give
    // the same answer by accident. Flipping the priorities catches that.
    CHECK(read32(plic, ctx_off(CTX_S, PLIC_CLAIM)) == VIRTIO0_IRQ);

    Plic other;
    enable_irq(other, CTX_S, UART0_IRQ, 7);
    enable_irq(other, CTX_S, VIRTIO0_IRQ, 1);
    other.set_pending(UART0_IRQ, true);
    other.set_pending(VIRTIO0_IRQ, true);
    CHECK(read32(other, ctx_off(CTX_S, PLIC_CLAIM)) == UART0_IRQ);
}

void test_plic_contexts_are_independent() {
    Plic plic;
    CsrFile csrs;
    // Only machine mode is interested in this source.
    enable_irq(plic, CTX_M, VIRTIO0_IRQ, 1);
    plic.set_pending(VIRTIO0_IRQ, true);
    plic.update(csrs);

    CHECK((csrs.read(csr::MIP) & csr::MIP_MEIP) != 0);
    CHECK((csrs.read(csr::MIP) & csr::MIP_SEIP) == 0);
    CHECK(read32(plic, ctx_off(CTX_S, PLIC_CLAIM)) == 0);
    CHECK(read32(plic, ctx_off(CTX_M, PLIC_CLAIM)) == VIRTIO0_IRQ);
}

// --- virtio-blk -------------------------------------------------------------

// Guest-memory addresses for a hand-built virtqueue. The layout is ours to
// choose; a real driver picks these too.
constexpr u64 DESC_ADDR   = DRAM_BASE + 0x1000;
constexpr u64 AVAIL_ADDR  = DRAM_BASE + 0x2000;
constexpr u64 USED_ADDR   = DRAM_BASE + 0x3000;
constexpr u64 HEADER_ADDR = DRAM_BASE + 0x4000;
constexpr u64 DATA_ADDR   = DRAM_BASE + 0x5000;
constexpr u64 STATUS_ADDR = DRAM_BASE + 0x6000;

constexpr u16 DESC_F_NEXT  = 1;
constexpr u16 DESC_F_WRITE = 2;

// A test rig: bus + DRAM + PLIC + a virtio device backed by a synthetic disk.
struct DiskRig {
    Bus       bus;
    Plic*     plic = nullptr;
    VirtioBlk* blk = nullptr;

    explicit DiskRig(u64 sectors) {
        auto dram_owned = std::make_unique<Dram>(1024 * 1024);
        auto plic_owned = std::make_unique<Plic>();
        auto blk_owned  = std::make_unique<VirtioBlk>();
        plic = plic_owned.get();
        blk  = blk_owned.get();
        CHECK(bus.attach(std::move(dram_owned)));
        CHECK(bus.attach(std::move(plic_owned)));
        CHECK(bus.attach(std::move(blk_owned)));
        blk->attach(&bus, plic, VIRTIO0_IRQ);

        // Fill the disk with a recognisable pattern: byte i of sector s is
        // s + i, so a wrong sector or a wrong offset both show up.
        blk->data().assign(sectors * VirtioBlk::SECTOR_SIZE, 0);
        for (u64 s = 0; s < sectors; ++s) {
            for (u64 i = 0; i < VirtioBlk::SECTOR_SIZE; ++i) {
                blk->data()[s * VirtioBlk::SECTOR_SIZE + i] =
                    static_cast<u8>(s + i);
            }
        }
    }

    void poke(u64 addr, unsigned size, u64 value) { CHECK(bus.store(addr, size, value)); }
    u64  peek(u64 addr, unsigned size) {
        auto r = bus.load(addr, size, AccessType::Load);
        CHECK(r);
        return r.value;
    }

    // Write one 16-byte descriptor: {addr, len, flags, next}.
    void descriptor(u16 index, u64 addr, u32 len, u16 flags, u16 next) {
        const u64 at = DESC_ADDR + index * 16;
        poke(at + 0, 8, addr);
        poke(at + 8, 4, len);
        poke(at + 12, 2, flags);
        poke(at + 14, 2, next);
    }

    // Point the device at the rings and mark the queue ready, the way a
    // driver's initialisation does.
    void setup_queue() {
        blk->store(0x030, 4, 0);          // QueueSel = 0
        blk->store(0x038, 4, 8);          // QueueNum
        blk->store(0x080, 4, DESC_ADDR & 0xffffffff);
        blk->store(0x084, 4, DESC_ADDR >> 32);
        blk->store(0x090, 4, AVAIL_ADDR & 0xffffffff);
        blk->store(0x094, 4, AVAIL_ADDR >> 32);
        blk->store(0x0a0, 4, USED_ADDR & 0xffffffff);
        blk->store(0x0a4, 4, USED_ADDR >> 32);
        blk->store(0x044, 4, 1);          // QueueReady
    }

    // Build the standard three-descriptor block request and notify the device.
    // `type` is 0 for a read and 1 for a write.
    void submit(u32 type, u64 sector, u16 avail_idx) {
        poke(HEADER_ADDR + 0, 4, type);
        poke(HEADER_ADDR + 4, 4, 0);
        poke(HEADER_ADDR + 8, 8, sector);
        poke(STATUS_ADDR, 1, 0xff);       // poison, so "0 = OK" means something

        descriptor(0, HEADER_ADDR, 16, DESC_F_NEXT, 1);
        descriptor(1, DATA_ADDR, VirtioBlk::SECTOR_SIZE,
                   static_cast<u16>(DESC_F_NEXT | (type == 0 ? DESC_F_WRITE : 0)), 2);
        descriptor(2, STATUS_ADDR, 1, DESC_F_WRITE, 0);

        poke(AVAIL_ADDR + 4 + (avail_idx % 8) * 2, 2, 0);  // ring[i] = head 0
        poke(AVAIL_ADDR + 2, 2, avail_idx + 1);            // avail.idx
        blk->store(0x050, 4, 0);                           // QueueNotify
    }
};

void test_virtio_identifies_itself() {
    VirtioBlk blk;
    auto magic = blk.load(0x000, 4);
    CHECK(magic);
    CHECK(magic.value == 0x74726976);      // "virt", little-endian
    auto version = blk.load(0x004, 4);
    CHECK(version);
    CHECK(version.value == 2);             // modern virtio-mmio, not legacy
    auto device_id = blk.load(0x008, 4);
    CHECK(device_id);
    CHECK(device_id.value == 2);           // 2 = block device
}

void test_virtio_reports_capacity() {
    DiskRig rig(4);
    CHECK(rig.blk->sectors() == 4);
    // Config space offset 0 is the capacity in 512-byte sectors.
    auto cap = rig.blk->load(0x100, 4);
    CHECK(cap);
    CHECK(cap.value == 4);
}

void test_virtio_read_delivers_sector_data() {
    DiskRig rig(4);
    rig.setup_queue();
    rig.submit(/*type=*/0, /*sector=*/2, /*avail_idx=*/0);

    // The data descriptor now holds sector 2, whose byte i is 2 + i.
    for (u64 i = 0; i < VirtioBlk::SECTOR_SIZE; i += 97) {
        CHECK(rig.peek(DATA_ADDR + i, 1) == static_cast<u8>(2 + i));
    }
    // Status 0 means the request succeeded.
    CHECK(rig.peek(STATUS_ADDR, 1) == 0);
    // And the device reported the completion in the used ring.
    CHECK(rig.peek(USED_ADDR + 2, 2) == 1);
    CHECK(rig.peek(USED_ADDR + 4, 4) == 0);   // used.ring[0].id = chain head
}

void test_virtio_write_reaches_the_disk() {
    DiskRig rig(4);
    rig.setup_queue();

    for (u64 i = 0; i < VirtioBlk::SECTOR_SIZE; ++i) {
        rig.poke(DATA_ADDR + i, 1, static_cast<u8>(0xa0 + i));
    }
    rig.submit(/*type=*/1, /*sector=*/3, /*avail_idx=*/0);

    CHECK(rig.peek(STATUS_ADDR, 1) == 0);
    for (u64 i = 0; i < VirtioBlk::SECTOR_SIZE; i += 61) {
        CHECK(rig.blk->data()[3 * VirtioBlk::SECTOR_SIZE + i] ==
              static_cast<u8>(0xa0 + i));
    }
    // Sector 2 is untouched: a write must not spill into its neighbour.
    CHECK(rig.blk->data()[2 * VirtioBlk::SECTOR_SIZE] == 2);
}

void test_virtio_raises_an_interrupt_through_the_plic() {
    DiskRig rig(4);
    CsrFile csrs;
    enable_irq(*rig.plic, CTX_S, VIRTIO0_IRQ, 1);

    rig.setup_queue();
    rig.submit(0, 0, 0);

    rig.plic->update(csrs);
    CHECK((csrs.read(csr::MIP) & csr::MIP_SEIP) != 0);
    CHECK(read32(*rig.plic, ctx_off(CTX_S, PLIC_CLAIM)) == VIRTIO0_IRQ);

    // InterruptStatus bit 0 = "the used ring advanced". Acknowledging it drops
    // the device's line.
    auto status = rig.blk->load(0x060, 4);
    CHECK(status);
    CHECK((status.value & 1) != 0);
    rig.blk->store(0x064, 4, 1);
    status = rig.blk->load(0x060, 4);
    CHECK(status);
    CHECK((status.value & 1) == 0);
}

void test_virtio_handles_several_requests_in_sequence() {
    DiskRig rig(4);
    rig.setup_queue();

    // Two requests without the device being reset in between. The second must
    // be noticed, which only works if last_avail_ advanced past the first.
    rig.submit(0, 1, 0);
    CHECK(rig.peek(DATA_ADDR, 1) == 1);
    rig.submit(0, 3, 1);
    CHECK(rig.peek(DATA_ADDR, 1) == 3);
    CHECK(rig.peek(USED_ADDR + 2, 2) == 2);
}

void test_virtio_rejects_a_request_past_the_end_of_the_disk() {
    DiskRig rig(4);
    rig.setup_queue();
    rig.submit(0, /*sector=*/99, 0);
    // Non-zero status is the device saying "I could not do that", which is what
    // a driver checks. It must not fault the machine or read out of bounds.
    CHECK(rig.peek(STATUS_ADDR, 1) != 0);
}

// --- regressions ------------------------------------------------------------

void test_plic_registers_beyond_the_bitmaps_read_zero() {
    // The pending and enable bitmaps are 64 bits, so only words 0 and 1 exist.
    // A read of word 2 shifts a u64 by 64, which is undefined behaviour - and
    // on x86 the shift count is taken modulo 64, so word 2 quietly aliases word
    // 0 and reports interrupts that are not pending.
    Plic plic;
    plic.store(PLIC_ENABLE, 4, 0xdeadbeef);
    plic.set_pending(UART0_IRQ, true);

    CHECK_EQ_U(read32(plic, PLIC_ENABLE), 0xdeadbeef);
    CHECK_EQ_U(read32(plic, PLIC_ENABLE + 8), 0);       // word 2: nothing there
    CHECK_EQ_U(read32(plic, PLIC_PENDING + 8), 0);
}

void test_virtio_offers_version_1() {
    // A version-2 MMIO device that does not offer VIRTIO_F_VERSION_1 is a
    // contradiction, and Linux refuses it: "device uses modern interface but
    // does not have VIRTIO_F_VERSION_1", and the device is never probed.
    //
    // The bit is number 32, in the upper half of a 64-bit feature space read
    // through 32-bit registers - so it is only reachable if the device honours
    // the select register. Setting the bit without honouring the select would
    // leave it just as invisible.
    DiskRig rig(4);

    rig.blk->store(0x014, 4, 0);                 // DeviceFeaturesSel = 0
    auto low = rig.blk->load(0x010, 4);
    CHECK(low);
    CHECK_EQ_U(low.value, 0);                    // no optional features

    rig.blk->store(0x014, 4, 1);                 // DeviceFeaturesSel = 1
    auto high = rig.blk->load(0x010, 4);
    CHECK(high);
    CHECK_EQ_U(high.value, 1);                   // bit 32 = VIRTIO_F_VERSION_1
}

void test_virtio_rejects_a_request_that_would_overflow_the_bounds_check() {
    // The obvious bounds check, `offset + length > size`, overflows: a sector
    // number near 2^55 makes the sum wrap to zero and the check passes, after
    // which the device indexes gigabytes outside its own buffer. Every field
    // here comes from guest-written descriptors, so all of it is hostile input.
    DiskRig rig(4);
    rig.setup_queue();
    rig.submit(/*type=*/0, /*sector=*/0x7fff'ffff'ffc0'00ull, 0);

    // Refused with a non-zero status, and - the part that matters - still here
    // to say so.
    CHECK(rig.peek(STATUS_ADDR, 1) != 0);
}

}  // namespace

int main() {
    test_plic_reports_nothing_when_idle();
    test_plic_pending_irq_drives_seip();
    test_plic_ignores_a_disabled_or_zero_priority_source();
    test_plic_claim_and_complete();
    test_plic_picks_the_highest_priority_source();
    test_plic_contexts_are_independent();

    test_virtio_identifies_itself();
    test_virtio_reports_capacity();
    test_virtio_read_delivers_sector_data();
    test_virtio_write_reaches_the_disk();
    test_virtio_raises_an_interrupt_through_the_plic();
    test_virtio_handles_several_requests_in_sequence();
    test_virtio_rejects_a_request_past_the_end_of_the_disk();

    test_plic_registers_beyond_the_bitmaps_read_zero();
    test_virtio_offers_version_1();
    test_virtio_rejects_a_request_that_would_overflow_the_bounds_check();
    return testutil::summary("interrupt devices");
}
