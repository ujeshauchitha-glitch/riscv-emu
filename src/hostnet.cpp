#include "hostnet.hpp"

#include <cstring>

#ifdef _WIN32
#include <iphlpapi.h>
#include <vector>
#else
#include <fstream>
#include <sstream>
#include <string>
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <unistd.h>
#endif

namespace hostnet {

namespace {

#ifdef _WIN32
bool would_block() {
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
}
bool in_progress_error() {
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
}
#else
bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
bool in_progress_error() { return errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EINTR; }
#endif

bool make_nonblocking(sock_t s) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

sockaddr_in make_addr(u32 ip, u16 port) {
    sockaddr_in a;
    std::memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_port        = htons(port);
    a.sin_addr.s_addr = htonl(ip);
    return a;
}

}  // namespace

bool init() {
#ifdef _WIN32
    static bool done = false;
    static bool ok   = false;
    if (!done) {
        WSADATA data;
        ok   = (WSAStartup(MAKEWORD(2, 2), &data) == 0);
        done = true;
    }
    return ok;
#else
    return true;
#endif
}

sock_t tcp_socket() {
    if (!init()) return INVALID_SOCK;
    sock_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCK) return s;
    if (!make_nonblocking(s)) { close_socket(s); return INVALID_SOCK; }
    // The guest's own TCP does its own coalescing, and a delayed segment is
    // latency the guest cannot see the cause of.
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
    return s;
}

sock_t udp_socket() {
    if (!init()) return INVALID_SOCK;
    sock_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCK) return s;
    if (!make_nonblocking(s)) { close_socket(s); return INVALID_SOCK; }
    return s;
}

void close_socket(sock_t s) {
    if (s == INVALID_SOCK) return;
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

int connect_ipv4(sock_t s, u32 ip, u16 port) {
    sockaddr_in a = make_addr(ip, port);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) return 1;
    return in_progress_error() ? 0 : -1;
}

int connect_result(sock_t s) {
    // A non-blocking connect settles when the socket becomes writable (success)
    // or reports an error through SO_ERROR (failure).
    fd_set wr, ex;
    FD_ZERO(&wr);
    FD_ZERO(&ex);
    FD_SET(s, &wr);
    FD_SET(s, &ex);
    timeval tv{0, 0};
    const int n = ::select(static_cast<int>(s) + 1, nullptr, &wr, &ex, &tv);
    if (n < 0) return -1;
    if (n == 0) return 0;

    int err = 0;
#ifdef _WIN32
    int len = sizeof err;
#else
    socklen_t len = sizeof err;
#endif
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) != 0) return -1;
    if (err != 0 || FD_ISSET(s, &ex)) return -1;
    return FD_ISSET(s, &wr) ? 1 : 0;
}

u16 bind_ipv4(sock_t s, u32 ip, u16 port) {
    int one = 1;
#ifndef _WIN32
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
#endif
    (void)one;
    sockaddr_in a = make_addr(ip, port);
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return 0;
    sockaddr_in got;
#ifdef _WIN32
    int len = sizeof got;
#else
    socklen_t len = sizeof got;
#endif
    if (getsockname(s, reinterpret_cast<sockaddr*>(&got), &len) != 0) return 0;
    return ntohs(got.sin_port);
}

bool listen_on(sock_t s) { return ::listen(s, 8) == 0; }

sock_t accept_connection(sock_t listener) {
    sock_t c = ::accept(listener, nullptr, nullptr);
    if (c == INVALID_SOCK) return INVALID_SOCK;
    if (!make_nonblocking(c)) { close_socket(c); return INVALID_SOCK; }
    int one = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
    return c;
}

long send_data(sock_t s, const u8* data, std::size_t len) {
    const int flags = 0;
    const auto n = ::send(s, reinterpret_cast<const char*>(data), static_cast<int>(len), flags);
    if (n >= 0) return static_cast<long>(n);
    return would_block() ? 0 : -1;
}

long recv_data(sock_t s, u8* data, std::size_t len) {
    const auto n = ::recv(s, reinterpret_cast<char*>(data), static_cast<int>(len), 0);
    if (n > 0) return static_cast<long>(n);
    if (n == 0) return -2;                    // orderly end of stream
    return would_block() ? 0 : -1;
}

long send_to(sock_t s, const u8* data, std::size_t len, u32 ip, u16 port) {
    sockaddr_in a = make_addr(ip, port);
    const auto n = ::sendto(s, reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
                            reinterpret_cast<sockaddr*>(&a), sizeof a);
    if (n >= 0) return static_cast<long>(n);
    return would_block() ? 0 : -1;
}

long recv_from(sock_t s, u8* data, std::size_t len, u32& ip, u16& port) {
    sockaddr_in a;
#ifdef _WIN32
    int alen = sizeof a;
#else
    socklen_t alen = sizeof a;
#endif
    const auto n = ::recvfrom(s, reinterpret_cast<char*>(data), static_cast<int>(len), 0,
                              reinterpret_cast<sockaddr*>(&a), &alen);
    if (n >= 0) {
        ip   = ntohl(a.sin_addr.s_addr);
        port = ntohs(a.sin_port);
        return static_cast<long>(n);
    }
#ifdef _WIN32
    // A UDP socket can report the previous send's ICMP "port unreachable" as a
    // reset on the next receive; that is not a reason to give up on the flow.
    if (WSAGetLastError() == WSAECONNRESET) return 0;
#endif
    return would_block() ? 0 : -1;
}

u32 system_dns_server() {
#ifdef _WIN32
    ULONG len = 0;
    GetNetworkParams(nullptr, &len);
    if (len == 0) return 0;
    std::vector<unsigned char> buf(len);
    auto* info = reinterpret_cast<FIXED_INFO*>(buf.data());
    if (GetNetworkParams(info, &len) != NO_ERROR) return 0;
    for (const IP_ADDR_STRING* a = &info->DnsServerList; a != nullptr; a = a->Next) {
        in_addr addr;
        if (inet_pton(AF_INET, a->IpAddress.String, &addr) == 1 && addr.s_addr != 0) {
            return ntohl(addr.s_addr);
        }
    }
    return 0;
#else
    std::ifstream f("/etc/resolv.conf");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream words(line);
        std::string key, value;
        if ((words >> key >> value) && key == "nameserver") {
            in_addr addr;
            if (inet_pton(AF_INET, value.c_str(), &addr) == 1 && addr.s_addr != 0) {
                return ntohl(addr.s_addr);
            }
        }
    }
    return 0;
#endif
}

void shutdown_write(sock_t s) {
#ifdef _WIN32
    ::shutdown(s, SD_SEND);
#else
    ::shutdown(s, SHUT_WR);
#endif
}

}  // namespace hostnet
