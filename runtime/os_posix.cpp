// rt::os on POSIX (Linux, macOS): mmap, clock_gettime, pthreads, signal-based crash reports.
// Stack frames are named with dladdr, so the executable is linked with -rdynamic (the
// recompiled f_XXXXXXXX functions then show up as guest functions in reports).
#include "os.h"
#include "runtime.h"

#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rt::os {

void *reserve(size_t size) {
    void *p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}
bool commit(void *p, size_t size) { return mprotect(p, size, PROT_READ | PROT_WRITE) == 0; }

static int64_t clock_ns(clockid_t id) {
    timespec ts;
    clock_gettime(id, &ts);
    return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}
int64_t monotonic_ns() { return clock_ns(CLOCK_MONOTONIC); }
int64_t realtime_ns() { return clock_ns(CLOCK_REALTIME); }
void sleep_ns(int64_t ns) {
    if (ns <= 0) { sched_yield(); return; }
    timespec req{time_t(ns / 1000000000), long(ns % 1000000000)}, rem;
    while (nanosleep(&req, &rem) != 0 && errno == EINTR) req = rem;
}
void yield() { sched_yield(); }
void raise_thread_priority() {}  // needs privileges (real-time policies) on Linux and macOS
uint32_t process_id() { return uint32_t(getpid()); }
int64_t process_cpu_ns() { return clock_ns(CLOCK_PROCESS_CPUTIME_ID); }
bool local_time(int64_t t, struct tm &out, int32_t &utc_offset) {
    time_t tt = time_t(t);
    if (!localtime_r(&tt, &out)) return false;
    utc_offset = int32_t(out.tm_gmtoff);
    return true;
}
bool utc_time(int64_t t, struct tm &out) {
    time_t tt = time_t(t);
    return gmtime_r(&tt, &out) != nullptr;
}
uint64_t thread_id() {
#if defined(__linux__)
    return uint64_t(syscall(SYS_gettid));
#elif defined(__APPLE__)
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return id;
#else
    return uint64_t(uintptr_t(pthread_self()));
#endif
}

// ---- threads ----------------------------------------------------------------------------
namespace {
// Signal handlers run on an alternate stack so stack overflows can be reported too.
void install_alt_stack() {
    static thread_local bool done;
    if (done) return;
    done = true;
    stack_t ss{};
    ss.ss_size = 256 << 10;
    ss.ss_sp = std::malloc(ss.ss_size);
    if (ss.ss_sp) sigaltstack(&ss, nullptr);
}
struct Start { void (*fn)(void *); void *arg; };
void *trampoline(void *p) {
    Start s = *static_cast<Start *>(p);
    delete static_cast<Start *>(p);
    install_alt_stack();
    s.fn(s.arg);
    return nullptr;
}
}  // namespace

bool start_thread(void (*fn)(void *), void *arg, size_t stack_reserve) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_reserve);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    auto *start = new Start{fn, arg};
    pthread_t t;
    int r = pthread_create(&t, &attr, trampoline, start);
    pthread_attr_destroy(&attr);
    if (r != 0) { delete start; return false; }
    return true;
}

void exit_process(int code) {
    pgo_flush();
    log_flush();  // the asynchronous log first
    std::fflush(nullptr);
    _exit(code);
}

void disk_space(const char *path, uint64_t &free_bytes, uint64_t &total_bytes) {
    struct statvfs s;
    if (statvfs(path, &s) != 0) { free_bytes = total_bytes = 0; return; }
    free_bytes = uint64_t(s.f_bavail) * s.f_frsize;
    total_bytes = uint64_t(s.f_blocks) * s.f_frsize;
}

// ---- crash reports ------------------------------------------------------------------------
namespace {

void print_stack(int skip) {
    std::fprintf(stderr, "call stack (innermost first):\n");
    void *pcs[80];
    int n = backtrace(pcs, 80);
    for (int i = skip; i < n; i++) {
        Dl_info info{};
        void *pc = static_cast<char *>(pcs[i]) - (i > skip ? 1 : 0);
        print_frame(i - skip, dladdr(pc, &info) && info.dli_sname ? info.dli_sname : "?", nullptr, 0);
    }
}

void crash_signal(int sig, siginfo_t *si, void *) {
    const char *what = sig == SIGSEGV ? "segmentation fault" : sig == SIGBUS ? "bus error"
                       : sig == SIGILL ? "illegal instruction" : sig == SIGFPE ? "arithmetic exception" : "signal";
    log_flush();
    report_lock().lock();
    std::fprintf(stderr, "\n=== [thread %llu] host signal %d (%s) ===\n", (unsigned long long)thread_id(), sig, what);
    bool addr = sig == SIGSEGV || sig == SIGBUS;
    report_fault(what, addr ? si->si_addr : nullptr, -1);
    print_stack(1);
    std::fflush(stderr);
    _exit(3);
}

std::atomic<bool> g_watchdog_dump{false};
void watchdog_signal(int) {
    if (!g_watchdog_dump.exchange(false)) return;
    print_stack(1);
    std::fflush(stderr);
}

}  // namespace

void install_crash_handler() {
    install_alt_stack();
    struct sigaction sa{};
    sa.sa_sigaction = crash_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) sigaction(sig, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);  // writes to closed sockets return EPIPE instead
}

void print_current_stack() { print_stack(1); }
bool map_file(uint32_t, uint32_t, const char *, uint64_t, bool) { return false; }
void discard(void *, size_t) {}
size_t capture_stack(void **, size_t) { return 0; }
bool host_symbol(const void *, char *, size_t) { return false; }
void working_set_frame(uint64_t) {}
bool working_set_on() { return false; }

void start_watchdog(int seconds) {
    struct sigaction sa{};
    sa.sa_handler = watchdog_signal;
    sa.sa_flags = SA_ONSTACK | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, nullptr);
    struct Args { pthread_t target; int seconds; };
    auto *args = new Args{pthread_self(), seconds};
    pthread_t t;
    pthread_create(&t, nullptr, [](void *p) -> void * {
        auto *a = static_cast<Args *>(p);
        uint64_t last = 0;
        int stalled = 0;
        for (;;) {
            sleep(1);
            uint64_t now = watchdog_frames();
            stalled = now == last ? stalled + 1 : 0;
            last = now;
            if (stalled != a->seconds) continue;
            log_flush();
            std::lock_guard<std::mutex> lock(report_lock());
            std::fprintf(stderr, "\n=== watchdog: no frame for %d s; game thread stack ===\n", a->seconds);
            g_watchdog_dump = true;
            pthread_kill(a->target, SIGUSR2);  // the target prints its own stack
            for (int i = 0; i < 100 && g_watchdog_dump; i++) usleep(10000);
        }
    }, args);
    pthread_detach(t);
}

}  // namespace rt::os

namespace rt::os {
int64_t thread_cpu_ns() {
    timespec ts;
    return clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0 ? int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec : -1;
}
void start_profiler() {}
void start_profiler_continuous(int) {}
void report_profiler() {}
}  // namespace rt::os

namespace rt::os {
void start_background(void (*fn)(void *), void *arg) {
    struct Start { void (*fn)(void *); void *arg; };
    pthread_t t;
    if (pthread_create(&t, nullptr, [](void *p) -> void * {
            const Start s = *static_cast<Start *>(p);
            delete static_cast<Start *>(p);
            s.fn(s.arg);
            return nullptr;
        }, new Start{fn, arg}) == 0)
        pthread_detach(t);
}
}  // namespace rt::os
