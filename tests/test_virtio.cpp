#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "bus.hpp"
#include "dram.hpp"
#include "plic.hpp"
#include "rtc.hpp"
#include "test_util.hpp"
#include "virtio.hpp"
#include "virtio_net.hpp"

// ---------------------------------------------------------------------------
// The virtio devices, driven the way a guest driver drives them: by building
// virtqueues in guest memory, writing the device's registers, and reading back
// what it left in the used ring.
// ---------------------------------------------------------------------------

namespace {

constexpr u16 F_NEXT  = 1;
constexpr u16 F_WRITE = 2;

struct Seg {
    u64  addr;
    u32  len;
    bool device_writes;
};

// Guest memory layout for the rig. Everything lives in the first MiB of DRAM.
constexpr u64 QUEUE_AREA  = DRAM_BASE + 0x10000;   // 0x10000 per queue
constexpr u64 BUFFER_AREA = DRAM_BASE + 0x60000;

struct Rig {
    Bus   bus;
    Plic* plic = nullptr;
    VirtioMmio* dev = nullptr;
    Dram* dram = nullptr;

    struct Queue {
        u64 desc = 0, avail = 0, used = 0;
        u16 num = 16;
        u16 avail_idx = 0;
        u16 next_desc = 0;
    };
    Queue queues[2];

    explicit Rig(std::unique_ptr<VirtioMmio> device) {
        auto d = std::make_unique<Dram>(1024 * 1024);
        dram = d.get();
        bus.attach(std::move(d));
        auto p = std::make_unique<Plic>();
        plic = p.get();
        bus.attach(std::move(p));
        dev = device.get();
        bus.attach(std::move(device));
        dev->attach(&bus, plic, VIRTIO0_IRQ);
    }

    void w32(u64 off, u32 v) { dev->store(off, 4, v); }
    u32  r32(u64 off) { Result<u64> r = dev->load(off, 4); return static_cast<u32>(r.value); }

    void poke(u64 addr, unsigned size, u64 v) { bus.store(addr, size, v); }
    u64  peek(u64 addr, unsigned size) { return bus.load(addr, size, AccessType::Load).value; }

    void setup_queue(u32 qi) {
        Queue& q = queues[qi];
        q.desc  = QUEUE_AREA + qi * 0x10000;
        q.avail = q.desc + 0x1000;
        q.used  = q.desc + 0x2000;
        w32(0x030, qi);
        w32(0x038, q.num);
        w32(0x080, static_cast<u32>(q.desc));  w32(0x084, static_cast<u32>(q.desc >> 32));
        w32(0x090, static_cast<u32>(q.avail)); w32(0x094, static_cast<u32>(q.avail >> 32));
        w32(0x0a0, static_cast<u32>(q.used));  w32(0x0a4, static_cast<u32>(q.used >> 32));
        w32(0x044, 1);
    }

    // Post one descriptor chain and ring the doorbell. Returns the head index.
    u16 post(u32 qi, const std::vector<Seg>& segs, bool notify = true) {
        Queue& q = queues[qi];
        const u16 head = q.next_desc;
        for (std::size_t i = 0; i < segs.size(); ++i) {
            const u16 index = static_cast<u16>((head + i) % q.num);
            const bool last = (i + 1 == segs.size());
            const u64 at = q.desc + static_cast<u64>(index) * 16;
            poke(at + 0, 8, segs[i].addr);
            poke(at + 8, 4, segs[i].len);
            poke(at + 12, 2, static_cast<u16>((segs[i].device_writes ? F_WRITE : 0) | (last ? 0 : F_NEXT)));
            poke(at + 14, 2, static_cast<u16>((head + i + 1) % q.num));
        }
        q.next_desc = static_cast<u16>((q.next_desc + segs.size()) % q.num);
        poke(q.avail + 4 + (q.avail_idx % q.num) * 2, 2, head);
        ++q.avail_idx;
        poke(q.avail + 2, 2, q.avail_idx);
        if (notify) w32(0x050, qi);
        return head;
    }

