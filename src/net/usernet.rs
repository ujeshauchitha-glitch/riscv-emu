//! A user-mode network stack: the guest gets real internet access with no host setup
//! and no privileges.
//!
//! The virtio-net card hands us raw Ethernet frames. Rather than bridging them onto a
//! real interface (which needs root and a configured host), this plays every network
//! service the guest expects - a switch, a router, a DHCP server, a DNS server - and
//! *proxies* the guest's traffic through ordinary sockets on the host:
//!
//!   ARP    answered for the two addresses we pretend to own
//!   DHCP   hands the guest 10.0.2.15, with the gateway and DNS server
//!   ICMP   echo requests to the gateway are answered
//!   UDP    each flow gets a host UDP socket (this is how DNS works)
//!   TCP    each guest connection becomes a host connection, with the TCP state machine
//!          (handshake, sequence numbers, window, FIN/RST) played on the guest side
//!
//! The network is the same shape as QEMU's slirp: guest 10.0.2.15, gateway (and
//! "the host") 10.0.2.2, DNS 10.0.2.3.
//!
//! This code parses packets from an untrusted guest and so is written defensively:
//! lengths are checked before they are used, fragments are refused, and the number of
//! flows and the amount of buffered data are bounded.

use std::collections::{BTreeMap, VecDeque};
use std::net::UdpSocket;
use std::time::{Duration, Instant};

use super::hostnet::{self, ConnectState, HostTcp, Recv};
use crate::virtio::net::NetBackend;

const ETH_IPV4: u16 = 0x0800;
const ETH_ARP: u16 = 0x0806;

const PROTO_ICMP: u8 = 1;
const PROTO_TCP: u8 = 6;
const PROTO_UDP: u8 = 17;

const TCP_FIN: u8 = 0x01;
const TCP_SYN: u8 = 0x02;
const TCP_RST: u8 = 0x04;
const TCP_PSH: u8 = 0x08;
const TCP_ACK: u8 = 0x10;

const ETH_HEADER: usize = 14;
const MTU: usize = 1500;
/// MTU less the 40 bytes of IP and TCP headers.
const MSS_OURS: u16 = 1460;

/// How much unsent guest data a flow may hold before it stops accepting more. The
/// window we advertise shrinks as this fills, which is what throttles the guest.
const TO_HOST_LIMIT: usize = 128 * 1024;
const MAX_TCP_FLOWS: usize = 256;
const MAX_UDP_FLOWS: usize = 256;

const TCP_HANDSHAKE_TIMEOUT: Duration = Duration::from_secs(20);
const TCP_LINGER_TIMEOUT: Duration = Duration::from_secs(60);
const UDP_IDLE_TIMEOUT: Duration = Duration::from_secs(60);

const GATEWAY_MAC: [u8; 6] = [0x52, 0x55, 0x0a, 0x00, 0x02, 0x02];
const BROADCAST_MAC: [u8; 6] = [0xff; 6];

// --- big-endian helpers: every field on the wire is network byte order --------------

fn rd16(p: &[u8]) -> u16 {
    u16::from_be_bytes([p[0], p[1]])
}
fn rd32(p: &[u8]) -> u32 {
    u32::from_be_bytes([p[0], p[1], p[2], p[3]])
}
fn wr16(p: &mut [u8], v: u16) {
    p[..2].copy_from_slice(&v.to_be_bytes());
}
fn wr32(p: &mut [u8], v: u32) {
    p[..4].copy_from_slice(&v.to_be_bytes());
}

