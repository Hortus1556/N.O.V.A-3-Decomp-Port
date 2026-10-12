// The runtime's only operating-system dependencies: os_win.cpp (Win32), os_posix.cpp (Linux,
// macOS), os_switch.cpp, os_3ds.cpp or os_gc.cpp (GameCube). Everything else in the runtime and host is portable C++ and SDL.
#pragma once
#include <cstddef>
#include <cstdint>
#include <ctime>
#if defined(_MSC_VER) && (defined(_M_X64) || defined(__x86_64__))
#include <intrin.h>
#elif defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace rt::os {

// Address space: reserve inaccessible, then commit read/write on demand.
void *reserve(size_t size);
bool commit(void *p, size_t size);
// The contents of whole pages in [p, p + size) are no longer needed (freed guest memory): the
// GameCube's pager drops them instead of keeping them in ARAM; elsewhere nothing happens.
void discard(void *p, size_t size);
// Maps guest [va, va + size) read-only from `path` at `offset`, its pages read when first
// touched (the GameCube's pager); false where the system does not (the caller reads it in).
bool map_file(uint32_t va, uint32_t size, const char *path, uint64_t offset, bool exec = false);

int64_t monotonic_ns();
int64_t realtime_ns();     // since 1970-01-01 UTC
void sleep_ns(int64_t ns);  // high resolution; <= 0 yields
void yield();
// The calling thread runs ahead of normal threads (audio mixing: it sleeps most of the time,
// and must not wait behind busy game threads).
void raise_thread_priority();
int64_t process_cpu_ns();  // user + kernel CPU time of the process
// Calendar conversions (thread-safe). local_time also reports the UTC offset in seconds.
bool local_time(int64_t t, struct tm &out, int32_t &utc_offset);
bool utc_time(int64_t t, struct tm &out);
uint64_t thread_id();
uint32_t process_id();

// A host thread with a large stack reservation (recompiled code nests one host frame per
// guest call). Detached: the system takes it back when it ends (guest pthread_join waits on
// its own, runtime/thread.cpp). False when it cannot start.
bool start_thread(void (*fn)(void *), void *arg, size_t stack_reserve);
// A small detached service thread of the runtime (the log writer): never the game thread's
// core on the Switch, and no C++ library threading (std::thread / call_once hung there).
void start_background(void (*fn)(void *), void *arg);

// Ends the process without running destructors (guest threads may still be running).
[[noreturn]] void exit_process(int code);
// Writes the profile of an instrumented build (-DNOVA3_PGO=generate; runtime/pgo.cpp).
void pgo_flush();

// Fatal faults (access violation / SIGSEGV, illegal instruction, ...) print a report with
// the guest context and a symbolized host stack, then end the process.
void install_crash_handler();
void print_current_stack();
// The calling thread's host return addresses, innermost first, and the name of the function
// holding one (Windows; elsewhere none): the heap profile's call stacks (heap.cpp).
size_t capture_stack(void **pcs, size_t max);
bool host_symbol(const void *pc, char *name, size_t size);
// Working-set profile (NOVA3_WORKING_SET=N, Windows; elsewhere off): every frame all guest
// pages lose their access and a touch gives it back and notes the frame, so every N frames the
// log tells how many distinct pages the last 1, 10, 60 and N frames touched, by region (what a
// demand-paged port must keep resident). Kernel reads and writes of guest memory would fail
// while a page has no access: host I/O copies through a buffer while working_set_on().
void working_set_frame(uint64_t frame);
bool working_set_on();
// Hang watchdog for the calling thread: prints its stack when no frame completed for
// `seconds` (frames are counted by rt::watchdog_frame()).
void start_watchdog(int seconds);
// Benchmarks: CPU time used by the calling thread (-1 if unknown), and a sampling profiler of
// the calling thread (Windows; elsewhere a no-op): report() prints the hottest functions.
int64_t thread_cpu_ns();
// A cheap timestamp for frequent measurements (the CPU's counter: a few cycles), in ticks of
// an unspecified rate: convert with a rate measured against monotonic_ns over a while.
inline uint64_t ticks() {
#if defined(__aarch64__)
    // the physical counter, as libnx's armGetSystemTick reads it: Horizon lets user code read
    // it, while cntvct_el0 trapped (in Citron at least)
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
#elif defined(_M_X64) || defined(__x86_64__)
    return __rdtsc();
#elif defined(__3DS__)
    // svcGetSystemTick (the ARM11's 268 MHz count), without libctru's header
    register uint32_t lo __asm__("r0"), hi __asm__("r1");
    __asm__ volatile("svc 0x28" : "=r"(lo), "=r"(hi) : : "r2", "r3", "r12", "lr", "memory");  // (the kernel clobbers r2, r3: Azahar does not)
    return uint64_t(hi) << 32 | lo;
#elif defined(__GAMECUBE__)
    // the time base (bus clock / 4: 40.5 MHz)
    uint32_t hi, lo, hi2;
    do __asm__ volatile("mftbu %0; mftb %1; mftbu %2" : "=r"(hi), "=r"(lo), "=r"(hi2));
    while (hi != hi2);
    return uint64_t(hi) << 32 | lo;
#else
    return uint64_t(monotonic_ns());
#endif
}
void start_profiler();
void report_profiler();
void start_profiler_continuous(int seconds);  // the calling thread, reported every `seconds`

// Free and total bytes of the file system holding `path` (0 if unknown).
void disk_space(const char *path, uint64_t &free_bytes, uint64_t &total_bytes);

}  // namespace rt::os

#include <mutex>

namespace rt {
// Shared crash-report helpers (runtime.cpp), used by the os_* crash handlers.
std::mutex &report_lock();  // one report at a time: later failing threads wait to be killed
uint64_t watchdog_frames();  // frames completed so far (rt::watchdog_frame())
void report_fault(const char *what, const void *fault_addr, int access);  // access: 0 read, 1 write, -1 n/a
// One stack frame: recompiled guest functions (f_XXXXXXXX / t_XXXXXXXX) are shown as guest
// symbols; `insn` is the /*@addr*/ comment of the source line when known.
void print_frame(int index, const char *symbol, const char *file, unsigned line);
}  // namespace rt
