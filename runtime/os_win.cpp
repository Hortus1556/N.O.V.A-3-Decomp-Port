// rt::os on Win32: VirtualAlloc, high-resolution timers, threads with reserved stacks,
// vectored exception handler + DbgHelp stack walks.
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

#include "os.h"
#include "runtime.h"

#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace rt::os {

void *reserve(size_t size) { return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS); }
bool commit(void *p, size_t size) { return VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE) != nullptr; }

int64_t realtime_ns() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return ((int64_t(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) - 116444736000000000ll) * 100;
}
int64_t monotonic_ns() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return int64_t(double(c.QuadPart) * 1e9 / double(freq.QuadPart));
}
void sleep_ns(int64_t ns) {
    if (ns <= 0) { SwitchToThread(); return; }
    static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -(ns / 100);  // relative, 100 ns units
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
    else Sleep(DWORD(ns / 1000000));
}
void yield() { SwitchToThread(); }
void raise_thread_priority() { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL); }
int64_t process_cpu_ns() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return int64_t((uint64_t(u.dwHighDateTime) << 32 | u.dwLowDateTime) + (uint64_t(k.dwHighDateTime) << 32 | k.dwLowDateTime)) * 100;
}
bool local_time(int64_t t, struct tm &out, int32_t &utc_offset) {
    time_t tt = time_t(t);
    if (localtime_s(&out, &tt)) return false;
    long bias = 0;
    _get_timezone(&bias);
    utc_offset = -int32_t(bias) + (out.tm_isdst > 0 ? 3600 : 0);
    return true;
}
bool utc_time(int64_t t, struct tm &out) {
    time_t tt = time_t(t);
    return gmtime_s(&out, &tt) == 0;
}
uint64_t thread_id() { return GetCurrentThreadId(); }
uint32_t process_id() { return GetCurrentProcessId(); }

namespace {
struct Start { void (*fn)(void *); void *arg; };
DWORD WINAPI trampoline(void *p) {
    Start s = *static_cast<Start *>(p);
    delete static_cast<Start *>(p);
    s.fn(s.arg);
    return 0;
}
}  // namespace
bool start_thread(void (*fn)(void *), void *arg, size_t stack_reserve) {
    auto *start = new Start{fn, arg};
    HANDLE h = CreateThread(nullptr, stack_reserve, trampoline, start, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) { delete start; return false; }
    CloseHandle(h);
    return true;
}

void exit_process(int code) {
    pgo_flush();
    log_flush();  // the asynchronous log first
    std::fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), UINT(code));
    ExitProcess(UINT(code));
}

void disk_space(const char *path, uint64_t &free_bytes, uint64_t &total_bytes) {
    ULARGE_INTEGER avail{}, total{}, freeb{};
    GetDiskFreeSpaceExA(path, &avail, &total, &freeb);
    free_bytes = avail.QuadPart;
    total_bytes = total.QuadPart;
}

// ---- crash reports ------------------------------------------------------------------------
namespace {

// The watchdog's copy of the game thread's stack (taken while it was suspended); other memory
// (code, unwind tables) is read live.
struct StackCopy { DWORD64 lo = 0, hi = 0; const uint8_t *data = nullptr; } g_stack_copy;
BOOL CALLBACK read_stack_copy(HANDLE proc, DWORD64 addr, PVOID buf, DWORD size, LPDWORD read) {
    const StackCopy &c = g_stack_copy;
    if (addr >= c.lo && addr + size <= c.hi) {
        std::memcpy(buf, c.data + (addr - c.lo), size);
        if (read) *read = size;
        return TRUE;
    }
    SIZE_T n = 0;
    const BOOL ok = ReadProcessMemory(proc, reinterpret_cast<void *>(addr), buf, size, &n);
    if (read) *read = DWORD(n);
    return ok;
}

void walk(CONTEXT ctx, HANDLE thread, PREAD_PROCESS_MEMORY_ROUTINE64 read = nullptr) {
    std::fprintf(stderr, "call stack (innermost first):\n");
    HANDLE proc = GetCurrentProcess();
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrPC.Mode = frame.AddrStack.Mode = frame.AddrFrame.Mode = AddrModeFlat;
    for (int i = 0; i < 64; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &frame, &ctx, read, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr) ||
            !frame.AddrPC.Offset)
            break;
        DWORD64 pc = i ? frame.AddrPC.Offset - 1 : frame.AddrPC.Offset;
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
        auto *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        IMAGEHLP_LINE64 line{sizeof(IMAGEHLP_LINE64)};
        DWORD ldisp = 0;
        bool has_line = SymGetLineFromAddr64(proc, pc, &ldisp, &line);
        print_frame(i, SymFromAddr(proc, pc, &disp, sym) ? sym->Name : "?", has_line ? line.FileName : nullptr,
                    has_line ? unsigned(line.LineNumber) : 0);
    }
}