/// The Internet checksum: the ones'-complement sum of 16-bit words, complemented. `sum`
/// carries in the contribution of a pseudo-header, if any.
fn checksum(data: &[u8], mut sum: u32) -> u16 {
    let mut chunks = data.chunks_exact(2);
    for c in &mut chunks {
        sum += u16::from_be_bytes([c[0], c[1]]) as u32;
    }
    if let [last] = chunks.remainder() {
        sum += (*last as u32) << 8;
    }
    while sum >> 16 != 0 {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    !(sum as u16)
}

/// The pseudo-header that TCP and UDP checksums cover in addition to their own bytes:
/// it ties the segment to the addresses it claims to travel between.
fn pseudo_header_sum(src: u32, dst: u32, proto: u8, length: usize) -> u32 {
    (src >> 16) + (src & 0xffff) + (dst >> 16) + (dst & 0xffff) + proto as u32 + length as u32
}

/// TCP sequence numbers wrap, so "a is before b" is a signed difference.
fn seq_lt(a: u32, b: u32) -> bool {
    (a.wrapping_sub(b) as i32) < 0
}
fn seq_le(a: u32, b: u32) -> bool {
    (a.wrapping_sub(b) as i32) <= 0
}

fn flow_key(guest_port: u16, ip: u32, port: u16) -> u64 {
    ((guest_port as u64) << 48) | ((ip as u64) << 16) | port as u64
}

#[derive(Clone, Debug)]
pub struct Config {
    pub guest_ip: u32,
    pub gateway_ip: u32,
    pub dns_ip: u32,
    pub netmask: u32,
    /// What the gateway address really is on the host (traffic to 10.0.2.2 reaches the
    /// host's loopback).
    pub host_ip: u32,
    /// The DNS server lookups are forwarded to; 0 means find the host's.
    pub dns_server: u32,
    pub dns_port: u16,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            guest_ip: 0x0a00_020f,   // 10.0.2.15
            gateway_ip: 0x0a00_0202, // 10.0.2.2
            dns_ip: 0x0a00_0203,     // 10.0.2.3
            netmask: 0xffff_ff00,
            host_ip: 0x7f00_0001,
            dns_server: 0,
            dns_port: 53,
        }
    }
}

struct UdpFlow {
    sock: UdpSocket,
    guest_port: u16,
    /// The destination the guest thinks it is talking to.
    virt_ip: u32,
    virt_port: u16,
    last: Instant,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum TcpState {
    Connecting,
    SynAckSent,
    Established,
}

struct TcpFlow {
    state: TcpState,
    sock: HostTcp,
    guest_port: u16,
    dst_ip: u32,
    dst_port: u16,

    snd_nxt: u32,       // next sequence number we will use
    snd_una: u32,       // oldest sequence number the guest has not acknowledged
    rcv_nxt: u32,       // next sequence number we expect from the guest
    guest_window: u32,  // how many bytes the guest said it can take
    mss: u16,           // the guest's maximum segment size

    to_host: Vec<u8>,   // guest bytes not yet written to the host socket
    guest_fin: bool,    // the guest finished sending
    host_shut: bool,    // we have half-closed the host socket
    host_eof: bool,     // the host finished sending
    fin_sent: bool,     // we sent the guest a FIN
    fin_acked: bool,    // ...and it acknowledged it
    last: Instant,
}

/// Everything that builds frames to send to the guest. Kept apart from the flow tables
/// so a flow can be edited while frames are queued.
struct Wire {
    cfg: Config,
    guest_mac: [u8; 6],
    have_guest_mac: bool,
    rx: VecDeque<Vec<u8>>, // frames waiting for the guest
    ip_id: u16,
}

impl Wire {
    fn queue_frame(&mut self, ethertype: u16, payload: &[u8]) {
        let dst = if self.have_guest_mac { self.guest_mac } else { BROADCAST_MAC };
        self.queue_frame_to(&dst, ethertype, payload);
    }

    fn queue_frame_to(&mut self, dst_mac: &[u8; 6], ethertype: u16, payload: &[u8]) {
        let mut frame = vec![0u8; ETH_HEADER + payload.len()];
        frame[..6].copy_from_slice(dst_mac);
        frame[6..12].copy_from_slice(&GATEWAY_MAC);
        wr16(&mut frame[12..], ethertype);
        frame[ETH_HEADER..].copy_from_slice(payload);
        self.rx.push_back(frame);
    }

    fn queue_ip(&mut self, proto: u8, src: u32, dst: u32, payload: &[u8], dst_mac: Option<&[u8; 6]>) {
        let mut packet = vec![0u8; 20 + payload.len()];
        packet[0] = 0x45; // IPv4, 5-word header
        let total = packet.len() as u16;
        wr16(&mut packet[2..], total);
        wr16(&mut packet[4..], self.ip_id);
        self.ip_id = self.ip_id.wrapping_add(1);
        wr16(&mut packet[6..], 0x4000); // don't fragment
        packet[8] = 64; // TTL
        packet[9] = proto;
        wr32(&mut packet[12..], src);
        wr32(&mut packet[16..], dst);
        let sum = checksum(&packet[..20], 0);
        wr16(&mut packet[10..], sum);
        packet[20..].copy_from_slice(payload);

        match dst_mac {
            None => self.queue_frame(ETH_IPV4, &packet),
            Some(m) => self.queue_frame_to(m, ETH_IPV4, &packet),
        }
    }

