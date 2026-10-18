#pragma once

#include <array>
#include <chrono>
#include <deque>
#include <map>
#include <vector>

#include "hostnet.hpp"
#include "types.hpp"
#include "virtio_net.hpp"

// ---------------------------------------------------------------------------
// A user-mode network.
//
// Gives the guest a working internet connection without a tap device, bridge,
// root privileges or any host configuration - the same idea as QEMU's "user"
// networking. The guest believes it is on a small private network with a router;
// this class plays everything on the other side of that router and turns the
// guest's traffic into ordinary sockets on the host.
//
//     10.0.2.0/24
//       10.0.2.2    the gateway. Also stands for the host: connecting to it
//                   reaches the host's own 127.0.0.1, which is how a guest
//                   talks to a server running on the machine hosting it.
//       10.0.2.3    DNS. Queries are forwarded to the host's resolver.
//       10.0.2.15   the guest, which learns all of this over DHCP.
//
// **What it speaks.** ARP (it answers for the gateway and DNS addresses), DHCP
// (it is the only DHCP server on the network, and hands out the guest's
// address), ICMP echo to the gateway and DNS addresses, UDP (every guest
// datagram becomes a datagram from a host socket, and replies are mapped back),
// and TCP.
//
// **TCP is a proxy, not a bridge.** The guest's TCP connection ends *here*: this
// class is the guest's peer, with its own sequence numbers and its own state
// machine, and opens a separate host socket for the other half. Bytes are moved
// between the two. That is what makes it possible without raw sockets or
// privileges, and it is simpler than it sounds because the link between guest
// and proxy is a function call - it never drops, reorders or duplicates a
// segment, so no retransmission machinery is needed on this side. (The guest
// still retransmits if it wants; duplicates are recognised and re-acknowledged.)
//
// **What it does not do.** ICMP echo to addresses beyond the gateway (that needs
// raw sockets on most hosts), IPv6, IP fragments, and any TCP option beyond MSS.
// ---------------------------------------------------------------------------
class UserNet : public NetBackend {
public:
    struct Config {
        u32 guest_ip   = 0x0a00020f;   // 10.0.2.15
        u32 gateway_ip = 0x0a000202;   // 10.0.2.2
        u32 dns_ip     = 0x0a000203;   // 10.0.2.3
        u32 netmask    = 0xffffff00;

        // Where the gateway address leads on the host. 127.0.0.1 by default;
        // tests point other things at it.
        u32 host_ip    = 0x7f000001;

        // The host resolver DNS queries go to. 0 means ask the host which one it
        // uses.
        u32 dns_server = 0;
        u16 dns_port   = 53;
    };

    UserNet();
    explicit UserNet(const Config& config);
    ~UserNet() override;

    void send(const u8* frame, std::size_t len) override;
    bool receive(std::vector<u8>& frame) override;
    bool io_pending() const override;

    // Service the host sockets: finish connects, move data both ways, expire idle
    // flows. receive() does this on a throttle; tests call it directly.
    void pump();

    // The address the proxy answers ARP with.
    static const std::array<u8, 6>& gateway_mac();

    // --- statistics, for tests and for curiosity -----------------------------
    std::size_t tcp_flows() const { return tcp_.size(); }
    std::size_t udp_flows() const { return udp_.size(); }
    u64 dropped() const { return dropped_; }

private:
    using Clock = std::chrono::steady_clock;

    Config                       cfg_;
    std::array<u8, 6>            guest_mac_{};
    bool                         have_guest_mac_ = false;
    std::deque<std::vector<u8>>  rx_;            // frames waiting for the guest
    u16                          ip_id_ = 1;
    u64                          dropped_ = 0;
    u32                          receive_calls_ = 0;
    u32                          next_iss_ = 0x20000000;

    // --- UDP ---------------------------------------------------------------------
    struct UdpFlow {
        sock_t sock = INVALID_SOCK;
        u16    guest_port = 0;
        u32    virt_ip = 0;      // the destination the guest thinks it is talking to
        u16    virt_port = 0;
        Clock::time_point last;
    };
    std::map<u64, UdpFlow> udp_;

    // --- TCP ---------------------------------------------------------------------
    struct TcpFlow {
        enum class State { Connecting, SynAckSent, Established };
        State  state = State::Connecting;
        sock_t sock = INVALID_SOCK;
        u16    guest_port = 0;
        u32    dst_ip = 0;
        u16    dst_port = 0;

        u32    snd_nxt = 0;           // next sequence number we will use
        u32    snd_una = 0;           // oldest sequence number the guest has not acknowledged
        u32    rcv_nxt = 0;           // next sequence number we expect from the guest
        u32    guest_window = 0;      // how many bytes the guest said it can take
        u16    mss = 536;             // the guest's maximum segment size

        std::vector<u8> to_host;      // guest bytes not yet written to the host socket
        bool   guest_fin = false;     // the guest finished sending
        bool   host_shut = false;     // we have half-closed the host socket
        bool   host_eof = false;      // the host finished sending
        bool   fin_sent = false;      // we sent the guest a FIN
        bool   fin_acked = false;     // ...and it acknowledged it
        Clock::time_point last;
    };
    std::map<u64, TcpFlow> tcp_;

    // --- frame handling ------------------------------------------------------------
    void handle_arp(const u8* p, std::size_t len);
    void handle_ipv4(const u8* p, std::size_t len);
    void handle_icmp(u32 src, u32 dst, const u8* p, std::size_t len);
    void handle_udp(u32 src, u32 dst, const u8* p, std::size_t len);
    void handle_dhcp(const u8* p, std::size_t len);
    void handle_tcp(u32 src, u32 dst, const u8* p, std::size_t len);

    // --- frame construction ----------------------------------------------------------
    void queue_frame(u16 ethertype, const std::vector<u8>& payload);
    void queue_ip(u8 proto, u32 src, u32 dst, const std::vector<u8>& payload,
                  const u8* dst_mac = nullptr);
    void queue_udp(u32 src, u16 sport, u32 dst, u16 dport, const u8* data, std::size_t len,
                   const u8* dst_mac = nullptr);
    void queue_tcp(const TcpFlow& f, u8 flags, u32 seq, u32 ack, const u8* data, std::size_t len,
                   bool with_mss = false);
    void queue_tcp_raw(u32 src, u16 sport, u32 dst, u16 dport, u8 flags, u32 seq, u32 ack,
                       u16 window, const u8* data, std::size_t len, bool with_mss);
    void reset_for(u32 dst_ip, u16 dst_port, u16 guest_port, bool had_ack, u32 seq, u32 ack,
                   u32 seg_len);

    // --- flows -----------------------------------------------------------------------
    static u64 flow_key(u16 guest_port, u32 ip, u16 port) {
        return (static_cast<u64>(guest_port) << 48) | (static_cast<u64>(ip) << 16) | port;
    }
    bool map_destination(u32 virt_ip, u16 virt_port, u32& host_ip, u16& host_port) const;
    u16  window_for(const TcpFlow& f) const;
    void pump_udp();
    void pump_tcp();
};
