// The runtime's OS layer on the GameCube (libogc2; docs/gamecube.md).
//  * Memory: guest addresses are host addresses (g_mem = 0); committing guest memory maps zero
//    pages in the pager (gc_pager.c), which the platform's main sets up with its frames.
//  * Threads: libogc's LWP threads. Their stacks must be in MEM1 (libogc's exception entry
//    stores the registers at the stack pointer's physical address, so a paged stack cannot
//    take a page fault), which bounds them: the translated code nests one host frame per guest
//    call. A thread that ends is freed by the next thread start or the watchdog.
//  * Time: the time base (40.5 MHz) for clocks, newlib's time for the calendar.
#include "os.h"
#include "runtime.h"
#include "tls.h"

extern "C" {
#include "gc_dvdfs.h"
#include "gc_pager.h"
#include "gc_mprof.h"
#include "gc_report.h"
}

#include <gccore.h>
#include <ogc/lwp_objmgr.h>
#include <ogc/lwp_watchdog.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <exception>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <malloc.h>
#include <mutex>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace rt {
// Thread-local variables (tls.h): one value per LWP thread slot; a thread starting in a slot gets
// fresh values.
namespace {
struct SlotReset { void (*reset)(void *, int); void *self; };
SlotReset g_slot_resets[64];
std::atomic<int> g_slot_reset_count{0};
}  // namespace

int thread_slot() { return int(LWP_OBJMASKID(LWP_GetSelf())) % kThreadSlots; }

void register_thread_slots(void (*reset)(void *, int), void *self) {
    const int i = g_slot_reset_count++;
    if (i >= int(sizeof g_slot_resets / sizeof g_slot_resets[0])) fatal("too many thread-local variables (tls.h)");
    g_slot_resets[i] = {reset, self};
}

static void reset_thread_slots(int slot) {
    for (int i = 0, n = g_slot_reset_count; i < n; i++) g_slot_resets[i].reset(g_slot_resets[i].self, slot);
}
}  // namespace rt

