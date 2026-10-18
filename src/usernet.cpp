#include "usernet.hpp"

#include <algorithm>
#include <cstring>

// GCC's -Wnull-dereference cannot tell that a std::vector it has just sized is
// non-empty, so it reports every read and write through `buffer.data()` in the
// packet builders below as a possible null access. They are not: each buffer is
// constructed with its final length before it is touched. Suppressing the
// warning for this file keeps the project's warning-free build honest instead of
// scattering casts and dummy checks through the byte-twiddling.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wnull-dereference"
#endif

namespace {

constexpr u16 ETH_IPV4 = 0x0800;
constexpr u16 ETH_ARP  = 0x0806;

constexpr u8 PROTO_ICMP = 1;
constexpr u8 PROTO_TCP  = 6;
constexpr u8 PROTO_UDP  = 17;

constexpr u8 TCP_FIN = 0x01;
constexpr u8 TCP_SYN = 0x02;
constexpr u8 TCP_RST = 0x04;
constexpr u8 TCP_PSH = 0x08;
constexpr u8 TCP_ACK = 0x10;

constexpr std::size_t ETH_HEADER = 14;
constexpr std::size_t MTU = 1500;
constexpr u16 MSS_OURS = 1460;                // MTU less the 40 bytes of IP and TCP headers

// How much unsent guest data a flow may hold before it stops accepting more. The
// window we advertise shrinks as this fills, which is what throttles the guest.
constexpr std::size_t TO_HOST_LIMIT = 128 * 1024;
constexpr std::size_t MAX_TCP_FLOWS = 256;
constexpr std::size_t MAX_UDP_FLOWS = 256;

constexpr auto TCP_HANDSHAKE_TIMEOUT = std::chrono::seconds(20);
constexpr auto TCP_LINGER_TIMEOUT    = std::chrono::seconds(60);
constexpr auto UDP_IDLE_TIMEOUT      = std::chrono::seconds(60);

// --- big-endian helpers: every field on the wire is network byte order -------------

u16 rd16(const u8* p) { return static_cast<u16>((p[0] << 8) | p[1]); }
u32 rd32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | p[3];
}
void wr16(u8* p, u16 v) { p[0] = static_cast<u8>(v >> 8); p[1] = static_cast<u8>(v); }
void wr32(u8* p, u32 v) {
    p[0] = static_cast<u8>(v >> 24); p[1] = static_cast<u8>(v >> 16);
    p[2] = static_cast<u8>(v >> 8);  p[3] = static_cast<u8>(v);
}

// The Internet checksum: the ones'-complement sum of 16-bit words, complemented.
// `sum` carries in the contribution of a pseudo-header, if any.
u16 checksum(const u8* data, std::size_t len, u32 sum = 0) {
    for (std::size_t i = 0; i + 1 < len; i += 2) sum += rd16(data + i);
    if (len & 1) sum += static_cast<u32>(data[len - 1]) << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return static_cast<u16>(~sum);
}

// The pseudo-header that TCP and UDP checksums cover in addition to their own
// bytes: it ties the segment to the addresses it claims to travel between.
u32 pseudo_header_sum(u32 src, u32 dst, u8 proto, std::size_t length) {
    u32 sum = 0;
    sum += (src >> 16) + (src & 0xffff);
    sum += (dst >> 16) + (dst & 0xffff);
    sum += proto;
    sum += static_cast<u32>(length);
    return sum;
}

// TCP sequence numbers wrap, so "a is before b" is a signed difference.
bool seq_lt(u32 a, u32 b) { return static_cast<i32>(a - b) < 0; }
bool seq_le(u32 a, u32 b) { return static_cast<i32>(a - b) <= 0; }

constexpr std::array<u8, 6> GATEWAY_MAC = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
constexpr std::array<u8, 6> BROADCAST_MAC = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

}  // namespace

const std::array<u8, 6>& UserNet::gateway_mac() { return GATEWAY_MAC; }

UserNet::UserNet() : UserNet(Config{}) {}

