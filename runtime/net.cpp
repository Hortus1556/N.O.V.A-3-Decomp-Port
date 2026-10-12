// BSD sockets (bionic ABI) on host sockets (Winsock or POSIX): enough for LAN multiplayer and the engine's
// internal networking. Name resolution only handles numeric addresses and localhost, so
// the game's (defunct) online services stay offline instead of timing out.
//
// Guest sockets are ordinary guest fds (libc_io.cpp marks them kSocketFd); the host socket
// lives here. Linux option numbers, errno values and the AF_INET6 family number are
// translated; struct sockaddr_in/_in6 layouts are otherwise identical.
#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#if !defined(__SWITCH__) && !defined(__3DS__)
#include <ifaddrs.h>
#include <net/if.h>
#endif
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif
#ifdef __SWITCH__
#include <switch.h>  // nifm: the console's address and subnet
#endif
#ifdef __3DS__
#include <3ds.h>  // SOCU_GetIPInfo
// libctru's socket headers (the soc:U service) leave these out: options the service does not
// know, which setsockopt then refuses (the game carries on without them); no IPv6, no FIONREAD
#define POLLRDBAND 0
#define SO_KEEPALIVE -101
#define SO_RCVTIMEO -102
#define SO_SNDTIMEO -103
#define IP_MULTICAST_IF -104
#define IPPROTO_IPV6 -105
#define IPV6_V6ONLY -106
#define FIONREAD -107
#endif

#include "os.h"
#include "runtime.h"

#include <algorithm>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace rt {
namespace {

using u32 = uint32_t;


// ---- host socket API differences ----------------------------------------------------------------
#ifdef _WIN32
using sock_t = SOCKET;
using socklen = int;
const sock_t kNoSock = INVALID_SOCKET;
int sock_error() { return WSAGetLastError(); }
void sock_close(sock_t s) { closesocket(s); }
void sock_set_nonblock(sock_t s, bool on) { u_long v = on ? 1 : 0; ioctlsocket(s, FIONBIO, &v); }
int sock_available(sock_t s, uint32_t &n) { u_long v = 0; int r = ioctlsocket(s, FIONREAD, &v); n = uint32_t(v); return r; }
int sock_poll(sock_t s, short events, short &revents) {
    WSAPOLLFD p{s, events, 0};
    int r = WSAPoll(&p, 1, 0);
    revents = p.revents;
    return r;
}
constexpr int kErrWouldBlock = WSAEWOULDBLOCK, kErrInProgress = WSAEINPROGRESS, kErrMsgSize = WSAEMSGSIZE;
constexpr short kPollRead = POLLRDNORM, kPollWrite = POLLWRNORM;  // what WSAPoll accepts
void sock_startup() {
    static bool done = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    (void)done;
}
#else
using sock_t = int;
using socklen = socklen_t;
constexpr sock_t kNoSock = -1;
int sock_error() { return errno; }
void sock_close(sock_t s) { ::close(s); }
void sock_set_nonblock(sock_t s, bool on) {
    int f = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? f | O_NONBLOCK : f & ~O_NONBLOCK);
}
int sock_available(sock_t s, uint32_t &n) { int v = 0; int r = ioctl(s, FIONREAD, &v); n = uint32_t(v); return r; }
int sock_poll(sock_t s, short events, short &revents) {
    pollfd p{s, events, 0};
    int r = ::poll(&p, 1, 0);
    revents = p.revents;
    return r;
}
constexpr int kErrWouldBlock = EWOULDBLOCK, kErrInProgress = EINPROGRESS, kErrMsgSize = EMSGSIZE;
constexpr short kPollRead = POLLIN, kPollWrite = POLLOUT;  // the portable pair (the Switch's BSD service)
void sock_startup() {}
#endif

std::mutex g_lock;
std::unordered_map<int, sock_t> g_socks;
std::unordered_map<int, bool> g_nonblock;
std::unordered_map<int, bool> g_dgram;  // UDP sockets (readiness by peek, Switch)

sock_t sock_of(int fd) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_socks.find(fd);
    return it == g_socks.end() ? kNoSock : it->second;
}

int linux_errno(int e) {
#ifdef _WIN32
#define E(name) WSA##name
#else
#define E(name) name
#endif
    switch (e) {
    case E(EWOULDBLOCK): return kEAGAIN;
    case E(EINPROGRESS): return 115;
    case E(EALREADY): return 114;
    case E(ENOTSOCK): return kENOTSOCK;
    case E(EMSGSIZE): return 90;
    case E(EADDRINUSE): return 98;
    case E(EADDRNOTAVAIL): return 99;
    case E(ENETDOWN): return kENETDOWN;
    case E(ENETUNREACH): return kENETUNREACH;
    case E(ECONNABORTED): return 103;
    case E(ECONNRESET): return 104;
    case E(EISCONN): return 106;
    case E(ENOTCONN): return 107;
    case E(ETIMEDOUT): return kETIMEDOUT;
    case E(ECONNREFUSED): return kECONNREFUSED;
    case E(EHOSTUNREACH): return 113;
    case E(EAFNOSUPPORT): return kEAFNOSUPPORT;
    case E(EINVAL): return kEINVAL;
    case E(EINTR): return kEINTR;
    case E(EACCES): return kEACCES;
    case E(EMFILE): return kEMFILE;
#ifndef _WIN32
    case EPIPE: return 32;
#endif
    }
#undef E
    return kEIO;
}
// Each call's first failure with each host error is logged: what the consoles' socket services
// refuse shows in the log.
void note_error(const char *what, int detail, int e) {
    static std::mutex lock;
    static std::set<std::string> seen;
    const std::string key = std::string(what) + ' ' + std::to_string(detail) + ' ' + std::to_string(e);
    std::lock_guard<std::mutex> guard(lock);
    if (seen.size() >= 64 || !seen.insert(key).second) return;
    if (detail >= 0) log("net: %s (%d) failed: host error %d", what, detail, e);
    else log("net: %s failed: host error %d", what, e);
}
// NOVA3_TRACE_NET=1: log every datagram sent and received (size, address, result).
bool tracing_net() {
    static const bool on = std::getenv("NOVA3_TRACE_NET") != nullptr;
    return on;
}
void trace_datagram(const char *op, int fd, int r, const sockaddr_storage *a) {
    char name[INET_ADDRSTRLEN] = "-";
    int port = 0;
    if (a && a->ss_family == AF_INET) {
        const sockaddr_in *in = reinterpret_cast<const sockaddr_in *>(a);
        inet_ntop(AF_INET, &in->sin_addr, name, sizeof name);
        port = ntohs(in->sin_port);
    }
    log("net: %s fd %d -> %d, %s:%d", op, fd, r, name, port);
}
// A failed host call: errno for the guest.
int fail(const char *what, int detail = -1) {
    const int e = sock_error();
    if (e != kErrWouldBlock) note_error(what, detail, e);
    set_errno(linux_errno(e));
    return -1;
}

