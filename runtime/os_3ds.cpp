// rt::os on the New Nintendo 3DS (libctru, devkitARM).
//
// Memory: the game's code runs natively (runtime/native_3ds.cpp), so there is no guest address
// space to reserve: guest addresses are host addresses. __system_allocateHeaps below lays out
// the application's memory (178 MB in the New 3DS mode the exheader asks for):
//   * the regular heap, 96 MB (0x08000000 up to OS_HEAP_AREA_END, what a console's kernel
//     allows): first the game's image (kNativeImageReserve: the library, its ARMv6K stubs and
//     hook trampolines), then newlib's heap growing up and the guest heap growing down
//     (guest_heap_3ds.c), which continues in the linear heap;
//   * the linear heap (GPU memory: textures, vertex buffers, command lists) gets the rest.
//
// Threads: libctru threads with the stack size asked for (the game's code uses the host stack).
// The kernel schedules by strict priority with no time slices among equal priorities, so the
// game thread (the first one the runtime starts) has core 0 to itself; the guest's other
// threads run on core 2 (the New 3DS's third core), the runtime's service threads (log writer,
// watchdog) on core 1, the system core, within the time share APT grants (host/3ds/main.cpp).
//
// Crash reports: a per-thread exception handler (libctru's threadOnException) prints the
// registers, the game's link addresses for pc / lr, and a scan of the stack for return
// addresses into the game.
#include "os.h"
#include "runtime.h"

#include <3ds.h>

#include <malloc.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

extern "C" {
extern char *fake_heap_start, *fake_heap_end;
int host_heap_walk(const char *where);  // guest_heap_3ds.c (NOVA3_HEAP_CHECK=1)
extern u32 __ctru_heap, __ctru_heap_size, __ctru_linear_heap, __ctru_linear_heap_size;
void mappableInit(u32 addr_min, u32 addr_max);

// libctru calls this before main (and before C++ constructors). The default gives the linear
// heap 32 MB and the regular heap the rest, which the heap region (0x08000000-0x10000000) caps.
void __system_allocateHeaps(void) {
    Handle limit = 0;
    s64 max = 0, used = 0;
    ResourceLimitType type = RESLIMIT_COMMIT;
    svcGetResourceLimit(&limit, CUR_PROCESS_HANDLE);
    svcGetResourceLimitLimitValues(&max, limit, &type, 1);
    svcGetResourceLimitCurrentValues(&used, limit, &type, 1);
    svcCloseHandle(limit);
    const u32 remaining = u32(max - used) & ~0xfffu;
    // the heap region ends at the main thread's stack (mapped just below 0x10000000)
    MemInfo mi;
    PageInfo pi;
    const u32 stack = R_SUCCEEDED(svcQueryMemory(&mi, &pi, u32(uintptr_t(&limit)))) ? mi.base_addr : 0x0e000000u;
    // A console's kernel refuses a regular heap past OS_HEAP_AREA_END (96 MB); Azahar allowed one up
    // to the stack (128 MB), and the console stopped here at the first start. The guest heap takes
    // the rest from the linear heap (guest_heap_3ds.c).
    const u32 heap_max = std::min<u32>({stack - 0x1000, OS_HEAP_AREA_END, 0x10000000u}) - OS_HEAP_AREA_BEGIN;
    constexpr u32 kLinearMin = 24u << 20;
    u32 heap = remaining > kLinearMin ? std::min(remaining - kLinearMin, heap_max) : remaining / 2;
    heap &= ~0xfffu;
    if (heap < rt::kNativeImageReserve + (16u << 20)) svcBreak(USERBREAK_PANIC);  // the old 3DS: far too little
    u32 tmp = 0;
    __ctru_heap = OS_HEAP_AREA_BEGIN;
    __ctru_heap_size = heap;
    if (R_FAILED(svcControlMemory(&tmp, __ctru_heap, 0, heap, MEMOP_ALLOC, MemPerm(MEMPERM_READ | MEMPERM_WRITE))))
        svcBreak(USERBREAK_PANIC);
    __ctru_linear_heap_size = remaining - heap;
    if (R_FAILED(svcControlMemory(&__ctru_linear_heap, 0, 0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR,
                                  MemPerm(MEMPERM_READ | MEMPERM_WRITE))))
        svcBreak(USERBREAK_PANIC);
    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
    // the game's image first (runtime/native_3ds.cpp loads it at kNativeLoadBase), then newlib's heap
    fake_heap_start = reinterpret_cast<char *>(__ctru_heap + rt::kNativeImageReserve);
    fake_heap_end = reinterpret_cast<char *>(__ctru_heap + __ctru_heap_size);
}
}