UserNet::UserNet(const Config& config) : cfg_(config) {
    hostnet::init();
    if (cfg_.dns_server == 0) cfg_.dns_server = hostnet::system_dns_server();
    if (cfg_.dns_server == 0) cfg_.dns_server = 0x08080808;   // 8.8.8.8, as a last resort
}

UserNet::~UserNet() {
    for (auto& [key, f] : udp_) hostnet::close_socket(f.sock);
    for (auto& [key, f] : tcp_) hostnet::close_socket(f.sock);
}

bool UserNet::receive(std::vector<u8>& frame) {
    // The sockets are serviced on a throttle. A poll costs a system call per open
    // flow, and the card asks for frames every scheduling round - hundreds of
    // times for every millisecond - so doing it each time would slow the guest to
    // a crawl whenever a connection was open. Every 32nd call is a poll roughly
    // every tenth of a millisecond of guest time, which no network can notice.
    if (rx_.empty() && (++receive_calls_ & 31) == 0) pump();
    if (rx_.empty()) return false;
    frame = std::move(rx_.front());
    rx_.pop_front();
    return true;
}

bool UserNet::io_pending() const {
    // The machine uses this to decide whether an idle guest may skip ahead in
    // time. It must not while the outside world is still expected to answer:
    // jumping the guest's clock past a DNS timeout before the reply has had a
    // moment to arrive turns a working network into a broken one.
    const auto now = Clock::now();
    for (const auto& [key, f] : udp_) {
        if (now - f.last < std::chrono::seconds(2)) return true;
    }
    for (const auto& [key, f] : tcp_) {
        if (f.state != TcpFlow::State::Established) return true;
        if (f.snd_una != f.snd_nxt) return true;        // data in flight
        if (!f.to_host.empty()) return true;
    }
    return false;
}

// --- sending to the guest -------------------------------------------------------------

void UserNet::queue_frame(u16 ethertype, const std::vector<u8>& payload) {
    std::vector<u8> frame(ETH_HEADER + payload.size());
    std::memcpy(frame.data(), have_guest_mac_ ? guest_mac_.data() : BROADCAST_MAC.data(), 6);
    std::memcpy(frame.data() + 6, GATEWAY_MAC.data(), 6);
    wr16(frame.data() + 12, ethertype);
    std::memcpy(frame.data() + ETH_HEADER, payload.data(), payload.size());
    rx_.push_back(std::move(frame));
}

void UserNet::queue_ip(u8 proto, u32 src, u32 dst, const std::vector<u8>& payload,
                       const u8* dst_mac) {
    std::vector<u8> packet(20 + payload.size());
    u8* h = packet.data();
    h[0] = 0x45;                                  // IPv4, 5-word header
    h[1] = 0;
    wr16(h + 2, static_cast<u16>(packet.size()));
    wr16(h + 4, ip_id_++);
    wr16(h + 6, 0x4000);                          // don't fragment
    h[8] = 64;                                    // TTL
    h[9] = proto;
    wr32(h + 12, src);
    wr32(h + 16, dst);
    wr16(h + 10, 0);
    wr16(h + 10, checksum(h, 20));
    std::memcpy(h + 20, payload.data(), payload.size());

    if (dst_mac == nullptr) {
        queue_frame(ETH_IPV4, packet);
        return;
    }
    std::vector<u8> frame(ETH_HEADER + packet.size());
    std::memcpy(frame.data(), dst_mac, 6);
    std::memcpy(frame.data() + 6, GATEWAY_MAC.data(), 6);
    wr16(frame.data() + 12, ETH_IPV4);
    std::memcpy(frame.data() + ETH_HEADER, packet.data(), packet.size());
    rx_.push_back(std::move(frame));
}