// guest sockaddr <-> host sockaddr. bionic's begins with a 16-bit family (AF_INET6 is 10 there),
// and so do Winsock's, Linux's and libctru's; the Switch's is BSD's: a length byte, then an 8-bit
// family (AF_INET6 28). The rest (port, address) is laid out alike. The Switch's BSD service got
// bionic's bytes as they were, length 2 and family 0 (AF_UNSPEC): emulators take that as IPv4,
// the console refused the join search's broadcast (a sendto to an address the game built).
int to_host_addr(u32 g, u32 len, sockaddr_storage &out) {
    std::memset(&out, 0, sizeof out);
    len = std::min<u32>(len, sizeof out);
    std::memcpy(&out, ptr(g), len);
    if (len < 2) return int(len);
    const uint16_t family = uint16_t(LD16(g));
    std::memset(&out, 0, 2);
#ifdef __SWITCH__
    out.ss_len = uint8_t(len);
#endif
    out.ss_family = decltype(out.ss_family)(family == 10 ? AF_INET6 : family);
    return int(len);
}
void to_guest_addr(const sockaddr_storage &a, int len, u32 g, u32 lenp) {
    if (!g || !lenp) return;
    sockaddr_storage c = a;
    const uint16_t family = uint16_t(a.ss_family == AF_INET6 ? 10 : a.ss_family);
    std::memcpy(&c, &family, 2);
    // bionic's sizes: the game drops a datagram whose sender's length is not 16 (Citron's BSD
    // service reports other lengths)
    if (a.ss_family == AF_INET) {
        len = 16;
        std::memset(reinterpret_cast<char *>(&c) + 8, 0, 8);  // sin_zero (the game compares whole addresses)
    } else if (a.ss_family == AF_INET6) {
        len = 28;
    }
    u32 cap = LD32(lenp);
    std::memcpy(ptr(g), &c, std::min<u32>(cap, u32(len)));
    ST32(lenp, u32(len));
}

int host_flags(int f) {
    int w = 0;
    if (f & 1) w |= MSG_OOB;
    if (f & 2) w |= MSG_PEEK;
    if (f & 0x100) w |= MSG_WAITALL;
#if defined(MSG_NOSIGNAL) && !defined(__SWITCH__)  // (the Switch has no signals; emulators assert on the flag)
    w |= MSG_NOSIGNAL;  // broken connections report EPIPE instead of raising SIGPIPE
#endif
    return w;  // MSG_DONTWAIT (0x40) is handled by the caller
}

bool is_dgram(int fd) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_dgram.find(fd);
    return it != g_dgram.end() && it->second;
}

#ifdef __SWITCH__
// The Switch's UDP readiness: the next datagram, received ahead (see socket_ready) and handed
// to the game's next read of that socket.
struct Pending {
    std::vector<char> data;
    sockaddr_storage from;
    socklen len;
};
std::mutex g_pending_lock;  // also serialises the read-ahead
std::unordered_map<int, Pending> g_pending;

// 1: a datagram is waiting (received ahead), 0: none, -1: the receive failed (errno)
int read_ahead(int fd, sock_t s) {
    std::lock_guard<std::mutex> lock(g_pending_lock);
    if (g_pending.count(fd)) return 1;
    static char buf[65536];
    Pending p;
    p.len = sizeof p.from;
    const ssize_t r = ::recvfrom(s, buf, sizeof buf, MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&p.from), &p.len);
    if (r < 0) return -1;
    p.data.assign(buf, buf + r);
    g_pending[fd] = std::move(p);
    return 1;
}
// A read of a datagram socket with one waiting: it is returned (truncated to n, as Linux does),
// and kept for a MSG_PEEK. Returns -1 when none is waiting.
int take_pending(int fd, uint32_t buf, uint32_t n, int flags, sockaddr_storage *from, socklen *len) {
    std::lock_guard<std::mutex> lock(g_pending_lock);
    auto it = g_pending.find(fd);
    if (it == g_pending.end()) return -1;
    const int r = int(std::min<size_t>(n, it->second.data.size()));
    std::memcpy(ptr(buf), it->second.data.data(), size_t(r));
    if (from) *from = it->second.from;
    if (len) *len = it->second.len;
    if (!(flags & 2)) g_pending.erase(it);  // MSG_PEEK leaves it
    return r;
}
#endif

