// rt::os on the Nintendo Switch (homebrew: libnx, devkitA64).
//
// Guest memory: the 32-bit guest address space is a 4 GB range of the process's alias region
// (unused by libnx), with physical memory mapped into it as the runtime commits pages
// (svcMapPhysicalMemory, the call applications use for growing memory; it maps whatever is
// still unmapped in the range). Homebrew gets enough of it only when started with the full
// memory of an application (holding R while launching a game), not from the album applet.
//
// Thread stacks are committed memory here (no demand paging), so the large reservations the
// runtime asks for (256 MB per guest thread, 512 MB for the game thread: on a PC only the
// touched pages count) are cut to a sixteenth.
//
// Crash reports: libnx's exception handler (CPU exceptions of any thread) prints the fault
// with the guest context; host stacks are not walked (no unwinder), the host PC / LR are
// printed for addr2line on the .elf.
#include "os.h"
#include "runtime.h"

#include <switch.h>

#include <malloc.h>
#include <pthread.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <typeinfo>
#include <vector>

namespace rt::os {

void *reserve(size_t size) {
    u64 base = 0, region = 0;
    if (R_FAILED(svcGetInfo(&base, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0)) ||
        R_FAILED(svcGetInfo(&region, InfoType_AliasRegionSize, CUR_PROCESS_HANDLE, 0)) || region < size)
        return nullptr;
    // the top of the region, 4 GB aligned (the address computations do not care, it reads well)
    u64 at = (base + region - size) & ~((u64(1) << 32) - 1);
    if (at < base) at = base;
    return reinterpret_cast<void *>(at);
}
// Memory budget: libnx would take all of the application's memory as its heap at start, and
// svcMapPhysicalMemory takes from the same pool, so the heap (libstdc++, SDL, Mesa's GPU
// buffers: textures and geometry, ~1.1 GB at most in the campaign) leaves kGuestBudget to
// the guest (it committed 200 MB at most). The application's memory counts as at most a
// retail console's 3285 MB, so emulators offering more test the real limits.
constexpr u64 kGuestBudget = u64(768) << 20;
constexpr u64 kRetailAppMemory = u64(3285) << 20;
std::atomic<u64> g_committed{0};

bool commit(void *p, size_t size) {
    const uintptr_t lo = uintptr_t(p) & ~uintptr_t(0xfff);
    const uintptr_t hi = (uintptr_t(p) + size + 0xfff) & ~uintptr_t(0xfff);
    // maps whatever is unmapped in the range: count only that (QueryMemory per block)
    u64 fresh = 0;
    for (uintptr_t a = lo; a < hi;) {
        MemoryInfo mi{};
        u32 pi = 0;
        if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
        const uintptr_t end = std::min<uintptr_t>(hi, mi.addr + mi.size);
        if (mi.type == MemType_Unmapped) fresh += end - a;
        a = end;
    }
    if (R_FAILED(svcMapPhysicalMemory(reinterpret_cast<void *>(lo), hi - lo))) {
        u64 resource = 0;
        svcGetInfo(&resource, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
        if (!resource)  // the NPDM's system resource: the installed title has one; an .nro may not
            rt::fatal("this process cannot map memory for the game (no system resource): install the .nsp, "
                      "or start the .nro over a game that allows it");
        rt::log("guest memory: mapping %llu KB failed with %llu MB committed", (unsigned long long)(fresh >> 10),
                (unsigned long long)(g_committed.load() >> 20));
        return false;
    }
    const u64 before = g_committed.fetch_add(fresh), after = before + fresh;
    if ((before >> 26) != (after >> 26)) rt::log("guest memory: %llu MB committed", (unsigned long long)(after >> 20));
    return true;
}

int64_t monotonic_ns() { return int64_t(armTicksToNs(armGetSystemTick())); }
int64_t realtime_ns() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}
void sleep_ns(int64_t ns) { svcSleepThread(ns > 0 ? ns : 0); }  // 0: yield to threads of the same priority
// YieldToAnyThread: lets lower-priority threads run too (a raised thread spinning on a lock held
// by a normal one would otherwise starve it on their core: no time slices)
void yield() { svcSleepThread(YieldType_ToAnyThread); }
// Vox's mixer: just above the game's threads (0x2c), so loaders and workers sharing its core
// (never the game thread's, see trampoline) cannot starve it; it sleeps between chunks.
void raise_thread_priority() { svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2b); }
uint32_t process_id() {
    u64 pid = 0;
    svcGetProcessId(&pid, CUR_PROCESS_HANDLE);
    return uint32_t(pid);
}
int64_t process_cpu_ns() { return monotonic_ns(); }  // no per-process CPU time; only used for statistics
bool local_time(int64_t t, struct tm &out, int32_t &utc_offset) {
    time_t tt = time_t(t);
    struct tm utc;
    if (!localtime_r(&tt, &out) || !gmtime_r(&tt, &utc)) return false;
    utc_offset = int32_t(mktime(&out) - mktime(&utc));  // newlib's struct tm has no tm_gmtoff
    return true;
}
bool utc_time(int64_t t, struct tm &out) {
    time_t tt = time_t(t);
    return gmtime_r(&tt, &out) != nullptr;
}
uint64_t thread_id() { return uint64_t(uintptr_t(threadGetSelf())); }