LONG CALLBACK crash_handler(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    const char *what = code == EXCEPTION_ACCESS_VIOLATION       ? "access violation"
                       : code == EXCEPTION_ILLEGAL_INSTRUCTION  ? "illegal instruction"
                       : code == EXCEPTION_INT_DIVIDE_BY_ZERO   ? "integer divide by zero"
                       : code == EXCEPTION_STACK_OVERFLOW       ? "stack overflow"
                       : code == EXCEPTION_PRIV_INSTRUCTION     ? "privileged instruction"
                                                                : nullptr;
    if (!what) return EXCEPTION_CONTINUE_SEARCH;
    log_flush();
    report_lock().lock();
    std::fprintf(stderr, "\n=== [thread %lu] host exception %08lx (%s) at %p ===\n", GetCurrentThreadId(), code, what,
                 ep->ExceptionRecord->ExceptionAddress);
    bool av = code == EXCEPTION_ACCESS_VIOLATION;
    report_fault(what, av ? reinterpret_cast<void *>(ep->ExceptionRecord->ExceptionInformation[1]) : nullptr,
                 av ? int(ep->ExceptionRecord->ExceptionInformation[0] != 0) : -1);
    walk(*ep->ContextRecord, GetCurrentThread());
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void install_crash_handler() {
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    AddVectoredExceptionHandler(1, crash_handler);
}

namespace {
const uint64_t g_ws_every = [] { const char *e = std::getenv("NOVA3_WORKING_SET"); return e ? std::strtoull(e, nullptr, 10) : 0; }();
constexpr uint32_t kWsPages = 1u << 20;  // 4 KB pages of the 4 GiB guest window
std::vector<uint32_t> g_ws_last;         // per page: the frame of its last touch (0: none yet)
std::vector<uint8_t> g_ws_prot;          // per page: its protection before ours (0: not ours)
std::atomic<uint32_t> g_ws_frame{1};
std::atomic<uint64_t> g_ws_faults{0};

LONG CALLBACK ws_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    auto *a = reinterpret_cast<uint8_t *>(ep->ExceptionRecord->ExceptionInformation[1]);
    if (a < g_mem || a >= g_mem + (1ull << 32)) return EXCEPTION_CONTINUE_SEARCH;
    const uint32_t page = uint32_t((a - g_mem) >> 12);
    const uint8_t prot = g_ws_prot[page];
    if (!prot) return EXCEPTION_CONTINUE_SEARCH;
    DWORD old;
    if (!VirtualProtect(g_mem + (uint64_t(page) << 12), 4096, prot == 1 ? PAGE_READWRITE : PAGE_READONLY, &old))
        return EXCEPTION_CONTINUE_SEARCH;
    g_ws_last[page] = g_ws_frame.load(std::memory_order_relaxed);
    g_ws_faults.fetch_add(1, std::memory_order_relaxed);
    return EXCEPTION_CONTINUE_EXECUTION;
}

void ws_report(uint32_t frame) {
    static const char *const kRegion[] = {"image", "heap", "other"};
    const uint32_t windows[] = {1, 10, 60, uint32_t(g_ws_every)};
    uint64_t n[4][3] = {}, ever[3] = {};
    const uint32_t image_end = (g_load_base >> 12) + 0x2000;  // the library's 25 MB and a margin
    for (uint32_t p = 0; p < kWsPages; p++) {
        const uint32_t last = g_ws_last[p];
        if (!last) continue;
        const int r = p >= (g_load_base >> 12) && p < image_end ? 0 : p >= (kHeapBase >> 12) && p < (kHeapEnd >> 12) ? 1 : 2;
        ever[r]++;
        for (int w = 0; w < 4; w++)
            if (last + windows[w] > frame) n[w][r]++;
    }
    for (int r = 0; r < 3; r++)
        rt::log("working set: frame %u %-5s last 1/10/60/%u frames: %.2f / %.2f / %.2f / %.2f MB (touched ever %.1f MB)",
                frame, kRegion[r], windows[3], n[0][r] / 256.0, n[1][r] / 256.0, n[2][r] / 256.0, n[3][r] / 256.0,
                ever[r] / 256.0);
    rt::log("working set: %.0f faults a frame", double(g_ws_faults.exchange(0)) / double(g_ws_every));
}
}  // namespace