void UserNet::queue_udp(u32 src, u16 sport, u32 dst, u16 dport, const u8* data, std::size_t len,
                        const u8* dst_mac) {
    std::vector<u8> seg(8 + len);
    wr16(seg.data() + 0, sport);
    wr16(seg.data() + 2, dport);
    wr16(seg.data() + 4, static_cast<u16>(seg.size()));
    wr16(seg.data() + 6, 0);
    if (len != 0) std::memcpy(seg.data() + 8, data, len);
    u16 sum = checksum(seg.data(), seg.size(), pseudo_header_sum(src, dst, PROTO_UDP, seg.size()));
    if (sum == 0) sum = 0xffff;                   // zero means "no checksum" in UDP
    wr16(seg.data() + 6, sum);
    queue_ip(PROTO_UDP, src, dst, seg, dst_mac);
}

void UserNet::queue_tcp_raw(u32 src, u16 sport, u32 dst, u16 dport, u8 flags, u32 seq, u32 ack,
                            u16 window, const u8* data, std::size_t len, bool with_mss) {
    const std::size_t header = with_mss ? 24 : 20;
    std::vector<u8> seg(header + len);
    wr16(seg.data() + 0, sport);
    wr16(seg.data() + 2, dport);
    wr32(seg.data() + 4, seq);
    wr32(seg.data() + 8, ack);
    seg[12] = static_cast<u8>((header / 4) << 4);
    seg[13] = flags;
    wr16(seg.data() + 14, window);
    wr16(seg.data() + 16, 0);
    wr16(seg.data() + 18, 0);
    if (with_mss) {
        seg[20] = 2; seg[21] = 4;                 // option: maximum segment size
        wr16(seg.data() + 22, MSS_OURS);
    }
    if (len != 0) std::memcpy(seg.data() + header, data, len);
    wr16(seg.data() + 16,
         checksum(seg.data(), seg.size(), pseudo_header_sum(src, dst, PROTO_TCP, seg.size())));
    queue_ip(PROTO_TCP, src, dst, seg);
}

u16 UserNet::window_for(const TcpFlow& f) const {
    const std::size_t room = f.to_host.size() >= TO_HOST_LIMIT ? 0 : TO_HOST_LIMIT - f.to_host.size();
    return static_cast<u16>(std::min<std::size_t>(room, 65535));
}

void UserNet::queue_tcp(const TcpFlow& f, u8 flags, u32 seq, u32 ack, const u8* data,
                        std::size_t len, bool with_mss) {
    queue_tcp_raw(f.dst_ip, f.dst_port, cfg_.guest_ip, f.guest_port, flags, seq, ack,
                  window_for(f), data, len, with_mss);
}

// A reset in answer to a segment for a connection that does not exist (RFC 793):
// if the offending segment carried an ACK, the reset's sequence number is that
// acknowledgment; otherwise it is zero and the reset acknowledges the segment.
void UserNet::reset_for(u32 dst_ip, u16 dst_port, u16 guest_port, bool had_ack, u32 seq, u32 ack,
                        u32 seg_len) {
    if (had_ack) {
        queue_tcp_raw(dst_ip, dst_port, cfg_.guest_ip, guest_port, TCP_RST, ack, 0, 0, nullptr, 0,
                      false);
    } else {
        queue_tcp_raw(dst_ip, dst_port, cfg_.guest_ip, guest_port, TCP_RST | TCP_ACK, 0,
                      seq + seg_len, 0, nullptr, 0, false);
    }
}

// --- frames from the guest ---------------------------------------------------------------

void UserNet::send(const u8* frame, std::size_t len) {
    if (len < ETH_HEADER) { ++dropped_; return; }

    // Remember who the guest is. Every frame it sends carries its address, and
    // we need it to address replies.
    std::memcpy(guest_mac_.data(), frame + 6, 6);
    have_guest_mac_ = true;

    const u16 ethertype = rd16(frame + 12);
    if (ethertype == ETH_ARP) handle_arp(frame + ETH_HEADER, len - ETH_HEADER);
    else if (ethertype == ETH_IPV4) handle_ipv4(frame + ETH_HEADER, len - ETH_HEADER);
    else ++dropped_;
}