    fn queue_udp(&mut self, src: u32, sport: u16, dst: u32, dport: u16, data: &[u8], dst_mac: Option<&[u8; 6]>) {
        let mut seg = vec![0u8; 8 + data.len()];
        wr16(&mut seg[0..], sport);
        wr16(&mut seg[2..], dport);
        let len = seg.len() as u16;
        wr16(&mut seg[4..], len);
        seg[8..].copy_from_slice(data);
        let mut sum = checksum(&seg, pseudo_header_sum(src, dst, PROTO_UDP, seg.len()));
        if sum == 0 {
            sum = 0xffff; // zero means "no checksum" in UDP
        }
        wr16(&mut seg[6..], sum);
        self.queue_ip(PROTO_UDP, src, dst, &seg, dst_mac);
    }

    #[allow(clippy::too_many_arguments)]
    fn queue_tcp_raw(
        &mut self,
        src: u32,
        sport: u16,
        dst: u32,
        dport: u16,
        flags: u8,
        seq: u32,
        ack: u32,
        window: u16,
        data: &[u8],
        with_mss: bool,
    ) {
        let header = if with_mss { 24 } else { 20 };
        let mut seg = vec![0u8; header + data.len()];
        wr16(&mut seg[0..], sport);
        wr16(&mut seg[2..], dport);
        wr32(&mut seg[4..], seq);
        wr32(&mut seg[8..], ack);
        seg[12] = ((header / 4) << 4) as u8;
        seg[13] = flags;
        wr16(&mut seg[14..], window);
        if with_mss {
            seg[20] = 2; // option: maximum segment size
            seg[21] = 4;
            wr16(&mut seg[22..], MSS_OURS);
        }
        seg[header..].copy_from_slice(data);
        let sum = checksum(&seg, pseudo_header_sum(src, dst, PROTO_TCP, seg.len()));
        wr16(&mut seg[16..], sum);
        self.queue_ip(PROTO_TCP, src, dst, &seg, None);
    }

    fn queue_tcp(&mut self, f: &TcpFlow, flags: u8, seq: u32, ack: u32, data: &[u8], with_mss: bool) {
        let (guest_ip, window) = (self.cfg.guest_ip, window_for(f));
        self.queue_tcp_raw(f.dst_ip, f.dst_port, guest_ip, f.guest_port, flags, seq, ack, window, data, with_mss);
    }

    /// A reset in answer to a segment for a connection that does not exist (RFC 793): if
    /// the offending segment carried an ACK, the reset's sequence number is that
    /// acknowledgment; otherwise it is zero and the reset acknowledges the segment.
    #[allow(clippy::too_many_arguments)]
    fn reset_for(&mut self, dst_ip: u32, dst_port: u16, guest_port: u16, had_ack: bool, seq: u32, ack: u32, seg_len: u32) {
        let guest_ip = self.cfg.guest_ip;
        if had_ack {
            self.queue_tcp_raw(dst_ip, dst_port, guest_ip, guest_port, TCP_RST, ack, 0, 0, &[], false);
        } else {
            self.queue_tcp_raw(
                dst_ip,
                dst_port,
                guest_ip,
                guest_port,
                TCP_RST | TCP_ACK,
                0,
                seq.wrapping_add(seg_len),
                0,
                &[],
                false,
            );
        }
    }
}

fn window_for(f: &TcpFlow) -> u16 {
    let room = TO_HOST_LIMIT.saturating_sub(f.to_host.len());
    room.min(65535) as u16
}

pub struct UserNet {
    w: Wire,
    dropped: u64,
    receive_calls: u32,
    next_iss: u32,
    udp: BTreeMap<u64, UdpFlow>,
    tcp: BTreeMap<u64, TcpFlow>,
}

impl Default for UserNet {
    fn default() -> Self {
        UserNet::new(Config::default())
    }
}

impl UserNet {
    pub fn new(mut cfg: Config) -> UserNet {
        if cfg.dns_server == 0 {
            cfg.dns_server = hostnet::system_dns_server();
        }
        if cfg.dns_server == 0 {
            cfg.dns_server = 0x0808_0808; // 8.8.8.8, as a last resort
        }
        UserNet {
            w: Wire { cfg, guest_mac: [0; 6], have_guest_mac: false, rx: VecDeque::new(), ip_id: 1 },
            dropped: 0,
            receive_calls: 0,
            next_iss: 0x2000_0000,
            udp: BTreeMap::new(),
            tcp: BTreeMap::new(),
        }
    }