namespace rt {
void native_report_threads(FILE *out);  // native_3ds.cpp
bool native_stop_code();                // native_3ds.cpp
void io_read_stats(uint64_t &bytes, uint64_t &requests);  // libc_io.cpp
}

namespace rt::os {

void *reserve(size_t) { return nullptr; }  // not used: guest addresses are host addresses
bool commit(void *, size_t) { return true; }

namespace {
// svcGetSystemTick counts at the ARM11's 268,111,856 Hz
int64_t ticks_to_ns(uint64_t t) {
    return int64_t(t / SYSCLOCK_ARM11) * 1000000000 + int64_t((t % SYSCLOCK_ARM11) * 1000000000 / SYSCLOCK_ARM11);
}
}  // namespace

int64_t monotonic_ns() { return ticks_to_ns(svcGetSystemTick()); }
// The wall clock: libctru's (gettimeofday: osGetTime, the system time from the shared page
// refined with the tick counter) read once, then the tick counter. The game asks every frame (and
// GaiaMgr::Update did); on a console the game thread twice stopped for good inside osGetTime, as
// the last thing it did (newlib's clock_gettime(CLOCK_REALTIME) is not backed by it here).
int64_t realtime_ns() {
    static const int64_t base = [] {
        timeval tv;
        gettimeofday(&tv, nullptr);
        return int64_t(tv.tv_sec) * 1000000000 + int64_t(tv.tv_usec) * 1000 - monotonic_ns();
    }();
    return base + monotonic_ns();
}
void sleep_ns(int64_t ns) { svcSleepThread(ns > 0 ? ns : 0); }  // 0: lets threads of the same priority run
void yield() { svcSleepThread(0); }
// Vox's mixer: above the game's threads (0x30), so a busy one sharing its core cannot starve it
void raise_thread_priority() { svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2f); }
uint32_t process_id() {
    u32 pid = 0;
    svcGetProcessId(&pid, CUR_PROCESS_HANDLE);
    return pid;
}
int64_t process_cpu_ns() { return monotonic_ns(); }  // no per-process CPU time; only used for statistics
int64_t thread_cpu_ns() { return -1; }
void working_set_frame(uint64_t) {}
bool working_set_on() { return false; }
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
uint64_t thread_id() {
    u32 id = 0;
    svcGetThreadId(&id, CUR_THREAD_HANDLE);
    return id;
}

void disk_space(const char *path, uint64_t &free_bytes, uint64_t &total_bytes) {
    struct statvfs s;
    if (statvfs(path, &s) != 0) { free_bytes = total_bytes = 0; return; }
    free_bytes = uint64_t(s.f_bavail) * s.f_frsize;
    total_bytes = uint64_t(s.f_blocks) * s.f_frsize;
}

void start_profiler() {}
void start_profiler_continuous(int) {}
void report_profiler() {}

// ---- crash handler ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_crash_handler{false};
std::atomic<bool> g_hang_probe{false};  // the watchdog took the game's code its execute permission
constexpr size_t kHandlerStack = 16u << 10;