// Wait-free readiness of one socket: POLLIN 1, POLLOUT 4, POLLERR 8, POLLHUP 0x10.
int socket_ready(int fd, sock_t s, int events) {
    (void)fd;
#ifdef __SWITCH__
    // Readable? A non-blocking peek answers like a poll; emulators (Ryujinx) answer a
    // zero-timeout poll only after ~50 ms, and the game polls its sockets every frame (10 fps
    // in a local match). Sockets the peek says nothing about (listening ones) are polled.
    // A datagram socket's next datagram is received ahead instead, whole, and handed to the
    // game's next read: Citron's service ignores MSG_PEEK (a peek consumed the datagram; the
    // host's answers and the match's pings were lost, and the joining player was dropped
    // before the clock sync).
    if (events == 1 && is_dgram(fd)) {
        const int r = read_ahead(fd, s);
        if (r > 0) return 1;
        if (errno == EWOULDBLOCK || errno == EAGAIN) return 0;
        if (errno != ENOTCONN && errno != EINVAL && errno != EOPNOTSUPP) {
            note_error("read ahead", -1, errno);
            return 8 | 1;  // the read reports it
        }
    } else if (events == 1) {
        char c;
        const ssize_t r = ::recv(s, &c, 1, MSG_PEEK | MSG_DONTWAIT);
        if (r > 0) return 1;
        if (r == 0) return 0x10 | 1;  // a stream at its end: a read returns EOF
        if (errno == EWOULDBLOCK || errno == EAGAIN) return 0;
        if (errno != ENOTCONN && errno != EINVAL && errno != EOPNOTSUPP) {
            note_error("peek", -1, errno);
            return 8 | 1;  // the read reports it
        }
    }
#endif
    short revents = 0;
    if (sock_poll(s, short(((events & 1) ? kPollRead : 0) | ((events & 4) ? kPollWrite : 0)), revents) <= 0) return 0;
    int r = 0;
    if (revents & (kPollRead | POLLRDBAND)) r |= 1;
    if (revents & kPollWrite) r |= 4;
    if (revents & POLLERR) r |= 8;
    if (revents & POLLHUP) r |= 0x10 | 1;  // a read returns EOF
    return r;
}

// MSG_DONTWAIT on a blocking socket: fail with EAGAIN instead of blocking.
bool would_block(int fd, sock_t s, int flags, int events) {
    if (!(flags & 0x40)) return false;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_nonblock[fd]) return false;
    }
    if (socket_ready(fd, s, events)) return false;
    set_errno(kEAGAIN);
    return true;
}

// ---- socket calls --------------------------------------------------------------------------------
int n_socket(int domain, int type, int protocol) {
    sock_startup();
    bool nonblock = type & 04000;  // SOCK_NONBLOCK
    type &= 0xf;                   // drop SOCK_NONBLOCK / SOCK_CLOEXEC
    sock_t s = ::socket(domain == 10 ? AF_INET6 : domain, type, protocol);
    if (s == kNoSock) return fail("socket");
#ifdef _WIN32
    // A UDP socket on Linux never reports another machine's ICMP "port unreachable"; Winsock
    // fails its next recvfrom with WSAECONNRESET, and the game counted each one as a comms error
    // (a player who quit a local match).
    if (type == SOCK_DGRAM) {
        BOOL report = FALSE;
        DWORD bytes = 0;
        WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof report, nullptr, 0, &bytes, nullptr, nullptr);
    }
#endif
    int fd = io_alloc_socket_fd();
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_socks[fd] = s;
        g_nonblock[fd] = false;
        g_dgram[fd] = type == SOCK_DGRAM;
    }
    if (nonblock) net_set_nonblock(fd, true);
    return fd;
}
int n_bind(int fd, u32 addr, u32 len) {
    sock_t s = sock_of(fd);
    if (s == kNoSock) { set_errno(kEBADF); return -1; }
    sockaddr_storage a;
    int l = to_host_addr(addr, len, a);
    return ::bind(s, reinterpret_cast<sockaddr *>(&a), l) == 0 ? 0 : fail("bind");
}
bool local_network(const sockaddr_storage &a);
int n_connect(int fd, u32 addr, u32 len) {
    sock_t s = sock_of(fd);
    if (s == kNoSock) { set_errno(kEBADF); return -1; }
    sockaddr_storage a;
    int l = to_host_addr(addr, len, a);
    // No internet services exist any more (docs/offline.md): a connection out of the local
    // network is refused at once, as a dead server would. On a console without a route (the
    // 3DS) such a connect sat until a long timeout while Gaia's online init held its lock, and
    // the title menu's settings save waited on that lock.
    if (!local_network(a)) {
        set_errno(kECONNREFUSED);
        return -1;
    }
    if (::connect(s, reinterpret_cast<sockaddr *>(&a), l) == 0) return 0;
    const int e = sock_error();
    if (e != kErrWouldBlock && e != kErrInProgress) return fail("connect");
    set_errno(115);  // EINPROGRESS: a non-blocking connect under way
    return -1;
}
int n_listen(int fd, int backlog) {
    sock_t s = sock_of(fd);
    if (s == kNoSock) { set_errno(kEBADF); return -1; }
    return ::listen(s, backlog) == 0 ? 0 : fail("listen");
}
int n_accept(int fd, u32 addr, u32 lenp) {
    sock_t s = sock_of(fd);
    if (s == kNoSock) { set_errno(kEBADF); return -1; }
    sockaddr_storage a;
    socklen l = sizeof a;
    sock_t c = ::accept(s, reinterpret_cast<sockaddr *>(&a), &l);
    if (c == kNoSock) return fail("accept");
    to_guest_addr(a, l, addr, lenp);
    int nfd = io_alloc_socket_fd();
    std::lock_guard<std::mutex> lock(g_lock);
    g_socks[nfd] = c;
    g_nonblock[nfd] = false;
    g_dgram[nfd] = false;
    return nfd;
}
int n_send(int fd, u32 buf, u32 len, int flags) { return net_send(fd, buf, len, flags); }
int n_recv(int fd, u32 buf, u32 len, int flags) { return net_recv(fd, buf, len, flags); }
// Subnet broadcast addresses (network byte order) of the IPv4 interfaces that are up, loopback
// left out; refreshed every 5 s, and logged when they change.
std::vector<uint32_t> interface_broadcasts() {
    static std::mutex lock;
    static std::vector<uint32_t> cached;
    static int64_t refreshed = 0;
    std::lock_guard<std::mutex> guard(lock);
    const int64_t now = os::monotonic_ns();
    if (refreshed && now - refreshed < 5'000'000'000) return cached;
    refreshed = now;
    std::vector<uint32_t> found;
    auto add = [&](uint32_t addr_be, int prefix) {
        if (prefix <= 0 || prefix >= 31) return;
        const uint32_t mask = ~0u << (32 - prefix);
        const uint32_t bcast = htonl((ntohl(addr_be) & mask) | ~mask);
        if (std::find(found.begin(), found.end(), bcast) == found.end()) found.push_back(bcast);
    };
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<char> mem;
    ULONG r = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && r == ERROR_BUFFER_OVERFLOW; tries++) {
        mem.resize(size);
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(mem.data()), &size);
    }
    if (r == NO_ERROR)
        for (auto *a = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(mem.data()); a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto *u = a->FirstUnicastAddress; u; u = u->Next)
                if (u->Address.lpSockaddr->sa_family == AF_INET)
                    add(reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr)->sin_addr.s_addr, u->OnLinkPrefixLength);
        }