bool working_set_on() { return g_ws_every != 0; }

void working_set_frame(uint64_t frame) {
    if (!g_ws_every) return;
    if (g_ws_last.empty()) {
        g_ws_last.assign(kWsPages, 0);
        g_ws_prot.assign(kWsPages, 0);
        AddVectoredExceptionHandler(1, ws_handler);  // added after the crash handler: runs first
    }
    const uint32_t f = uint32_t(frame) + 1;
    if (f % g_ws_every == 0) ws_report(f - 1);
    g_ws_frame = f;
    // take every committed guest page's access (pages still without it keep their earlier state)
    MEMORY_BASIC_INFORMATION mi;
    for (uint8_t *p = g_mem; p < g_mem + (1ull << 32) && VirtualQuery(p, &mi, sizeof mi); p = static_cast<uint8_t *>(mi.BaseAddress) + mi.RegionSize) {
        if (mi.State != MEM_COMMIT || (mi.Protect != PAGE_READWRITE && mi.Protect != PAGE_READONLY)) continue;
        const uint32_t first = uint32_t((static_cast<uint8_t *>(mi.BaseAddress) - g_mem) >> 12);
        const uint32_t count = uint32_t(mi.RegionSize >> 12);
        for (uint32_t i = 0; i < count; i++) g_ws_prot[first + i] = mi.Protect == PAGE_READWRITE ? 1 : 2;
        DWORD old;
        VirtualProtect(mi.BaseAddress, mi.RegionSize, PAGE_NOACCESS, &old);
    }
}

bool map_file(uint32_t, uint32_t, const char *, uint64_t, bool) { return false; }
void discard(void *, size_t) {}
size_t capture_stack(void **pcs, size_t max) { return RtlCaptureStackBackTrace(1, DWORD(max), pcs, nullptr); }

bool host_symbol(const void *pc, char *name, size_t size) {
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (!SymFromAddr(GetCurrentProcess(), DWORD64(pc) - 1, &disp, sym)) return false;
    std::snprintf(name, size, "%s", sym->Name);
    return true;
}

void print_current_stack() {
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    walk(ctx, GetCurrentThread());
}