void scan_stack(u32 sp, const Cpu *cpu = rt::current_cpu()) {
    // return addresses into the game on the faulting stack (words just after a BL / BLX), as
    // link addresses for tools/disasm.py; the top is the thread's first call into the game
    u32 top = cpu ? rt::guest_stack_top(cpu) : 0;
    if (!top || top < sp || top - sp > (4u << 20)) top = sp + (64u << 10);
    const u32 lo = g_load_base + 4, hi = g_load_base + rt::kNativeImageReserve;
    std::fprintf(stderr, "game callers (stack scan from %08lx):", (unsigned long)sp);
    int found = 0;
    for (u32 a = sp & ~3u; a < top && found < 32; a += 4) {
        if (!rt::guest_mapped(a)) break;
        const u32 w = *reinterpret_cast<const u32 *>(a) & ~1u;
        if (w < lo || w >= hi || !rt::guest_mapped(w - 4)) continue;
        const u32 insn = *reinterpret_cast<const u32 *>(w - 4);
        if ((insn & 0x0f000000u) != 0x0b000000u && (insn & 0xfe000000u) != 0xfa000000u && (insn & 0x0ffffff0u) != 0x012fff30u)
            continue;
        std::fprintf(stderr, " %08lx", (unsigned long)(w - g_load_base));
        found++;
    }
    std::fputc('\n', stderr);
}

// The words below `sp` (the game thread's stack at its last gate) that are return addresses, into
// the game (g, link address) or into this program (h: addr2line -e nova3.elf), as offsets below
// sp: the code it went into since without a gate (the direct imports: GL, malloc, the mutexes,
// the string and math functions) leaves its frames there. The current chain runs up from the
// deepest live frame; deeper words are older calls' leftovers.
extern "C" char __start__[], __exidx_start[];
void scan_below(u32 sp) {
    const u32 glo = g_load_base + 4, ghi = g_load_base + rt::kNativeImageReserve;
    const u32 hlo = u32(uintptr_t(__start__)), hhi = u32(uintptr_t(__exidx_start));
    std::fprintf(stderr, "return addresses below it (offset: g game link / h host):");
    int found = 0;
    for (u32 a = (sp & ~3u) - 4; a > sp - (48u << 10) && found < 96; a -= 4) {
        if (!rt::guest_mapped(a)) break;
        const u32 w = *reinterpret_cast<const u32 *>(a);
        const bool thumb = w & 1, game = w >= glo && w < ghi;
        if (!game && !(w >= hlo + 4 && w < hhi)) continue;
        bool call;
        if (thumb) {  // a 32-bit BL / BLX before it
            const u16 *h = reinterpret_cast<const u16 *>(w - 1 - 4);
            call = (h[0] & 0xf800u) == 0xf000u && (h[1] & 0xd000u) >= 0xc000u;
        } else {
            const u32 insn = *reinterpret_cast<const u32 *>(w - 4);
            call = (insn & 0x0f000000u) == 0x0b000000u || (insn & 0xfe000000u) == 0xfa000000u || (insn & 0x0ffffff0u) == 0x012fff30u;
        }
        if (!call) continue;
        std::fprintf(stderr, "%s -%lx %c %08lx", found % 6 ? "" : "\n   ", (unsigned long)(sp - a), game ? 'g' : 'h',
                     (unsigned long)(game ? w - g_load_base : w));
        found++;
    }
    std::fputc('\n', stderr);
}

void describe(const char *name, u32 a) {
    if (a >= g_load_base && a < g_load_base + rt::kNativeImageReserve) {
        const std::string sym = rt::guest_symbol(a - g_load_base);
        std::fprintf(stderr, "  %s %08lx = game link %08lx %s\n", name, (unsigned long)a, (unsigned long)(a - g_load_base), sym.c_str());
    } else {
        std::fprintf(stderr, "  %s %08lx (host: addr2line -e nova3.elf)\n", name, (unsigned long)a);
    }
}