#elif defined(__SWITCH__)
    // one interface (Wi-Fi / LAN adapter): its address and subnet mask from nifm
    u32 addr = 0, mask = 0, gateway = 0, dns1 = 0, dns2 = 0;
    if (R_SUCCEEDED(nifmInitialize(NifmServiceType_User))) {
        if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&addr, &mask, &gateway, &dns1, &dns2)) && addr)
            add(addr, __builtin_popcount(mask));
        nifmExit();
    }
#elif defined(__3DS__)
    in_addr addr{}, mask{}, bcast{};
    if (SOCU_GetIPInfo(&addr, &mask, &bcast) == 0 && addr.s_addr) add(addr.s_addr, __builtin_popcount(mask.s_addr));
#else
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs *i = list; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_netmask) continue;
            if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
            const uint32_t mask = ntohl(reinterpret_cast<sockaddr_in *>(i->ifa_netmask)->sin_addr.s_addr);
            add(reinterpret_cast<sockaddr_in *>(i->ifa_addr)->sin_addr.s_addr, __builtin_popcount(mask));
        }
        freeifaddrs(list);
    }
#endif
    if (found != cached) {
        std::string text;
        for (uint32_t b : found) {
            in_addr ia{};
            ia.s_addr = b;
            char name[INET_ADDRSTRLEN] = "?";
            inet_ntop(AF_INET, &ia, name, sizeof name);
            text += std::string(text.empty() ? "" : ", ") + name;
        }
        log("net: broadcasts go to %s", text.empty() ? "(no interface)" : text.c_str());
        cached = found;
    }
    return cached;
}

std::mutex g_peers_lock;
std::vector<uint32_t> g_peers;  // net_set_peers (network byte order); the menus change it in play