    pub fn gateway_mac() -> [u8; 6] {
        GATEWAY_MAC
    }
    pub fn tcp_flows(&self) -> usize {
        self.tcp.len()
    }
    pub fn udp_flows(&self) -> usize {
        self.udp.len()
    }
    pub fn dropped(&self) -> u64 {
        self.dropped
    }

    /// Service the host sockets: move data between them and the guest.
    pub fn pump(&mut self) {
        self.pump_udp();
        self.pump_tcp();
    }

    // --- frames from the guest ------------------------------------------------------

    fn handle_frame(&mut self, frame: &[u8]) {
        if frame.len() < ETH_HEADER {
            self.dropped += 1;
            return;
        }
        // Remember who the guest is. Every frame it sends carries its address, and we
        // need it to address replies.
        self.w.guest_mac.copy_from_slice(&frame[6..12]);
        self.w.have_guest_mac = true;

        match rd16(&frame[12..]) {
            ETH_ARP => self.handle_arp(&frame[ETH_HEADER..]),
            ETH_IPV4 => self.handle_ipv4(&frame[ETH_HEADER..]),
            _ => self.dropped += 1,
        }
    }

    fn handle_arp(&mut self, p: &[u8]) {
        // htype 1 (Ethernet), ptype 0x0800 (IPv4), hlen 6, plen 4: 28 bytes.
        if p.len() < 28 || rd16(p) != 1 || rd16(&p[2..]) != ETH_IPV4 || p[4] != 6 || p[5] != 4 {
            self.dropped += 1;
            return;
        }
        if rd16(&p[6..]) != 1 {
            return; // only requests get answered
        }
        let sender_ip = rd32(&p[14..]);
        let target_ip = rd32(&p[24..]);

        // We answer for the two addresses we pretend to own.
        if target_ip != self.w.cfg.gateway_ip && target_ip != self.w.cfg.dns_ip {
            return;
        }
        if sender_ip == 0 {
            return; // an address probe, not a question
        }

        let mut reply = [0u8; 28];
        wr16(&mut reply[0..], 1);
        wr16(&mut reply[2..], ETH_IPV4);
        reply[4] = 6;
        reply[5] = 4;
        wr16(&mut reply[6..], 2); // reply
        reply[8..14].copy_from_slice(&GATEWAY_MAC);
        wr32(&mut reply[14..], target_ip);
        reply[18..24].copy_from_slice(&p[8..14]); // back to whoever asked
        wr32(&mut reply[24..], sender_ip);
        self.w.queue_frame(ETH_ARP, &reply);
    }

    fn handle_ipv4(&mut self, p: &[u8]) {
        if p.len() < 20 || p[0] >> 4 != 4 {
            self.dropped += 1;
            return;
        }
        let ihl = (p[0] & 0x0f) as usize * 4;
        let total = rd16(&p[2..]) as usize;
        if ihl < 20 || total < ihl || total > p.len() {
            self.dropped += 1;
            return;
        }
        // Fragments are not reassembled. A guest sending below the MTU never produces
        // them, and reassembly is a resource-exhaustion hazard for a component that
        // exists to be exposed to untrusted traffic.
        if rd16(&p[6..]) & 0x3fff != 0 {
            self.dropped += 1;
            return;
        }
        let proto = p[9];
        let (src, dst) = (rd32(&p[12..]), rd32(&p[16..]));
        let body = &p[ihl..total];
        match proto {
            PROTO_ICMP => self.handle_icmp(src, dst, body),
            PROTO_UDP => self.handle_udp(dst, body),
            PROTO_TCP => self.handle_tcp(dst, body),
            _ => self.dropped += 1,
        }
    }

    fn handle_icmp(&mut self, src: u32, dst: u32, p: &[u8]) {
        // Echo request (type 8) to an address that is ours: answer it. Pings further
        // afield would need a raw socket on the host, so they go unanswered.
        if p.len() < 8 || p[0] != 8 || p[1] != 0 {
            self.dropped += 1;
            return;
        }
        if dst != self.w.cfg.gateway_ip && dst != self.w.cfg.dns_ip {
            self.dropped += 1;
            return;
        }
        let mut reply = p.to_vec();
        reply[0] = 0; // echo reply
        wr16(&mut reply[2..], 0);
        let sum = checksum(&reply, 0);
        wr16(&mut reply[2..], sum);
        self.w.queue_ip(PROTO_ICMP, dst, src, &reply, None);
    }

    // --- DHCP -----------------------------------------------------------------------