// ---- threads ----------------------------------------------------------------------------
namespace {
struct Start { void (*fn)(void *); void *arg; };
// Cores: libnx starts every thread on the process's default core (0), and Horizon schedules by
// strict priority without time slices, so a busy thread starves every other thread of its
// priority on that core. The game thread (the first one started) gets core 1 to itself: no
// loader or worker can hold it up, and it holds none of them up. The guest's other threads
// alternate between cores 2 and 0 (core 0 also has the main thread, which only waits, the
// audio feeder, the log writer and the watchdog).
std::atomic<int> g_started{0};
// libnx has no pthread_detach (it returns ENOSYS): a thread's stack (committed memory, 16 MB
// here) is freed only by pthread_join. So threads end by queueing themselves, and the next
// start_thread joins whatever has ended. (The game's web services start threads every few
// seconds and never join some of them: their stacks filled the heap within an hour.)
std::mutex g_ended_lock;
std::vector<pthread_t> g_ended;
void *trampoline(void *p) {
    Start s = *static_cast<Start *>(p);
    delete static_cast<Start *>(p);
    const int n = g_started++;
    if (n == 0) svcSetThreadCoreMask(CUR_THREAD_HANDLE, 1, 1u << 1);
    else svcSetThreadCoreMask(CUR_THREAD_HANDLE, n % 2 ? 2 : 0, (1u << 0) | (1u << 2));
    s.fn(s.arg);
    std::lock_guard<std::mutex> lock(g_ended_lock);
    g_ended.push_back(pthread_self());
    return nullptr;
}
}  // namespace

bool start_thread(void (*fn)(void *), void *arg, size_t stack_reserve) {
    std::vector<pthread_t> ended;
    {
        std::lock_guard<std::mutex> lock(g_ended_lock);
        ended.swap(g_ended);
    }
    for (pthread_t t : ended) pthread_join(t, nullptr);  // they have returned: no wait
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, std::max<size_t>(stack_reserve / 16, size_t(1) << 20));
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
    // the log stays open all along: its last writes reach the SD card only once the handle is
    // flushed (svcExitProcess does not close it)
    fsync(fileno(stderr));
    // The guest threads are still going: no static destructors, and for the installed title not
    // even libnx's service teardown (closing the SD card under a thread that is writing to it
    // crashed newlib): the process just ends and the system cleans up. The .nro leaves through
    // libnx (_Exit), which is how it gets back to the homebrew menu.
    if (envIsNso()) svcExitProcess();
    std::_Exit(code);
}

void disk_space(const char *path, uint64_t &free_bytes, uint64_t &total_bytes) {
    struct statvfs s;
    if (statvfs(path, &s) != 0) { free_bytes = total_bytes = 0; return; }
    free_bytes = uint64_t(s.f_bavail) * s.f_frsize;
    total_bytes = uint64_t(s.f_blocks) * s.f_frsize;
}