void UserNet::handle_arp(const u8* p, std::size_t len) {
    // htype 1 (Ethernet), ptype 0x0800 (IPv4), hlen 6, plen 4: 28 bytes.
    if (len < 28 || rd16(p) != 1 || rd16(p + 2) != ETH_IPV4 || p[4] != 6 || p[5] != 4) {
        ++dropped_;
        return;
    }
    if (rd16(p + 6) != 1) return;                 // only requests get answered
    const u32 sender_ip = rd32(p + 14);
    const u32 target_ip = rd32(p + 24);

    // We answer for the two addresses we pretend to own.
    if (target_ip != cfg_.gateway_ip && target_ip != cfg_.dns_ip) return;
    if (sender_ip == 0) return;                   // an address probe, not a question

    std::vector<u8> reply(28);
    wr16(reply.data() + 0, 1);
    wr16(reply.data() + 2, ETH_IPV4);
    reply[4] = 6; reply[5] = 4;
    wr16(reply.data() + 6, 2);                    // reply
    std::memcpy(reply.data() + 8, GATEWAY_MAC.data(), 6);
    wr32(reply.data() + 14, target_ip);
    std::memcpy(reply.data() + 18, p + 8, 6);     // back to whoever asked
    wr32(reply.data() + 24, sender_ip);
    queue_frame(ETH_ARP, reply);
}

void UserNet::handle_ipv4(const u8* p, std::size_t len) {
    if (len < 20 || (p[0] >> 4) != 4) { ++dropped_; return; }
    const std::size_t ihl = static_cast<std::size_t>(p[0] & 0x0f) * 4;
    const std::size_t total = rd16(p + 2);
    if (ihl < 20 || total < ihl || total > len) { ++dropped_; return; }

    // Fragments are not reassembled. A guest sending below the MTU never
    // produces them, and reassembly is a resource-exhaustion hazard for a
    // component that exists to be exposed to untrusted traffic.
    if (rd16(p + 6) & 0x3fff) { ++dropped_; return; }

    const u8  proto = p[9];
    const u32 src   = rd32(p + 12);
    const u32 dst   = rd32(p + 16);
    const u8* body  = p + ihl;
    const std::size_t body_len = total - ihl;

    switch (proto) {
        case PROTO_ICMP: handle_icmp(src, dst, body, body_len); break;
        case PROTO_UDP:  handle_udp(src, dst, body, body_len);  break;
        case PROTO_TCP:  handle_tcp(src, dst, body, body_len);  break;
        default: ++dropped_; break;
    }
}

void UserNet::handle_icmp(u32 src, u32 dst, const u8* p, std::size_t len) {
    // Echo request (type 8) to an address that is ours: answer it. Pings further
    // afield would need a raw socket on the host, so they go unanswered.
    if (len < 8 || p[0] != 8 || p[1] != 0) { ++dropped_; return; }
    if (dst != cfg_.gateway_ip && dst != cfg_.dns_ip) { ++dropped_; return; }

    std::vector<u8> reply(p, p + len);
    reply[0] = 0;                                 // echo reply
    wr16(reply.data() + 2, 0);
    wr16(reply.data() + 2, checksum(reply.data(), reply.size()));
    queue_ip(PROTO_ICMP, dst, src, reply);
}

// --- DHCP ------------------------------------------------------------------------------------