void exception_handler(ERRF_ExceptionInfo *ex, CpuRegisters *regs) {
    // the hang probe is after the game thread (no Cpu of its own on some paths, or the main one);
    // the game's other threads stop here, so that its report is the one written
    if (g_hang_probe && rt::current_cpu() && rt::current_cpu() != rt::main_cpu()) {
        std::fprintf(stderr, "hang probe: thread %lu stopped at %08lx (link %08lx)\n", (unsigned long)thread_id(),
                     (unsigned long)regs->pc, (unsigned long)(regs->pc - g_load_base));
        for (;;) svcSleepThread(1000000000);
    }
    rt::log_flush();
    rt::report_lock().lock();
    static const char *const kinds[] = {"prefetch abort", "data abort", "undefined instruction", "VFP exception"};
    std::fprintf(stderr, "\n=== crash: %s (thread %lu): pc %08lx, address %08lx, fsr %08lx ===\n",
                 unsigned(ex->type) < 4 ? kinds[ex->type] : "exception", (unsigned long)thread_id(), (unsigned long)regs->pc,
                 (unsigned long)ex->far, (unsigned long)ex->fsr);
    describe("pc", regs->pc);
    describe("lr", regs->lr);
    for (int i = 0; i < 13; i++)
        std::fprintf(stderr, "  r%-2d %08lx%s", i, (unsigned long)regs->r[i], i % 4 == 3 ? "\n" : "");
    std::fprintf(stderr, "  sp  %08lx  cpsr %08lx\n", (unsigned long)regs->sp, (unsigned long)regs->cpsr);
    scan_stack(regs->sp);
    // the words around pc and lr (compare with the game file: tools/disasm.py) and the stack
    for (const u32 at : {u32(regs->pc), u32(regs->lr) & ~1u}) {
        std::fprintf(stderr, "  words at %08lx:", (unsigned long)(at - 16));
        for (u32 a = (at - 16) & ~3u; a < at + 16; a += 4)
            if (rt::guest_mapped(a)) std::fprintf(stderr, " %08lx", (unsigned long)*reinterpret_cast<const u32 *>(a));
        std::fputc('\n', stderr);
    }
    std::fprintf(stderr, "  stack from %08lx:", (unsigned long)(regs->sp - 64));
    for (u32 a = (regs->sp - 64) & ~3u, i = 0; i < 48; a += 4, i++) {
        if (!rt::guest_mapped(a)) break;
        std::fprintf(stderr, "%s %08lx", i % 8 == 0 ? "\n   " : "", (unsigned long)*reinterpret_cast<const u32 *>(a));
    }
    std::fputc('\n', stderr);
    rt::report_fault("exception", nullptr, -1);  // the last import and the registers it saw
    std::fflush(stderr);
    exit_process(3);
}

// on this thread (libctru keeps the handler in the thread's TLS); its own stack, so a stack
// overflow can still be reported
void install_for_this_thread() {
    if (!g_crash_handler) return;
    void *stack = std::malloc(kHandlerStack);
    if (stack) threadOnException(exception_handler, static_cast<u8 *>(stack) + kHandlerStack, WRITE_DATA_TO_HANDLER_STACK);
}
}  // namespace

void install_crash_handler() {
    g_crash_handler = true;
    install_for_this_thread();
}
void print_current_stack() {
    u32 sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    scan_stack(sp);
}

// ---- threads ----------------------------------------------------------------------------------
// The lowest address the thread's stack may reach before the import gates report an overflow
// (runtime/native_3ds.cpp): the game's code shares the thread's stack with the runtime.
thread_local uint32_t t_stack_limit;