    fn handle_dhcp(&mut self, p: &[u8]) {
        // BOOTP: op htype hlen hops | xid | secs flags | ciaddr yiaddr siaddr giaddr |
        // chaddr[16] sname[64] file[128] | magic cookie | options
        if p.len() < 240 || p[0] != 1 || rd32(&p[236..]) != 0x6382_5363 {
            self.dropped += 1;
            return;
        }
        let cfg = self.w.cfg.clone();

        let mut message_type = 0u8;
        let mut requested_ip = 0u32;
        let mut i = 240;
        while i < p.len() {
            let code = p[i];
            if code == 255 {
                break;
            }
            if code == 0 {
                i += 1;
                continue;
            }
            if i + 1 >= p.len() {
                break;
            }
            let olen = p[i + 1] as usize;
            if i + 2 + olen > p.len() {
                break;
            }
            if code == 53 && olen == 1 {
                message_type = p[i + 2];
            }
            if code == 50 && olen == 4 {
                requested_ip = rd32(&p[i + 2..]);
            }
            i += 2 + olen;
        }

        // 1 DISCOVER, 3 REQUEST, 8 INFORM. Anything else (RELEASE, DECLINE) needs no
        // reply - there is only ever one address to give out.
        let reply_type = match message_type {
            1 => 2, // OFFER
            3 => {
                // A guest asking to keep an address other than ours is told no, so that
                // it starts over with DISCOVER instead of using a stale lease.
                if requested_ip == 0 || requested_ip == cfg.guest_ip {
                    5 // ACK
                } else {
                    6 // NAK
                }
            }
            8 => 5,
            _ => return,
        };

        let mut r = vec![0u8; 240];
        r[0] = 2; // BOOTREPLY
        r[1] = p[1]; // htype
        r[2] = p[2]; // hlen
        r[4..8].copy_from_slice(&p[4..8]); // xid: how the guest matches reply to request
        r[10..12].copy_from_slice(&p[10..12]); // flags
        if reply_type != 6 {
            wr32(&mut r[16..], cfg.guest_ip); // yiaddr
        }
        wr32(&mut r[20..], cfg.gateway_ip); // siaddr
        r[28..44].copy_from_slice(&p[28..44]); // chaddr
        wr32(&mut r[236..], 0x6382_5363); // magic cookie

        let opt = |r: &mut Vec<u8>, code: u8, bytes: &[u8]| {
            r.push(code);
            r.push(bytes.len() as u8);
            r.extend_from_slice(bytes);
        };
        let opt32 = |r: &mut Vec<u8>, code: u8, v: u32| opt(r, code, &v.to_be_bytes());
        opt(&mut r, 53, &[reply_type]);
        opt32(&mut r, 54, cfg.gateway_ip); // server identifier
        if reply_type != 6 {
            opt32(&mut r, 51, 86400); // lease time: a day
            opt32(&mut r, 58, 43200); // T1
            opt32(&mut r, 59, 75600); // T2
            opt32(&mut r, 1, cfg.netmask);
            opt32(&mut r, 3, cfg.gateway_ip); // router
            opt32(&mut r, 6, cfg.dns_ip); // DNS server
            opt32(&mut r, 28, (cfg.guest_ip & cfg.netmask) | !cfg.netmask); // broadcast
        }
        r.push(255); // end
        while r.len() < 300 {
            r.push(0); // some clients want a minimum size
        }

        // A client with no address yet cannot be reached by unicast, so the reply is
        // broadcast.
        self.w.queue_udp(cfg.gateway_ip, 67, 0xffff_ffff, 68, &r, Some(&BROADCAST_MAC));
    }

    // --- UDP ------------------------------------------------------------------------

    /// Where a guest-visible destination really is on the host, or None if it should go
    /// nowhere.
    fn map_destination(&self, virt_ip: u32, virt_port: u16) -> Option<(u32, u16)> {
        let c = &self.w.cfg;
        if virt_ip == c.gateway_ip {
            return Some((c.host_ip, virt_port));
        }
        if virt_ip == c.dns_ip {
            if virt_port != 53 {
                return None; // the DNS address only serves DNS
            }
            return Some((c.dns_server, c.dns_port));
        }
        // The rest of our own subnet is empty, and the broadcast and multicast ranges
        // are not something one host socket can speak for.
        if virt_ip & c.netmask == c.guest_ip & c.netmask {
            return None;
        }
        if virt_ip == 0xffff_ffff || virt_ip >> 28 == 0xe {
            return None;
        }
        Some((virt_ip, virt_port))
    }