void UserNet::handle_dhcp(const u8* p, std::size_t len) {
    // BOOTP: op htype hlen hops | xid | secs flags | ciaddr yiaddr siaddr giaddr |
    // chaddr[16] sname[64] file[128] | magic cookie | options
    if (len < 240 || p[0] != 1 || rd32(p + 236) != 0x63825363) { ++dropped_; return; }

    u8   message_type = 0;
    u32  requested_ip = 0;
    for (std::size_t i = 240; i < len;) {
        const u8 code = p[i];
        if (code == 255) break;
        if (code == 0) { ++i; continue; }
        if (i + 1 >= len) break;
        const std::size_t olen = p[i + 1];
        if (i + 2 + olen > len) break;
        if (code == 53 && olen == 1) message_type = p[i + 2];
        if (code == 50 && olen == 4) requested_ip = rd32(p + i + 2);
        i += 2 + olen;
    }

    // 1 DISCOVER, 3 REQUEST, 8 INFORM. Anything else (RELEASE, DECLINE) needs no
    // reply - there is only ever one address to give out.
    u8 reply_type;
    if (message_type == 1) reply_type = 2;                     // OFFER
    else if (message_type == 3) {
        // A guest asking to keep an address other than ours is told no, so that
        // it starts over with DISCOVER instead of using a stale lease.
        reply_type = (requested_ip == 0 || requested_ip == cfg_.guest_ip) ? 5 : 6;  // ACK / NAK
    } else if (message_type == 8) reply_type = 5;
    else return;

    std::vector<u8> r(240, 0);
    r[0] = 2;                                     // BOOTREPLY
    r[1] = p[1]; r[2] = p[2];                     // htype, hlen
    std::memcpy(r.data() + 4, p + 4, 4);          // xid: how the guest matches reply to request
    std::memcpy(r.data() + 10, p + 10, 2);        // flags
    if (reply_type != 6) wr32(r.data() + 16, cfg_.guest_ip);   // yiaddr
    wr32(r.data() + 20, cfg_.gateway_ip);         // siaddr
    std::memcpy(r.data() + 28, p + 28, 16);       // chaddr
    wr32(r.data() + 236, 0x63825363);             // magic cookie

    auto opt = [&](u8 code, std::initializer_list<u8> bytes) {
        r.push_back(code);
        r.push_back(static_cast<u8>(bytes.size()));
        r.insert(r.end(), bytes);
    };
    auto opt32 = [&](u8 code, u32 v) {
        opt(code, {static_cast<u8>(v >> 24), static_cast<u8>(v >> 16), static_cast<u8>(v >> 8),
                   static_cast<u8>(v)});
    };
    opt(53, {reply_type});
    opt32(54, cfg_.gateway_ip);                   // server identifier
    if (reply_type != 6) {
        opt32(51, 86400);                         // lease time: a day
        opt32(58, 43200);                         // T1
        opt32(59, 75600);                         // T2
        opt32(1, cfg_.netmask);
        opt32(3, cfg_.gateway_ip);                // router
        opt32(6, cfg_.dns_ip);                    // DNS server
        opt32(28, (cfg_.guest_ip & cfg_.netmask) | ~cfg_.netmask);   // broadcast
    }
    r.push_back(255);                             // end
    while (r.size() < 300) r.push_back(0);        // some clients want a minimum size

    // A client with no address yet cannot be reached by unicast, so the reply is
    // broadcast - to the client's own MAC at layer 2 would be rejected by some
    // stacks that filter on it before they have an address.
    queue_udp(cfg_.gateway_ip, 67, 0xffffffff, 68, r.data(), r.size(), BROADCAST_MAC.data());
}

// --- UDP ----------------------------------------------------------------------------------------

// Where a guest-visible destination really is on the host, or false if it should
// go nowhere.
bool UserNet::map_destination(u32 virt_ip, u16 virt_port, u32& host_ip, u16& host_port) const {
    host_port = virt_port;
    if (virt_ip == cfg_.gateway_ip) { host_ip = cfg_.host_ip; return true; }
    if (virt_ip == cfg_.dns_ip) {
        if (virt_port != 53) return false;        // the DNS address only serves DNS
        host_ip = cfg_.dns_server;
        host_port = cfg_.dns_port;
        return true;
    }
    // The rest of our own subnet is empty, and the broadcast and multicast ranges
    // are not something one host socket can speak for.
    if ((virt_ip & cfg_.netmask) == (cfg_.guest_ip & cfg_.netmask)) return false;
    if (virt_ip == 0xffffffff || (virt_ip >> 28) == 0xe) return false;
    host_ip = virt_ip;
    return true;
}