void start_watchdog(int seconds) {
    struct Args { HANDLE thread; int seconds; };
    HANDLE self = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
    auto *args = new Args{self, seconds};
    HANDLE h = CreateThread(nullptr, 0, [](void *p) -> DWORD {
        auto *a = static_cast<Args *>(p);
        uint64_t last = 0;
        int stalled = 0;
        for (;;) {
            Sleep(1000);
            uint64_t now = watchdog_frames();
            stalled = now == last ? stalled + 1 : 0;
            last = now;
            if (stalled != a->seconds) continue;
            // While the thread is suspended, only copy its registers and the top of its stack: it
            // may hold the heap or loader lock (a long load), and DbgHelp takes both, so walking
            // it suspended deadlocked the watchdog and left the game suspended for good.
            SuspendThread(a->thread);
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            GetThreadContext(a->thread, &ctx);
            static uint8_t stack[256 * 1024];
            SIZE_T got = 0;
            ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(ctx.Rsp), stack, sizeof stack, &got);
            ResumeThread(a->thread);
            g_stack_copy = {ctx.Rsp, ctx.Rsp + got, stack};
            {
                log_flush();
                std::lock_guard<std::mutex> lock(report_lock());
                std::fprintf(stderr, "\n=== watchdog: no frame for %d s; game thread stack ===\n", a->seconds);
                walk(ctx, a->thread, read_stack_copy);  // the copy taken above, not the live stack
                std::fprintf(stderr, "guest threads:\n");
                report_threads(stderr);
                std::fflush(stderr);
            }
            g_stack_copy = {};
        }
    }, args, 0, nullptr);
    if (h) CloseHandle(h);
}

}  // namespace rt::os