    fn handle_udp(&mut self, dst: u32, p: &[u8]) {
        if p.len() < 8 {
            self.dropped += 1;
            return;
        }
        let (sport, dport) = (rd16(p), rd16(&p[2..]));
        let ulen = rd16(&p[4..]) as usize;
        if ulen < 8 || ulen > p.len() {
            self.dropped += 1;
            return;
        }
        let data = &p[8..ulen];

        if dport == 67 {
            self.handle_dhcp(data);
            return;
        }
        let Some((host_ip, host_port)) = self.map_destination(dst, dport) else {
            self.dropped += 1;
            return;
        };

        let key = flow_key(sport, dst, dport);
        if !self.udp.contains_key(&key) {
            if self.udp.len() >= MAX_UDP_FLOWS {
                self.dropped += 1;
                return;
            }
            let Some(sock) = hostnet::udp_socket() else {
                self.dropped += 1;
                return;
            };
            self.udp.insert(key, UdpFlow { sock, guest_port: sport, virt_ip: dst, virt_port: dport, last: Instant::now() });
        }
        let f = self.udp.get_mut(&key).unwrap();
        f.last = Instant::now();
        hostnet::udp_send_to(&f.sock, data, host_ip, host_port);
    }

    fn pump_udp(&mut self) {
        let now = Instant::now();
        let mut buf = [0u8; 2048];
        let guest_ip = self.w.cfg.guest_ip;
        let mut expired = Vec::new();
        for (key, f) in self.udp.iter_mut() {
            for _ in 0..16 {
                let Some(n) = hostnet::udp_recv_from(&f.sock, &mut buf) else {
                    break;
                };
                f.last = now;
                // The reply has to look like it came from where the guest sent the
                // request, not from the host socket that actually carried it.
                if n + 28 > MTU {
                    self.dropped += 1;
                    continue;
                }
                self.w.queue_udp(f.virt_ip, f.virt_port, guest_ip, f.guest_port, &buf[..n], None);
            }
            if now.duration_since(f.last) > UDP_IDLE_TIMEOUT {
                expired.push(*key);
            }
        }
        for k in expired {
            self.udp.remove(&k);
        }
    }

    // --- TCP ------------------------------------------------------------------------

