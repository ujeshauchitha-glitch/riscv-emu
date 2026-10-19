//! The host's network, as the user-mode stack needs it: non-blocking TCP and UDP
//! sockets, and a way to find the host's DNS server.
//!
//! The standard library has everything but a non-blocking `connect`, so connecting is
//! done on a short-lived thread that hands the finished socket back over a channel;
//! everything after that is polled without ever blocking the emulator.

use std::io::{ErrorKind, Read, Write};
use std::net::{Ipv4Addr, SocketAddr, SocketAddrV4, TcpStream, UdpSocket};
use std::sync::mpsc::{channel, Receiver, TryRecvError};
use std::time::Duration;

pub const CONNECT_TIMEOUT: Duration = Duration::from_secs(20);

pub fn ipv4(ip: u32) -> Ipv4Addr {
    Ipv4Addr::from(ip)
}

/// A TCP connection to the host network: connecting, or connected.
pub enum HostTcp {
    Connecting(Receiver<std::io::Result<TcpStream>>),
    Connected(TcpStream),
    Failed,
}

#[derive(Debug, PartialEq, Eq)]
pub enum ConnectState {
    Pending,
    Up,
    Failed,
}

#[derive(Debug, PartialEq, Eq)]
pub enum Recv {
    Data(usize),
    WouldBlock,
    /// The peer closed its end cleanly.
    Eof,
    Error,
}

impl HostTcp {
    /// Begin connecting. Never blocks.
    pub fn connect(ip: u32, port: u16) -> HostTcp {
        let (tx, rx) = channel();
        let addr = SocketAddr::V4(SocketAddrV4::new(ipv4(ip), port));
        std::thread::spawn(move || {
            let r = TcpStream::connect_timeout(&addr, CONNECT_TIMEOUT).and_then(|s| {
                s.set_nonblocking(true)?;
                // The guest's own TCP does its own coalescing, and a delayed segment
                // is added latency for nothing.
                let _ = s.set_nodelay(true);
                Ok(s)
            });
            let _ = tx.send(r);
        });
        HostTcp::Connecting(rx)
    }

    /// Wrap an already-connected stream (used by tests that accept on a listener).
    pub fn from_stream(s: TcpStream) -> HostTcp {
        let _ = s.set_nonblocking(true);
        HostTcp::Connected(s)
    }

    /// Where is the connection? Advances `Connecting` to `Connected` when it can.
    pub fn poll_connect(&mut self) -> ConnectState {
        if let HostTcp::Connecting(rx) = self {
            match rx.try_recv() {
                Ok(Ok(stream)) => *self = HostTcp::Connected(stream),
                Ok(Err(_)) | Err(TryRecvError::Disconnected) => *self = HostTcp::Failed,
                Err(TryRecvError::Empty) => {}
            }
        }
        match self {
            HostTcp::Connecting(_) => ConnectState::Pending,
            HostTcp::Connected(_) => ConnectState::Up,
            HostTcp::Failed => ConnectState::Failed,
        }
    }

    /// Bytes accepted by the host socket (0 if it would block), or None on error.
    pub fn send(&mut self, data: &[u8]) -> Option<usize> {
        let HostTcp::Connected(s) = self else {
            return None;
        };
        match s.write(data) {
            Ok(n) => Some(n),
            Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::Interrupted) => Some(0),
            Err(_) => None,
        }
    }

    pub fn recv(&mut self, buf: &mut [u8]) -> Recv {
        let HostTcp::Connected(s) = self else {
            return Recv::Error;
        };
        match s.read(buf) {
            Ok(0) => Recv::Eof,
            Ok(n) => Recv::Data(n),
            Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::Interrupted) => Recv::WouldBlock,
            Err(_) => Recv::Error,
        }
    }

    /// Tell the host we have no more to send, keeping the receive side open.
    pub fn shutdown_write(&mut self) {
        if let HostTcp::Connected(s) = self {
            let _ = s.shutdown(std::net::Shutdown::Write);
        }
    }
}

/// A non-blocking UDP socket on an ephemeral port.
pub fn udp_socket() -> Option<UdpSocket> {
    let s = UdpSocket::bind("0.0.0.0:0").ok()?;
    s.set_nonblocking(true).ok()?;
    Some(s)
}

pub fn udp_send_to(s: &UdpSocket, data: &[u8], ip: u32, port: u16) -> bool {
    s.send_to(data, SocketAddrV4::new(ipv4(ip), port)).is_ok()
}

/// One datagram if one is waiting.
pub fn udp_recv_from(s: &UdpSocket, buf: &mut [u8]) -> Option<usize> {
    s.recv_from(buf).ok().map(|(n, _)| n)
}

/// The first IPv4 DNS server the host is configured with, or 0 if none is found.
#[cfg(windows)]
pub fn system_dns_server() -> u32 {
    use std::ffi::c_void;

    #[repr(C)]
    struct IpAddrString {
        next: *mut IpAddrString,
        ip_address: [u8; 16],
        ip_mask: [u8; 16],
        context: u32,
    }
    #[repr(C)]
    struct FixedInfo {
        host_name: [u8; 132],
        domain_name: [u8; 132],
        current_dns_server: *mut IpAddrString,
        dns_server_list: IpAddrString,
        node_type: u32,
        scope_id: [u8; 260],
        enable_routing: u32,
        enable_proxy: u32,
        enable_dns: u32,
    }
    #[link(name = "iphlpapi")]
    extern "system" {
        fn GetNetworkParams(info: *mut c_void, len: *mut u32) -> u32;
    }

    // SAFETY: the standard two-call pattern - ask for the size, then fill a buffer of
    // at least that size (u64-aligned, which satisfies FixedInfo) - and then walk the
    // linked list the call built inside that buffer.
    unsafe {
        let mut len = 0u32;
        GetNetworkParams(std::ptr::null_mut(), &mut len);
        if len == 0 {
            return 0;
        }
        let mut buf = vec![0u64; (len as usize + 7) / 8];
        if GetNetworkParams(buf.as_mut_ptr() as *mut c_void, &mut len) != 0 {
            return 0;
        }
        let info = &*(buf.as_ptr() as *const FixedInfo);
        let mut node: *const IpAddrString = &info.dns_server_list;
        while !node.is_null() {
            let text = &(*node).ip_address;
            let end = text.iter().position(|&c| c == 0).unwrap_or(text.len());
            if let Ok(s) = std::str::from_utf8(&text[..end]) {
                if let Ok(ip) = s.parse::<Ipv4Addr>() {
                    if !ip.is_unspecified() {
                        return u32::from(ip);
                    }
                }
            }
            node = (*node).next;
        }
        0
    }
}

#[cfg(not(windows))]
pub fn system_dns_server() -> u32 {
    let Ok(text) = std::fs::read_to_string("/etc/resolv.conf") else {
        return 0;
    };
    for line in text.lines() {
        let mut words = line.split_whitespace();
        if words.next() == Some("nameserver") {
            if let Some(Ok(ip)) = words.next().map(|w| w.parse::<Ipv4Addr>()) {
                if !ip.is_unspecified() {
                    return u32::from(ip);
                }
            }
        }
    }
    0
}