// Addresses local play can reach: loopback, the private ranges, link-local, carrier-grade NAT
// (Tailscale), Hamachi's 25/8, the subnets of this machine's interfaces and the listed players.
bool local_network(const sockaddr_storage &a) {
    if (a.ss_family == AF_INET6) {
        const uint8_t *b = reinterpret_cast<const uint8_t *>(&a) + 8;  // sockaddr_in6: family, port, flow info, address
        static const uint8_t loop[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        return !std::memcmp(b, loop, 16) || (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) || (b[0] & 0xfe) == 0xfc;
    }
    if (a.ss_family != AF_INET) return true;
    const uint32_t be = reinterpret_cast<const sockaddr_in *>(&a)->sin_addr.s_addr, ip = ntohl(be);
    auto in = [ip](uint32_t net, int bits) { return (ip >> (32 - bits)) == (net >> (32 - bits)); };
    if (in(0x7f000000, 8) || in(0x0a000000, 8) || in(0xac100000, 12) || in(0xc0a80000, 16) || in(0xa9fe0000, 16) ||
        in(0x64400000, 10) || in(0x19000000, 8) || ip == 0xffffffffu || ip == 0)
        return true;
    {
        std::lock_guard<std::mutex> lock(g_peers_lock);
        if (std::find(g_peers.begin(), g_peers.end(), be) != g_peers.end()) return true;
    }
    for (uint32_t bcast : interface_broadcasts()) {  // each interface's subnet: its broadcast's host bits are ones
        const uint32_t b = ntohl(bcast), host = ~b ? __builtin_ctz(~b) : 32, mask = host >= 32 ? 0 : ~0u << host;
        if (host >= 2 && (ip & mask) == (b & mask)) return true;
    }
    static std::mutex logged_lock;
    static std::vector<uint32_t> logged;
    std::lock_guard<std::mutex> lock(logged_lock);
    if (std::find(logged.begin(), logged.end(), be) == logged.end()) {
        logged.push_back(be);
        char name[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in *>(&a)->sin_addr, name, sizeof name);
        log("net: no internet services: connect to %s refused", name);
    }
    return false;
}

int n_sendto(int fd, u32 buf, u32 len, int flags, u32 addr, u32 alen) {
    sock_t s = sock_of(fd);
    if (s == kNoSock) { set_errno(kEBADF); return -1; }
    if (would_block(fd, s, flags, 4)) return -1;
    sockaddr_storage a;
    int l = addr ? to_host_addr(addr, alen, a) : 0;
    int r = ::sendto(s, ptr(buf), int(len), host_flags(flags), addr ? reinterpret_cast<sockaddr *>(&a) : nullptr, l);
    // A limited broadcast (255.255.255.255; local Wi-Fi multiplayer finds games with one) leaves
    // Windows on one interface only, the preferred one: games on a virtual LAN (ZeroTier,
    // Hamachi, a VPN) or a second adapter never saw it. It is sent to every interface's subnet
    // broadcast as well. Where the limited broadcast itself is refused (a console's network
    // service may), the game is told it went out when one of those did: a failed send ends the
    // search for games.
    if (addr && a.ss_family == AF_INET && reinterpret_cast<sockaddr_in *>(&a)->sin_addr.s_addr == htonl(INADDR_BROADCAST)) {
        const int error = r < 0 ? sock_error() : 0;
        if (error == kErrWouldBlock) return fail("sendto");
        std::vector<uint32_t> targets = interface_broadcasts();
        {
            std::lock_guard<std::mutex> lock(g_peers_lock);
            targets.insert(targets.end(), g_peers.begin(), g_peers.end());  // and straight to the listed players
        }
        bool sent = false;
        for (uint32_t b : targets) {
            sockaddr_in copy = *reinterpret_cast<sockaddr_in *>(&a);
            copy.sin_addr.s_addr = b;
            if (::sendto(s, ptr(buf), int(len), host_flags(flags), reinterpret_cast<sockaddr *>(&copy), sizeof copy) >= 0)
                sent = true;
            else
                note_error("sendto (subnet / player)", -1, sock_error());
        }
        if (r < 0) {
            if (tracing_net()) trace_datagram("sendto", fd, sent ? int(len) : r, &a);
            note_error("sendto 255.255.255.255", -1, error);
            if (sent) return int(len);
            set_errno(linux_errno(error));
            return -1;
        }
    }
    if (tracing_net()) trace_datagram("sendto", fd, r, addr ? &a : nullptr);
    return r < 0 ? fail("sendto") : r;
}
int n_recvfrom(int fd, u32 buf, u32 len, int flags, u32 addr, u32 lenp) {
    sock_t s = sock_of(fd);
    if (s == kNoSock) { set_errno(kEBADF); return -1; }
    if (would_block(fd, s, flags, 1)) return -1;
    sockaddr_storage a;
    socklen l = sizeof a;
#ifdef __SWITCH__
    if (const int r = take_pending(fd, buf, len, flags, &a, &l); r >= 0) {
        if (tracing_net()) trace_datagram("recvfrom", fd, r, &a);
        to_guest_addr(a, l, addr, lenp);
        return r;
    }
#endif
    int r = ::recvfrom(s, ptr(buf), int(len), host_flags(flags), reinterpret_cast<sockaddr *>(&a), &l);
    if (tracing_net()) trace_datagram("recvfrom", fd, r, r >= 0 ? &a : nullptr);
    if (r < 0) {
        if (sock_error() == kErrMsgSize) r = int(len);  // datagram truncated (Linux returns the part read)
        else return fail("recvfrom");
    }
    to_guest_addr(a, l, addr, lenp);
    return r;
}
// struct msghdr (bionic, 32-bit): name, namelen, iov, iovlen, control, controllen, flags
int n_sendmsg(int fd, u32 msg, int flags) {
    std::vector<char> data;
    u32 iov = LD32(msg + 8), n = LD32(msg + 12);
    for (u32 i = 0; i < n; i++) {
        u32 base = LD32(iov + 8 * i), len = LD32(iov + 8 * i + 4);
        data.insert(data.end(), ptr(base), ptr(base) + len);
    }
    u32 tmp = gmalloc(u32(data.size()) + 1);
    std::memcpy(ptr(tmp), data.data(), data.size());
    int r = n_sendto(fd, tmp, u32(data.size()), flags, LD32(msg), LD32(msg + 4));
    gfree(tmp);
    return r;
}
int n_recvmsg(int fd, u32 msg, int flags) {
    u32 iov = LD32(msg + 8), n = LD32(msg + 12), total = 0;
    for (u32 i = 0; i < n; i++) total += LD32(iov + 8 * i + 4);
    u32 tmp = gmalloc(total + 1);
    u32 namelen = msg + 4;
    int r = n_recvfrom(fd, tmp, total, flags, LD32(msg), LD32(msg) ? namelen : 0);
    if (r > 0) {
        u32 off = 0;
        for (u32 i = 0; i < n && off < u32(r); i++) {
            u32 base = LD32(iov + 8 * i), len = std::min(LD32(iov + 8 * i + 4), u32(r) - off);
            std::memcpy(ptr(base), ptr(tmp + off), len);
            off += len;
        }
    }
    ST32(msg + 20, 0);  // no control data
    ST32(msg + 24, 0);  // msg_flags
    gfree(tmp);
    return r;
}
int n_shutdown(int fd, int how) {
    sock_t s = sock_of(fd);
    if (s == kNoSock) { set_errno(kEBADF); return -1; }
    return ::shutdown(s, how) == 0 ? 0 : fail("shutdown");
}
int n_getsockname(int fd, u32 addr, u32 lenp) {
    sock_t s = sock_of(fd);
    if (s == kNoSock) { set_errno(kEBADF); return -1; }
    sockaddr_storage a;
    socklen l = sizeof a;
    if (::getsockname(s, reinterpret_cast<sockaddr *>(&a), &l) != 0) return fail("getsockname");
    to_guest_addr(a, l, addr, lenp);
    return 0;
}
int n_getpeername(int fd, u32 addr, u32 lenp) {
    sock_t s = sock_of(fd);
    if (s == kNoSock) { set_errno(kEBADF); return -1; }
    sockaddr_storage a;
    socklen l = sizeof a;
    if (::getpeername(s, reinterpret_cast<sockaddr *>(&a), &l) != 0) return fail("getpeername");
    to_guest_addr(a, l, addr, lenp);
    return 0;
}

// Linux (level, option) -> host; returns false for options that are accepted and ignored.
bool map_option(int level, int opt, int &wlevel, int &wopt) {
    if (level == 1) {  // SOL_SOCKET
        wlevel = SOL_SOCKET;
        switch (opt) {
        case 2: wopt = SO_REUSEADDR; return true;
        case 3: wopt = SO_TYPE; return true;
        case 4: wopt = SO_ERROR; return true;
#ifdef __3DS__
        case 6: return false;  // SO_BROADCAST: soc:U sends broadcasts without it (libctru's 0 is no option)
#else
        case 6: wopt = SO_BROADCAST; return true;
#endif
        case 7: wopt = SO_SNDBUF; return true;
        case 8: wopt = SO_RCVBUF; return true;
        case 9: wopt = SO_KEEPALIVE; return true;
        case 13: wopt = SO_LINGER; return true;
        case 20: wopt = SO_RCVTIMEO; return true;
        case 21: wopt = SO_SNDTIMEO; return true;
        }
        return false;
    }
    if (level == 0) {  // IPPROTO_IP
        wlevel = IPPROTO_IP;
        switch (opt) {
        case 1: wopt = IP_TOS; return true;
        case 2: wopt = IP_TTL; return true;
        case 32: wopt = IP_MULTICAST_IF; return true;
        case 33: wopt = IP_MULTICAST_TTL; return true;
        case 34: wopt = IP_MULTICAST_LOOP; return true;
        case 35: wopt = IP_ADD_MEMBERSHIP; return true;
        case 36: wopt = IP_DROP_MEMBERSHIP; return true;
        }
        return false;
    }
    if (level == 6 && opt == 1) { wlevel = IPPROTO_TCP; wopt = TCP_NODELAY; return true; }
    if (level == 41 && opt == 26) { wlevel = IPPROTO_IPV6; wopt = IPV6_V6ONLY; return true; }
    return false;
}

int n_setsockopt(int fd, int level, int opt, u32 val, u32 len) {
    sock_t s = sock_of(fd);
    if (s == kNoSock) { set_errno(kEBADF); return -1; }
    int wl, wo;
    if (!map_option(level, opt, wl, wo)) return 0;  // e.g. SO_REUSEPORT, SO_NOSIGPIPE: accept
    if (wo == SO_RCVTIMEO || wo == SO_SNDTIMEO) {  // guest struct timeval (32-bit fields)
        int32_t sec = int32_t(LD32(val)), usec = int32_t(LD32(val + 4));
#ifdef _WIN32
        DWORD t = DWORD(sec) * 1000 + DWORD(usec) / 1000;  // Winsock takes milliseconds
#else
        timeval t{time_t(sec), suseconds_t(usec)};
#endif
        return ::setsockopt(s, wl, wo, reinterpret_cast<const char *>(&t), sizeof t) == 0 ? 0 : fail("setsockopt", level * 1000 + opt);
    }
    if (wo == SO_LINGER) {
        linger l{};
        l.l_onoff = decltype(l.l_onoff)(LD32(val) != 0);
        l.l_linger = decltype(l.l_linger)(LD32(val + 4));
        return ::setsockopt(s, wl, wo, reinterpret_cast<const char *>(&l), sizeof l) == 0 ? 0 : fail("setsockopt", level * 1000 + opt);
    }
    if (wl == IPPROTO_IP && (wo == IP_MULTICAST_TTL || wo == IP_MULTICAST_LOOP)) {
        int v = int(len == 1 ? LD8(val) : LD32(val));  // Linux accepts a byte or an int
        return ::setsockopt(s, wl, wo, reinterpret_cast<const char *>(&v), sizeof v) == 0 ? 0 : fail("setsockopt", level * 1000 + opt);
    }
    return ::setsockopt(s, wl, wo, ptr(val), socklen(len)) == 0 ? 0 : fail("setsockopt", level * 1000 + opt);
}
int n_getsockopt(int fd, int level, int opt, u32 val, u32 lenp) {
    sock_t s = sock_of(fd);
    if (s == kNoSock) { set_errno(kEBADF); return -1; }
    int wl, wo;
    if (!map_option(level, opt, wl, wo)) {
        if (LD32(lenp) >= 4) ST32(val, 0);
        return 0;
    }
    char buf[64] = {};
    socklen l = sizeof buf;
    if (::getsockopt(s, wl, wo, buf, &l) != 0) return fail("getsockopt", level * 1000 + opt);
    int32_t v;
    std::memcpy(&v, buf, 4);
    if (wo == SO_ERROR && v) v = linux_errno(v);
    if (LD32(lenp) >= 4) ST32(val, u32(v));
    ST32(lenp, 4);
    return 0;
}

// ---- select / poll over sockets and pipes ----------------------------------------------------------
// Polls readiness in short slices so sockets and non-socket fds can be mixed.
template <class Check>
int wait_ready(int timeout_ms, Check check) {
    int64_t deadline = timeout_ms < 0 ? INT64_MAX : os::monotonic_ns() + int64_t(timeout_ms) * 1000000;
    for (;;) {
        int n = check();
        if (n || os::monotonic_ns() >= deadline) return n;
        os::sleep_ns(1000000);
    }
}

int poll_one(int fd, int events) {
    sock_t s = sock_of(fd);
    return s != kNoSock ? socket_ready(fd, s, events) : io_poll_fd(fd, events);
}

int n_select(int nfds, u32 rd, u32 wr, u32 ex, u32 tv) {
    auto bit = [](u32 set, int fd) { return set && (LD32(set + 4 * u32(fd / 32)) >> (fd % 32)) & 1; };
    std::vector<int> want_r, want_w;
    for (int fd = 0; fd < nfds && fd < 1024; fd++) {
        if (bit(rd, fd)) want_r.push_back(fd);
        if (bit(wr, fd)) want_w.push_back(fd);
    }
    int timeout = tv ? int(int32_t(LD32(tv)) * 1000 + int32_t(LD32(tv + 4)) / 1000) : -1;
    std::vector<int> got_r, got_w;
    int n = wait_ready(timeout, [&] {
        got_r.clear();
        got_w.clear();
        for (int fd : want_r) if (poll_one(fd, 1) & 0x19) got_r.push_back(fd);
        for (int fd : want_w) if (poll_one(fd, 4) & 0x1c) got_w.push_back(fd);
        return int(got_r.size() + got_w.size());
    });
    auto rebuild = [&](u32 set, const std::vector<int> &fds) {
        if (!set) return;
        for (u32 w = 0; w < u32((nfds + 31) / 32); w++) ST32(set + 4 * w, 0);
        for (int fd : fds) ST32(set + 4 * u32(fd / 32), LD32(set + 4 * u32(fd / 32)) | (1u << (fd % 32)));
    };
    rebuild(rd, got_r);
    rebuild(wr, got_w);
    if (ex) for (u32 w = 0; w < u32((nfds + 31) / 32); w++) ST32(ex + 4 * w, 0);
    return n;
}

// struct pollfd: int fd, short events, short revents
int n_poll(u32 fds, u32 nfds, int timeout) {
    return wait_ready(timeout, [&] {
        int ready = 0;
        for (u32 i = 0; i < nfds; i++) {
            int fd = int32_t(LD32(fds + 8 * i));
            int events = LD16(fds + 8 * i + 4);
            int r = fd < 0 ? 0 : poll_one(fd, events) & (events | 0x38);
            ST16(fds + 8 * i + 6, u32(r));
            ready += r != 0;
        }
        return ready;
    });
}

// ---- name resolution: numeric addresses and localhost only ------------------------------------------
bool resolve_ipv4(const char *node, u32 &addr_be) {
    if (!node || !std::strcmp(node, "localhost")) { addr_be = htonl(INADDR_LOOPBACK); return true; }
    in_addr a;
    if (inet_pton(AF_INET, node, &a) == 1) { addr_be = a.s_addr; return true; }
    return false;
}

// bionic struct addrinfo: flags, family, socktype, protocol, addrlen, canonname, addr, next
int n_getaddrinfo(u32 node, u32 service, u32 hints, u32 res) {
    if (res) ST32(res, 0);
    const char *n = str(node);
    int flags = hints ? int(LD32(hints)) : 0;
    u32 ip = 0;
    if (!n && (flags & 1)) ip = 0;  // AI_PASSIVE: INADDR_ANY
    else if (!resolve_ipv4(n, ip)) return 8;  // EAI_NONAME: no DNS (online services are offline)
    int socktype = hints ? int(LD32(hints + 8)) : 0, protocol = hints ? int(LD32(hints + 12)) : 0;
    uint16_t port = service ? uint16_t(std::atoi(str(service))) : 0;
    u32 ai = gcalloc(1, 32 + 16);
    u32 sa = ai + 32;
    ST32(ai + 4, AF_INET);
    ST32(ai + 8, u32(socktype ? socktype : SOCK_STREAM));
    ST32(ai + 12, u32(protocol));
    ST32(ai + 16, 16);
    ST32(ai + 24, sa);
    ST16(sa, AF_INET);
    ST16(sa + 2, htons(port));
    ST32(sa + 4, ip);
    ST32(res, ai);
    return 0;
}
void n_freeaddrinfo(u32 ai) {
    while (ai) {
        u32 next = LD32(ai + 28);
        gfree(ai);
        ai = next;
    }
}
u32 n_gai_strerror(int) { static u32 s = gstrdup("name resolution unavailable"); return s; }
// struct hostent: name, aliases, addrtype, length, addr_list
u32 n_gethostbyname(u32 name) {
    u32 ip;
    if (!resolve_ipv4(str(name), ip)) return 0;
    static u32 h = gcalloc(1, 64);
    u32 addr = h + 32, list = h + 40;
    ST32(h, name);
    ST32(h + 4, h + 48);  // empty alias list
    ST32(h + 8, AF_INET);
    ST32(h + 12, 4);
    ST32(h + 16, list);
    ST32(addr, ip);
    ST32(list, addr);
    ST32(list + 4, 0);
    return h;
}
int n_getnameinfo(u32 sa, u32, u32 host, u32 hostlen, u32 serv, u32 servlen, int) {
    char buf[64];
    if (host && hostlen) {
        inet_ntop(AF_INET, ptr(sa + 4), buf, sizeof buf);
        std::strncpy(ptr(host), buf, hostlen);
    }
    if (serv && servlen) std::snprintf(ptr(serv), servlen, "%u", ntohs(uint16_t(LD16(sa + 2))));
    return 0;
}
u32 n_inet_addr(u32 cp) {
    in_addr a;
    return inet_pton(AF_INET, str(cp), &a) == 1 ? a.s_addr : 0xffffffffu;
}
u32 n_inet_ntoa(u32 in) {
    static thread_local u32 buf;
    if (!buf) buf = gmalloc(16);
    std::snprintf(ptr(buf), 16, "%u.%u.%u.%u", in & 255, (in >> 8) & 255, (in >> 16) & 255, in >> 24);
    return buf;
}
u32 n_if_nametoindex(u32) { return 0; }

}  // namespace