void UserNet::handle_udp(u32 src, u32 dst, const u8* p, std::size_t len) {
    (void)src;
    if (len < 8) { ++dropped_; return; }
    const u16 sport = rd16(p);
    const u16 dport = rd16(p + 2);
    const std::size_t ulen = rd16(p + 4);
    if (ulen < 8 || ulen > len) { ++dropped_; return; }
    const u8* data = p + 8;
    const std::size_t dlen = ulen - 8;

    if (dport == 67) { handle_dhcp(data, dlen); return; }

    u32 host_ip;
    u16 host_port;
    if (!map_destination(dst, dport, host_ip, host_port)) { ++dropped_; return; }

    const u64 key = flow_key(sport, dst, dport);
    auto it = udp_.find(key);
    if (it == udp_.end()) {
        if (udp_.size() >= MAX_UDP_FLOWS) { ++dropped_; return; }
        UdpFlow f;
        f.sock = hostnet::udp_socket();
        if (f.sock == INVALID_SOCK) { ++dropped_; return; }
        f.guest_port = sport;
        f.virt_ip = dst;
        f.virt_port = dport;
        it = udp_.emplace(key, f).first;
    }
    it->second.last = Clock::now();
    hostnet::send_to(it->second.sock, data, dlen, host_ip, host_port);
}

void UserNet::pump_udp() {
    const auto now = Clock::now();
    u8 buf[2048];
    for (auto it = udp_.begin(); it != udp_.end();) {
        UdpFlow& f = it->second;
        for (int i = 0; i < 16; ++i) {
            u32 from_ip;
            u16 from_port;
            const long n = hostnet::recv_from(f.sock, buf, sizeof buf, from_ip, from_port);
            if (n <= 0) break;
            f.last = now;
            // The reply has to look like it came from where the guest sent the
            // request, not from the host socket that actually carried it.
            if (static_cast<std::size_t>(n) + 28 > MTU) { ++dropped_; continue; }
            queue_udp(f.virt_ip, f.virt_port, cfg_.guest_ip, f.guest_port, buf,
                      static_cast<std::size_t>(n));
        }
        if (now - f.last > UDP_IDLE_TIMEOUT) {
            hostnet::close_socket(f.sock);
            it = udp_.erase(it);
        } else {
            ++it;
        }
    }
}

// --- TCP ----------------------------------------------------------------------------------------