    fn handle_tcp(&mut self, dst: u32, p: &[u8]) {
        if p.len() < 20 {
            self.dropped += 1;
            return;
        }
        let (sport, dport) = (rd16(p), rd16(&p[2..]));
        let seq = rd32(&p[4..]);
        let ack = rd32(&p[8..]);
        let hlen = (p[12] >> 4) as usize * 4;
        let flags = p[13];
        let win = rd16(&p[14..]);
        if hlen < 20 || hlen > p.len() {
            self.dropped += 1;
            return;
        }
        let mut data = &p[hlen..];
        let orig_dlen = data.len() as u32;
        let syn = flags & TCP_SYN != 0;
        let fin = flags & TCP_FIN != 0;
        let rst = flags & TCP_RST != 0;
        let has_ack = flags & TCP_ACK != 0;

        let key = flow_key(sport, dst, dport);

        // --- a new connection -------------------------------------------------------
        if syn && !has_ack {
            if let Some(f) = self.tcp.get(&key) {
                // A retransmitted SYN while we are still connecting or waiting for the
                // handshake to finish: the answer is on its way, say nothing. On an
                // established flow it is a stale reuse of the port - start over.
                if f.state != TcpState::Established {
                    return;
                }
                self.tcp.remove(&key);
            }

            let mapped = if self.tcp.len() >= MAX_TCP_FLOWS { None } else { self.map_destination(dst, dport) };
            let Some((host_ip, host_port)) = mapped else {
                self.w.reset_for(dst, dport, sport, false, seq, ack, 1);
                return;
            };

            // The guest's maximum segment size is in a TCP option on its SYN.
            let mut mss = 536u16;
            let mut i = 20;
            while i + 1 < hlen {
                let kind = p[i];
                if kind == 0 {
                    break;
                }
                if kind == 1 {
                    i += 1;
                    continue;
                }
                let olen = p[i + 1] as usize;
                if olen < 2 || i + olen > hlen {
                    break;
                }
                if kind == 2 && olen == 4 {
                    mss = rd16(&p[i + 2..]).min(MSS_OURS);
                }
                i += olen;
            }
            if mss < 64 {
                mss = 536;
            }

            let iss = self.next_iss;
            self.next_iss = self.next_iss.wrapping_add(64000);
            self.tcp.insert(
                key,
                TcpFlow {
                    state: TcpState::Connecting,
                    sock: HostTcp::connect(host_ip, host_port),
                    guest_port: sport,
                    dst_ip: dst,
                    dst_port: dport,
                    snd_nxt: iss,
                    snd_una: iss,
                    rcv_nxt: seq.wrapping_add(1), // the SYN occupies one sequence number
                    guest_window: win as u32,
                    mss,
                    to_host: Vec::new(),
                    guest_fin: false,
                    host_shut: false,
                    host_eof: false,
                    fin_sent: false,
                    fin_acked: false,
                    last: Instant::now(),
                },
            );
            return;
        }

        // --- a segment for a connection we don't know -------------------------------
        let Some(f) = self.tcp.get_mut(&key) else {
            if !rst {
                let seg_len = orig_dlen + syn as u32 + fin as u32;
                self.w.reset_for(dst, dport, sport, has_ack, seq, ack, seg_len);
            }
            return;
        };
        f.last = Instant::now();

        if rst {
            self.tcp.remove(&key);
            return;
        }

        // --- acknowledgments --------------------------------------------------------
        if has_ack {
            if f.state == TcpState::SynAckSent && ack == f.snd_nxt {
                f.state = TcpState::Established;
            }
            if f.state == TcpState::Established && seq_le(f.snd_una, ack) && seq_le(ack, f.snd_nxt) {
                f.snd_una = ack;
                f.guest_window = win as u32;
                if f.fin_sent && ack == f.snd_nxt {
                    f.fin_acked = true;
                }
            }
        }
        if f.state != TcpState::Established {
            return;
        }

        // --- data from the guest ----------------------------------------------------
        let mut need_ack = false;
        if !data.is_empty() {
            need_ack = true;
            if seq_lt(seq, f.rcv_nxt) {
                // Retransmission or overlap: keep only the part we have not seen.
                let skip = f.rcv_nxt.wrapping_sub(seq) as usize;
                data = if skip >= data.len() { &[] } else { &data[skip..] };
            }
            if !data.is_empty() {
                let effective_seq = f.rcv_nxt;
                if seq_lt(seq, f.rcv_nxt) || seq == f.rcv_nxt {
                    if f.to_host.len() + data.len() <= TO_HOST_LIMIT {
                        f.to_host.extend_from_slice(data);
                        f.rcv_nxt = effective_seq.wrapping_add(data.len() as u32);
                    }
                    // else: no room - leave rcv_nxt alone; the guest will resend.
                }
                // seq beyond rcv_nxt is out of order. A lossless link never produces
                // it; if it happens the duplicate ACK below asks for what we want.
            }
        }

        if fin && !f.guest_fin && seq.wrapping_add(orig_dlen) == f.rcv_nxt {
            f.rcv_nxt = f.rcv_nxt.wrapping_add(1); // the FIN occupies one sequence number
            f.guest_fin = true;
            need_ack = true;
        }

        if need_ack {
            let (snd, rcv) = (f.snd_nxt, f.rcv_nxt);
            self.w.queue_tcp(f, TCP_ACK, snd, rcv, &[], false);
        }
        self.pump_tcp();
    }