// ---- hooks used by libc_io.cpp for fds that are sockets -------------------------------------------------
int net_recv(int fd, uint32_t buf, uint32_t n, int flags) {
    sock_t s = sock_of(fd);
    if (s == kNoSock) { set_errno(kEBADF); return -1; }
    if (would_block(fd, s, flags, 1)) return -1;
#ifdef __SWITCH__
    if (const int r = take_pending(fd, buf, n, flags, nullptr, nullptr); r >= 0) return r;
#endif
    int r = ::recv(s, ptr(buf), int(n), host_flags(flags));
    return r < 0 ? fail("recv") : r;
}
int net_send(int fd, uint32_t buf, uint32_t n, int flags) {
    sock_t s = sock_of(fd);
    if (s == kNoSock) { set_errno(kEBADF); return -1; }
    if (would_block(fd, s, flags, 4)) return -1;
    int r = ::send(s, ptr(buf), int(n), host_flags(flags));
    return r < 0 ? fail("send") : r;
}
void net_close(int fd) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_socks.find(fd);
    if (it == g_socks.end()) return;
    sock_close(it->second);
    g_socks.erase(it);
    g_nonblock.erase(fd);
    g_dgram.erase(fd);
#ifdef __SWITCH__
    std::lock_guard<std::mutex> pending(g_pending_lock);
    g_pending.erase(fd);