void UserNet::handle_tcp(u32 src, u32 dst, const u8* p, std::size_t len) {
    (void)src;
    if (len < 20) { ++dropped_; return; }
    const u16 sport = rd16(p);
    const u16 dport = rd16(p + 2);
    const u32 seq   = rd32(p + 4);
    const u32 ack   = rd32(p + 8);
    const std::size_t hlen = static_cast<std::size_t>(p[12] >> 4) * 4;
    const u8  flags = p[13];
    const u16 win   = rd16(p + 14);
    if (hlen < 20 || hlen > len) { ++dropped_; return; }

    const u8* data = p + hlen;
    std::size_t dlen = len - hlen;
    const bool syn = (flags & TCP_SYN) != 0;
    const bool fin = (flags & TCP_FIN) != 0;
    const bool rst = (flags & TCP_RST) != 0;
    const bool has_ack = (flags & TCP_ACK) != 0;

    const u64 key = flow_key(sport, dst, dport);
    auto it = tcp_.find(key);

    // --- a new connection --------------------------------------------------------------
    if (syn && !has_ack) {
        if (it != tcp_.end()) {
            // A retransmitted SYN while we are still connecting or waiting for
            // the handshake to finish: the answer is on its way, say nothing. On
            // an established flow it is a stale reuse of the port - start over.
            if (it->second.state != TcpFlow::State::Established) return;
            hostnet::close_socket(it->second.sock);
            tcp_.erase(it);
        }

        u32 host_ip;
        u16 host_port;
        if (tcp_.size() >= MAX_TCP_FLOWS || !map_destination(dst, dport, host_ip, host_port)) {
            reset_for(dst, dport, sport, false, seq, ack, 1);
            return;
        }

        TcpFlow f;
        f.sock = hostnet::tcp_socket();
        if (f.sock == INVALID_SOCK) { reset_for(dst, dport, sport, false, seq, ack, 1); return; }
        f.guest_port = sport;
        f.dst_ip = dst;
        f.dst_port = dport;
        f.rcv_nxt = seq + 1;                      // the SYN occupies one sequence number
        f.guest_window = win;
        f.last = Clock::now();

        // The guest's maximum segment size is in a TCP option on its SYN.
        for (std::size_t i = 20; i + 1 < hlen;) {
            const u8 kind = p[i];
            if (kind == 0) break;
            if (kind == 1) { ++i; continue; }
            const std::size_t olen = p[i + 1];
            if (olen < 2 || i + olen > hlen) break;
            if (kind == 2 && olen == 4) f.mss = std::min<u16>(rd16(p + i + 2), MSS_OURS);
            i += olen;
        }
        if (f.mss < 64) f.mss = 536;

        const int c = hostnet::connect_ipv4(f.sock, host_ip, host_port);
        if (c < 0) {
            hostnet::close_socket(f.sock);
            reset_for(dst, dport, sport, false, seq, ack, 1);
            return;
        }
        f.snd_nxt = f.snd_una = next_iss_;
        next_iss_ += 64000;
        auto placed = tcp_.emplace(key, std::move(f)).first;
        if (c == 1) pump_tcp();                   // connected at once: answer now
        (void)placed;
        return;
    }

    // --- a segment for a connection we don't know ----------------------------------------
    if (it == tcp_.end()) {
        if (!rst) {
            reset_for(dst, dport, sport, has_ack, seq, ack,
                      static_cast<u32>(dlen) + (syn ? 1 : 0) + (fin ? 1 : 0));
        }
        return;
    }

    TcpFlow& f = it->second;
    f.last = Clock::now();

    if (rst) {
        hostnet::close_socket(f.sock);
        tcp_.erase(it);
        return;
    }

    // --- acknowledgments -------------------------------------------------------------------
    if (has_ack) {
        if (f.state == TcpFlow::State::SynAckSent && ack == f.snd_nxt) {
            f.state = TcpFlow::State::Established;
        }
        if (f.state == TcpFlow::State::Established && seq_le(f.snd_una, ack) &&
            seq_le(ack, f.snd_nxt)) {
            f.snd_una = ack;
            f.guest_window = win;
            if (f.fin_sent && ack == f.snd_nxt) f.fin_acked = true;
        }
    }
    if (f.state != TcpFlow::State::Established) return;

    // --- data from the guest -----------------------------------------------------------------
    bool need_ack = false;
    if (dlen > 0) {
        need_ack = true;
        if (seq_lt(seq, f.rcv_nxt)) {
            // Retransmission or overlap: keep only the part we have not seen.
            const u32 skip = f.rcv_nxt - seq;
            if (skip >= dlen) { data += dlen; dlen = 0; }
            else { data += skip; dlen -= skip; }
        }
        if (dlen > 0) {
            const u32 effective_seq = static_cast<u32>(f.rcv_nxt);
            if (seq_lt(seq, f.rcv_nxt) || seq == f.rcv_nxt) {
                if (f.to_host.size() + dlen <= TO_HOST_LIMIT) {
                    f.to_host.insert(f.to_host.end(), data, data + dlen);
                    f.rcv_nxt = effective_seq + static_cast<u32>(dlen);
                }
                // else: no room - leave rcv_nxt alone; the guest will resend.
            }
            // seq beyond rcv_nxt is out of order. A lossless link never produces
            // it; if it happens the duplicate ACK below asks for what we want.
        }
    }

    if (fin && !f.guest_fin && seq + static_cast<u32>(len - hlen) == f.rcv_nxt) {
        f.rcv_nxt += 1;                           // the FIN occupies one sequence number
        f.guest_fin = true;
        need_ack = true;
    }

    if (need_ack) queue_tcp(f, TCP_ACK, f.snd_nxt, f.rcv_nxt, nullptr, 0);
    pump_tcp();
}