    fn pump_tcp(&mut self) {
        let now = Instant::now();
        let mut buf = [0u8; MSS_OURS as usize];
        let guest_ip = self.w.cfg.guest_ip;
        let mut remove_keys = Vec::new();

        for (key, f) in self.tcp.iter_mut() {
            let mut remove = false;

            match f.state {
                TcpState::Connecting => match f.sock.poll_connect() {
                    ConnectState::Up => {
                        // The host connection is up: complete the guest's handshake.
                        let (snd, rcv) = (f.snd_nxt, f.rcv_nxt);
                        self.w.queue_tcp(f, TCP_SYN | TCP_ACK, snd, rcv, &[], true);
                        f.snd_nxt = f.snd_nxt.wrapping_add(1); // our SYN occupies one sequence number
                        f.state = TcpState::SynAckSent;
                        f.last = now;
                    }
                    ConnectState::Failed => {
                        // Refused or unreachable: the guest sees an ordinary RST.
                        self.w.queue_tcp_raw(
                            f.dst_ip,
                            f.dst_port,
                            guest_ip,
                            f.guest_port,
                            TCP_RST | TCP_ACK,
                            0,
                            f.rcv_nxt,
                            0,
                            &[],
                            false,
                        );
                        remove = true;
                    }
                    ConnectState::Pending => {
                        if now.duration_since(f.last) > TCP_HANDSHAKE_TIMEOUT {
                            remove = true;
                        }
                    }
                },

                TcpState::SynAckSent => {
                    if now.duration_since(f.last) > TCP_HANDSHAKE_TIMEOUT {
                        remove = true;
                    }
                }

                TcpState::Established => {
                    // Guest -> host.
                    while !f.to_host.is_empty() {
                        match f.sock.send(&f.to_host) {
                            None => {
                                remove = true;
                                break;
                            }
                            Some(0) => break,
                            Some(n) => {
                                f.to_host.drain(..n);
                                // Room opened up; tell the guest it may send more.
                                let (snd, rcv) = (f.snd_nxt, f.rcv_nxt);
                                self.w.queue_tcp(f, TCP_ACK, snd, rcv, &[], false);
                            }
                        }
                    }
                    if remove {
                        let (snd, rcv) = (f.snd_nxt, f.rcv_nxt);
                        self.w.queue_tcp(f, TCP_RST | TCP_ACK, snd, rcv, &[], false);
                    } else {
                        if f.guest_fin && f.to_host.is_empty() && !f.host_shut {
                            f.sock.shutdown_write();
                            f.host_shut = true;
                        }

                        // Host -> guest, never sending more than the guest has room for.
                        for _ in 0..32 {
                            if f.host_eof {
                                break;
                            }
                            let in_flight = f.snd_nxt.wrapping_sub(f.snd_una);
                            if f.guest_window <= in_flight {
                                break;
                            }
                            let room = ((f.guest_window - in_flight) as usize).min(f.mss as usize).min(buf.len());
                            match f.sock.recv(&mut buf[..room]) {
                                Recv::WouldBlock => break,
                                Recv::Eof => {
                                    // The host closed its end: pass that on as a FIN.
                                    f.host_eof = true;
                                    let (snd, rcv) = (f.snd_nxt, f.rcv_nxt);
                                    self.w.queue_tcp(f, TCP_FIN | TCP_ACK, snd, rcv, &[], false);
                                    f.snd_nxt = f.snd_nxt.wrapping_add(1);
                                    f.fin_sent = true;
                                    break;
                                }
                                Recv::Error => {
                                    let (snd, rcv) = (f.snd_nxt, f.rcv_nxt);
                                    self.w.queue_tcp(f, TCP_RST | TCP_ACK, snd, rcv, &[], false);
                                    remove = true;
                                    break;
                                }
                                Recv::Data(n) => {
                                    let (snd, rcv) = (f.snd_nxt, f.rcv_nxt);
                                    self.w.queue_tcp(f, TCP_PSH | TCP_ACK, snd, rcv, &buf[..n], false);
                                    f.snd_nxt = f.snd_nxt.wrapping_add(n as u32);
                                    f.last = now;
                                }
                            }
                        }

                        // Both directions finished and acknowledged: the flow is done.
                        if f.fin_acked && f.guest_fin && f.to_host.is_empty() {
                            remove = true;
                        } else if now.duration_since(f.last) > TCP_LINGER_TIMEOUT && (f.guest_fin || f.host_eof) {
                            remove = true;
                        }
                    }
                }
            }

            if remove {
                remove_keys.push(*key);
            }
        }
        for k in remove_keys {
            self.tcp.remove(&k);
        }
    }
}

impl NetBackend for UserNet {
    fn send(&mut self, frame: &[u8]) {
        self.handle_frame(frame);
    }

    fn receive(&mut self) -> Option<Vec<u8>> {
        // The sockets are serviced on a throttle. A poll costs a system call per open
        // flow, and the card asks for frames every scheduling round - hundreds of
        // times for every millisecond - so doing it each time would slow the guest to a
        // crawl whenever a connection was open. Every 32nd call is a poll roughly every
        // tenth of a millisecond of guest time, which no network can notice.
        self.receive_calls = self.receive_calls.wrapping_add(1);
        if self.w.rx.is_empty() && self.receive_calls & 31 == 0 {
            self.pump();
        }
        self.w.rx.pop_front()
    }

    fn io_pending(&self) -> bool {
        // The machine uses this to decide whether an idle guest may skip ahead in time.
        // It must not while the outside world is still expected to answer: jumping the
        // guest's clock past a DNS timeout before the reply has had a moment to arrive
        // turns a working network into a broken one.
        let now = Instant::now();
        if self.udp.values().any(|f| now.duration_since(f.last) < Duration::from_secs(2)) {
            return true;
        }
        self.tcp.values().any(|f| f.state != TcpState::Established || f.snd_una != f.snd_nxt || !f.to_host.is_empty())
    }
}