#endif
}
void net_set_nonblock(int fd, bool on) {
    sock_t s = sock_of(fd);
    if (s == kNoSock) return;
    sock_set_nonblock(s, on);
    std::lock_guard<std::mutex> lock(g_lock);
    g_nonblock[fd] = on;
}
void net_set_peers(const std::string &list) {
    sock_startup();
    std::vector<uint32_t> peers;
    std::string text;
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t end = list.find_first_of(", ;", pos);
        if (end == std::string::npos) end = list.size();
        const std::string item = list.substr(pos, end - pos);
        in_addr a{};
        if (!item.empty() && inet_pton(AF_INET, item.c_str(), &a) == 1) {
            peers.push_back(a.s_addr);
            text += (text.empty() ? "" : ", ") + item;
        } else if (!item.empty()) {
            log("net: not an IPv4 address: %s", item.c_str());
        }
        pos = end + 1;
    }
    if (!peers.empty()) log("net: broadcasts also go to %s", text.c_str());
    std::lock_guard<std::mutex> lock(g_peers_lock);
    g_peers = std::move(peers);
}
int net_ioctl(int fd, uint32_t req, uint32_t arg) {
    sock_t s = sock_of(fd);
    if (s == kNoSock) { set_errno(kEBADF); return -1; }
    if (req == 0x5421) { net_set_nonblock(fd, LD32(arg) != 0); return 0; }  // FIONBIO
    if (req == 0x541B) {                                                        // FIONREAD
        uint32_t avail = 0;
#ifdef __SWITCH__
        {
            std::lock_guard<std::mutex> lock(g_pending_lock);
            if (auto it = g_pending.find(fd); it != g_pending.end()) {
                ST32(arg, uint32_t(it->second.data.size()));  // (the datagram a read returns next)
                return 0;
            }
        }
#endif
        if (sock_available(s, avail) != 0) return fail("ioctl FIONREAD");
        ST32(arg, avail);
        return 0;
    }
    static std::set<uint32_t> logged;
    std::lock_guard<std::mutex> lock(g_lock);
    if (logged.insert(req).second) log("net: ioctl 0x%x on a socket not supported", req);
    set_errno(kEINVAL);
    return -1;
}
int net_poll(int fd, int events) {
    sock_t s = sock_of(fd);
    return s == kNoSock ? 0 : socket_ready(fd, s, events);
}