    u16 used_idx(u32 qi) { return static_cast<u16>(peek(queues[qi].used + 2, 2)); }
    u32 used_len(u32 qi, u16 slot) { return static_cast<u32>(peek(queues[qi].used + 4 + slot * 8 + 4, 4)); }
    u32 used_id(u32 qi, u16 slot)  { return static_cast<u32>(peek(queues[qi].used + 4 + slot * 8, 4)); }

    std::vector<u8> read_bytes(u64 addr, std::size_t n) {
        std::vector<u8> out(n);
        for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<u8>(peek(addr + i, 1));
        return out;
    }
    void write_bytes(u64 addr, const std::vector<u8>& v) {
        for (std::size_t i = 0; i < v.size(); ++i) poke(addr + i, 1, v[i]);
    }
};

// A block-device request header: type, reserved, sector.
void write_blk_header(Rig& rig, u64 at, u32 type, u64 sector) {
    rig.poke(at + 0, 4, type);
    rig.poke(at + 4, 4, 0);
    rig.poke(at + 8, 8, sector);
}

VirtioBlk* make_blk(Rig*& out_rig, u64 sectors) {
    auto blk = std::make_unique<VirtioBlk>();
    VirtioBlk* raw = blk.get();
    blk->data().resize(sectors * VirtioBlk::SECTOR_SIZE);
    for (u64 i = 0; i < blk->data().size(); ++i) {
        // Byte i of the disk is derived from its sector and offset, so any
        // misplaced or misordered copy shows up as the wrong value.
        blk->data()[i] = static_cast<u8>((i / VirtioBlk::SECTOR_SIZE) * 7 + i);
    }
    out_rig = new Rig(std::move(blk));
    out_rig->setup_queue(0);
    return raw;
}

constexpr u64 HDR    = BUFFER_AREA;
constexpr u64 STATUS = BUFFER_AREA + 0x100;
constexpr u64 DATA1  = BUFFER_AREA + 0x1000;
constexpr u64 DATA2  = BUFFER_AREA + 0x4000;

// --- block: a request whose data is split over several buffers --------------------------

void test_blk_read_scatters_across_several_buffers() {
    // Linux sends one data descriptor per physically contiguous run of pages, so
    // a large request arrives as a chain of several. The device must follow the
    // whole chain, not assume the xv6 shape of header-data-status.
    Rig* rig; VirtioBlk* blk = make_blk(rig, 16);
    std::unique_ptr<Rig> guard(rig);

    write_blk_header(*rig, HDR, 0, 2);                    // read, starting at sector 2
    rig->poke(STATUS, 1, 0xff);
    rig->post(0, {{HDR, 16, false},
                  {DATA1, 512, true},                     // sector 2
                  {DATA2, 1024, true},                    // sectors 3 and 4
                  {STATUS, 1, true}});

    const std::vector<u8> a = rig->read_bytes(DATA1, 512);
    const std::vector<u8> b = rig->read_bytes(DATA2, 1024);
    for (std::size_t i = 0; i < 512; ++i)  CHECK_EQ_U(a[i], blk->data()[2 * 512 + i]);
    for (std::size_t i = 0; i < 1024; ++i) CHECK_EQ_U(b[i], blk->data()[3 * 512 + i]);
    CHECK_EQ_U(rig->peek(STATUS, 1), 0);
    CHECK_EQ_U(rig->used_idx(0), 1);
    CHECK_EQ_U(rig->used_len(0, 0), 512 + 1024 + 1);      // data plus the status byte
}

void test_blk_write_gathers_from_several_buffers() {
    Rig* rig; VirtioBlk* blk = make_blk(rig, 16);
    std::unique_ptr<Rig> guard(rig);

    std::vector<u8> first(512), second(512);
    for (int i = 0; i < 512; ++i) { first[i] = static_cast<u8>(0x10 + i); second[i] = static_cast<u8>(0xc0 - i); }
    rig->write_bytes(DATA1, first);
    rig->write_bytes(DATA2, second);

    write_blk_header(*rig, HDR, 1, 5);                    // write, starting at sector 5
    rig->post(0, {{HDR, 16, false}, {DATA1, 512, false}, {DATA2, 512, false}, {STATUS, 1, true}});

    CHECK_EQ_U(rig->peek(STATUS, 1), 0);
    for (int i = 0; i < 512; ++i) {
        CHECK_EQ_U(blk->data()[5 * 512 + i], first[i]);
        CHECK_EQ_U(blk->data()[6 * 512 + i], second[i]);
    }
    // The neighbours are untouched.
    CHECK_EQ_U(blk->data()[4 * 512], static_cast<u8>(4 * 7 + 4 * 512));
    CHECK_EQ_U(blk->data()[7 * 512], static_cast<u8>(7 * 7 + 7 * 512));
}

void test_blk_flush_and_get_id() {
    Rig* rig; make_blk(rig, 4);
    std::unique_ptr<Rig> guard(rig);

    write_blk_header(*rig, HDR, 4, 0);                    // flush
    rig->poke(STATUS, 1, 0xff);
    rig->post(0, {{HDR, 16, false}, {STATUS, 1, true}});
    CHECK_EQ_U(rig->peek(STATUS, 1), 0);

    write_blk_header(*rig, HDR, 8, 0);                    // get id
    rig->post(0, {{HDR, 16, false}, {DATA1, 20, true}, {STATUS, 1, true}});
    CHECK_EQ_U(rig->peek(STATUS, 1), 0);
    CHECK_EQ_U(rig->peek(DATA1, 1), 'r');

    write_blk_header(*rig, HDR, 99, 0);                   // not a request type
    rig->post(0, {{HDR, 16, false}, {STATUS, 1, true}});
    CHECK_EQ_U(rig->peek(STATUS, 1), 2);                  // UNSUPP
}

void test_blk_survives_hostile_chains() {
    Rig* rig; make_blk(rig, 4);
    std::unique_ptr<Rig> guard(rig);

    // A chain with no status byte at all: nowhere to report, so it completes
    // empty rather than crashing or writing somewhere guessed.
    write_blk_header(*rig, HDR, 0, 0);
    rig->post(0, {{HDR, 16, false}});
    CHECK_EQ_U(rig->used_idx(0), 1);
    CHECK_EQ_U(rig->used_len(0, 0), 0);

    // A header that is too short.
    rig->post(0, {{HDR, 4, false}, {STATUS, 1, true}});
    CHECK_EQ_U(rig->used_idx(0), 2);

    // A data buffer pointing at memory that does not exist: an I/O error, not a
    // host fault.
    write_blk_header(*rig, HDR, 0, 0);
    rig->poke(STATUS, 1, 0xff);
    rig->post(0, {{HDR, 16, false}, {0xdead0000, 512, true}, {STATUS, 1, true}});
    CHECK_EQ_U(rig->peek(STATUS, 1), 1);

    // A read whose buffers are not device-writable is backwards.
    rig->poke(STATUS, 1, 0xff);
    rig->post(0, {{HDR, 16, false}, {DATA1, 512, false}, {STATUS, 1, true}});
    CHECK_EQ_U(rig->peek(STATUS, 1), 1);

    // A descriptor chain that loops back on itself must not hang the host. Build
    // it by hand: descriptor 0 chains to descriptor 0 forever.
    Rig::Queue& q = rig->queues[0];
    rig->poke(q.desc + 0, 8, HDR);
    rig->poke(q.desc + 8, 4, 16);
    rig->poke(q.desc + 12, 2, F_NEXT);
    rig->poke(q.desc + 14, 2, 0);
    rig->poke(q.avail + 4 + (q.avail_idx % q.num) * 2, 2, 0);
    ++q.avail_idx;
    rig->poke(q.avail + 2, 2, q.avail_idx);
    rig->w32(0x050, 0);                                    // returns, which is the test
    CHECK(true);
}

void test_blk_config_space() {
    Rig* rig; make_blk(rig, 10);
    std::unique_ptr<Rig> guard(rig);
    CHECK_EQ_U(rig->dev->load(0x100, 8).value, 10);        // capacity
    CHECK_EQ_U(rig->dev->load(0x100 + 12, 4).value, 126);  // seg_max
    CHECK_EQ_U(rig->dev->load(0x100 + 20, 4).value, 512);  // blk_size
    CHECK_EQ_U(rig->r32(0x034), 256);                      // QueueNumMax
}

// --- block: file-backed -----------------------------------------------------------------------

std::string temp_image(const char* name, u64 sectors) {
    std::string path = std::string("test_virtio_") + name + ".img";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    for (u64 i = 0; i < sectors * 512; ++i) f.put(static_cast<char>((i / 512) + 1));
    return path;
}

std::vector<u8> file_contents(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void test_blk_file_mode_persists_writes() {
    const std::string path = temp_image("persist", 8);
    {
        auto blk = std::make_unique<VirtioBlk>();
        CHECK(blk->open_image(path, /*snapshot=*/false));
        CHECK_EQ_U(blk->sectors(), 8);
        Rig rig(std::move(blk));
        rig.setup_queue(0);

        std::vector<u8> payload(512, 0xab);
        rig.write_bytes(DATA1, payload);
        write_blk_header(rig, HDR, 1, 3);
        rig.post(0, {{HDR, 16, false}, {DATA1, 512, false}, {STATUS, 1, true}});
        CHECK_EQ_U(rig.peek(STATUS, 1), 0);

        // And it reads back what was there before, from the file.
        write_blk_header(rig, HDR, 0, 6);
        rig.post(0, {{HDR, 16, false}, {DATA2, 512, true}, {STATUS, 1, true}});
        CHECK_EQ_U(rig.peek(DATA2, 1), 7);                  // sector 6 holds the byte 7

        write_blk_header(rig, HDR, 4, 0);
        rig.post(0, {{HDR, 16, false}, {STATUS, 1, true}});
    }
    const std::vector<u8> on_disk = file_contents(path);
    CHECK_EQ_U(on_disk.size(), 8 * 512);
    CHECK_EQ_U(on_disk[3 * 512], 0xab);                     // the write reached the file
    CHECK_EQ_U(on_disk[3 * 512 + 511], 0xab);
    CHECK_EQ_U(on_disk[2 * 512], 3);                        // neighbours untouched
    CHECK_EQ_U(on_disk[4 * 512], 5);
    std::remove(path.c_str());
}

void test_blk_snapshot_mode_never_modifies_the_file() {
    const std::string path = temp_image("snapshot", 8);
    const std::vector<u8> before = file_contents(path);
    {
        auto blk = std::make_unique<VirtioBlk>();
        CHECK(blk->open_image(path, /*snapshot=*/true));
        Rig rig(std::move(blk));
        rig.setup_queue(0);

        // Overwrite a whole sector and half of another (via a 256-byte request at
        // a sector boundary), then read both back through the device.
        std::vector<u8> full(512, 0x5a);
        rig.write_bytes(DATA1, full);
        write_blk_header(rig, HDR, 1, 2);
        rig.post(0, {{HDR, 16, false}, {DATA1, 512, false}, {STATUS, 1, true}});
        CHECK_EQ_U(rig.peek(STATUS, 1), 0);

        std::vector<u8> half(256, 0x77);
        rig.write_bytes(DATA1, half);
        write_blk_header(rig, HDR, 1, 4);
        rig.post(0, {{HDR, 16, false}, {DATA1, 256, false}, {STATUS, 1, true}});

        write_blk_header(rig, HDR, 0, 2);
        rig.post(0, {{HDR, 16, false}, {DATA2, 512, true}, {STATUS, 1, true}});
        CHECK_EQ_U(rig.peek(DATA2, 1), 0x5a);               // the guest sees its own write

        write_blk_header(rig, HDR, 0, 4);
        rig.post(0, {{HDR, 16, false}, {DATA2, 512, true}, {STATUS, 1, true}});
        CHECK_EQ_U(rig.peek(DATA2 + 0, 1), 0x77);           // new half
        CHECK_EQ_U(rig.peek(DATA2 + 255, 1), 0x77);
        CHECK_EQ_U(rig.peek(DATA2 + 256, 1), 5);            // old half survives the partial write
        CHECK_EQ_U(rig.peek(DATA2 + 511, 1), 5);
    }
    CHECK(file_contents(path) == before);                   // ...but the host file never changed
    std::remove(path.c_str());
}

// --- network card ------------------------------------------------------------------------------

constexpr u64 TX_BUF = BUFFER_AREA;
constexpr u64 RX_BUF = BUFFER_AREA + 0x4000;

std::vector<u8> frame_of(std::size_t n, u8 seed) {
    std::vector<u8> f(n);
    for (std::size_t i = 0; i < n; ++i) f[i] = static_cast<u8>(seed + i * 3);
    return f;
}

void test_net_transmit_reaches_the_backend_and_loops_back_to_receive() {
    LoopbackBackend wire;
    const std::array<u8, 6> mac = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    auto net = std::make_unique<VirtioNet>(&wire, mac);
    VirtioNet* card = net.get();
    Rig rig(std::move(net));
    rig.setup_queue(0);     // receive
    rig.setup_queue(1);     // transmit

    // The card tells the driver its MAC through the config space.
    for (int i = 0; i < 6; ++i) CHECK_EQ_U(rig.dev->load(0x100 + i, 1).value, mac[i]);

    // Post a receive buffer first, then transmit one frame.
    rig.post(0, {{RX_BUF, 2048, true}});

    const std::vector<u8> payload = frame_of(60, 0x21);
    std::vector<u8> tx(12, 0);                              // zeroed virtio_net_hdr
    tx.insert(tx.end(), payload.begin(), payload.end());
    rig.write_bytes(TX_BUF, tx);
    rig.post(1, {{TX_BUF, static_cast<u32>(tx.size()), false}});
    CHECK_EQ_U(rig.used_idx(1), 1);                         // the transmit buffer was returned

    card->poll();                                           // the loopback frame comes back
    CHECK_EQ_U(rig.used_idx(0), 1);
    CHECK_EQ_U(rig.used_len(0, 0), 12 + 60);
    const std::vector<u8> got = rig.read_bytes(RX_BUF, 12 + 60);
    for (int i = 0; i < 10; ++i) CHECK_EQ_U(got[i], 0);     // header: no offloads
    for (std::size_t i = 0; i < payload.size(); ++i) CHECK_EQ_U(got[12 + i], payload[i]);
}

void test_net_holds_frames_until_the_guest_posts_a_buffer() {
    LoopbackBackend wire;
    auto net = std::make_unique<VirtioNet>(&wire, std::array<u8, 6>{});
    VirtioNet* card = net.get();
    Rig rig(std::move(net));
    rig.setup_queue(0);
    rig.setup_queue(1);

    std::vector<u8> tx(12 + 40, 0);
    rig.write_bytes(TX_BUF, tx);
    rig.post(1, {{TX_BUF, static_cast<u32>(tx.size()), false}});

    card->poll();                                           // no receive buffer yet
    CHECK_EQ_U(rig.used_idx(0), 0);                         // the frame is waiting, not lost

    rig.post(0, {{RX_BUF, 2048, true}});                    // now the guest supplies room
    CHECK_EQ_U(rig.used_idx(0), 1);                         // the doorbell alone delivers it
    CHECK_EQ_U(rig.used_len(0, 0), 12 + 40);
}

void test_net_drops_a_frame_that_does_not_fit() {
    LoopbackBackend wire;
    auto net = std::make_unique<VirtioNet>(&wire, std::array<u8, 6>{});
    VirtioNet* card = net.get();
    Rig rig(std::move(net));
    rig.setup_queue(0);
    rig.setup_queue(1);

    std::vector<u8> tx(12 + 300, 0);
    rig.write_bytes(TX_BUF, tx);
    rig.post(1, {{TX_BUF, static_cast<u32>(tx.size()), false}});
    rig.post(0, {{RX_BUF, 100, true}});                     // far too small
    card->poll();

    CHECK_EQ_U(card->dropped(), 1);
    CHECK_EQ_U(rig.used_len(0, 0), 0);                      // returned empty, not truncated
}

// --- entropy -------------------------------------------------------------------------------------

void test_rng_fills_buffers_and_can_be_seeded() {
    auto fill = [&](u64 seed) {
        auto rng = std::make_unique<VirtioRng>();
        rng->seed(seed);
        Rig rig(std::move(rng));
        rig.setup_queue(0);
        rig.post(0, {{DATA1, 64, true}});
        CHECK_EQ_U(rig.used_len(0, 0), 64);
        return rig.read_bytes(DATA1, 64);
    };
    const std::vector<u8> a = fill(1234), b = fill(1234), c = fill(99);
    CHECK(a == b);                                          // the same seed, the same bytes
    CHECK(a != c);
    int nonzero = 0;
    for (u8 byte : a) nonzero += (byte != 0);
    CHECK(nonzero > 40);                                    // it really is noise, not zeros
}

// --- the clock ---------------------------------------------------------------------------------------

void test_rtc_reports_wall_clock_time_and_alarms() {
    Rtc rtc;
    Plic plic;
    rtc.attach(&plic, RTC_IRQ);
    rtc.set_fixed_time(0x0000'0001'8000'0000ull);           // a time whose halves both matter

    const u64 lo = rtc.load(0x00, 4).value;
    const u64 hi = rtc.load(0x04, 4).value;
    CHECK_EQ_U((hi << 32) | lo, 0x0000'0001'8000'0000ull);

    // Reading LOW latches HIGH: advancing the clock between the two reads must
    // not tear the value.
    rtc.load(0x00, 4);
    rtc.advance(0x1'0000'0000ull);
    CHECK_EQ_U(rtc.load(0x04, 4).value, 1);

    // An alarm in the future does nothing yet...
    plic.store(4 * RTC_IRQ, 4, 1);                          // priority
    plic.store(0x2000, 4, 1u << RTC_IRQ);                   // enable for context 0
    const u64 now = rtc.now_ns();
    rtc.store(0x10, 4, 1);                                  // IRQ enabled
    rtc.store(0x0c, 4, static_cast<u32>((now + 1000) >> 32));
    rtc.store(0x08, 4, static_cast<u32>(now + 1000));
    rtc.poll();
    CsrFile csrs;
    plic.update(csrs, 0);
    CHECK((csrs.read(csr::MIP) & csr::MIP_MEIP) == 0);
    CHECK_EQ_U(rtc.load(0x18, 4).value, 1);                 // armed

    // ...until the time arrives.
    rtc.advance(2000);
    rtc.poll();
    plic.update(csrs, 0);
    CHECK((csrs.read(csr::MIP) & csr::MIP_MEIP) != 0);
    CHECK_EQ_U(rtc.load(0x18, 4).value, 0);                 // one-shot: disarmed

    rtc.store(0x1c, 4, 1);                                  // clear the interrupt
    plic.update(csrs, 0);
    CHECK((csrs.read(csr::MIP) & csr::MIP_MEIP) == 0);
}

void test_rtc_follows_the_host_clock_by_default() {
    Rtc rtc;
    const u64 a = rtc.now_ns();
    CHECK(a > 1'600'000'000ull * 1'000'000'000ull);         // after September 2020
}

}  // namespace

int main() {
    test_blk_read_scatters_across_several_buffers();
    test_blk_write_gathers_from_several_buffers();
    test_blk_flush_and_get_id();
    test_blk_survives_hostile_chains();
    test_blk_config_space();
    test_blk_file_mode_persists_writes();
    test_blk_snapshot_mode_never_modifies_the_file();
    test_net_transmit_reaches_the_backend_and_loops_back_to_receive();
    test_net_holds_frames_until_the_guest_posts_a_buffer();
    test_net_drops_a_frame_that_does_not_fit();
    test_rng_fills_buffers_and_can_be_seeded();
    test_rtc_reports_wall_clock_time_and_alarms();
    test_rtc_follows_the_host_clock_by_default();
    return testutil::summary("virtio");
}
