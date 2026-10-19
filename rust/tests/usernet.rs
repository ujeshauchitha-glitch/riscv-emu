//! The user-mode network, tested end to end against real sockets.
//!
//! The "guest" here is this test, which builds raw Ethernet frames the way a kernel's network
//! stack would and parses what comes back. The "internet" is a handful of servers on the host's
//! loopback interface. Nothing leaves the machine, but every layer in between - ARP, DHCP, ICMP,
//! UDP, a full TCP handshake with data and teardown - runs for real.
//!
//! The frame builders below are written independently of src/net/usernet.rs on purpose: a test
//! that used the stack's own encoders would agree with it even where both were wrong.

use std::io::{Read, Write};
use std::net::{Ipv4Addr, SocketAddrV4, TcpListener, TcpStream, UdpSocket};
use std::time::Duration;

use riscv_emu::net::usernet::{Config, UserNet};
use riscv_emu::virtio::net::NetBackend;

const GUEST: u32 = 0x0a00_020f; // 10.0.2.15
const GATEWAY: u32 = 0x0a00_0202; // 10.0.2.2
const DNS: u32 = 0x0a00_0203; // 10.0.2.3

const GUEST_MAC: [u8; 6] = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];
const GW_MAC: [u8; 6] = [0x52, 0x55, 0x0a, 0x00, 0x02, 0x02];
const BROADCAST: [u8; 6] = [0xff; 6];

fn put16(v: &mut Vec<u8>, x: u16) {
    v.extend_from_slice(&x.to_be_bytes());
}
fn put32(v: &mut Vec<u8>, x: u32) {
    v.extend_from_slice(&x.to_be_bytes());
}
fn get16(p: &[u8]) -> u16 {
    u16::from_be_bytes([p[0], p[1]])
}
fn get32(p: &[u8]) -> u32 {
    u32::from_be_bytes([p[0], p[1], p[2], p[3]])
}