void install_crash_handler() {}  // __libnx_exception_handler below is always installed
void print_current_stack() { std::fprintf(stderr, "(host stack not available on the Switch)\n"); }
bool map_file(uint32_t, uint32_t, const char *, uint64_t, bool) { return false; }
void discard(void *, size_t) {}
size_t capture_stack(void **, size_t) { return 0; }
bool host_symbol(const void *, char *, size_t) { return false; }
void working_set_frame(uint64_t) {}
bool working_set_on() { return false; }

void start_watchdog(int seconds) {
    pthread_t t;
    pthread_create(&t, nullptr, [](void *p) -> void * {
        const int seconds = int(intptr_t(p));
        // core 0 (pthread's default) above every other thread: it sleeps, and must report even
        // when a busy thread hogs a core
        svcSetThreadPriority(CUR_THREAD_HANDLE, 0x1c);
        uint64_t last = 0;
        int stalled = 0;
        for (;;) {
            svcSleepThread(1000000000);
            uint64_t now = watchdog_frames();
            static const bool sample = std::getenv("NOVA3_SAMPLE") != nullptr;  // a poor man's profiler
            if (sample) log_game_thread_callers();
            stalled = now == last ? stalled + 1 : 0;
            last = now;
            if (stalled != seconds) continue;
            log_flush();
            std::lock_guard<std::mutex> lock(report_lock());
            std::fprintf(stderr, "\n=== watchdog: no frame for %d s ===\n", seconds);
            log_game_thread_callers();
            std::fflush(stderr);
        }
    }, reinterpret_cast<void *>(intptr_t(seconds)));
    pthread_detach(t);
}

}  // namespace rt::os

// libnx: CPU exceptions of any thread land here (on its own stack).
extern "C" {
alignas(16) u8 __nx_exception_stack[0x8000];

// The heap (see kGuestBudget): what the application has, less the guest's budget. Under the
// homebrew loader (.nro) the loader's heap is used as given.
extern "C" void __libnx_initheap(void) {
    extern char *fake_heap_start, *fake_heap_end;
    void *addr = nullptr;
    u64 size = 0;
    if (envHasHeapOverride()) {
        addr = envGetHeapOverrideAddr();
        size = envGetHeapOverrideSize();
    } else {
        u64 total = 0, used = 0;
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        total = std::min(total, rt::os::kRetailAppMemory);
        const u64 avail = total > used + 0x200000 ? total - used - 0x200000 : 0;
        size = avail > rt::os::kGuestBudget + (u64(768) << 20) ? avail - rt::os::kGuestBudget : std::min<u64>(avail, u64(768) << 20);
        size &= ~u64(0x1fffff);
        if (R_FAILED(svcSetHeapSize(&addr, size))) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
    }
    fake_heap_start = static_cast<char *>(addr);
    fake_heap_end = static_cast<char *>(addr) + size;
}

u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump *ctx) {
    rt::log_flush();
    rt::report_lock().lock();
    std::fprintf(stderr, "\n=== host exception %u: pc %016llx lr %016llx far %016llx ===\n", ctx->error_desc,
                 (unsigned long long)ctx->pc.x, (unsigned long long)ctx->lr.x, (unsigned long long)ctx->far.x);
    rt::report_fault("exception", reinterpret_cast<const void *>(uintptr_t(ctx->far.x)), -1);
    std::fflush(stderr);
    std::exit(3);
}
}

// Host C++ exceptions are not expected in normal runs (the code paths use error codes): every
// throw is logged with its type and thrower, for addr2line on the .elf (link: --wrap=__cxa_throw).
extern "C" [[noreturn]] void __real___cxa_throw(void *obj, std::type_info *type, void (*dtor)(void *));
extern "C" [[noreturn]] void __wrap___cxa_throw(void *obj, std::type_info *type, void (*dtor)(void *)) {
    static std::atomic<int> count{0};
    if (count++ < 20) {
        u64 base = 0;
        MemoryInfo mi{};
        u32 pi = 0;
        if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, reinterpret_cast<u64>(&__wrap___cxa_throw)))) base = mi.addr;
        rt::log("host exception: %s thrown from elf+%llx", type ? type->name() : "?",
                (unsigned long long)(reinterpret_cast<u64>(__builtin_return_address(0)) - base));
    }
    __real___cxa_throw(obj, type, dtor);
}