namespace rt::os {

void *reserve(size_t) { return nullptr; }  // not used: there is no window to reserve (runtime.cpp)

bool commit(void *p, size_t size) {
    pager_map_zero(uint32_t(reinterpret_cast<uintptr_t>(p)), uint32_t(size));
    return true;
}

void discard(void *p, size_t size) { pager_discard(uint32_t(reinterpret_cast<uintptr_t>(p)), uint32_t(size)); }

namespace {
// A file mapped read-only (map_file): pages come from the disc in 64 KB blocks, four kept, so
// code and data read in order cost one disc read per sixteen pages. The pager calls the reader
// from inside a fault, with interrupts on (gc_pager.h).
constexpr uint32_t kFileBlock = 64u << 10, kFileBlocks = 4;
struct MappedFile {
    uint32_t disc_offset = 0, size = 0;  // (disc_offset: the disc's files)
    int fd = -1;                          // other files (the SD card's): read through their device
    uint32_t file_offset = 0;
    uint8_t *block[kFileBlocks] = {};
    uint32_t at[kFileBlocks] = {~0u, ~0u, ~0u, ~0u};
    uint32_t next = 0;
    mutex_t lock = LWP_MUTEX_NULL;
};

void file_page(void *ctx, uint32_t offset, void *page) {
    auto *m = static_cast<MappedFile *>(ctx);
    LWP_MutexLock(m->lock);
    const uint32_t block = offset & ~(kFileBlock - 1);
    int k = -1;
    for (uint32_t i = 0; i < kFileBlocks; i++)
        if (m->at[i] == block) k = int(i);
    if (k < 0) {
        k = int(m->next);
        m->next = (m->next + 1) % kFileBlocks;
        m->at[k] = ~0u;
        const uint32_t n = block < m->size ? std::min(kFileBlock, m->size - block) : 0;
        bool ok;
        if (m->fd < 0) {
            ok = gc_dvdfs_read_raw(m->block[k], n, m->disc_offset + block);
        } else {
            // (inside a fault, interrupts on: this thread never holds the card's locks while it
            // touches a source page, see docs/gamecube.md)
            ok = lseek(m->fd, off_t(m->file_offset + block), SEEK_SET) >= 0 &&
                 read(m->fd, m->block[k], n) == ssize_t(n);
        }
        if (n && !ok) {
            std::printf("gc: FAIL: a mapped file's read at %08x\n", unsigned(offset));
            std::memset(page, 0, 4096);
            LWP_MutexUnlock(m->lock);
            return;
        }
        if (n < kFileBlock) std::memset(m->block[k] + n, 0, kFileBlock - n);
        m->at[k] = block;
    }
    std::memcpy(page, m->block[k] + (offset - block), 4096);
    LWP_MutexUnlock(m->lock);
}
}  // namespace

// The disc's files ("dvd:") are read by disc offset, others (the SD card's) through their device.
bool map_file(uint32_t va, uint32_t size, const char *path, uint64_t offset, bool exec) {
    uint32_t file_size = 0, at = 0;
    int fd = -1;
    if (std::strncmp(path, "dvd:", 4) == 0) {
        if (!(at = gc_dvdfs_disc_offset(path, &file_size))) return false;
    } else {
        struct stat st;
        if ((fd = open(path, O_RDONLY)) < 0) return false;
        if (fstat(fd, &st) != 0) {
            close(fd);
            return false;
        }
        file_size = uint32_t(st.st_size);
    }
    if ((va & 0xfffu) != (offset & 0xfffu) || offset > file_size) {
        if (fd >= 0) close(fd);
        return false;
    }
    auto *m = new MappedFile;
    m->fd = fd;
    m->file_offset = uint32_t(offset & ~0xfffull);
    m->disc_offset = at + uint32_t(offset & ~0xfffull);
    m->size = file_size - uint32_t(offset & ~0xfffull);
    for (auto &b : m->block) b = static_cast<uint8_t *>(memalign(32, kFileBlock));
    LWP_MutexInit(&m->lock, false);
    pager_map_source(va & ~0xfffu, size + (va & 0xfffu), exec, file_page, m);
    rt::log("map: %s +%08x -> %08x (%u KB)%s", path, unsigned(offset), unsigned(va), unsigned(size >> 10), exec ? ", code" : "");
    return true;
}

namespace {
constexpr uint64_t kTicksPerSecond = 40500000;  // bus clock 162 MHz / 4
}

int64_t monotonic_ns() {
    const uint64_t t = ticks();
    return int64_t(t / kTicksPerSecond * 1000000000 + t % kTicksPerSecond * 1000000000 / kTicksPerSecond);
}

int64_t realtime_ns() {
    static const int64_t base = [] {
        timeval tv{};
        gettimeofday(&tv, nullptr);
        return int64_t(tv.tv_sec) * 1000000000 + int64_t(tv.tv_usec) * 1000 - monotonic_ns();
    }();
    return base + monotonic_ns();
}

void sleep_ns(int64_t ns) {
    if (ns <= 0) LWP_YieldThread();
    else usleep(useconds_t((ns + 999) / 1000));
}

void yield() { LWP_YieldThread(); }

// the audio mixer: above the game's threads (LWP_PRIO_NORMAL), below the service threads (start_background)
void raise_thread_priority() { LWP_SetThreadPriority(LWP_THREAD_NULL, LWP_PRIO_NORMAL + 4); }

int64_t process_cpu_ns() { return monotonic_ns(); }  // one process, one core: only for statistics

bool local_time(int64_t t, struct tm &out, int32_t &utc_offset) {
    const time_t s = time_t(t / 1000000000);
    utc_offset = 0;  // the console's clock is local time
    return gmtime_r(&s, &out) != nullptr;
}

bool utc_time(int64_t t, struct tm &out) {
    const time_t s = time_t(t / 1000000000);
    return gmtime_r(&s, &out) != nullptr;
}

uint64_t thread_id() { return uint64_t(LWP_GetSelf()); }
uint32_t process_id() { return 1; }

namespace {
// Host stacks: the first thread started is the game thread (the deepest guest call chains);
// the others the guest's threads and services. Sizes to be checked against the high-water
// marks the watchdog reports.
// (measured deepest use in the menus: the game thread 71 KB, the others 4 KB; the watchdog
// reports each thread's every 20 s)
constexpr size_t kGameStack = 256u << 10, kThreadStack = 64u << 10, kServiceStack = 32u << 10;
constexpr uint32_t kStackPaint = 0x57ac57acu;

struct Thread {
    lwp_t handle = LWP_THREAD_NULL;
    void (*fn)(void *) = nullptr;
    void *arg = nullptr;
    uint8_t *stack = nullptr;
    size_t size = 0;
    std::atomic<bool> ended{false};
};

std::mutex g_threads_lock;
std::vector<Thread *> g_threads;
std::atomic<int> g_started{0};

void reap_ended() {
    std::vector<Thread *> done;
    {
        std::lock_guard<std::mutex> lock(g_threads_lock);
        for (auto it = g_threads.begin(); it != g_threads.end();)
            if ((*it)->ended) done.push_back(*it), it = g_threads.erase(it);
            else ++it;
    }
    for (Thread *t : done) {
        LWP_JoinThread(t->handle, nullptr);  // past its last line: this waits for its exit
        std::free(t->stack);
        delete t;
    }
}

void *entry(void *p) {
    auto *t = static_cast<Thread *>(p);
    rt::reset_thread_slots(rt::thread_slot());
    t->fn(t->arg);
    t->ended = true;
    return nullptr;
}

bool create(void (*fn)(void *), void *arg, size_t stack, uint8_t prio) {
    reap_ended();
    auto *t = new Thread;
    t->fn = fn, t->arg = arg, t->size = stack;
    t->stack = static_cast<uint8_t *>(memalign(32, stack));
    if (!t->stack) {
        delete t;
        return false;
    }
    for (size_t i = 0; i < stack; i += 4) std::memcpy(t->stack + i, &kStackPaint, 4);
    {
        std::lock_guard<std::mutex> lock(g_threads_lock);
        g_threads.push_back(t);
    }
    if (LWP_CreateThread(&t->handle, entry, t, t->stack, u32(stack), prio) != 0) {
        std::lock_guard<std::mutex> lock(g_threads_lock);
        g_threads.pop_back();
        std::free(t->stack);
        delete t;
        return false;
    }
    return true;
}

// The deepest use of each live thread's stack (the painted words still intact from its base).
void log_stack_use() {
    std::lock_guard<std::mutex> lock(g_threads_lock);
    for (Thread *t : g_threads) {
        size_t untouched = 16;  // (libogc marks the base)
        while (untouched + 4 <= t->size && std::memcmp(t->stack + untouched, &kStackPaint, 4) == 0) untouched += 4;
        rt::log("thread %08x: stack %u KB, deepest use %u KB", unsigned(t->handle), unsigned(t->size >> 10),
                unsigned((t->size - untouched) >> 10));
    }
}
}  // namespace

bool start_thread(void (*fn)(void *), void *arg, size_t) {
    const bool game = g_started++ == 0;
    return create(fn, arg, game ? kGameStack : kThreadStack, LWP_PRIO_NORMAL);
}

// Service threads (the log writer, the watchdog, the audio feeder) above the game's: LWP runs the
// highest ready priority only, so below a busy game thread they would never run; they sleep most
// of the time.
void start_background(void (*fn)(void *), void *arg) { create(fn, arg, kServiceStack, LWP_PRIO_NORMAL + 8); }

void exit_process(int code) {
    rt::log_flush();
    std::fflush(stdout);
    std::printf("gc: exit %d\n", code);
    std::fflush(stdout);
    std::_Exit(code);
}


// A fault the pager cannot resolve: the guest context and the address, then libogc's handler
// (which shows its report on screen and restarts).
namespace {
// (stderr: also from inside the page fault handler)
void log_pager() {
    PagerStats s;
    pager_stats(&s);
    std::fprintf(stderr,
                 "pager: %u/%u frames, faults data %u code %u, fills zero %u aram %u source %u, writebacks %u, "
                 "zeros dropped %u; ARAM %u/%u KB, %u pages (compressed to %u%%); %u ms in faults\n",
                 s.frames_used, s.frames, s.faults_data, s.faults_code, s.fills_zero, s.fills_aram, s.fills_source,
                 s.writebacks, s.zero_dropped, s.aram_used / 4, s.aram_sectors / 4, s.aram_pages,
                 unsigned(s.aram_bytes_in ? s.aram_bytes_out * 100 / s.aram_bytes_in : 0),
                 unsigned(ticks_to_millisecs(s.fault_ticks)));
}
void log_host_heap(const char *when) {
    const struct mallinfo mi = mallinfo();
    std::fprintf(stderr, "host heap (%s): %u KB in use, %u KB from the system\n", when, unsigned(mi.uordblks >> 10),
                 unsigned(mi.arena >> 10));
}
[[noreturn]] void die(const char *why) {
    rt::log_flush();
    std::fprintf(stderr, "\n=== %s ===\n", why);
    log_host_heap("at the end");
    gc_mprof_dump("at the end", 40);
    print_current_stack();
    std::fflush(stderr);
    gc_report_flush_now();
    std::_Exit(3);
}
}  // namespace

void install_crash_handler() {
    std::set_terminate([] {
        const char *what = "terminate";
        try {
            if (std::exception_ptr e = std::current_exception()) std::rethrow_exception(e);
        } catch (const std::bad_alloc &) {
            what = "terminate: out of host memory (std::bad_alloc)";
        } catch (const std::exception &e) {
            static char buf[200];
            std::snprintf(buf, sizeof buf, "terminate: %s", e.what());
            what = buf;
        } catch (...) {
        }
        die(what);
    });
    std::signal(SIGABRT, [](int) { die("abort"); });
    pager_set_report([](uint32_t addr, int store, int exec, uint32_t pc) {
        rt::log_flush();
        std::fprintf(stderr, "\n=== unresolved %s fault at %08x (instruction at %08x) ===\n",
                     exec ? "instruction" : store ? "write" : "read", unsigned(addr), unsigned(pc));
        rt::report_fault(exec ? "instruction fetch" : "data access", reinterpret_cast<void *>(uintptr_t(addr)), exec ? -1 : store);
        log_pager();
        print_current_stack();
        std::fflush(stderr);
        gc_report_flush_now();
    });
    pager_set_full([] {
        rt::log_flush();
        std::fprintf(stderr, "\n=== the pager is out of ARAM (the guest's memory is larger than MEM1 + ARAM hold) ===\n");
        log_pager();
        print_current_stack();
        std::fflush(stderr);
        gc_report_flush_now();
    });
}

void print_current_stack() {
    uint32_t *sp;
    __asm__ volatile("mr %0, 1" : "=r"(sp));
    std::fprintf(stderr, "host stack (return addresses, for powerpc-eabi-addr2line):");
    for (int i = 0; i < 32 && sp && reinterpret_cast<uintptr_t>(sp) >= 0x80000000u; i++) {
        uint32_t *next = reinterpret_cast<uint32_t *>(sp[0]);
        if (!next) break;
        std::fprintf(stderr, " %08x", unsigned(next[1]));
        sp = next;
    }
    std::fprintf(stderr, "\n");
}

size_t capture_stack(void **, size_t) { return 0; }
bool host_symbol(const void *, char *, size_t) { return false; }
void working_set_frame(uint64_t) {}
bool working_set_on() { return false; }

void start_watchdog(int seconds) {
    struct Args { int seconds; };
    start_background([](void *p) {
        const int seconds = static_cast<Args *>(p)->seconds;
        uint64_t last = rt::watchdog_frames();
        int still = 0, n = 0;
        for (;;) {
            sleep(1);
            reap_ended();
            gc_report_flush();
            if (++n % 20 == 0) {
                log_pager();
                log_stack_use();
                const struct mallinfo mi = mallinfo();
                rt::log("host heap: %u KB in use, %u KB from the system", unsigned(mi.uordblks >> 10), unsigned(mi.arena >> 10));
            }
            const uint64_t now = rt::watchdog_frames();
            still = now == last ? still + 1 : 0;
            last = now;
            if (still == seconds) {
                rt::log("watchdog: no frame for %d s (frame %llu)", seconds, (unsigned long long)now);
                log_stack_use();
                rt::log_flush();
            }
        }
    }, new Args{seconds});
}

int64_t thread_cpu_ns() { return -1; }
void start_profiler() {}
void report_profiler() {}
void start_profiler_continuous(int) {}
void disk_space(const char *, uint64_t &free_bytes, uint64_t &total_bytes) { free_bytes = total_bytes = 0; }

}  // namespace rt::os