fn inet_checksum(data: &[u8], initial: u32) -> u16 {
    let mut sum = initial;
    let mut chunks = data.chunks_exact(2);
    for c in &mut chunks {
        sum += get16(c) as u32;
    }
    if let [last] = chunks.remainder() {
        sum += (*last as u32) << 8;
    }
    while sum >> 16 != 0 {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    !(sum as u16)
}

fn pseudo(src: u32, dst: u32, proto: u8, len: usize) -> u32 {
    (src >> 16) + (src & 0xffff) + (dst >> 16) + (dst & 0xffff) + proto as u32 + len as u32
}

fn eth(dst: &[u8; 6], ethertype: u16, payload: &[u8]) -> Vec<u8> {
    let mut f = dst.to_vec();
    f.extend_from_slice(&GUEST_MAC);
    put16(&mut f, ethertype);
    f.extend_from_slice(payload);
    f
}

fn ipv4(proto: u8, src: u32, dst: u32, payload: &[u8]) -> Vec<u8> {
    let mut h = vec![0x45, 0];
    put16(&mut h, (20 + payload.len()) as u16);
    put16(&mut h, 1);
    put16(&mut h, 0x4000);
    h.push(64);
    h.push(proto);
    put16(&mut h, 0);
    put32(&mut h, src);
    put32(&mut h, dst);
    let sum = inet_checksum(&h, 0);
    h[10..12].copy_from_slice(&sum.to_be_bytes());
    h.extend_from_slice(payload);
    h
}

fn udp_segment(src: u32, dst: u32, sport: u16, dport: u16, data: &[u8]) -> Vec<u8> {
    let mut s = Vec::new();
    put16(&mut s, sport);
    put16(&mut s, dport);
    put16(&mut s, (8 + data.len()) as u16);
    put16(&mut s, 0);
    s.extend_from_slice(data);
    let sum = inet_checksum(&s, pseudo(src, dst, 17, s.len()));
    s[6..8].copy_from_slice(&sum.to_be_bytes());
    s
}

#[allow(clippy::too_many_arguments)]
fn tcp_segment(src: u32, dst: u32, sport: u16, dport: u16, seq: u32, ack: u32, flags: u8, data: &[u8], mss: bool) -> Vec<u8> {
    let mut s = Vec::new();
    put16(&mut s, sport);
    put16(&mut s, dport);
    put32(&mut s, seq);
    put32(&mut s, ack);
    s.push((if mss { 6 } else { 5 }) << 4);
    s.push(flags);
    put16(&mut s, 65535);
    put16(&mut s, 0);
    put16(&mut s, 0);
    if mss {
        s.push(2);
        s.push(4);
        put16(&mut s, 1460);
    }
    s.extend_from_slice(data);
    let sum = inet_checksum(&s, pseudo(src, dst, 6, s.len()));
    s[16..18].copy_from_slice(&sum.to_be_bytes());
    s
}

// --- parsing what the stack sends back --------------------------------------------------

#[derive(Default, Clone)]
struct Parsed {
    ethertype: u16,
    dst_mac: [u8; 6],
    // IPv4
    proto: u8,
    src: u32,
    dst: u32,
    ip_checksum_ok: bool,
    l4: Vec<u8>,
    l4_checksum_ok: bool,
    // UDP / TCP convenience fields
    sport: u16,
    dport: u16,
    seq: u32,
    ack: u32,
    flags: u8,
    payload: Vec<u8>,
    raw: Vec<u8>,
}

fn parse(f: &[u8]) -> Parsed {
    let mut p = Parsed { raw: f.to_vec(), ..Parsed::default() };
    if f.len() < 14 {
        return p;
    }
    p.dst_mac.copy_from_slice(&f[..6]);
    p.ethertype = get16(&f[12..]);
    if p.ethertype != 0x0800 || f.len() < 14 + 20 {
        return p;
    }
    let ip = &f[14..];
    let ihl = (ip[0] & 0x0f) as usize * 4;
    let total = get16(&ip[2..]) as usize;
    p.proto = ip[9];
    p.src = get32(&ip[12..]);
    p.dst = get32(&ip[16..]);
    p.ip_checksum_ok = inet_checksum(&ip[..ihl], 0) == 0;
    p.l4 = ip[ihl..total].to_vec();

    let ph = pseudo(p.src, p.dst, p.proto, p.l4.len());
    if p.proto == 17 && p.l4.len() >= 8 {
        p.sport = get16(&p.l4);
        p.dport = get16(&p.l4[2..]);
        p.payload = p.l4[8..].to_vec();
        p.l4_checksum_ok = inet_checksum(&p.l4, ph) == 0;
    } else if p.proto == 6 && p.l4.len() >= 20 {
        p.sport = get16(&p.l4);
        p.dport = get16(&p.l4[2..]);
        p.seq = get32(&p.l4[4..]);
        p.ack = get32(&p.l4[8..]);
        let off = (p.l4[12] >> 4) as usize * 4;
        p.flags = p.l4[13];
        p.payload = p.l4[off..].to_vec();
        p.l4_checksum_ok = inet_checksum(&p.l4, ph) == 0;
    } else if p.proto == 1 {
        p.l4_checksum_ok = inet_checksum(&p.l4, 0) == 0;
    }
    p
}

/// Pump the stack and collect frames until a condition holds or we give up. Real sockets deliver
/// at their own pace, so this polls in short sleeps.
struct Harness {
    net: UserNet,
    inbox: Vec<Parsed>,
}

impl Harness {
    fn new() -> Harness {
        Harness::with_config(Config::default())
    }
    fn with_config(cfg: Config) -> Harness {
        Harness { net: UserNet::new(cfg), inbox: Vec::new() }
    }

    fn send(&mut self, frame: &[u8]) {
        self.net.send(frame);
    }

    fn drain(&mut self) {
        while let Some(f) = self.net.receive() {
            self.inbox.push(parse(&f));
        }
    }

    fn wait_for(&mut self, mut done: impl FnMut(&mut Harness) -> bool) -> bool {
        for _ in 0..=3000 {
            self.net.pump();
            self.drain();
            if done(self) {
                return true;
            }
            std::thread::sleep(Duration::from_millis(1));
        }
        false
    }

    /// The first not-yet-consumed frame matching `pred`, removed from the inbox.
    fn take(&mut self, pred: impl Fn(&Parsed) -> bool) -> Option<Parsed> {
        let i = self.inbox.iter().position(pred)?;
        Some(self.inbox.remove(i))
    }

    /// Wait for a frame matching `pred` and return it.
    fn wait_take(&mut self, pred: impl Fn(&Parsed) -> bool + Copy) -> Option<Parsed> {
        let mut found = None;
        self.wait_for(|h| {
            found = h.take(pred);
            found.is_some()
        });
        found
    }
}

// --- ARP --------------------------------------------------------------------------------------

fn arp_request(sender: u32, target: u32) -> Vec<u8> {
    let mut a = Vec::new();
    put16(&mut a, 1);
    put16(&mut a, 0x0800);
    a.push(6);
    a.push(4);
    put16(&mut a, 1);
    a.extend_from_slice(&GUEST_MAC);
    put32(&mut a, sender);
    a.extend_from_slice(&[0; 6]);
    put32(&mut a, target);
    eth(&BROADCAST, 0x0806, &a)
}

#[test]
fn arp_answers_for_the_gateway_and_dns_only() {
    let mut h = Harness::new();
    h.send(&arp_request(GUEST, GATEWAY));
    h.drain();
    assert_eq!(h.inbox.len(), 1);
    let r = &h.inbox[0];
    assert_eq!(r.ethertype, 0x0806);
    assert_eq!(get16(&r.raw[14 + 6..]), 2); // a reply
    assert_eq!(&r.raw[14 + 8..14 + 14], &GW_MAC); // sender hardware address
    assert_eq!(get32(&r.raw[14 + 14..]), GATEWAY);
    assert_eq!(r.dst_mac, GUEST_MAC);

    h.inbox.clear();
    h.send(&arp_request(GUEST, DNS));
    h.drain();
    assert_eq!(h.inbox.len(), 1);

    h.inbox.clear();
    h.send(&arp_request(GUEST, 0x0a00_0263)); // 10.0.2.99: nobody
    h.drain();
    assert_eq!(h.inbox.len(), 0);
}

// --- ICMP -------------------------------------------------------------------------------------

fn icmp_echo(body: &[u8]) -> Vec<u8> {
    let mut m = vec![8, 0, 0, 0, 0x12, 0x34, 0x00, 0x07];
    m.extend_from_slice(body);
    let sum = inet_checksum(&m, 0);
    m[2..4].copy_from_slice(&sum.to_be_bytes());
    m
}

#[test]
fn ping_to_the_gateway_is_answered() {
    let mut h = Harness::new();
    let body = b"abcdefghijklmnop";
    h.send(&eth(&GW_MAC, 0x0800, &ipv4(1, GUEST, GATEWAY, &icmp_echo(body))));
    h.drain();
    assert_eq!(h.inbox.len(), 1);
    let r = &h.inbox[0];
    assert_eq!(r.proto, 1);
    assert_eq!(r.src, GATEWAY);
    assert_eq!(r.dst, GUEST);
    assert!(r.ip_checksum_ok);
    assert!(r.l4_checksum_ok);
    assert_eq!(r.l4[0], 0); // echo reply
    assert_eq!(get16(&r.l4[4..]), 0x1234); // same identifier
    assert_eq!(get16(&r.l4[6..]), 7); // same sequence
    assert_eq!(&r.l4[8..], body);

    // A ping beyond the gateway needs a raw socket on the host; it is dropped.
    h.inbox.clear();
    h.send(&eth(&GW_MAC, 0x0800, &ipv4(1, GUEST, 0x0808_0808, &icmp_echo(body))));
    h.drain();
    assert_eq!(h.inbox.len(), 0);
}

// --- DHCP -------------------------------------------------------------------------------------

fn dhcp_message(kind: u8, requested: u32) -> Vec<u8> {
    let mut m = vec![0u8; 240];
    m[0] = 1;
    m[1] = 1;
    m[2] = 6;
    m[4..8].copy_from_slice(&[0xde, 0xad, 0xbe, 0xef]); // xid
    m[10] = 0x80; // broadcast flag
    m[28..34].copy_from_slice(&GUEST_MAC);
    m[236..240].copy_from_slice(&[0x63, 0x82, 0x53, 0x63]);
    m.extend_from_slice(&[53, 1, kind]);
    if requested != 0 {
        m.extend_from_slice(&[50, 4]);
        put32(&mut m, requested);
    }
    m.push(255);
    m
}

/// Find option `code` in a DHCP reply payload and return its bytes.
fn dhcp_option(m: &[u8], code: u8) -> Vec<u8> {
    let mut i = 240;
    while i + 1 < m.len() {
        if m[i] == 255 {
            break;
        }
        if m[i] == 0 {
            i += 1;
            continue;
        }
        let len = m[i + 1] as usize;
        if m[i] == code {
            return m[i + 2..i + 2 + len].to_vec();
        }
        i += 2 + len;
    }
    Vec::new()
}

fn dhcp_frame(msg: &[u8]) -> Vec<u8> {
    eth(&BROADCAST, 0x0800, &ipv4(17, 0, 0xffff_ffff, &udp_segment(0, 0xffff_ffff, 68, 67, msg)))
}

#[test]
fn dhcp_offers_and_acknowledges_the_guest_address() {
    let mut h = Harness::new();

    h.send(&dhcp_frame(&dhcp_message(1, 0)));
    h.drain();
    assert_eq!(h.inbox.len(), 1);
    let offer = h.inbox[0].clone();
    assert_eq!(offer.sport, 67);
    assert_eq!(offer.dport, 68);
    assert!(offer.ip_checksum_ok);
    assert!(offer.l4_checksum_ok);
    assert_eq!(get32(&offer.payload[16..]), GUEST); // yiaddr
    assert_eq!(offer.payload[4], 0xde); // xid echoed
    assert_eq!(dhcp_option(&offer.payload, 53)[0], 2); // OFFER
    assert_eq!(get32(&dhcp_option(&offer.payload, 3)), GATEWAY); // router
    assert_eq!(get32(&dhcp_option(&offer.payload, 6)), DNS); // DNS server
    assert_eq!(get32(&dhcp_option(&offer.payload, 1)), 0xffff_ff00);
    assert_eq!(get32(&dhcp_option(&offer.payload, 54)), GATEWAY); // server id

    h.inbox.clear();
    h.send(&dhcp_frame(&dhcp_message(3, GUEST)));
    h.drain();
    assert_eq!(h.inbox.len(), 1);
    assert_eq!(dhcp_option(&h.inbox[0].payload, 53)[0], 5); // ACK

    // Asking to keep some other address is refused, so the client starts over.
    h.inbox.clear();
    h.send(&dhcp_frame(&dhcp_message(3, 0x0a00_0299)));
    h.drain();
    assert_eq!(h.inbox.len(), 1);
    assert_eq!(dhcp_option(&h.inbox[0].payload, 53)[0], 6); // NAK
}

// --- UDP and DNS ------------------------------------------------------------------------------

/// A UDP server on the host's loopback: replies to whatever it receives with a transformed copy.
struct UdpServer {
    sock: UdpSocket,
    port: u16,
}

impl UdpServer {
    fn new() -> UdpServer {
        let sock = UdpSocket::bind(SocketAddrV4::new(Ipv4Addr::LOCALHOST, 0)).unwrap();
        sock.set_nonblocking(true).unwrap();
        let port = sock.local_addr().unwrap().port();
        UdpServer { sock, port }
    }

    /// Wait for a datagram; reply with `reply(request)`. Returns the request.
    fn serve_one(&self, reply: impl Fn(&[u8]) -> Vec<u8>) -> Vec<u8> {
        let mut buf = [0u8; 2048];
        for _ in 0..3000 {
            if let Ok((n, from)) = self.sock.recv_from(&mut buf) {
                let req = buf[..n].to_vec();
                self.sock.send_to(&reply(&req), from).unwrap();
                return req;
            }
            std::thread::sleep(Duration::from_millis(1));
        }
        Vec::new()
    }
}

#[test]
fn udp_round_trip_through_the_gateway() {
    let server = UdpServer::new();
    let mut h = Harness::new();

    h.send(&eth(&GW_MAC, 0x0800, &ipv4(17, GUEST, GATEWAY, &udp_segment(GUEST, GATEWAY, 40000, server.port, b"ping"))));

    let got = server.serve_one(|req| {
        let mut r = b"re:".to_vec();
        r.extend_from_slice(req);
        r
    });
    assert_eq!(got, b"ping"); // it reached the host

    let reply = h.wait_take(|p| p.proto == 17).expect("a UDP reply");
    assert_eq!(reply.src, GATEWAY); // looks like it came from where it was sent
    assert_eq!(reply.sport, server.port);
    assert_eq!(reply.dst, GUEST);
    assert_eq!(reply.dport, 40000);
    assert_eq!(reply.payload, b"re:ping");
    assert!(reply.ip_checksum_ok);
    assert!(reply.l4_checksum_ok);
    assert_eq!(h.net.udp_flows(), 1);
}

#[test]
fn dns_queries_are_forwarded_to_the_host_resolver() {
    let fake_resolver = UdpServer::new();
    let mut h = Harness::with_config(Config {
        dns_server: 0x7f00_0001,
        dns_port: fake_resolver.port,
        ..Config::default()
    });

    // A DNS-shaped message: id, flags, then a question.
    let query = vec![0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0, 3, b'f', b'o', b'o', 0, 0, 1, 0, 1];
    h.send(&eth(&GW_MAC, 0x0800, &ipv4(17, GUEST, DNS, &udp_segment(GUEST, DNS, 53001, 53, &query))));

    let seen = fake_resolver.serve_one(|q| {
        let mut a = q.to_vec();
        a[2] |= 0x80; // QR: this is a response
        a
    });
    assert_eq!(seen, query);

    let reply = h.wait_take(|p| p.proto == 17).expect("a DNS reply");
    assert_eq!(reply.src, DNS); // from 10.0.2.3, not the real resolver
    assert_eq!(reply.sport, 53);
    assert_eq!(reply.dport, 53001);
    assert_eq!(reply.payload[0], 0x12);
    assert_ne!(reply.payload[2] & 0x80, 0);

    // DNS is only offered on port 53.
    h.inbox.clear();
    h.send(&eth(&GW_MAC, 0x0800, &ipv4(17, GUEST, DNS, &udp_segment(GUEST, DNS, 53002, 80, &query))));
    h.drain();
    assert_eq!(h.inbox.len(), 0);
}

// --- TCP --------------------------------------------------------------------------------------

struct TcpServer {
    listener: TcpListener,
    conn: Option<TcpStream>,
    port: u16,
}

impl TcpServer {
    fn new() -> TcpServer {
        let listener = TcpListener::bind(SocketAddrV4::new(Ipv4Addr::LOCALHOST, 0)).unwrap();
        listener.set_nonblocking(true).unwrap();
        let port = listener.local_addr().unwrap().port();
        TcpServer { listener, conn: None, port }
    }

    fn accept_within(&mut self, h: &mut Harness) -> bool {
        let (listener, conn) = (&self.listener, &mut self.conn);
        h.wait_for(|_| {
            if conn.is_none() {
                if let Ok((s, _)) = listener.accept() {
                    s.set_nonblocking(true).unwrap();
                    *conn = Some(s);
                }
            }
            conn.is_some()
        })
    }

    /// Read until `want` bytes arrive (while pumping the stack so it can forward).
    fn read_exact(&mut self, h: &mut Harness, want: usize) -> Vec<u8> {
        let mut got = Vec::new();
        let conn = self.conn.as_mut().unwrap();
        h.wait_for(|_| {
            let mut buf = [0u8; 2048];
            loop {
                match conn.read(&mut buf) {
                    Ok(n) if n > 0 => got.extend_from_slice(&buf[..n]),
                    _ => break,
                }
            }
            got.len() >= want
        });
        got
    }
}

fn to_gateway(sport: u16, dport: u16, seq: u32, ack: u32, flags: u8, data: &[u8], mss: bool) -> Vec<u8> {
    eth(&GW_MAC, 0x0800, &ipv4(6, GUEST, GATEWAY, &tcp_segment(GUEST, GATEWAY, sport, dport, seq, ack, flags, data, mss)))
}

#[test]
fn tcp_connection_end_to_end() {
    let mut server = TcpServer::new();
    let mut h = Harness::new();

    let gport = 41000;
    let mut gseq = 1000u32; // the guest's initial sequence number

    // 1. The guest opens a connection to the gateway (the host).
    h.send(&to_gateway(gport, server.port, gseq, 0, 0x02, &[], true));

    let synack = h.wait_take(|p| p.proto == 6 && (p.flags & 0x12) == 0x12).expect("a SYN-ACK");
    assert!(synack.ip_checksum_ok);
    assert!(synack.l4_checksum_ok);
    assert_eq!(synack.src, GATEWAY);
    assert_eq!(synack.sport, server.port);
    assert_eq!(synack.ack, gseq + 1); // acknowledges our SYN
    gseq += 1;
    let mut server_seq = synack.seq.wrapping_add(1); // their SYN used one number

    // 2. The guest completes the handshake; the proxy has connected to the host.
    h.send(&to_gateway(gport, server.port, gseq, server_seq, 0x10, &[], false));
    assert!(server.accept_within(&mut h));

    // 3. Guest -> host data.
    h.send(&to_gateway(gport, server.port, gseq, server_seq, 0x18, b"hello", false));
    gseq += 5;
    assert_eq!(server.read_exact(&mut h, 5), b"hello");
    let expected_ack = gseq;
    h.wait_take(move |p| p.proto == 6 && p.flags == 0x10 && p.ack == expected_ack).expect("an ACK for the data");

    // 4. Host -> guest data.
    let reply = b"world!";
    server.conn.as_mut().unwrap().write_all(reply).unwrap();
    let data = h.wait_take(|p| p.proto == 6 && !p.payload.is_empty()).expect("data for the guest");
    assert_eq!(data.payload, reply);
    assert_eq!(data.seq, server_seq);
    assert!(data.l4_checksum_ok);
    server_seq = server_seq.wrapping_add(reply.len() as u32);
    h.send(&to_gateway(gport, server.port, gseq, server_seq, 0x10, &[], false));

    // 5. The host closes its end: the guest receives a FIN.
    server.conn = None;
    let fin = h.wait_take(|p| p.proto == 6 && p.flags & 0x01 != 0).expect("a FIN");
    assert_eq!(fin.seq, server_seq);
    server_seq = server_seq.wrapping_add(1);

    // 6. The guest acknowledges it and sends its own FIN; the flow is torn down.
    h.send(&to_gateway(gport, server.port, gseq, server_seq, 0x11, &[], false));
    assert!(h.wait_for(|h| h.net.tcp_flows() == 0));
}

#[test]
fn tcp_to_a_closed_port_is_reset() {
    // Find a port with nothing listening by binding and releasing one.
    let closed = {
        let probe = TcpListener::bind(SocketAddrV4::new(Ipv4Addr::LOCALHOST, 0)).unwrap();
        probe.local_addr().unwrap().port()
    };

    let mut h = Harness::new();
    h.send(&to_gateway(42000, closed, 5000, 0, 0x02, &[], true));
    let rst = h.wait_take(|p| p.proto == 6 && p.flags & 0x04 != 0).expect("a RST");
    assert_eq!(rst.ack, 5001);
    assert_eq!(h.net.tcp_flows(), 0);
}

#[test]
fn tcp_segment_for_an_unknown_connection_is_reset() {
    let mut h = Harness::new();
    h.send(&to_gateway(43000, 80, 777, 888, 0x10, b"stray", false));
    h.drain();
    assert_eq!(h.inbox.len(), 1);
    assert_ne!(h.inbox[0].flags & 0x04, 0); // RST
    assert_eq!(h.inbox[0].seq, 888); // sequence taken from the ACK
}

#[test]
fn traffic_to_nowhere_is_dropped_not_crashed_on() {
    let mut h = Harness::new();
    // Runt frames, a bad IP header, an unknown ethertype, a fragment.
    h.net.send(b"short");
    h.send(&eth(&GW_MAC, 0x86dd, &[0; 40]));
    h.send(&eth(&GW_MAC, 0x0800, &[0x45; 10]));
    let mut frag = ipv4(17, GUEST, GATEWAY, &udp_segment(GUEST, GATEWAY, 1, 2, b"x"));
    frag[6] = 0x20; // more-fragments
    h.send(&eth(&GW_MAC, 0x0800, &frag));
    // Multicast and broadcast UDP have nowhere to go.
    h.send(&eth(&GW_MAC, 0x0800, &ipv4(17, GUEST, 0xe000_0001, &udp_segment(GUEST, 0xe000_0001, 5, 5, b"m"))));
    h.drain();
    assert_eq!(h.inbox.len(), 0);
    assert!(h.net.dropped() >= 5);
}

#[test]
fn a_hostile_header_cannot_make_the_stack_read_out_of_bounds() {
    // Lengths in the IP, UDP and TCP headers come from the guest. None may be trusted.
    let mut h = Harness::new();
    // IP total length larger than the frame.
    let mut p = ipv4(17, GUEST, GATEWAY, &udp_segment(GUEST, GATEWAY, 1, 2, b"x"));
    p[2..4].copy_from_slice(&1400u16.to_be_bytes());
    h.send(&eth(&GW_MAC, 0x0800, &p));
    // UDP length smaller than a header.
    let mut u = udp_segment(GUEST, GATEWAY, 1, 2, b"x");
    u[4..6].copy_from_slice(&3u16.to_be_bytes());
    h.send(&eth(&GW_MAC, 0x0800, &ipv4(17, GUEST, GATEWAY, &u)));
    // TCP data offset beyond the segment.
    let mut t = tcp_segment(GUEST, GATEWAY, 1, 2, 0, 0, 0x10, &[], false);
    t[12] = 0xf0;
    h.send(&eth(&GW_MAC, 0x0800, &ipv4(6, GUEST, GATEWAY, &t)));
    // DHCP too short to hold its fixed part.
    h.send(&dhcp_frame(&[1, 1, 6, 0]));
    h.drain();
    assert_eq!(h.inbox.len(), 0);
}
