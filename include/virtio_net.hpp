#pragma once

#include <array>
#include <deque>
#include <memory>
#include <vector>

#include "types.hpp"
#include "virtio_mmio.hpp"

// ---------------------------------------------------------------------------
// Where a network card's frames go.
//
// The card moves Ethernet frames between the guest and "the wire". What the
// wire is - a host tap device, another virtual machine, a user-mode network
// stack that fakes the whole internet - is a separate question, so the card
// talks to this interface and does not care.
// ---------------------------------------------------------------------------
class NetBackend {
public:
    virtual ~NetBackend() = default;

    // The guest transmitted a frame (Ethernet header onwards, no virtio header).
    virtual void send(const u8* frame, std::size_t len) = 0;

    // Fetch the next frame addressed to the guest, if one has arrived. Must not
    // block: it is called from the scheduling loop.
    virtual bool receive(std::vector<u8>& frame) = 0;

    // Is the wire still expecting something from the outside world? (See
    // Device::io_pending.)
    virtual bool io_pending() const { return false; }
};

// A wire that hands every transmitted frame straight back. Useful for testing
// the card on its own, and as a sanity check that the guest's driver works.
class LoopbackBackend : public NetBackend {
public:
    void send(const u8* frame, std::size_t len) override {
        queue_.emplace_back(frame, frame + len);
    }
    bool receive(std::vector<u8>& frame) override {
        if (queue_.empty()) return false;
        frame = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

private:
    std::deque<std::vector<u8>> queue_;
};

// ---------------------------------------------------------------------------
// virtio-net.
//
// Two queues: 0 is the receive queue (the driver posts empty buffers, the
// device fills them with incoming frames) and 1 is the transmit queue (the
// driver posts frames, the device sends them and hands the buffers back).
//
// Every frame is preceded by a 12-byte virtio_net_hdr carrying offload hints.
// None of the offload features are negotiated - no checksum offload, no
// segmentation offload, no merged receive buffers - so the guest does all of that
// itself and the header is all zeros going in either direction. That keeps the
// device a pure frame mover, at the price of the guest's CPU doing work a real
// card would; on an emulator whose "card" is a memcpy, that is the right trade.
// ---------------------------------------------------------------------------
class VirtioNet : public VirtioMmio {
public:
    static constexpr std::size_t HEADER_BYTES = 12;

    VirtioNet(NetBackend* backend, const std::array<u8, 6>& mac, u64 base = VIRTIO_NET_BASE);

    const char* name() const override { return "virtio-net"; }

    // Pull any frames the backend has for the guest into receive buffers.
    void poll() override;
    bool io_pending() const override { return backend_ != nullptr && backend_->io_pending(); }

    const std::array<u8, 6>& mac() const { return mac_; }

    // Frames dropped because the guest had no receive buffers posted.
    u64 dropped() const { return dropped_; }

protected:
    u64  device_features() const override;
    u64  config_read(u64 offset, unsigned size_bytes) override;
    void queue_notify(u32 queue) override;

private:
    NetBackend*       backend_;
    std::array<u8, 6> mac_;

    // Frames received from the backend that the guest had no buffer for yet.
    // Bounded: a guest that never posts buffers must not grow this forever.
    std::deque<std::vector<u8>> pending_;
    static constexpr std::size_t MAX_PENDING = 512;
    u64 dropped_ = 0;

    void transmit();
    void deliver();
};

// ---------------------------------------------------------------------------
// virtio-rng.
//
// A source of entropy for the guest. A modern Linux boot stalls waiting for its
// random pool to fill (systemd, ssh key generation, and TLS all block on it),
// and in an emulator whose every instruction is perfectly predictable there is
// almost nothing to gather it from - so the guest asks the host.
// ---------------------------------------------------------------------------
class VirtioRng : public VirtioMmio {
public:
    explicit VirtioRng(u64 base = VIRTIO_RNG_BASE);

    const char* name() const override { return "virtio-rng"; }

    // Make the output reproducible. By default it is seeded from the host's
    // entropy source.
    void seed(u64 value);

protected:
    void queue_notify(u32 queue) override;

private:
    u64 state_;
    u64 next_u64();
};