namespace {
struct Start { void (*fn)(void *); void *arg; size_t stack; };
std::atomic<int> g_started{0};
// NOVA3_HEAP_CHECK=1 (a test script's env, set after the first threads start): newlib's heap walked
bool heap_check() {
    const char *v = std::getenv("NOVA3_HEAP_CHECK");
    return v && *v == '1';
}

// Threads are joinable, and ended ones are joined and freed by the next thread start (or the
// watchdog). libctru frees a detached thread's memory, its stack included, inside threadExit while
// the thread still runs on that stack: the end of free() and the lock release after it read
// return addresses from a block another core may already have been given (four of the game's
// threads end at once early in the boot).
std::mutex g_ended_lock;
std::vector<Thread> g_ended;
void reap_ended() {
    std::vector<Thread> done;
    {
        std::lock_guard<std::mutex> lock(g_ended_lock);
        done.swap(g_ended);
    }
    for (Thread t : done) {
        threadJoin(t, U64_MAX);  // (it is past its last line: this waits for its svcExitThread)
        threadFree(t);
    }
}

void trampoline(void *p) {
    Start s = *static_cast<Start *>(p);
    delete static_cast<Start *>(p);
    u32 sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    t_stack_limit = sp - u32(s.stack) + (32u << 10);
    rt::log("thread %lu: runs %p (stack %08lx-%08lx) on core %ld", (unsigned long)thread_id(), reinterpret_cast<void *>(s.fn),
            (unsigned long)(sp - s.stack), (unsigned long)sp, (long)svcGetProcessorID());
    install_for_this_thread();
    if (heap_check()) host_heap_walk("a thread's start");
    s.fn(s.arg);
    if (heap_check()) host_heap_walk("a thread's end");
    rt::log("thread %lu: ends", (unsigned long)thread_id());
    std::lock_guard<std::mutex> lock(g_ended_lock);
    g_ended.push_back(threadGetCurrent());
}

bool create(void (*fn)(void *), void *arg, size_t stack, int prio, int core) {
    reap_ended();
    stack = (std::max<size_t>(stack, 32u << 10) + 7) & ~size_t(7);
    auto *start = new Start{fn, arg, stack};
    if (threadCreate(trampoline, start, stack, prio, core, false)) return true;
    if (core != -2 && threadCreate(trampoline, start, stack, prio, -2, false)) return true;  // the default core
    delete start;
    return false;
}
}  // namespace

// The first thread started is the game thread: core 0 to itself. The guest's threads: core 2.
bool start_thread(void (*fn)(void *), void *arg, size_t stack_size) {
    const int n = g_started++;
    return create(fn, arg, stack_size, 0x30, n == 0 ? 0 : 2);
}

// the runtime's service threads (the log writer): the system core, below the game's priority
// (above the game's priority: it sleeps, and a game thread spinning on a lock must not starve it)
void start_background(void (*fn)(void *), void *arg) { create(fn, arg, 64u << 10, 0x2e, 1); }

// Not libctru's exit (std::exit and _Exit take the same path): it runs the static destructors,
// then unmaps both heaps under the threads still running on them (guest threads, the log
// writer, the sound's feeder), which fault as the process ends: a crash dump on the console,
// where Azahar only logs unmapped accesses. Here the services are closed as libctru's exit does
// (__appExit: userAppExit in host/3ds/main.cpp, the sound and the screens; then fs, hid, and
// APT, which gives the console back to the HOME Menu), and the kernel ends every thread at once.
// (A CIA: no loader to return to, which is what the heaps' unmapping is for.)
extern "C" void __appExit();
void exit_process(int) {
    pgo_flush();
    log_flush();  // the asynchronous log first
    std::fflush(nullptr);
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x18);  // ahead of the threads of its core meanwhile
    __appExit();
    svcExitProcess();
}

// The thread that started the watchdog (the game thread), for the priority raise below.
static Handle g_watched_thread;

// The application suspended (the HOME Menu, sleep): no frames then, and no hang. APT's hooks run
// on the thread in aptMainLoop (the game thread); the watchdog and the main thread's status line
// count from the return. (Before, a stay of 120 s in the HOME Menu ran the hang probe, which makes
// the game's code non-executable: the game crashed on purpose.)
static std::atomic<bool> g_suspended{false};
static std::atomic<uint32_t> g_resumes{0};
bool app_suspended() { return g_suspended.load(); }
uint32_t app_resumes() { return g_resumes.load(); }

