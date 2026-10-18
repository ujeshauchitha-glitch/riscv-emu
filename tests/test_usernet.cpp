#include <array>
#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "hostnet.hpp"
#include "test_util.hpp"
#include "usernet.hpp"

// ---------------------------------------------------------------------------
// The user-mode network, tested end to end against real sockets.
//
// The "guest" here is this test, which builds raw Ethernet frames the way a
// kernel's network stack would and parses what comes back. The "internet" is a
// handful of servers on the host's loopback interface. Nothing leaves the
// machine, but every layer in between - ARP, DHCP, ICMP, UDP, a full TCP
// handshake with data and teardown - runs for real.
//
// The frame builders below are written independently of src/usernet.cpp on
// purpose: a test that used the stack's own encoders would agree with it even
// where both were wrong.
// ---------------------------------------------------------------------------

namespace {

constexpr u32 GUEST   = 0x0a00020f;   // 10.0.2.15
constexpr u32 GATEWAY = 0x0a000202;   // 10.0.2.2
constexpr u32 DNS     = 0x0a000203;   // 10.0.2.3
constexpr u32 LOOPBACK = 0x7f000001;

const std::array<u8, 6> GUEST_MAC = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
const std::array<u8, 6> GW_MAC    = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};

void put16(std::vector<u8>& v, u16 x) { v.push_back(static_cast<u8>(x >> 8)); v.push_back(static_cast<u8>(x)); }
void put32(std::vector<u8>& v, u32 x) { put16(v, static_cast<u16>(x >> 16)); put16(v, static_cast<u16>(x)); }
u16 get16(const u8* p) { return static_cast<u16>((p[0] << 8) | p[1]); }
u32 get32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }

u16 inet_checksum(const std::vector<u8>& data, u32 initial = 0) {
    u32 sum = initial;
    for (std::size_t i = 0; i + 1 < data.size(); i += 2) sum += get16(data.data() + i);
    if (data.size() & 1) sum += static_cast<u32>(data.back()) << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return static_cast<u16>(~sum);
}

u32 pseudo(u32 src, u32 dst, u8 proto, std::size_t len) {
    return (src >> 16) + (src & 0xffff) + (dst >> 16) + (dst & 0xffff) + proto + static_cast<u32>(len);
}