RT_IMPORT(socket, n_socket)
RT_IMPORT(bind, n_bind)
RT_IMPORT(connect, n_connect)
RT_IMPORT(listen, n_listen)
RT_IMPORT(accept, n_accept)
RT_IMPORT(send, n_send)
RT_IMPORT(recv, n_recv)
RT_IMPORT(sendto, n_sendto)
RT_IMPORT(recvfrom, n_recvfrom)
RT_IMPORT(sendmsg, n_sendmsg)
RT_IMPORT(recvmsg, n_recvmsg)
RT_IMPORT(shutdown, n_shutdown)
RT_IMPORT(getsockname, n_getsockname)
RT_IMPORT(getpeername, n_getpeername)
RT_IMPORT(setsockopt, n_setsockopt)
RT_IMPORT(getsockopt, n_getsockopt)
RT_IMPORT(select, n_select)
RT_IMPORT(poll, n_poll)
RT_IMPORT(getaddrinfo, n_getaddrinfo)
RT_IMPORT(freeaddrinfo, n_freeaddrinfo)
RT_IMPORT(gai_strerror, n_gai_strerror)
RT_IMPORT(gethostbyname, n_gethostbyname)
RT_IMPORT(getnameinfo, n_getnameinfo)
RT_IMPORT(inet_addr, n_inet_addr)
RT_IMPORT(inet_ntoa, n_inet_ntoa)
RT_IMPORT(if_nametoindex, n_if_nametoindex)

}  // namespace rt