void start_watchdog(int seconds) {
    svcDuplicateHandle(&g_watched_thread, CUR_THREAD_HANDLE);
    static aptHookCookie apt;
    aptHook(&apt, [](APT_HookType type, void *) {
        if (type == APTHOOK_ONSUSPEND || type == APTHOOK_ONSLEEP) g_suspended = true;
        else if (type == APTHOOK_ONRESTORE || type == APTHOOK_ONWAKEUP) g_suspended = false, g_resumes++;
    }, nullptr);
    create([](void *p) {
        const int seconds = int(intptr_t(p));
        uint64_t last = 0;
        int stalled = 0;
        for (;;) {
            svcSleepThread(1000000000);
            const uint64_t now = watchdog_frames();
            static const bool sample = std::getenv("NOVA3_SAMPLE") != nullptr;  // a poor man's profiler
            if (sample) log_game_thread_callers();
            reap_ended();
            static uint32_t resumes;
            if (g_suspended || g_resumes != resumes) {  // suspended, or back since the last look: no stall
                resumes = g_resumes;
                stalled = 0;
                last = now;
                continue;
            }
            if (now == last && heap_check()) host_heap_walk("the watchdog's check");  // (a long step: a load)
            stalled = now == last ? stalled + 1 : 0;
            last = now;
            static bool raised;  // the game thread's priority, raised below
            if (raised && !stalled) {
                raised = false;
                svcSetThreadPriority(g_watched_thread, 0x30);
                rt::log("watchdog: frames again after the raise: the game thread was kept from running (priority back to 0x30)");
            }
            if (stalled < seconds || stalled % seconds) continue;  // every `seconds` while it lasts
            log_flush();
            std::unique_lock<std::mutex> lock(report_lock());
            std::fprintf(stderr, "\n=== watchdog: no frame for %d s ===\n", stalled);
            // the game thread at its last import (the gates keep its registers): which one, from where
            if (const Cpu *cpu = rt::main_cpu()) {
                std::fprintf(stderr, "last import: slot %lu (%s), called from %08lx (link %08lx)\n", (unsigned long)cpu->r[12],
                             cpu->r[12] < g_import_count ? g_import_names[cpu->r[12]] : "?", (unsigned long)cpu->r[14],
                             (unsigned long)(cpu->r[14] - g_load_base));
                scan_stack(cpu->r[13], cpu);
                if (stalled == seconds || stalled == 120) scan_below(cpu->r[13]);
            }
            {  // what the files gave since the last report (a long stall is usually a load)
                static uint64_t bytes0, reads0;
                uint64_t bytes, reads;
                io_read_stats(bytes, reads);
                std::fprintf(stderr, "file reads since the last report: %.1f MB in %llu requests\n", (bytes - bytes0) / 1048576.0,
                             (unsigned long long)(reads - reads0));
                bytes0 = bytes, reads0 = reads;
            }
            std::fprintf(stderr, "threads in the game's code (gate numbers: imports, or hooks when called from a hooked function):\n");
            native_report_threads(stderr);
            log_flush();
            std::fflush(stderr);
            lock.unlock();
            // Starved or stuck? The game thread (0x30) shares core 0 with the main thread (0x2c)
            // and the sound feeder (0x18), and the kernel never time-slices: a thread above it
            // that never blocks keeps it from running at all. Raised to the top (0x18), it runs
            // unless the one keeping it out is at 0x18 too; frames resuming say it was starved.
            if (stalled == 30 && g_watched_thread) {
                s32 prio = 0;
                svcGetThreadPriority(&prio, g_watched_thread);
                const Result rc = svcSetThreadPriority(g_watched_thread, 0x18);
                raised = R_SUCCEEDED(rc);
                std::fprintf(stderr, "watchdog: the game thread's priority %#lx raised to 0x18 (%08lx)\n", (long)prio, (unsigned long)rc);
                std::fflush(stderr);
            }
            // A hang this long is no load. The game thread is then usually in a loop of the game's
            // code that calls nothing (its last import above stays the same): with the code made
            // non-executable it faults where it runs, and the crash report gives the loop's address.
            if (stalled >= 120 && !g_hang_probe.exchange(true)) {
                std::fprintf(stderr, "hang probe: the game's code made non-executable\n");
                std::fflush(stderr);
                if (!native_stop_code()) {
                    std::fprintf(stderr, "hang probe: svcControlProcessMemory failed\n");
                    continue;
                }
                svcSleepThread(10000000000LL);  // the crash report ends the process
                std::fprintf(stderr, "hang probe: no fault in 10 s: the game thread waits outside the game's code\n");
                std::fflush(stderr);
                exit_process(4);
            }
        }
    },reinterpret_cast<void *>(intptr_t(seconds)), 32u << 10, 0x18, 1);  // above every other thread: it sleeps
}

}  // namespace rt::os