std::vector<u8> eth(const std::array<u8, 6>& dst, u16 type, const std::vector<u8>& payload) {
    std::vector<u8> f(dst.begin(), dst.end());
    f.insert(f.end(), GUEST_MAC.begin(), GUEST_MAC.end());
    put16(f, type);
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

std::vector<u8> ipv4(u8 proto, u32 src, u32 dst, const std::vector<u8>& payload) {
    std::vector<u8> h = {0x45, 0};
    put16(h, static_cast<u16>(20 + payload.size()));
    put16(h, 1);
    put16(h, 0x4000);
    h.push_back(64);
    h.push_back(proto);
    put16(h, 0);
    put32(h, src);
    put32(h, dst);
    const u16 sum = inet_checksum(h);
    h[10] = static_cast<u8>(sum >> 8);
    h[11] = static_cast<u8>(sum);
    h.insert(h.end(), payload.begin(), payload.end());
    return h;
}

std::vector<u8> udp_segment(u32 src, u32 dst, u16 sport, u16 dport, const std::vector<u8>& data) {
    std::vector<u8> s;
    put16(s, sport); put16(s, dport); put16(s, static_cast<u16>(8 + data.size())); put16(s, 0);
    s.insert(s.end(), data.begin(), data.end());
    const u16 sum = inet_checksum(s, pseudo(src, dst, 17, s.size()));
    s[6] = static_cast<u8>(sum >> 8);
    s[7] = static_cast<u8>(sum);
    return s;
}

std::vector<u8> tcp_segment(u32 src, u32 dst, u16 sport, u16 dport, u32 seq, u32 ack, u8 flags,
                            const std::vector<u8>& data, bool mss = false) {
    std::vector<u8> s;
    put16(s, sport); put16(s, dport); put32(s, seq); put32(s, ack);
    s.push_back(static_cast<u8>((mss ? 6 : 5) << 4));
    s.push_back(flags);
    put16(s, 65535);
    put16(s, 0); put16(s, 0);
    if (mss) { s.push_back(2); s.push_back(4); put16(s, 1460); }
    s.insert(s.end(), data.begin(), data.end());
    const u16 sum = inet_checksum(s, pseudo(src, dst, 6, s.size()));
    s[16] = static_cast<u8>(sum >> 8);
    s[17] = static_cast<u8>(sum);
    return s;
}

// --- parsing what the stack sends back --------------------------------------------------

struct Parsed {
    bool ok = false;
    u16  ethertype = 0;
    std::array<u8, 6> src_mac{}, dst_mac{};
    // IPv4
    u8  proto = 0;
    u32 src = 0, dst = 0;
    bool ip_checksum_ok = false;
    std::vector<u8> l4;          // the transport segment
    bool l4_checksum_ok = false;
    // UDP / TCP convenience fields
    u16 sport = 0, dport = 0;
    u32 seq = 0, ack = 0;
    u8  flags = 0;
    std::vector<u8> payload;
    std::vector<u8> raw;
};

Parsed parse(const std::vector<u8>& f) {
    Parsed p;
    p.raw = f;
    if (f.size() < 14) return p;
    std::memcpy(p.dst_mac.data(), f.data(), 6);
    std::memcpy(p.src_mac.data(), f.data() + 6, 6);
    p.ethertype = get16(f.data() + 12);
    p.ok = true;
    if (p.ethertype != 0x0800 || f.size() < 14 + 20) return p;

    const u8* ip = f.data() + 14;
    const std::size_t ihl = (ip[0] & 0x0f) * 4;
    const std::size_t total = get16(ip + 2);
    p.proto = ip[9];
    p.src = get32(ip + 12);
    p.dst = get32(ip + 16);
    p.ip_checksum_ok = inet_checksum(std::vector<u8>(ip, ip + ihl)) == 0;
    p.l4.assign(ip + ihl, ip + total);

    const u32 ph = pseudo(p.src, p.dst, p.proto, p.l4.size());
    if (p.proto == 17 && p.l4.size() >= 8) {
        p.sport = get16(p.l4.data()); p.dport = get16(p.l4.data() + 2);
        p.payload.assign(p.l4.begin() + 8, p.l4.end());
        p.l4_checksum_ok = inet_checksum(p.l4, ph) == 0;
    } else if (p.proto == 6 && p.l4.size() >= 20) {
        p.sport = get16(p.l4.data()); p.dport = get16(p.l4.data() + 2);
        p.seq = get32(p.l4.data() + 4); p.ack = get32(p.l4.data() + 8);
        const std::size_t off = (p.l4[12] >> 4) * 4;
        p.flags = p.l4[13];
        p.payload.assign(p.l4.begin() + off, p.l4.end());
        p.l4_checksum_ok = inet_checksum(p.l4, ph) == 0;
    } else if (p.proto == 1) {
        p.l4_checksum_ok = inet_checksum(p.l4) == 0;
    }
    return p;
}

// Pump the stack and collect frames until `done` is satisfied or we give up.
// Real sockets deliver at their own pace, so this polls in short sleeps.
struct Harness {
    UserNet net;
    std::vector<Parsed> inbox;

    explicit Harness(const UserNet::Config& cfg = UserNet::Config{}) : net(cfg) {}

    void send(const std::vector<u8>& frame) { net.send(frame.data(), frame.size()); }

    void drain() {
        std::vector<u8> f;
        while (net.receive(f)) inbox.push_back(parse(f));
    }

    bool wait_for(const std::function<bool()>& done, int ms = 3000) {
        for (int waited = 0; waited <= ms; ++waited) {
            net.pump();
            drain();
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    // The first not-yet-consumed frame matching `pred`, removed from the inbox.
    bool take(const std::function<bool(const Parsed&)>& pred, Parsed& out) {
        for (std::size_t i = 0; i < inbox.size(); ++i) {
            if (pred(inbox[i])) { out = inbox[i]; inbox.erase(inbox.begin() + i); return true; }
        }
        return false;
    }
};

std::vector<u8> bytes(const std::string& s) { return std::vector<u8>(s.begin(), s.end()); }

// --- ARP --------------------------------------------------------------------------------------

std::vector<u8> arp_request(u32 sender, u32 target) {
    std::vector<u8> a;
    put16(a, 1); put16(a, 0x0800); a.push_back(6); a.push_back(4); put16(a, 1);
    a.insert(a.end(), GUEST_MAC.begin(), GUEST_MAC.end());
    put32(a, sender);
    for (int i = 0; i < 6; ++i) a.push_back(0);
    put32(a, target);
    return eth({0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, 0x0806, a);
}

void test_arp_answers_for_the_gateway_and_dns_only() {
    Harness h;
    h.send(arp_request(GUEST, GATEWAY));
    h.drain();
    CHECK_EQ(h.inbox.size(), 1);
    const Parsed& r = h.inbox[0];
    CHECK_EQ_U(r.ethertype, 0x0806);
    CHECK_EQ_U(get16(r.raw.data() + 14 + 6), 2);                       // a reply
    CHECK(std::memcmp(r.raw.data() + 14 + 8, GW_MAC.data(), 6) == 0);  // sender hardware address
    CHECK_EQ_U(get32(r.raw.data() + 14 + 14), GATEWAY);
    CHECK(std::memcmp(r.dst_mac.data(), GUEST_MAC.data(), 6) == 0);

    h.inbox.clear();
    h.send(arp_request(GUEST, DNS));
    h.drain();
    CHECK_EQ(h.inbox.size(), 1);

    h.inbox.clear();
    h.send(arp_request(GUEST, 0x0a000263));                            // 10.0.2.99: nobody
    h.drain();
    CHECK_EQ(h.inbox.size(), 0);
}

// --- ICMP -------------------------------------------------------------------------------------

std::vector<u8> icmp_echo(const std::vector<u8>& body) {
    std::vector<u8> m = {8, 0, 0, 0, 0x12, 0x34, 0x00, 0x07};
    m.insert(m.end(), body.begin(), body.end());
    const u16 sum = inet_checksum(m);
    m[2] = static_cast<u8>(sum >> 8);
    m[3] = static_cast<u8>(sum);
    return m;
}

void test_ping_to_the_gateway_is_answered() {
    Harness h;
    const std::vector<u8> body = bytes("abcdefghijklmnop");
    h.send(eth(GW_MAC, 0x0800, ipv4(1, GUEST, GATEWAY, icmp_echo(body))));
    h.drain();
    CHECK_EQ(h.inbox.size(), 1);
    const Parsed& r = h.inbox[0];
    CHECK_EQ_U(r.proto, 1);
    CHECK_EQ_U(r.src, GATEWAY);
    CHECK_EQ_U(r.dst, GUEST);
    CHECK(r.ip_checksum_ok);
    CHECK(r.l4_checksum_ok);
    CHECK_EQ_U(r.l4[0], 0);                                            // echo reply
    CHECK_EQ_U(get16(r.l4.data() + 4), 0x1234);                        // same identifier
    CHECK_EQ_U(get16(r.l4.data() + 6), 7);                             // same sequence
    CHECK(std::vector<u8>(r.l4.begin() + 8, r.l4.end()) == body);

    // A ping beyond the gateway needs a raw socket on the host; it is dropped.
    h.inbox.clear();
    h.send(eth(GW_MAC, 0x0800, ipv4(1, GUEST, 0x08080808, icmp_echo(body))));
    h.drain();
    CHECK_EQ(h.inbox.size(), 0);
}

// --- DHCP -------------------------------------------------------------------------------------

std::vector<u8> dhcp_message(u8 type, u32 requested = 0) {
    std::vector<u8> m(240, 0);
    m[0] = 1; m[1] = 1; m[2] = 6;
    m[4] = 0xde; m[5] = 0xad; m[6] = 0xbe; m[7] = 0xef;                 // xid
    m[10] = 0x80;                                                       // broadcast flag
    std::memcpy(m.data() + 28, GUEST_MAC.data(), 6);
    m[236] = 0x63; m[237] = 0x82; m[238] = 0x53; m[239] = 0x63;
    m.push_back(53); m.push_back(1); m.push_back(type);
    if (requested) { m.push_back(50); m.push_back(4); put32(m, requested); }
    m.push_back(255);
    return m;
}

// Find option `code` in a DHCP reply payload and return its bytes.
std::vector<u8> dhcp_option(const std::vector<u8>& m, u8 code) {
    for (std::size_t i = 240; i + 1 < m.size();) {
        if (m[i] == 255) break;
        if (m[i] == 0) { ++i; continue; }
        const std::size_t len = m[i + 1];
        if (m[i] == code) return std::vector<u8>(m.begin() + i + 2, m.begin() + i + 2 + len);
        i += 2 + len;
    }
    return {};
}

void test_dhcp_offers_and_acknowledges_the_guest_address() {
    Harness h;
    const std::array<u8, 6> bcast = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

    h.send(eth(bcast, 0x0800, ipv4(17, 0, 0xffffffff, udp_segment(0, 0xffffffff, 68, 67, dhcp_message(1)))));
    h.drain();
    CHECK_EQ(h.inbox.size(), 1);
    Parsed offer = h.inbox[0];
    CHECK_EQ_U(offer.sport, 67);
    CHECK_EQ_U(offer.dport, 68);
    CHECK(offer.ip_checksum_ok);
    CHECK(offer.l4_checksum_ok);
    CHECK_EQ_U(get32(offer.payload.data() + 16), GUEST);                // yiaddr
    CHECK_EQ_U(offer.payload[4], 0xde);                                 // xid echoed
    CHECK_EQ_U(dhcp_option(offer.payload, 53)[0], 2);                   // OFFER
    CHECK_EQ_U(get32(dhcp_option(offer.payload, 3).data()), GATEWAY);   // router
    CHECK_EQ_U(get32(dhcp_option(offer.payload, 6).data()), DNS);       // DNS server
    CHECK_EQ_U(get32(dhcp_option(offer.payload, 1).data()), 0xffffff00);
    CHECK_EQ_U(get32(dhcp_option(offer.payload, 54).data()), GATEWAY);  // server id

    h.inbox.clear();
    h.send(eth(bcast, 0x0800, ipv4(17, 0, 0xffffffff, udp_segment(0, 0xffffffff, 68, 67, dhcp_message(3, GUEST)))));
    h.drain();
    CHECK_EQ(h.inbox.size(), 1);
    CHECK_EQ_U(dhcp_option(h.inbox[0].payload, 53)[0], 5);              // ACK

    // Asking to keep some other address is refused, so the client starts over.
    h.inbox.clear();
    h.send(eth(bcast, 0x0800, ipv4(17, 0, 0xffffffff, udp_segment(0, 0xffffffff, 68, 67, dhcp_message(3, 0x0a000299)))));
    h.drain();
    CHECK_EQ(h.inbox.size(), 1);
    CHECK_EQ_U(dhcp_option(h.inbox[0].payload, 53)[0], 6);              // NAK
}

// --- UDP and DNS ------------------------------------------------------------------------------

// A UDP server on the host's loopback: replies to whatever it receives with a
// transformed copy of it.
struct UdpServer {
    sock_t sock = INVALID_SOCK;
    u16 port = 0;
    UdpServer() {
        hostnet::init();
        sock = hostnet::udp_socket();
        port = hostnet::bind_ipv4(sock, LOOPBACK, 0);
    }
    ~UdpServer() { hostnet::close_socket(sock); }

    // Wait for a datagram; reply with `reply(request)`. Returns the request.
    std::vector<u8> serve_one(const std::function<std::vector<u8>(const std::vector<u8>&)>& reply) {
        u8 buf[2048];
        for (int i = 0; i < 3000; ++i) {
            u32 ip; u16 p;
            const long n = hostnet::recv_from(sock, buf, sizeof buf, ip, p);
            if (n > 0) {
                std::vector<u8> req(buf, buf + n);
                const std::vector<u8> r = reply(req);
                hostnet::send_to(sock, r.data(), r.size(), ip, p);
                return req;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return {};
    }
};

void test_udp_round_trip_through_the_gateway() {
    UdpServer server;
    CHECK(server.port != 0);
    Harness h;

    h.send(eth(GW_MAC, 0x0800, ipv4(17, GUEST, GATEWAY, udp_segment(GUEST, GATEWAY, 40000, server.port, bytes("ping")))));

    const std::vector<u8> got = server.serve_one([](const std::vector<u8>& req) {
        std::vector<u8> r = bytes("re:");
        r.insert(r.end(), req.begin(), req.end());
        return r;
    });
    CHECK(got == bytes("ping"));                                        // it reached the host

    Parsed reply;
    CHECK(h.wait_for([&] { return h.take([](const Parsed& p) { return p.proto == 17; }, reply); }));
    CHECK_EQ_U(reply.src, GATEWAY);                                     // looks like it came from where it was sent
    CHECK_EQ_U(reply.sport, server.port);
    CHECK_EQ_U(reply.dst, GUEST);
    CHECK_EQ_U(reply.dport, 40000);
    CHECK(reply.payload == bytes("re:ping"));
    CHECK(reply.ip_checksum_ok);
    CHECK(reply.l4_checksum_ok);
    CHECK_EQ(h.net.udp_flows(), 1);
}

void test_dns_queries_are_forwarded_to_the_host_resolver() {
    UdpServer fake_resolver;
    UserNet::Config cfg;
    cfg.dns_server = LOOPBACK;
    cfg.dns_port = fake_resolver.port;
    Harness h(cfg);

    // A DNS-shaped message: id, flags, then a question.
    std::vector<u8> query = {0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0,
                             3, 'f', 'o', 'o', 0, 0, 1, 0, 1};
    h.send(eth(GW_MAC, 0x0800, ipv4(17, GUEST, DNS, udp_segment(GUEST, DNS, 53001, 53, query))));

    const std::vector<u8> seen = fake_resolver.serve_one([](const std::vector<u8>& q) {
        std::vector<u8> a = q;
        a[2] |= 0x80;                                                   // QR: this is a response
        return a;
    });
    CHECK(seen == query);

    Parsed reply;
    CHECK(h.wait_for([&] { return h.take([](const Parsed& p) { return p.proto == 17; }, reply); }));
    CHECK_EQ_U(reply.src, DNS);                                         // from 10.0.2.3, not the real resolver
    CHECK_EQ_U(reply.sport, 53);
    CHECK_EQ_U(reply.dport, 53001);
    CHECK_EQ_U(reply.payload[0], 0x12);
    CHECK((reply.payload[2] & 0x80) != 0);

    // DNS is only offered on port 53.
    h.inbox.clear();
    h.send(eth(GW_MAC, 0x0800, ipv4(17, GUEST, DNS, udp_segment(GUEST, DNS, 53002, 80, query))));
    h.drain();
    CHECK_EQ(h.inbox.size(), 0);
}

// --- TCP --------------------------------------------------------------------------------------

struct TcpServer {
    sock_t listener = INVALID_SOCK;
    sock_t conn = INVALID_SOCK;
    u16 port = 0;
    TcpServer() {
        hostnet::init();
        listener = hostnet::tcp_socket();
        port = hostnet::bind_ipv4(listener, LOOPBACK, 0);
        hostnet::listen_on(listener);
    }
    ~TcpServer() { hostnet::close_socket(conn); hostnet::close_socket(listener); }

    bool accept_within(Harness& h) {
        return h.wait_for([&] {
            if (conn == INVALID_SOCK) conn = hostnet::accept_connection(listener);
            return conn != INVALID_SOCK;
        });
    }
    // Read until `want` bytes arrive (while pumping the stack so it can forward).
    std::string read_exact(Harness& h, std::size_t want) {
        std::string got;
        h.wait_for([&] {
            u8 buf[2048];
            long n;
            while ((n = hostnet::recv_data(conn, buf, sizeof buf)) > 0) got.append(reinterpret_cast<char*>(buf), n);
            return got.size() >= want;
        });
        return got;
    }
};

void test_tcp_connection_end_to_end() {
    TcpServer server;
    CHECK(server.port != 0);
    Harness h;

    const u16 gport = 41000;
    u32 gseq = 1000;                                                    // the guest's initial sequence number

    // 1. The guest opens a connection to the gateway (the host).
    h.send(eth(GW_MAC, 0x0800, ipv4(6, GUEST, GATEWAY,
           tcp_segment(GUEST, GATEWAY, gport, server.port, gseq, 0, 0x02, {}, /*mss=*/true))));

    Parsed synack;
    CHECK(h.wait_for([&] { return h.take([](const Parsed& p) { return p.proto == 6 && (p.flags & 0x12) == 0x12; }, synack); }));
    CHECK(synack.ip_checksum_ok);
    CHECK(synack.l4_checksum_ok);
    CHECK_EQ_U(synack.src, GATEWAY);
    CHECK_EQ_U(synack.sport, server.port);
    CHECK_EQ_U(synack.ack, gseq + 1);                                   // acknowledges our SYN
    ++gseq;
    u32 server_seq = synack.seq + 1;                                    // their SYN used one number

    // 2. The guest completes the handshake; the proxy has connected to the host.
    h.send(eth(GW_MAC, 0x0800, ipv4(6, GUEST, GATEWAY,
           tcp_segment(GUEST, GATEWAY, gport, server.port, gseq, server_seq, 0x10, {}))));
    CHECK(server.accept_within(h));

    // 3. Guest -> host data.
    h.send(eth(GW_MAC, 0x0800, ipv4(6, GUEST, GATEWAY,
           tcp_segment(GUEST, GATEWAY, gport, server.port, gseq, server_seq, 0x18, bytes("hello")))));
    gseq += 5;
    CHECK(server.read_exact(h, 5) == "hello");
    Parsed ack;
    CHECK(h.wait_for([&] { return h.take([&](const Parsed& p) { return p.proto == 6 && p.flags == 0x10 && p.ack == gseq; }, ack); }));

    // 4. Host -> guest data.
    const std::string reply = "world!";
    CHECK(hostnet::send_data(server.conn, reinterpret_cast<const u8*>(reply.data()), reply.size()) == static_cast<long>(reply.size()));
    Parsed data;
    CHECK(h.wait_for([&] { return h.take([](const Parsed& p) { return p.proto == 6 && !p.payload.empty(); }, data); }));
    CHECK(data.payload == bytes(reply));
    CHECK_EQ_U(data.seq, server_seq);
    CHECK(data.l4_checksum_ok);
    server_seq += static_cast<u32>(reply.size());
    h.send(eth(GW_MAC, 0x0800, ipv4(6, GUEST, GATEWAY,
           tcp_segment(GUEST, GATEWAY, gport, server.port, gseq, server_seq, 0x10, {}))));

    // 5. The host closes its end: the guest receives a FIN.
    hostnet::close_socket(server.conn);
    server.conn = INVALID_SOCK;
    Parsed fin;
    CHECK(h.wait_for([&] { return h.take([](const Parsed& p) { return p.proto == 6 && (p.flags & 0x01); }, fin); }));
    CHECK_EQ_U(fin.seq, server_seq);
    server_seq += 1;

    // 6. The guest acknowledges it and sends its own FIN; the flow is torn down.
    h.send(eth(GW_MAC, 0x0800, ipv4(6, GUEST, GATEWAY,
           tcp_segment(GUEST, GATEWAY, gport, server.port, gseq, server_seq, 0x11, {}))));
    CHECK(h.wait_for([&] { return h.net.tcp_flows() == 0; }));
}

void test_tcp_to_a_closed_port_is_reset() {
    // Find a port with nothing listening by binding and releasing one.
    hostnet::init();
    sock_t probe = hostnet::tcp_socket();
    const u16 closed = hostnet::bind_ipv4(probe, LOOPBACK, 0);
    hostnet::close_socket(probe);

    Harness h;
    h.send(eth(GW_MAC, 0x0800, ipv4(6, GUEST, GATEWAY,
           tcp_segment(GUEST, GATEWAY, 42000, closed, 5000, 0, 0x02, {}, true))));
    Parsed rst;
    CHECK(h.wait_for([&] { return h.take([](const Parsed& p) { return p.proto == 6 && (p.flags & 0x04); }, rst); }));
    CHECK_EQ_U(rst.ack, 5001);
    CHECK_EQ(h.net.tcp_flows(), 0);
}

void test_tcp_segment_for_an_unknown_connection_is_reset() {
    Harness h;
    h.send(eth(GW_MAC, 0x0800, ipv4(6, GUEST, GATEWAY,
           tcp_segment(GUEST, GATEWAY, 43000, 80, 777, 888, 0x10, bytes("stray")))));
    h.drain();
    CHECK_EQ(h.inbox.size(), 1);
    CHECK((h.inbox[0].flags & 0x04) != 0);                              // RST
    CHECK_EQ_U(h.inbox[0].seq, 888);                                    // sequence taken from the ACK
}

void test_traffic_to_nowhere_is_dropped_not_crashed_on() {
    Harness h;
    // Runt frames, a bad IP header, an unknown ethertype, a fragment.
    h.net.send(reinterpret_cast<const u8*>("short"), 5);
    h.send(eth(GW_MAC, 0x86dd, std::vector<u8>(40, 0)));
    h.send(eth(GW_MAC, 0x0800, std::vector<u8>(10, 0x45)));
    std::vector<u8> frag = ipv4(17, GUEST, GATEWAY, udp_segment(GUEST, GATEWAY, 1, 2, bytes("x")));
    frag[6] = 0x20;                                                     // more-fragments
    h.send(eth(GW_MAC, 0x0800, frag));
    // Multicast and broadcast UDP have nowhere to go.
    h.send(eth(GW_MAC, 0x0800, ipv4(17, GUEST, 0xe0000001, udp_segment(GUEST, 0xe0000001, 5, 5, bytes("m")))));
    h.drain();
    CHECK_EQ(h.inbox.size(), 0);
    CHECK(h.net.dropped() >= 5);
}

}  // namespace

int main() {
    test_arp_answers_for_the_gateway_and_dns_only();
    test_ping_to_the_gateway_is_answered();
    test_dhcp_offers_and_acknowledges_the_guest_address();
    test_udp_round_trip_through_the_gateway();
    test_dns_queries_are_forwarded_to_the_host_resolver();
    test_tcp_connection_end_to_end();
    test_tcp_to_a_closed_port_is_reset();
    test_tcp_segment_for_an_unknown_connection_is_reset();
    test_traffic_to_nowhere_is_dropped_not_crashed_on();
    return testutil::summary("usernet");
}
