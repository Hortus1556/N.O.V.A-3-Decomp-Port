// Sockets on a system without networking (the GameCube): every bionic socket call the game
// imports exists and fails as on a device with no network (socket() -> EAFNOSUPPORT, no names
// resolve), while select() and poll() still wait on the guest's other descriptors (pipes,
// files) as runtime/net.cpp does. The online services are removed anyway (docs/offline.md);
// local multiplayer needs net.cpp's real sockets.
#include "os.h"
#include "runtime.h"

#include <cstdio>
#include <vector>

namespace rt {
namespace {

using u32 = uint32_t;
constexpr int kEafnosupport = 97, kEnotsock = 88;

int fail(int err) {
    set_errno(err);
    return -1;
}

template <class Check>
int wait_ready(int timeout_ms, Check check) {
    const int64_t deadline = timeout_ms < 0 ? INT64_MAX : os::monotonic_ns() + int64_t(timeout_ms) * 1000000;
    for (;;) {
        const int n = check();
        if (n || os::monotonic_ns() >= deadline) return n;
        os::sleep_ns(1000000);
    }
}

int n_socket(int, int, int) { return fail(kEafnosupport); }
int n_not_socket_i(int) { return fail(kEnotsock); }
int n_bind(int, u32, u32) { return fail(kEnotsock); }
int n_connect(int, u32, u32) { return fail(kEnotsock); }
int n_listen(int, int) { return fail(kEnotsock); }
int n_accept(int, u32, u32) { return fail(kEnotsock); }
int n_send(int, u32, u32, int) { return fail(kEnotsock); }
int n_recv(int, u32, u32, int) { return fail(kEnotsock); }
int n_sendto(Cpu *cpu) { (void)cpu; return fail(kEnotsock); }
int n_recvfrom(Cpu *cpu) { (void)cpu; return fail(kEnotsock); }
int n_msg(int, u32, int) { return fail(kEnotsock); }
int n_shutdown(int, int) { return fail(kEnotsock); }
int n_name(int, u32, u32) { return fail(kEnotsock); }
int n_setsockopt(Cpu *cpu) { (void)cpu; return fail(kEnotsock); }
int n_getsockopt(Cpu *cpu) { (void)cpu; return fail(kEnotsock); }

int n_select(int nfds, u32 rd, u32 wr, u32 ex, u32 tv) {
    auto bit = [](u32 set, int fd) { return set && (LD32(set + 4 * u32(fd / 32)) >> (fd % 32)) & 1; };
    std::vector<int> want_r, want_w, got_r, got_w;
    for (int fd = 0; fd < nfds && fd < 1024; fd++) {
        if (bit(rd, fd)) want_r.push_back(fd);
        if (bit(wr, fd)) want_w.push_back(fd);
    }
    const int timeout = tv ? int(int32_t(LD32(tv)) * 1000 + int32_t(LD32(tv + 4)) / 1000) : -1;
    const int n = wait_ready(timeout, [&] {
        got_r.clear();
        got_w.clear();
        for (int fd : want_r) if (io_poll_fd(fd, 1) & 1) got_r.push_back(fd);
        for (int fd : want_w) if (io_poll_fd(fd, 4) & 4) got_w.push_back(fd);
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
            const int fd = int32_t(LD32(fds + 8 * i));
            const int events = LD16(fds + 8 * i + 4);
            const int r = fd < 0 ? 0 : io_poll_fd(fd, events) & (events | 0x38);
            ST16(fds + 8 * i + 6, u32(r));
            ready += r != 0;
        }
        return ready;
    });
}

int n_getaddrinfo(u32, u32, u32, u32 res) {
    if (res) ST32(res, 0);
    return 8;  // EAI_NONAME
}
void n_freeaddrinfo(u32) {}
u32 n_gai_strerror(int) {
    static const u32 msg = gstrdup("no network");
    return msg;
}
u32 n_gethostbyname(u32) { return 0; }
int n_getnameinfo(u32, u32, u32, u32, u32, u32, int) { return 8; }
u32 n_inet_addr(u32 cp) {
    unsigned a, b, c, d;
    if (std::sscanf(str(cp), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0xffffffffu;
    return a | b << 8 | c << 16 | d << 24;  // network order in the guest's memory
}
u32 n_inet_ntoa(u32 in) {
    static u32 buf;
    if (!buf) buf = gmalloc(16);
    std::snprintf(ptr(buf), 16, "%u.%u.%u.%u", in & 255, (in >> 8) & 255, (in >> 16) & 255, in >> 24);
    return buf;
}
u32 n_if_nametoindex(u32) { return 0; }

}  // namespace

// libc_io.cpp's hooks for socket descriptors: there are none
int net_recv(int, uint32_t, uint32_t, int) { return fail(kEnotsock); }
int net_send(int, uint32_t, uint32_t, int) { return fail(kEnotsock); }
void net_close(int) {}
void net_set_nonblock(int, bool) {}
void net_set_peers(const std::string &) {}
int net_ioctl(int, uint32_t, uint32_t) { return fail(kEnotsock); }
int net_poll(int, int) { return 0x20; }  // POLLNVAL

RT_IMPORT(socket, n_socket)
RT_IMPORT(bind, n_bind)
RT_IMPORT(connect, n_connect)
RT_IMPORT(listen, n_listen)
RT_IMPORT(accept, n_accept)
RT_IMPORT(send, n_send)
RT_IMPORT(recv, n_recv)
RT_IMPORT(sendto, n_sendto)
RT_IMPORT(recvfrom, n_recvfrom)
RT_IMPORT(sendmsg, n_msg)
RT_IMPORT(recvmsg, n_msg)
RT_IMPORT(shutdown, n_shutdown)
RT_IMPORT(getsockname, n_name)
RT_IMPORT(getpeername, n_name)
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