// ---- benchmarks ----------------------------------------------------------------------------
namespace rt::os {

int64_t thread_cpu_ns() {
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return -1;
    auto ns = [](FILETIME t) { return ((int64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime) * 100; };
    return ns(kernel) + ns(user);
}


namespace {
std::mutex g_prof_lock;
std::vector<DWORD64> g_prof_samples;
std::atomic<bool> g_prof_on{false};
}  // namespace

// Sampling profiler: another thread suspends the profiled one ~1000 times a second and records
// its instruction pointer; the report symbolizes them with dbghelp (the PDB's line tables).
void start_profiler() {
    HANDLE self = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
    g_prof_on = true;
    HANDLE h = CreateThread(nullptr, 0, [](void *p) -> DWORD {
        HANDLE thread = static_cast<HANDLE>(p);
        timeBeginPeriod(1);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        while (g_prof_on) {
            Sleep(1);
            // nothing that can allocate while the thread is suspended: it may hold the heap's
            // lock (a push_back that grew the vector then waited for it forever: the game hung)
            if (SuspendThread(thread) == DWORD(-1)) break;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            const bool sampled = GetThreadContext(thread, &ctx) != 0;
            ResumeThread(thread);
            if (sampled) {
                std::lock_guard<std::mutex> lock(g_prof_lock);
                g_prof_samples.push_back(ctx.Rip);
            }
        }
        timeEndPeriod(1);
        return 0;
    }, self, 0, nullptr);
    if (h) CloseHandle(h);
}

void print_profile(const std::vector<DWORD64> &samples, size_t top_functions);

void report_profiler() {
    g_prof_on = false;
    Sleep(5);
    std::vector<DWORD64> samples;
    {
        std::lock_guard<std::mutex> lock(g_prof_lock);
        samples.swap(g_prof_samples);
    }
    print_profile(samples, 40);
}

// The log's continuous profile (perf reports): the game thread sampled ~500 times a second,
// the hottest functions printed every `seconds` by the sampling thread itself (symbolizing
// takes a while: never on the game thread). NOVA3_PROFILE=0 turns it off.
void start_profiler_continuous(int seconds) {
    if (const char *e = std::getenv("NOVA3_PROFILE"); e && e[0] == '0') return;
    HANDLE self = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
    struct Args { HANDLE thread; int seconds; };
    HANDLE h = CreateThread(nullptr, 0, [](void *p) -> DWORD {
        const Args a = *static_cast<Args *>(p);
        delete static_cast<Args *>(p);
        timeBeginPeriod(1);
        std::vector<DWORD64> samples;
        int64_t next = monotonic_ns() + int64_t(a.seconds) * 1'000'000'000;
        for (;;) {
            Sleep(2);
            if (SuspendThread(a.thread) == DWORD(-1)) break;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            const bool sampled = GetThreadContext(a.thread, &ctx) != 0;
            ResumeThread(a.thread);  // before the push_back (see start_profiler)
            if (sampled) samples.push_back(ctx.Rip);
            if (monotonic_ns() >= next) {
                print_profile(samples, 15);
                samples.clear();
                next = monotonic_ns() + int64_t(a.seconds) * 1'000'000'000;
            }
        }
        return 0;
    }, new Args{self, seconds}, 0, nullptr);
    if (h) {
        SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
        CloseHandle(h);
    }
}

void print_profile(const std::vector<DWORD64> &samples, size_t top_functions) {
    if (samples.empty()) return;
    HANDLE proc = GetCurrentProcess();
    std::unordered_map<std::string, int> by_func, by_cat, by_line_kind;
    std::unordered_map<std::string, std::vector<std::string>> files;  // generated C, for line kinds
    for (DWORD64 pc : samples) {
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
        auto *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        std::string name = SymFromAddr(proc, pc, &disp, sym) ? sym->Name : "";
        char module[MAX_PATH] = {};
        HMODULE mod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(pc), &mod))
            GetModuleFileNameA(mod, module, MAX_PATH);
        std::string mod_name = module;
        mod_name = mod_name.substr(mod_name.find_last_of("\/") + 1);
        const bool exe = mod == GetModuleHandleA(nullptr);
        if (name.empty()) name = "[" + mod_name + "]";
        by_func[exe ? name : mod_name + "!" + name]++;
        std::string cat = !exe ? mod_name : name.rfind("f_", 0) == 0 ? "guest code (f_*)" : "runtime / host";
        by_cat[cat]++;
        if (exe && name.rfind("f_", 0) == 0) {
            IMAGEHLP_LINE64 line{sizeof(IMAGEHLP_LINE64)};
            DWORD ldisp = 0;
            std::string kind = "?";
            if (SymGetLineFromAddr64(proc, pc, &ldisp, &line)) {
                auto &text = files[line.FileName];
                if (text.empty()) {
                    FILE *f = std::fopen(line.FileName, "rb");
                    if (f) {
                        std::string all;
                        char chunk[65536];
                        size_t n;
                        while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) all.append(chunk, n);
                        std::fclose(f);
                        size_t pos = 0;
                        while (pos <= all.size()) {
                            size_t e = all.find('\n', pos);
                            if (e == std::string::npos) e = all.size();
                            text.push_back(all.substr(pos, e - pos));
                            pos = e + 1;
                        }
                    }
                }
                const std::string l = line.LineNumber >= 1 && line.LineNumber <= text.size() ? text[line.LineNumber - 1] : "";
                kind = l.find("CALL") != std::string::npos ? "calls (spill/reload/check)"
                       : l.find("RET") != std::string::npos ? "returns (spill)"
                       : l.find("PROLOGUE") != std::string::npos ? "prologue (reload)"
                       : l.find("wide") != std::string::npos || l.find("n = ") != std::string::npos ? "flag-setting ops"
                       : l.find("LD") != std::string::npos || l.find("ST") != std::string::npos ? "loads / stores"
                       : "other ops";
            }
            by_line_kind[kind]++;
        }
    }
    auto print = [&](const char *title, std::unordered_map<std::string, int> &m, size_t top) {
        std::vector<std::pair<std::string, int>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second > b.second; });
        log("profile: %s", title);
        for (size_t i = 0; i < v.size() && i < top; i++)
            log("  %5.1f%%  %s", 100.0 * v[i].second / samples.size(), v[i].first.c_str());
    };
    log("profile: %zu samples of the game thread", samples.size());
    print("by category", by_cat, 12);
    print("guest code by kind of generated line (% of all samples)", by_line_kind, 10);
    print("hottest functions", by_func, top_functions);
}

}  // namespace rt::os

namespace rt::os {
void start_background(void (*fn)(void *), void *arg) {
    struct Start { void (*fn)(void *); void *arg; };
    HANDLE h = CreateThread(nullptr, 0, [](void *p) -> DWORD {
        const Start s = *static_cast<Start *>(p);
        delete static_cast<Start *>(p);
        s.fn(s.arg);
        return 0;
    }, new Start{fn, arg}, 0, nullptr);
    if (h) CloseHandle(h);
}
}  // namespace rt::os