// Allocation log (NOVA3_ALLOC_LOG, host/switch/main.cpp): every heap block of 1 MB or more,
// allocated and freed, with its caller for addr2line on the .elf; for finding what fills the
// heap (Mesa's GPU buffers live there too). Links with --wrap for each function.
extern "C" {
bool nova3_alloc_log = false;
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__real_memalign(size_t, size_t);
void *__real_aligned_alloc(size_t, size_t);
int __real_posix_memalign(void **, size_t, size_t);
void __real_free(void *);
}
namespace {
thread_local bool t_in_alloc_log = false;
void alloc_logged(const char *what, void *p, size_t size, void *caller) {
    if (t_in_alloc_log) return;
    t_in_alloc_log = true;
    static u64 base = [] {
        MemoryInfo mi{};
        u32 pi = 0;
        return R_SUCCEEDED(svcQueryMemory(&mi, &pi, reinterpret_cast<u64>(&alloc_logged))) ? mi.addr : 0;
    }();
    rt::log("alloc: %s %p %zu KB from elf+%llx", what, p, size >> 10, (unsigned long long)(reinterpret_cast<u64>(caller) - base));
    t_in_alloc_log = false;
}
constexpr size_t kAllocLogMin = size_t(1) << 20;
}  // namespace
extern "C" void *__wrap_malloc(size_t n) {
    void *p = __real_malloc(n);
    if (nova3_alloc_log && n >= kAllocLogMin) alloc_logged("malloc", p, n, __builtin_return_address(0));
    return p;
}
extern "C" void *__wrap_calloc(size_t n, size_t m) {
    void *p = __real_calloc(n, m);
    if (nova3_alloc_log && n * m >= kAllocLogMin) alloc_logged("calloc", p, n * m, __builtin_return_address(0));
    return p;
}
extern "C" void *__wrap_realloc(void *old, size_t n) {
    void *p = __real_realloc(old, n);
    if (nova3_alloc_log && n >= kAllocLogMin) alloc_logged("realloc", p, n, __builtin_return_address(0));
    return p;
}
extern "C" void *__wrap_memalign(size_t align, size_t n) {
    void *p = __real_memalign(align, n);
    if (nova3_alloc_log && n >= kAllocLogMin) alloc_logged("memalign", p, n, __builtin_return_address(0));
    return p;
}
extern "C" void *__wrap_aligned_alloc(size_t align, size_t n) {
    void *p = __real_aligned_alloc(align, n);
    if (nova3_alloc_log && n >= kAllocLogMin) alloc_logged("aligned_alloc", p, n, __builtin_return_address(0));
    return p;
}
extern "C" int __wrap_posix_memalign(void **out, size_t align, size_t n) {
    const int r = __real_posix_memalign(out, align, n);
    if (nova3_alloc_log && n >= kAllocLogMin && !r) alloc_logged("posix_memalign", *out, n, __builtin_return_address(0));
    return r;
}
extern "C" void __wrap_free(void *p) {
    if (nova3_alloc_log && p && malloc_usable_size(p) >= kAllocLogMin)
        alloc_logged("free", p, malloc_usable_size(p), __builtin_return_address(0));
    __real_free(p);
}

namespace rt::os {
int64_t thread_cpu_ns() {
    u64 ticks = 0;  // the calling thread's run time on all cores
    if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, u64(-1)))) return -1;
    return int64_t(armTicksToNs(ticks));
}
void start_profiler() {}
void start_profiler_continuous(int) {}
void report_profiler() {}
}  // namespace rt::os

namespace rt::os {
// libnx's own threads (not pthread / std::thread): core 0, just below the game's priority
void start_background(void (*fn)(void *), void *arg) {
    static ::Thread threads[4];  // libnx's Thread
    static int used = 0;
    if (used >= 4) return;
    ::Thread *t = &threads[used++];
    if (R_SUCCEEDED(threadCreate(t, fn, arg, nullptr, 0x10000, 0x2d, 0))) threadStart(t);
}
}  // namespace rt::os
