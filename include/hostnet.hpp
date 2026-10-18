#pragma once

#include <cstddef>

#include "types.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#endif

// ---------------------------------------------------------------------------
// A thin portable layer over the host's sockets.
//
// Windows and POSIX spell the same handful of operations differently - a
// socket is a SOCKET on one and an int on the other, closing is closesocket()
// or close(), "would block" is WSAEWOULDBLOCK or EAGAIN, and Windows needs
// WSAStartup before anything works. Everything in the emulator that talks to
// the outside world (the user-mode network, the GDB stub) goes through these
// few functions so none of it needs an #ifdef.
//
// Everything here is IPv4 and non-blocking, because the emulator polls its
// sockets from the scheduling loop and must never stall the guest waiting on
// one.
// ---------------------------------------------------------------------------

#ifdef _WIN32
using sock_t = SOCKET;
constexpr sock_t INVALID_SOCK = INVALID_SOCKET;
#else
using sock_t = int;
constexpr sock_t INVALID_SOCK = -1;
#endif

namespace hostnet {

// Initialise the socket library. Safe to call repeatedly. Returns false if the
// host has no usable sockets.
bool init();

// New non-blocking sockets, or INVALID_SOCK.
sock_t tcp_socket();
sock_t udp_socket();

void close_socket(sock_t s);

// Addresses are host byte order: 127.0.0.1 is 0x7f000001.

// Start connecting. Returns 1 if connected immediately, 0 if the connection is
// in progress (poll connect_result() until it settles), -1 on failure.
int connect_ipv4(sock_t s, u32 ip, u16 port);

// For a socket with a connect in progress: 1 = connected, 0 = still waiting,
// -1 = failed (refused, unreachable, ...).
int connect_result(sock_t s);

// Bind to ip:port (port 0 picks one) and, for TCP, listen. Returns the port
// actually bound, or 0 on failure.
u16 bind_ipv4(sock_t s, u32 ip, u16 port);
bool listen_on(sock_t s);

// Accept a pending connection, or INVALID_SOCK if none is waiting.
sock_t accept_connection(sock_t listener);

// Non-blocking transfers. send_data/recv_data return the byte count, 0 for "no
// progress right now" (would block), and -1 for a closed or failed connection;
// recv_data additionally returns -2 for an orderly end-of-stream.
long send_data(sock_t s, const u8* data, std::size_t len);
long recv_data(sock_t s, u8* data, std::size_t len);

// Datagrams.
long send_to(sock_t s, const u8* data, std::size_t len, u32 ip, u16 port);
// Returns the datagram length, 0 if none is waiting, -1 on error. The sender is
// stored in ip/port.
long recv_from(sock_t s, u8* data, std::size_t len, u32& ip, u16& port);

// Half-close the sending direction of a TCP connection.
void shutdown_write(sock_t s);

// The first IPv4 DNS server the host is configured to use (host byte order), or
// 0 if it cannot tell. A guest's DNS queries are forwarded there.
u32 system_dns_server();

}  // namespace hostnet