void UserNet::pump_tcp() {
    const auto now = Clock::now();
    u8 buf[MSS_OURS];

    for (auto it = tcp_.begin(); it != tcp_.end();) {
        TcpFlow& f = it->second;
        bool remove = false;

        switch (f.state) {
            case TcpFlow::State::Connecting: {
                const int r = hostnet::connect_result(f.sock);
                if (r > 0) {
                    // The host connection is up: complete the guest's handshake.
                    queue_tcp(f, TCP_SYN | TCP_ACK, f.snd_nxt, f.rcv_nxt, nullptr, 0, true);
                    f.snd_nxt += 1;                // our SYN occupies one sequence number
                    f.state = TcpFlow::State::SynAckSent;
                    f.last = now;
                } else if (r < 0) {
                    // Refused or unreachable: the guest sees an ordinary RST.
                    queue_tcp_raw(f.dst_ip, f.dst_port, cfg_.guest_ip, f.guest_port,
                                  TCP_RST | TCP_ACK, 0, f.rcv_nxt, 0, nullptr, 0, false);
                    remove = true;
                } else if (now - f.last > TCP_HANDSHAKE_TIMEOUT) {
                    remove = true;
                }
                break;
            }

            case TcpFlow::State::SynAckSent:
                if (now - f.last > TCP_HANDSHAKE_TIMEOUT) remove = true;
                break;

            case TcpFlow::State::Established: {
                // Guest -> host.
                while (!f.to_host.empty()) {
                    const long n = hostnet::send_data(f.sock, f.to_host.data(), f.to_host.size());
                    if (n < 0) { remove = true; break; }
                    if (n == 0) break;
                    f.to_host.erase(f.to_host.begin(), f.to_host.begin() + n);
                    // Room opened up; tell the guest it may send more.
                    queue_tcp(f, TCP_ACK, f.snd_nxt, f.rcv_nxt, nullptr, 0);
                }
                if (remove) {
                    queue_tcp(f, TCP_RST | TCP_ACK, f.snd_nxt, f.rcv_nxt, nullptr, 0);
                    break;
                }
                if (f.guest_fin && f.to_host.empty() && !f.host_shut) {
                    hostnet::shutdown_write(f.sock);
                    f.host_shut = true;
                }

                // Host -> guest, never sending more than the guest has room for.
                for (int segments = 0; segments < 32 && !f.host_eof; ++segments) {
                    const u32 in_flight = f.snd_nxt - f.snd_una;
                    if (f.guest_window <= in_flight) break;
                    const std::size_t room = std::min<std::size_t>(
                        {static_cast<std::size_t>(f.guest_window - in_flight), f.mss, sizeof buf});
                    const long n = hostnet::recv_data(f.sock, buf, room);
                    if (n == 0) break;
                    if (n == -2) {
                        // The host closed its end: pass that on as a FIN.
                        f.host_eof = true;
                        queue_tcp(f, TCP_FIN | TCP_ACK, f.snd_nxt, f.rcv_nxt, nullptr, 0);
                        f.snd_nxt += 1;
                        f.fin_sent = true;
                        break;
                    }
                    if (n < 0) {
                        queue_tcp(f, TCP_RST | TCP_ACK, f.snd_nxt, f.rcv_nxt, nullptr, 0);
                        remove = true;
                        break;
                    }
                    queue_tcp(f, TCP_PSH | TCP_ACK, f.snd_nxt, f.rcv_nxt, buf,
                              static_cast<std::size_t>(n));
                    f.snd_nxt += static_cast<u32>(n);
                    f.last = now;
                }

                // Both directions finished and acknowledged: the flow is done.
                if (f.fin_acked && f.guest_fin && f.to_host.empty()) remove = true;
                else if (now - f.last > TCP_LINGER_TIMEOUT && (f.guest_fin || f.host_eof)) remove = true;
                break;
            }
        }

        if (remove) {
            hostnet::close_socket(f.sock);
            it = tcp_.erase(it);
        } else {
            ++it;
        }
    }
}

void UserNet::pump() {
    pump_udp();
    pump_tcp();
}
