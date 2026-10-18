#include "virtio_net.hpp"

#include <algorithm>
#include <cstring>
#include <random>

namespace {
constexpr u32 NET_DEVICE = 1;
constexpr u32 RNG_DEVICE = 4;

constexpr u32 RX_QUEUE = 0;
constexpr u32 TX_QUEUE = 1;

constexpr u64 F_MAC    = 1ull << 5;     // the config space holds the card's MAC address
constexpr u64 F_STATUS = 1ull << 16;    // the config space holds a link-up flag

constexpr u16 LINK_UP = 1;
}  // namespace

// --- virtio-net ---------------------------------------------------------------------

VirtioNet::VirtioNet(NetBackend* backend, const std::array<u8, 6>& mac, u64 base)
    : VirtioMmio(base, NET_DEVICE, 2), backend_(backend), mac_(mac) {}

u64 VirtioNet::device_features() const { return F_MAC | F_STATUS; }

// struct virtio_net_config:  u8 mac[6];  u16 status;
u64 VirtioNet::config_read(u64 offset, unsigned size_bytes) {
    u8 config[8] = {};
    std::memcpy(config, mac_.data(), 6);
    config[6] = static_cast<u8>(LINK_UP & 0xff);
    config[7] = static_cast<u8>(LINK_UP >> 8);

    if (offset >= sizeof(config) || size_bytes == 0 || size_bytes > 8) return 0;
    u64 value = 0;
    for (unsigned i = 0; i < size_bytes && offset + i < sizeof(config); ++i) {
        value |= static_cast<u64>(config[offset + i]) << (8 * i);
    }
    return value;
}

void VirtioNet::queue_notify(u32 queue) {
    if (queue == TX_QUEUE) transmit();
    // New receive buffers: frames that were waiting for somewhere to go can go.
    else if (queue == RX_QUEUE) deliver();
}

void VirtioNet::transmit() {
    u16 head;
    std::vector<Segment> segs;
    bool any = false;
    while (pop_chain(TX_QUEUE, head, segs)) {
        any = true;
        // The frame is every driver-written byte in the chain, after the header.
        const u64 total = segments_length(segs, /*device_writes=*/false);
        if (total > HEADER_BYTES && backend_ != nullptr) {
            std::vector<u8> packet(static_cast<std::size_t>(total));
            const u64 got = read_from_segments(segs, 0, packet.data(), total);
            if (got == total) backend_->send(packet.data() + HEADER_BYTES, packet.size() - HEADER_BYTES);
        }
        push_used(TX_QUEUE, head, 0);
    }
    if (any) signal_used(TX_QUEUE);
}

void VirtioNet::deliver() {
    if (!queue_ready(RX_QUEUE)) return;

    // First anything the backend has produced since we last looked.
    std::vector<u8> frame;
    while (pending_.size() < MAX_PENDING && backend_ != nullptr && backend_->receive(frame)) {
        pending_.push_back(std::move(frame));
        frame.clear();
    }

    bool any = false;
    u16 head;
    std::vector<Segment> segs;
    while (!pending_.empty() && pop_chain(RX_QUEUE, head, segs)) {
        const std::vector<u8>& f = pending_.front();

        // The device-writable space must hold the header and the whole frame; a
        // frame that does not fit is dropped rather than truncated, since a
        // truncated Ethernet frame is worse than a missing one.
        const u64 room = segments_length(segs, /*device_writes=*/true);
        if (room >= HEADER_BYTES + f.size()) {
            u8 header[HEADER_BYTES] = {};
            header[10] = 1;                    // num_buffers = 1 (low byte; high is 0)
            write_to_segments(segs, 0, header, HEADER_BYTES);
            write_to_segments(segs, HEADER_BYTES, f.data(), f.size());
            push_used(RX_QUEUE, head, static_cast<u32>(HEADER_BYTES + f.size()));
            any = true;
        } else {
            ++dropped_;
            push_used(RX_QUEUE, head, 0);
        }
        pending_.pop_front();
    }
    if (any) signal_used(RX_QUEUE);
}

void VirtioNet::poll() { deliver(); }

// --- virtio-rng -----------------------------------------------------------------------

VirtioRng::VirtioRng(u64 base) : VirtioMmio(base, RNG_DEVICE, 1) {
    std::random_device rd;
    state_ = (static_cast<u64>(rd()) << 32) ^ rd();
    if (state_ == 0) state_ = 0x9e3779b97f4a7c15ull;
}

void VirtioRng::seed(u64 value) { state_ = value ? value : 0x9e3779b97f4a7c15ull; }

// xorshift64*: not cryptographically strong, and it does not need to be - this is
// the entropy a guest uses to seed its own generator, from a host that is also
// where the guest's every secret already lives.
u64 VirtioRng::next_u64() {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return state_ * 0x2545f4914f6cdd1dull;
}

void VirtioRng::queue_notify(u32 queue) {
    u16 head;
    std::vector<Segment> segs;
    bool any = false;
    while (pop_chain(queue, head, segs)) {
        any = true;
        u32 written = 0;
        for (const Segment& s : segs) {
            if (!s.device_writes) continue;
            std::vector<u8> bytes(s.len);
            for (std::size_t i = 0; i < bytes.size(); i += 8) {
                const u64 r = next_u64();
                std::memcpy(bytes.data() + i, &r, std::min<std::size_t>(8, bytes.size() - i));
            }
            if (write_guest_bytes(s.addr, bytes.data(), bytes.size())) written += s.len;
        }
        push_used(queue, head, written);
    }
    if (any) signal_used(queue);
}
