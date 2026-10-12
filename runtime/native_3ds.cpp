// The New 3DS's native code loader: the game's own ARM code runs on the 3DS's ARM11 (docs/3ds.md).
//
// Instead of the recompiled C of the PC and Switch builds, libGloftNAHM.so is loaded the way
// Android loads it, at a fixed address (kNativeLoadBase: the start of the regular heap, which
// runtime/os_3ds.cpp sets aside), and guest addresses are host addresses (g_mem = 0):
//  * relocations as on Android (R_ARM_RELATIVE + base); function imports point at the assembly
//    gates of runtime/native_3ds_gates.S (one per import slot: rt::call_import with a Cpu filled from the
//    registers), data imports at data_import_address; the plain integer C functions the game
//    calls most (malloc, memcpy, strlen, ...) and setjmp / longjmp go straight to native code;
//    the guest heap is dlmalloc in memory of its own (runtime/guest_heap_3ds.c);
//  * the ARMv6K patch set (tools/v6patch.py, romfs:/v6patch.bin): the ~3% of the game's
//    instructions the ARM11 lacks, each replaced by a branch to a stub just past the image;
//  * the hook table (tools/hooktable.py, build/3ds/native_table.inc): the OVERRIDES of
//    recomp/arm2c.py. A hooked function starts with `ldr pc, [pc, #-4]` + its hook gate; a
//    wrap's `original` runs a trampoline (the displaced instructions, then the function's third);
//  * the Linux kernel user helpers the Thumb-1 Boost.Thread code calls (literal-pool addresses);
//  * then the data cache is flushed, the instruction cache invalidated, and the code pages made
//    read + execute (svcControlProcessMemory, which the exheader grants).
// Host -> game calls (rt::call) jump into the game's code on the calling thread's own stack: a
// guest thread is a host thread with the stack size the game asked for.
#include "os.h"
#include "runtime.h"

#include <3ds.h>

#include <malloc.h>
#include <sys/stat.h>
#include <zlib.h>

#include <algorithm>
#include <functional>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct NativeHook {
    uint32_t link;   // the hooked function
    uint32_t tramp;  // a wrap's trampoline (link address), 0 for a replacement
    uint8_t wrap;
    void *fn;        // void fn(Cpu *) or void fn(Cpu *, uint32_t entry, GuestFn original)
    const char *name;
};
struct KuserFixup {
    uint32_t link;   // a literal-pool word
    uint32_t helper; // the kernel helper address it holds
};
#include "native_table.inc"  // build/3ds, from tools/hooktable.py

uint32_t g_load_base = rt::kNativeLoadBase;

extern "C" {  // runtime/guest_heap_3ds.c
void *dlmalloc(size_t);
void dlfree(void *);
void *dlcalloc(size_t, size_t);
void *dlrealloc(void *, size_t);
void guest_heap_stats(size_t *live, size_t *footprint, size_t *peak_footprint);
// runtime/thread.cpp's direct entries for the game's most frequent thread calls
uint32_t rt_im_getspecific(uint32_t key);
uint32_t rt_im_self(void);
uint32_t rt_im_mutex_lock(uint32_t m);
uint32_t rt_im_mutex_unlock(uint32_t m);
}

extern "C" {
void rt_native_enter(Cpu *cpu, uint32_t target, const uint32_t *stack_args, uint32_t count);
void rt_native_setjmp();
void rt_native_longjmp();
void rt_kuser_cmpxchg();
void rt_kuser_barrier();
void rt_kuser_get_tls();
extern const uint32_t rt_kuser_version;
s32 rt_native_flush_caches_kernel();
s32 rt_native_sctlr_kernel();
s32 rt_native_unaligned_kernel();
volatile u32 rt_native_sctlr;  // written by those two in supervisor mode
extern const uint8_t rt_native_gates[];
extern const uint8_t rt_native_hook_gates[];
}

namespace rt {

namespace {
// The Cpu of the guest code running on this thread: the gates store the registers there. Set by
// rt::call (a host thread calling into the game brings its own Cpu).
thread_local Cpu *t_native_cpu;

// The threads running game code (hang reports: where each one last called the runtime).
struct Running { Cpu *cpu; uint32_t thread; };
std::mutex g_running_lock;
std::vector<Running> g_running;

}  // namespace

// For the watchdog (os_3ds.cpp): each thread in the game's code, with its last import or hook
// (the gates leave the number in r12) and the return address of that call.
void native_report_threads(FILE *out) {
    std::lock_guard<std::mutex> lock(g_running_lock);
    for (const Running &r : g_running)
        std::fprintf(out, "  thread %lu: last gate %lu (%s), from link %08lx, sp %08lx, %llu imports so far\n",
                     (unsigned long)r.thread, (unsigned long)r.cpu->r[12],
                     r.cpu->r[12] < g_import_count ? g_import_names[r.cpu->r[12]] : "?", (unsigned long)(r.cpu->r[14] - g_load_base),
                     (unsigned long)r.cpu->r[13], (unsigned long long)r.cpu->ex_val);
    std::fprintf(out, "guest threads:\n");
    report_threads(out);  // and which mutexes they are blocked on (thread.cpp)
}

// NOVA3_IMPORT_PROFILE=1: the game thread's calls per hook, listed with each perf report
uint32_t *g_hook_counts;
void native_hook_profile_log(int frames) {
    if (!g_hook_counts) {
        if (const char *v = std::getenv("NOVA3_IMPORT_PROFILE"); v && *v == '1') g_hook_counts = new uint32_t[256]();
        return;
    }
    std::vector<std::pair<uint32_t, uint32_t>> top;  // (calls, hook)
    uint64_t total = 0;
    for (uint32_t k = 0; k < 256; k++)
        if (g_hook_counts[k]) {
            top.push_back({g_hook_counts[k], k});
            total += g_hook_counts[k];
            g_hook_counts[k] = 0;
        }
    std::sort(top.begin(), top.end(), std::greater<>());
    std::string text;
    for (size_t k = 0; k < top.size() && k < 12; k++) {
        char item[128];
        std::snprintf(item, sizeof item, "%s%s %.0f", k ? ", " : "", ::kNativeHooks[top[k].second].name, top[k].first / double(frames));
        text += item;
    }
    log("hooks per frame: %.0f: %s", total / double(frames), text.c_str());
}

uint32_t native_gate_address(uint32_t slot) { return uint32_t(uintptr_t(rt_native_gates)) + 12 * slot; }

// ---- the load profile (NOVA3_LOAD_PROFILE=1) ---------------------------------------------------
// The 3DS cannot sample another thread's registers, so the game thread's gates note what it does:
// g_sample_state is 0x10000 + slot inside an import, 0x20000 + k inside hook k (a wrap's original
// included), 0 in the game's own code, whose last gate's return address (the Cpu's lr) tells the
// function that called it. A thread on the system core reads them every millisecond and keeps
// the samples of frames over 100 ms (loads), logged once frames run again for 2 s: by import
// (with the file system request it waits for), hook, and the game's functions. The samples go
// into a table allocated at the start (a load leaves little memory free).
Cpu *g_sampled_cpu;                    // the game thread's Cpu while profiling, else nullptr
volatile uint32_t g_sample_state;

namespace {
struct Sample { uint64_t key; uint32_t frame, total; };
constexpr uint32_t kSampleSlots = 8192;
Sample *g_samples;
uint32_t *g_frame_slots, g_frame_used;  // the slots this frame touched
uint32_t g_slots_used;

void count_sample(uint64_t key) {
    uint32_t h = uint32_t((key ^ (key >> 29)) * 2654435761u) >> 19;  // 13 bits
    for (uint32_t n = 0; n < kSampleSlots; n++, h = (h + 1) & (kSampleSlots - 1)) {
        Sample &s = g_samples[h];
        if (s.key && s.key != key) continue;
        if (!s.key) {
            if (g_slots_used >= kSampleSlots * 3 / 4) return;  // (full: dropped)
            s.key = key;
            g_slots_used++;
        }
        if (!s.frame++) g_frame_slots[g_frame_used++] = h;
        return;
    }
}

void log_load_profile(double seconds, const IoStats &io, const IoStats &io0) {
    std::vector<std::pair<std::string, uint32_t>> by_name;
    std::unordered_map<std::string, size_t> where;
    uint32_t n = 0;
    for (uint32_t i = 0; i < kSampleSlots; i++) {
        Sample &s = g_samples[i];
        if (!s.total) continue;
        const uint32_t st = uint32_t(s.key >> 32) & 0xffffff;
        std::string name;
        if (st >> 16 == 1) {
            static const char *const kinds[] = {"", " (SD read)", " (SD open)", " (SD stat)", " (unpacking)", "", "", ""};
            name = std::string("import ") + ((st & 0xffff) < g_import_count ? g_import_names[st & 0xffff] : "?") + kinds[(s.key >> 56) & 7];
        } else if (st >> 16 == 2) {
            name = std::string("hook ") + ::kNativeHooks[st & 0xffff].name;
        } else {
            const std::string sym = guest_symbol(uint32_t(s.key) - g_load_base);
            name = "game, last gate from " + (sym.empty() ? std::string("?") : sym.substr(0, sym.find('+')));
        }
        auto [it, added] = where.try_emplace(name, by_name.size());
        if (added) by_name.push_back({name, 0});
        by_name[it->second].second += s.total;
        n += s.total;
        s.total = 0;
    }
    std::sort(by_name.begin(), by_name.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    const double ms = 1000.0 / SYSCLOCK_ARM11;
    log("load profile: %.1f s in frames over 100 ms, %u samples; file system: %llu reads, %.1f MB, %.0f ms; %llu opens, %.0f ms; "
        "%llu stats, %.0f ms; %.1f MB unpacked, %.0f ms", seconds, n, (unsigned long long)(io.reads - io0.reads),
        (io.read_bytes - io0.read_bytes) / 1048576.0, (io.read_ticks - io0.read_ticks) * ms, (unsigned long long)(io.opens - io0.opens),
        (io.open_ticks - io0.open_ticks) * ms, (unsigned long long)(io.stats - io0.stats), (io.stat_ticks - io0.stat_ticks) * ms,
        (io.unpacked_bytes - io0.unpacked_bytes) / 1048576.0, (io.unpack_ticks - io0.unpack_ticks) * ms);
    for (size_t i = 0; i < by_name.size() && i < 40; i++)
        log("load profile: %5.1f%%  %s", 100.0 * by_name[i].second / n, by_name[i].first.substr(0, 160).c_str());
}
}  // namespace

void native_start_load_profile() {
    g_samples = static_cast<Sample *>(std::calloc(kSampleSlots, sizeof(Sample)));
    g_frame_slots = static_cast<uint32_t *>(std::calloc(kSampleSlots, sizeof(uint32_t)));
    if (!g_samples || !g_frame_slots) return;
    g_sampled_cpu = current_cpu();
    os::start_background([](void *) {
        uint64_t last_frames = watchdog_frames();
        int64_t frame_start = os::monotonic_ns(), steady_since = 0, long_ns = 0;
        IoStats io0{};
        io_stats(io0);
        for (;;) {
            svcSleepThread(1'000'000);
            const uint32_t state = g_sample_state;
            count_sample(state ? uint64_t(state) << 32 | uint64_t(io_now()) << 56 : g_sampled_cpu->r[14] | 1ull << 63);
            const uint64_t frames = watchdog_frames();
            const int64_t now = os::monotonic_ns();
            if (frames == last_frames) continue;
            const bool slow = now - frame_start > 100'000'000;
            const bool keep = slow || long_ns;  // (else the table holds only this frame's keys: emptied)
            for (uint32_t i = 0; i < g_frame_used; i++) {
                Sample &s = g_samples[g_frame_slots[i]];
                if (slow) s.total += s.frame;
                s.frame = 0;
                if (!keep) s.key = 0, g_slots_used--;
            }
            g_frame_used = 0;
            if (slow) long_ns += now - frame_start, steady_since = 0;
            else if (!steady_since) steady_since = now;
            last_frames = frames, frame_start = now;
            if (!long_ns || !steady_since || now - steady_since < 2'000'000'000) continue;
            IoStats io{};
            io_stats(io);
            log_load_profile(long_ns / 1e9, io, io0);
            std::memset(g_samples, 0, kSampleSlots * sizeof(Sample));
            g_slots_used = 0;
            io0 = io, long_ns = 0;
        }
    }, nullptr);
}

// ---- the guest heap: dlmalloc in memory of its own (runtime/guest_heap_3ds.c) -----------------
uint32_t gmalloc(uint32_t size) { return uint32_t(uintptr_t(dlmalloc(size ? size : 1))); }
void gfree(uint32_t p) { dlfree(reinterpret_cast<void *>(uintptr_t(p))); }
uint32_t gcalloc(uint32_t n, uint32_t size) { return uint32_t(uintptr_t(dlcalloc(n, size))); }
uint32_t grealloc(uint32_t p, uint32_t size) {
    return uint32_t(uintptr_t(dlrealloc(reinterpret_cast<void *>(uintptr_t(p)), size)));
}
uint32_t gstrdup(const char *s) {
    const size_t n = std::strlen(s) + 1;
    const uint32_t p = gmalloc(uint32_t(n));
    std::memcpy(ptr(p), s, n);
    return p;
}
// the guest heap: in use, the most it has taken from the system, what it has taken now
void heap_usage(uint64_t &live, uint64_t &peak, uint64_t &span) {
    size_t l, footprint, max_footprint;
    guest_heap_stats(&l, &footprint, &max_footprint);
    live = l, peak = max_footprint, span = footprint;
}
bool heap_committed(uint32_t addr) { return guest_mapped(addr); }

RT_IMPORT(malloc, gmalloc)
RT_IMPORT(free, gfree)
RT_IMPORT(calloc, gcalloc)
RT_IMPORT(realloc, grealloc)

// ---- memory --------------------------------------------------------------------------------
void init_memory() {}  // g_mem stays 0: a guest address is the host address
void commit(uint32_t, uint32_t) {}

bool guest_mapped(uint32_t addr) {
    MemInfo mi;
    PageInfo pi;
    return R_SUCCEEDED(svcQueryMemory(&mi, &pi, addr)) && mi.state != MEMSTATE_FREE && (mi.perm & MEMPERM_READ);
}

uint32_t map_region(uint32_t size) {
    void *p = memalign(0x1000, size);
    if (!p) fatal("out of memory: map_region(%u)", size);
    std::memset(p, 0, size);
    return uint32_t(uintptr_t(p));
}

// The game runs on the calling thread's own stack: no guest stack of its own. Its top is noted
// at the thread's first call into the game (crash reports scan up to it).
Cpu *new_cpu(uint32_t) {
    auto *cpu = static_cast<Cpu *>(std::calloc(1, sizeof(Cpu)));
    cpu->ex_addr = ~0u;
    return cpu;
}

// ---- host -> game ----------------------------------------------------------------------------
// As on the PC: r0-r3 and r14 change; the callee-saved registers and sp the Cpu held before the
// call are back afterwards (the gates of a nested call overwrite them).
uint32_t call(Cpu *cpu, uint32_t guest_fn, std::initializer_list<uint32_t> args) {
    uint32_t stack[16], n = 0, i = 0;
    for (uint32_t a : args) {
        if (i < 4) cpu->r[i] = a;
        else if (n < 16) stack[n++] = a;
        i++;
    }
    uint32_t keep[10];
    std::memcpy(keep, &cpu->r[4], sizeof keep);
    const bool first = !cpu->r[13];
    if (first) {  // this thread's first call into the game: its stack top
        const uint32_t top = uint32_t(uintptr_t(__builtin_frame_address(0)));
        note_guest_stack(cpu, top);
        keep[9] = cpu->r[13] = top;
        std::lock_guard<std::mutex> lock(g_running_lock);
        g_running.push_back({cpu, uint32_t(os::thread_id())});
    }
    Cpu *outer = t_native_cpu;
    t_native_cpu = cpu;
    const uint32_t state = g_sample_state;
    if (cpu == g_sampled_cpu) g_sample_state = 0;  // the game's own code again (a call from an import)
    rt_native_enter(cpu, guest_fn, stack, n);
    if (cpu == g_sampled_cpu) g_sample_state = state;
    t_native_cpu = outer;
    if (first) {
        std::lock_guard<std::mutex> lock(g_running_lock);
        for (size_t i = 0; i < g_running.size(); i++)
            if (g_running[i].cpu == cpu) { g_running.erase(g_running.begin() + long(i)); break; }
    }
    std::memcpy(&cpu->r[4], keep, sizeof keep);
    cpu->r[14] = cpu->ret_pc = kReturnSentinel;
    return cpu->r[0];
}

uint32_t call_nested(Cpu *cpu, uint32_t guest_fn, std::initializer_list<uint32_t> args) {
    Cpu saved = *cpu;  // the import's register state (arguments, return address)
    const uint32_t ret = call(cpu, guest_fn, args);
    const uint32_t ex_addr = cpu->ex_addr;
    *cpu = saved;
    cpu->ex_addr = ex_addr;
    return ret;
}

namespace {
// ---- hooks -----------------------------------------------------------------------------------
constexpr uint32_t kHookCount = sizeof(::kNativeHooks) / sizeof(::kNativeHooks[0]);
static_assert(kHookCount <= 256, "native_3ds_gates.S has 256 hook gates");

// A wrap's original: the trampoline (the hooked function's first two instructions, then a jump
// to its third) with the registers in the Cpu. Stack arguments: the 16 words at the caller's sp
// are copied to the top of this stack (no hooked function takes more).
void native_original(Cpu *cpu, uint32_t entry) {
    const NativeHook *h = std::lower_bound(std::begin(kNativeHooks), std::end(kNativeHooks), entry,
                                           [](const NativeHook &a, uint32_t l) { return a.link < l; });
    if (h == std::end(kNativeHooks) || h->link != entry || !h->tramp) fatal("hook: no original for %08x", entry);
    uint32_t stack[16];
    std::memcpy(stack, reinterpret_cast<const void *>(uintptr_t(cpu->r[13])), sizeof stack);
    Cpu *outer = t_native_cpu;
    t_native_cpu = cpu;
    const uint32_t lr = cpu->r[14];
    rt_native_enter(cpu, g_load_base + h->tramp, stack, 16);
    t_native_cpu = outer;
    cpu->ret_pc = lr;
}
}  // namespace

}  // namespace rt

extern "C" Cpu *rt_native_cpu() {
    if (Cpu *cpu = rt::t_native_cpu) return cpu;
    rt::fatal("game code running on a thread without a Cpu");
}

extern "C" uint32_t rt_native_tls() { return rt::current_tls(); }

// dlmalloc found a bad free or a damaged chunk (guest_heap_3ds.c): stop (the report scans this
// stack for the game's callers)
extern "C" void guest_heap_error(const char *what, void *p) { rt::fatal("guest heap: %s (%p)", what, p); }

// newlib's heap failed a check (guest_heap_3ds.c; the malloc lock held: a damaged heap is not
// used again): the chunk and the one before it (its overrun is the likeliest cause; host code
// addresses in them name its owner: addr2line -e nova3.elf), then a stop with the caller.
extern "C" void host_heap_error(const char *where, const char *what, const void *block, const void *chunk, const void *prev,
                                const void *caller) {
    rt::log_flush();  // (then straight to the file: the log's buffer would allocate)
    std::fprintf(stderr, "host heap: %s at %s: block %p, chunk %p, called from %p (host: addr2line -e nova3.elf)\n", what, where,
                 block, chunk, caller);
    for (const void *c : {prev, chunk}) {
        if (!c || !rt::guest_mapped(uint32_t(uintptr_t(c))) || !rt::guest_mapped(uint32_t(uintptr_t(c)) + 63)) continue;
        const uint32_t *w = static_cast<const uint32_t *>(c);
        std::fprintf(stderr, "host heap: %s %p:", c == prev ? "chunk before" : "chunk", c);
        for (int i = 0; i < 16; i++) std::fprintf(stderr, " %08lx", (unsigned long)w[i]);
        std::fputc('\n', stderr);
    }
    rt::fatal("host heap: %s at %s", what, where);
}

namespace rt::os {
extern thread_local uint32_t t_stack_limit;  // os_3ds.cpp: this thread's stack bottom (+ a margin)
}

// import gate (native_3ds_gates.S): the import, after a check that the stack the game's code
// shares with the runtime is not about to run out (it would overwrite the thread's own state)
extern "C" void rt_native_import(Cpu *cpu, uint32_t slot) {
    if (cpu->r[13] < rt::os::t_stack_limit) {
        rt::os::t_stack_limit = 0;  // the report's own calls
        rt::log_guest_callers(cpu);
        rt::fatal("stack overflow: sp %08x at an import call from %08x (link %08x)", cpu->r[13], cpu->r[14],
                  cpu->r[14] - g_load_base);
    }
    cpu->ex_val++;  // calls into the runtime (hang reports; the exclusive monitor is the CPU's own here)
    if (cpu != rt::g_sampled_cpu) return rt_call_import(cpu, slot);
    const uint32_t outer = rt::g_sample_state;  // (an import inside a call back into the game)
    rt::g_sample_state = 0x10000 | slot;
    rt_call_import(cpu, slot);
    rt::g_sample_state = outer;
}

// hook gate k (native_3ds_gates.S): the host function, with the hooked function's link address as
// `entry` (a wrap's original finds its trampoline by it)
extern "C" void rt_native_hook(Cpu *cpu, uint32_t k) {
    const NativeHook &h = ::kNativeHooks[k];
    if (rt::g_hook_counts && cpu == rt::main_cpu()) rt::g_hook_counts[k]++;
    const uint32_t outer = rt::g_sample_state;
    if (cpu == rt::g_sampled_cpu) rt::g_sample_state = 0x20000 | k;
    if (h.wrap) reinterpret_cast<void (*)(Cpu *, uint32_t, GuestFn)>(h.fn)(cpu, h.link, rt::native_original);
    else reinterpret_cast<void (*)(Cpu *)>(h.fn)(cpu);
    if (cpu == rt::g_sampled_cpu) rt::g_sample_state = outer;
}

namespace rt {
namespace {

// The game's softfp math: floats and doubles in core registers (pcs("aapcs")), newlib's
// hard-float functions inside.
#define SOFTFP __attribute__((pcs("aapcs")))
#define F1(name, T) SOFTFP T n_##name(T x) { return ::name(x); }
#define F2(name, T) SOFTFP T n_##name(T x, T y) { return ::name(x, y); }
F1(acos, double) F1(asin, double) F1(atan, double) F1(ceil, double) F1(cos, double) F1(cosh, double)
F1(exp, double) F1(floor, double) F1(log, double) F1(log10, double) F1(sin, double) F1(sinh, double)
F1(tan, double) F1(tanh, double)
F2(atan2, double) F2(fmod, double) F2(pow, double)
F1(acosf, float) F1(asinf, float) F1(atanf, float) F1(ceilf, float) F1(cosf, float) F1(expf, float)
F1(floorf, float) F1(log10f, float) F1(logf, float) F1(sinf, float) F1(tanf, float)
F2(atan2f, float) F2(fmodf, float) F2(powf, float)
SOFTFP double n_ldexp(double x, int e) { return ::ldexp(x, e); }
SOFTFP int32_t n_lrintf(float x) { return int32_t(::lrintf(x)); }
SOFTFP double n_frexp(double x, int *e) { return ::frexp(x, e); }
SOFTFP double n_modf(double x, double *i) { return ::modf(x, i); }
#undef F1
#undef F2

// bionic's C locale: ASCII classes and case folding (as runtime/libc_string.cpp)
int n_isalnum(int c) { return unsigned((c | 32) - 'a') < 26u || unsigned(c - '0') < 10u; }
int n_isalpha(int c) { return unsigned((c | 32) - 'a') < 26u; }
int n_iscntrl(int c) { return unsigned(c) < 32u || c == 127; }
int n_islower(int c) { return unsigned(c - 'a') < 26u; }
int n_isupper(int c) { return unsigned(c - 'A') < 26u; }
int n_isprint(int c) { return unsigned(c - 32) < 95u; }
int n_ispunct(int c) { return n_isprint(c) && c != ' ' && !n_isalnum(c); }
int n_isxdigit(int c) { return unsigned(c - '0') < 10u || unsigned((c | 32) - 'a') < 6u; }
int n_tolower(int c) { return n_isupper(c) ? c + 32 : c; }
int n_toupper(int c) { return n_islower(c) ? c - 32 : c; }
int n_strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = uint8_t(a[i]), cb = uint8_t(b[i]);
        ca += (unsigned(ca - 'A') < 26u) << 5;
        cb += (unsigned(cb - 'A') < 26u) << 5;
        if (ca != cb || !ca) return ca - cb;
    }
    return 0;
}
int n_strcasecmp(const char *a, const char *b) { return n_strncasecmp(a, b, size_t(-1)); }

// Imports bound straight to native code: integer-only C functions with the same meaning in
// bionic and newlib (the AAPCS core-register convention is the same for softfp and hard-float
// code), the guest heap's (dlmalloc) included; the math above; the game's most frequent thread
// calls (runtime/thread.cpp's direct entries: the recompiled code calls them the same way);
// and setjmp / longjmp on the game's own registers.
struct NativeImport { const char *name; const void *fn; };
#define N(name) {#name, reinterpret_cast<const void *>(&n_##name)}
const NativeImport kNativeImports[] = {
    {"malloc", reinterpret_cast<const void *>(&dlmalloc)},
    {"free", reinterpret_cast<const void *>(&dlfree)},
    {"calloc", reinterpret_cast<const void *>(&dlcalloc)},
    {"realloc", reinterpret_cast<const void *>(&dlrealloc)},
    {"memcpy", reinterpret_cast<const void *>(&memcpy)},
    {"memmove", reinterpret_cast<const void *>(&memmove)},
    {"memset", reinterpret_cast<const void *>(&memset)},
    {"memcmp", reinterpret_cast<const void *>(&memcmp)},
    {"memchr", reinterpret_cast<const void *>(static_cast<void *(*)(const void *, int, size_t)>(&memchr))},
    {"strlen", reinterpret_cast<const void *>(&strlen)},
    {"strcmp", reinterpret_cast<const void *>(&strcmp)},
    {"strncmp", reinterpret_cast<const void *>(&strncmp)},
    {"strcpy", reinterpret_cast<const void *>(&strcpy)},
    {"strncpy", reinterpret_cast<const void *>(&strncpy)},
    {"strcat", reinterpret_cast<const void *>(&strcat)},
    {"strncat", reinterpret_cast<const void *>(&strncat)},
    {"strchr", reinterpret_cast<const void *>(static_cast<char *(*)(const char *, int)>(&strchr))},
    {"strrchr", reinterpret_cast<const void *>(static_cast<char *(*)(const char *, int)>(&strrchr))},
    {"strstr", reinterpret_cast<const void *>(static_cast<char *(*)(const char *, const char *)>(&strstr))},
    {"setjmp", reinterpret_cast<const void *>(&rt_native_setjmp)},
    {"longjmp", reinterpret_cast<const void *>(&rt_native_longjmp)},
    {"pthread_mutex_lock", reinterpret_cast<const void *>(&rt_im_mutex_lock)},
    {"pthread_mutex_unlock", reinterpret_cast<const void *>(&rt_im_mutex_unlock)},
    {"pthread_getspecific", reinterpret_cast<const void *>(&rt_im_getspecific)},
    {"pthread_self", reinterpret_cast<const void *>(&rt_im_self)},
    N(acos), N(asin), N(atan), N(ceil), N(cos), N(cosh), N(exp), N(floor), N(log), N(log10), N(sin), N(sinh),
    N(tan), N(tanh), N(atan2), N(fmod), N(pow), N(acosf), N(asinf), N(atanf), N(ceilf), N(cosf), N(expf),
    N(floorf), N(log10f), N(logf), N(sinf), N(tanf), N(atan2f), N(fmodf), N(powf), N(ldexp), N(lrintf),
    N(frexp), N(modf),
    N(isalnum), N(isalpha), N(iscntrl), N(islower), N(isupper), N(isprint), N(ispunct), N(isxdigit),
    N(tolower), N(toupper), N(strcasecmp), N(strncasecmp),
};
#undef N

// The GL calls (a few hundred to 3,400 a frame) go straight to runtime/gles.cpp's typed functions
// where their arguments are all words (direct_entry), except in the 1 frame of 8 whose GL time
// the performance report measures (runtime.cpp g_gl_timing_hook): the game's GOT slots for them
// are switched between the two at those frames' ends. NOVA3_IMPORT_PROFILE=1 keeps the gates (it
// counts every call), and so does NOVA3_LOAD_PROFILE=1 (the GL calls show in it).
struct GlSlot { uint32_t where, gate, direct; };
std::vector<GlSlot> g_gl_slots;
void note_gl_slot(const char *name, uint32_t where) {
    static const bool counting = [] {
        const char *v = std::getenv("NOVA3_IMPORT_PROFILE"), *l = std::getenv("NOVA3_LOAD_PROFILE");
        return (v && *v == '1') || (l && *l == '1');
    }();
    if (counting || name[0] != 'g' || name[1] != 'l') return;
    const void *direct = import_direct(name);
    const uint32_t gate = import_function_address(name, false);
    if (direct && gate && LD32(where) == gate) g_gl_slots.push_back({where, gate, uint32_t(uintptr_t(direct))});
}
void bind_gl(bool timed) {
    for (const GlSlot &s : g_gl_slots) ST32(s.where, timed ? s.gate : s.direct);
}

uint32_t native_import(const char *name) {
    for (const NativeImport &n : kNativeImports)
        if (!std::strcmp(n.name, name)) return uint32_t(uintptr_t(n.fn));
    return 0;
}

uint32_t kuser_address(uint32_t helper) {
    switch (helper) {
    case 0xffff0fc0u: return uint32_t(uintptr_t(&rt_kuser_cmpxchg));
    case 0xffff0fa0u: return uint32_t(uintptr_t(&rt_kuser_barrier));
    case 0xffff0fe0u: return uint32_t(uintptr_t(&rt_kuser_get_tls));
    case 0xffff0ffcu: return uint32_t(uintptr_t(&rt_kuser_version));
    }
    fatal("kernel user helper %08x is not implemented", helper);
}

// The library's compressed copy (libGloftNAHM.so.z, tools/3ds_slimdata.py lib): 11 MB to read from
// the SD card instead of 26 (the plain read took 4.8 s of the boot in Azahar, whose SD card costs
// 183 ns a byte). Its segments come in raw deflate pieces of 256 KB, inflated on this thread while
// a reader thread fetches the next ones (the SD card transfers without the CPU). It is used when
// the size it notes is the library's, or alone when the plain library is not there (the console
// package leaves it out: 26 MB); with the plain library present, anything failing reads that.
struct LibZ {
    struct Segment { uint32_t offset, size, first, count; };
    FILE *f = nullptr;
    uint32_t piece = 0;
    std::vector<uint8_t> head;                          // the ELF header and the program headers
    std::vector<Segment> segments;                      // PT_LOAD: file offset, size, its pieces
    std::vector<std::pair<uint32_t, uint32_t>> pieces;  // where each piece is, its size
};
bool open_libz(const char *path, LibZ &z) {
    struct stat st;
    const bool plain = ::stat(path, &st) == 0;
    FILE *f = std::fopen((std::string(path) + ".z").c_str(), "rb");
    if (!f) return false;
    std::setvbuf(f, nullptr, _IONBF, 0);
    std::vector<uint8_t> b(16384);  // the header and tables (about 1 KB)
    b.resize(std::fread(b.data(), 1, b.size(), f));
    size_t at = 0;
    const auto u32 = [&b, &at](uint32_t &v) {
        if (at + 4 > b.size()) return false;
        std::memcpy(&v, &b[at], 4);
        at += 4;
        return true;
    };
    uint32_t magic = 0, version = 0, size = 0, head = 0, n = 0;
    bool ok = u32(magic) && u32(version) && u32(size) && u32(z.piece) && u32(head) && magic == 0x5a4c334e /*'N3LZ'*/ &&
              version == 1 && (!plain || size == uint32_t(st.st_size)) && z.piece && at + head <= b.size();
    if (ok) z.head.assign(b.begin() + long(at), b.begin() + long(at + head)), at += head;
    ok = ok && u32(n) && n < 64;
    for (uint32_t i = 0; ok && i < n; i++) {
        LibZ::Segment s;
        ok = u32(s.offset) && u32(s.size) && u32(s.first) && u32(s.count);
        z.segments.push_back(s);
    }
    ok = ok && u32(n) && n < 65536;
    for (uint32_t i = 0; ok && i < n; i++) {
        uint32_t p, len;
        ok = u32(p) && u32(len);
        z.pieces.push_back({p, len});
    }
    for (const LibZ::Segment &s : z.segments)
        ok = ok && s.first + s.count <= z.pieces.size() && uint64_t(s.count) * z.piece >= s.size;
    if (!ok) {
        log("native: %s.z does not match the library: not used", path);
        std::fclose(f);
        return false;
    }
    z.f = f;
    return true;
}
// The reader fills slots ahead of the inflater; the two hand them over through counters, polled
// every 0.1 ms (the arbiter-based waits once lost a wakeup in Azahar: Status, the title-menu hang).
struct PieceReader {
    static constexpr uint32_t kSlots = 3;
    const LibZ *z;
    const LibZ::Segment *s;
    std::vector<uint8_t> buf[kSlots];
    uint32_t got[kSlots];
    std::atomic<uint32_t> read{0}, used{0};  // pieces read, pieces inflated
};
void read_pieces(void *arg) {
    auto &r = *static_cast<PieceReader *>(arg);
    for (uint32_t k = 0; k < r.s->count; k++) {
        while (k - r.used.load(std::memory_order_acquire) >= PieceReader::kSlots) svcSleepThread(100'000);
        const auto [at, len] = r.z->pieces[r.s->first + k];
        std::vector<uint8_t> &b = r.buf[k % PieceReader::kSlots];
        r.got[k % PieceReader::kSlots] =
            len <= b.size() && std::fseek(r.z->f, long(at), SEEK_SET) == 0 ? uint32_t(std::fread(b.data(), 1, len, r.z->f)) : 0;
        r.read.store(k + 1, std::memory_order_release);
    }
}
// one segment into place: false if a piece is missing or damaged
bool inflate_segment(const LibZ &z, const LibZ::Segment &s, uint8_t *dst) {
    PieceReader r{&z, &s, {}, {}};
    uint32_t largest = 0;
    for (uint32_t k = 0; k < s.count; k++) largest = std::max(largest, z.pieces[s.first + k].second);
    for (auto &b : r.buf) b.resize(largest);
    // above this thread on its core: it only issues a request and waits for it
    Thread reader = threadCreate(read_pieces, &r, 16 * 1024, 0x2f, 0, false);
    if (!reader) return false;
    z_stream zs{};
    bool ok = inflateInit2(&zs, -15) == Z_OK;
    for (uint32_t k = 0; k < s.count; k++) {
        while (r.read.load(std::memory_order_acquire) <= k) svcSleepThread(100'000);
        const uint32_t slot = k % PieceReader::kSlots, at = k * z.piece, len = std::min(z.piece, s.size - at);
        if (ok) {
            inflateReset(&zs);
            zs.next_in = r.buf[slot].data(), zs.avail_in = r.got[slot];
            zs.next_out = dst + at, zs.avail_out = len;
            ok = r.got[slot] == z.pieces[s.first + k].second && inflate(&zs, Z_FINISH) == Z_STREAM_END && zs.avail_out == 0;
        }
        r.used.store(k + 1, std::memory_order_release);  // (to the end even after a failure: the reader finishes)
    }
    threadJoin(reader, U64_MAX);
    threadFree(reader);
    inflateEnd(&zs);
    return ok;
}

// reads `size` bytes at `offset` of an open file into host memory, or fails the load
void read_at(FILE *f, uint32_t offset, void *dst, uint32_t size, const char *what) {
    if (std::fseek(f, long(offset), SEEK_SET) != 0 || std::fread(dst, 1, size, f) != size)
        fatal("cannot read %s (%u bytes at %u)", what, size, offset);
}

// The ARMv6K patch set: the stubs go past the image, then each ARMv7 instruction becomes a
// branch to its stub. Its Thumb island (functions copied among the stubs) and their unwind table
// go into lib, for the game's unwinder (__gnu_Unwind_Find_exidx).
uint32_t apply_patch_set(const char *path, LoadedLib &lib) {
    FILE *f = std::fopen(path, "rb");
    if (!f) fatal("cannot open %s (the ARMv6K patch set, tools/v6patch.py)", path);
    uint32_t head[4];
    read_at(f, 0, head, sizeof head, path);
    if (std::memcmp(head, "N3V6", 4)) fatal("%s: not a patch set", path);
    const uint32_t stub_base = head[1], stub_size = head[2], count = head[3];
    if (stub_base + stub_size > kTrampolineBase) fatal("%s: the stubs overlap the hook trampolines", path);
    read_at(f, 16, ptr(g_load_base + stub_base), stub_size, path);
    std::vector<uint32_t> pairs(2 * 4096);
    for (uint32_t done = 0; done < count;) {
        const uint32_t n = std::min<uint32_t>(4096, count - done);
        if (std::fread(pairs.data(), 8, n, f) != n) fatal("%s: truncated", path);
        for (uint32_t i = 0; i < n; i++) ST32(g_load_base + pairs[2 * i], pairs[2 * i + 1]);
        done += n;
    }
    uint32_t island[4];  // start, code end, unwind table, its entries
    if (std::fread(island, 4, 4, f) != 4) fatal("%s: no Thumb island record (an older tools/v6patch.py)", path);
    std::fclose(f);
    lib.moved = g_load_base + island[0], lib.moved_end = g_load_base + island[1];
    lib.moved_exidx = g_load_base + island[2], lib.moved_exidx_count = island[3];
    log("native: %u instructions patched for the ARM11, %u KB of stubs, %u Thumb functions moved", count, stub_size >> 10,
        island[3] ? island[3] - 1 : 0);
    return stub_base;
}

// The kernel's cache maintenance (svcBackdoor) on every core the game's code can run on: each
// ARM11 core has its own instruction cache. Core 0 runs the game thread (this one), core 2 the
// guest threads, core 1 only host code, but a guest thread may land there when core 2 refuses.
void flush_caches_here(void *) { svcBackdoor(rt_native_flush_caches_kernel); }
// Unaligned access the ARMv7 way on this core (see native_3ds_gates.S): logged, set if it is not
void unaligned_here(void *core) {
    rt_native_sctlr = 0;
    svcBackdoor(rt_native_sctlr_kernel);
    const u32 before = rt_native_sctlr;
    if (before && (!(before & (1u << 22)) || (before & 2))) svcBackdoor(rt_native_unaligned_kernel);
    log("cpu: core %d control register %08lx%s", int(intptr_t(core)), (unsigned long)before,
        !before ? " (not readable)"
        : rt_native_sctlr != before ? " -> unaligned access turned on" : " (unaligned access on)");
}
void on_each_core(ThreadFunc fn, const char *what) {
    for (int core : {0, 1, 2}) {
        if (Thread t = threadCreate(fn, reinterpret_cast<void *>(intptr_t(core)), 4096, 0x18, core, false)) {
            threadJoin(t, U64_MAX);
            threadFree(t);
        } else {
            log("native: no thread on core %d for %s", core, what);
        }
    }
}
void flush_caches_all_cores() {
    flush_caches_here(nullptr);
    on_each_core(flush_caches_here, "the cache flush");
}

// Code pages: the data cache written back, the instruction cache invalidated, then read +
// execute. On a console svcBackdoor runs the cache maintenance in the kernel; emulators keep
// no caches (and Azahar has no svcBackdoor).
std::vector<std::pair<uint32_t, uint32_t>> g_code;  // the pages made executable (native_stop_code)

void make_executable(uint32_t lo, uint32_t hi, bool emulator) {
    lo &= ~0xfffu;
    hi = (hi + 0xfff) & ~0xfffu;
    Handle self = 0;
    Result rc = svcDuplicateHandle(&self, CUR_PROCESS_HANDLE);
    if (R_SUCCEEDED(rc)) rc = svcFlushProcessDataCache(self, lo, hi - lo);
    if (R_SUCCEEDED(rc))
        rc = svcControlProcessMemory(self, lo, 0, hi - lo, MEMOP_PROT, MemPerm(MEMPERM_READ | MEMPERM_EXECUTE));
    if (self) svcCloseHandle(self);
    g_code.push_back({lo, hi});
    if (R_FAILED(rc)) {
        if (!emulator) fatal("cannot make the game's code executable (%08lx): the .cia's exheader grants svcControlProcessMemory", rc);
        log("native: svcControlProcessMemory %08x-%08x failed (%08lx); the emulator executes it anyway", lo, hi, rc);
    }
}

}  // namespace

// For the watchdog's hang probe (os_3ds.cpp): the game's code (.text, the stubs, the Thumb island
// and the trampolines) readable only, so a thread running it faults at its next instruction there
// (no thread can read another's registers on a 3DS).
bool native_stop_code() {
    Handle self = 0;
    Result rc = svcDuplicateHandle(&self, CUR_PROCESS_HANDLE);
    for (const auto &[lo, hi] : g_code)
        if (R_SUCCEEDED(rc)) rc = svcControlProcessMemory(self, lo, 0, hi - lo, MEMOP_PROT, MEMPERM_READ);
    if (self) svcCloseHandle(self);
    return R_SUCCEEDED(rc) && !g_code.empty();
}

// Citra / Azahar answer system info type 0x20000 with 1. Success alone is not the test: on a
// console (Luma3DS) the call succeeds too, and taking the console for an emulator skipped the
// instruction-cache invalidate: the first start ran stale instructions (the pages had held
// another program's code) and stopped in the static constructors.
bool native_emulator() {
    static const bool emu = [] {
        s64 v = 0;
        return R_SUCCEEDED(svcGetSystemInfo(&v, 0x20000, 0)) && v == 1;
    }();
    return emu;
}

LoadedLib load_library(const char *path) {
    const int64_t t0 = os::monotonic_ns();
    const auto secs = [t0] { return (os::monotonic_ns() - t0) / 1e9; };
    LibZ z;
    const bool packed = open_libz(path, z);
    FILE *f = nullptr;
    // a part of the library file: from the compressed copy when it has it (the headers, whole
    // segments), else from the library
    const auto read_part = [&](uint32_t offset, void *dst, uint32_t size) {
        if (packed && offset + size <= z.head.size()) return void(std::memcpy(dst, z.head.data() + offset, size));
        if (packed)
            for (const LibZ::Segment &s : z.segments)
                if (s.offset == offset && s.size == size) {
                    if (inflate_segment(z, s, static_cast<uint8_t *>(dst))) return;
                    log("native: %s.z is damaged: the library is read instead", path);
                }
        if (!f && !(f = std::fopen(path, "rb"))) fatal("cannot open %s", path);
        read_at(f, offset, dst, size, path);
    };
    uint8_t eh[52];
    read_part(0, eh, sizeof eh);
    auto u32at = [](const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return v; };
    auto u16at = [](const uint8_t *p) { uint16_t v; std::memcpy(&v, p, 2); return v; };
    if (std::memcmp(eh, "\x7f" "ELF\x01\x01", 6)) fatal("%s: not an ELF32 LE file", path);
    const uint32_t phoff = u32at(eh + 0x1c), phnum = u16at(eh + 0x2c);
    std::vector<uint8_t> ph(phnum * 32);
    read_part(phoff, ph.data(), uint32_t(ph.size()));

    LoadedLib lib{};
    const uint32_t base = g_load_base;
    uint32_t dyn = 0, dyn_size = 0, text_end = 0;
    for (uint32_t i = 0; i < phnum; i++) {
        const uint8_t *p = &ph[i * 32];
        const uint32_t type = u32at(p), off = u32at(p + 4), va = u32at(p + 8), filesz = u32at(p + 16),
                       memsz = u32at(p + 20), flags = u32at(p + 24);
        if (type == 1) {  // PT_LOAD
            if (va + memsz > kTrampolineBase) fatal("%s does not fit below the patch stubs", path);
            read_part(off, ptr(base + va), filesz);
            std::memset(ptr(base + va + filesz), 0, memsz - filesz);
            lib.end = std::max(lib.end, base + va + memsz);
            if (flags & 1) text_end = std::max(text_end, va + memsz);  // PF_X
        } else if (type == 2) {
            dyn = va;
            dyn_size = filesz;
        } else if (type == 0x70000001) {  // PT_ARM_EXIDX
            lib.exidx = base + va;
            lib.exidx_count = memsz / 8;
        }
    }
    if (f) std::fclose(f);
    if (z.f) std::fclose(z.f);
    log("native: %s loaded at %08x-%08x (%.2f s%s)", path, base, lib.end, secs(), packed ? ", from the compressed copy" : "");

    // the dynamic section, symbols and relocations are inside the loaded image
    uint32_t rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0, symtab = 0, strtab = 0, hash = 0;
    for (uint32_t o = dyn; o < dyn + dyn_size; o += 8) {
        const int32_t tag = int32_t(LD32(base + o));
        const uint32_t val = LD32(base + o + 4);
        switch (tag) {
        case 17: rel = val; break;
        case 18: relsz = val; break;
        case 23: jmprel = val; break;
        case 2: pltrelsz = val; break;
        case 6: symtab = val; break;
        case 5: strtab = val; break;
        case 4: hash = val; break;
        case 25: lib.init_array = base + val; break;
        case 27: lib.init_array_count = val / 4; break;
        }
    }
    auto sym_name = [&](uint32_t i) { return str(base + strtab + LD32(base + symtab + i * 16)); };
    auto sym_value = [&](uint32_t i) { return LD32(base + symtab + i * 16 + 4); };
    auto sym_shndx = [&](uint32_t i) { return LD16(base + symtab + i * 16 + 14); };
    uint32_t direct = 0, gated = 0;
    auto resolve = [&](uint32_t si) {
        if (sym_shndx(si)) return base + sym_value(si);
        const char *name = sym_name(si);
        if (uint32_t a = native_import(name)) { direct++; return a; }
        if (uint32_t a = data_import_address(name)) return a;
        gated++;
        return import_function_address(name, true);  // its gate (an unimplemented one traps when called)
    };
    auto apply = [&](uint32_t table, uint32_t size) {
        for (uint32_t o = table; o < table + size; o += 8) {
            const uint32_t where = base + LD32(base + o), info = LD32(base + o + 4), type = info & 0xff;
            switch (type) {
            case 23: ST32(where, LD32(where) + base); break;               // R_ARM_RELATIVE
            case 2: ST32(where, LD32(where) + resolve(info >> 8)); break;  // R_ARM_ABS32
            case 21: case 22:                                               // R_ARM_GLOB_DAT, R_ARM_JUMP_SLOT
                ST32(where, resolve(info >> 8));
                if (!sym_shndx(info >> 8)) note_gl_slot(sym_name(info >> 8), where);
                break;
            default: fatal("unsupported relocation type %u", type);
            }
        }
    };
    apply(rel, relsz);
    apply(jmprel, pltrelsz);
    if (!g_gl_slots.empty()) {
        g_gl_timing_hook = bind_gl;
        log("native: %zu GL import slots called directly outside timed frames", g_gl_slots.size());
    }
    log("native: relocations applied (%u imports bound to native functions, %u through gates; %.2f s)", direct, gated, secs());
    if (!hash) fatal("the library has no ELF hash table (DT_HASH)");
    lib.dynsym = base + symtab, lib.dynstr = base + strtab, lib.hash = base + hash;  // exports (LoadedLib)

    // the patch set, hooks, kernel helpers
    const uint32_t stub_base = apply_patch_set("romfs:/v6patch.bin", lib);
    std::memcpy(ptr(base + kTrampolineBase), kTrampolines, kTrampolineSize);
    if (kTrampolineBase + kTrampolineSize > kNativeImageReserve) fatal("the image reserve is too small");
    for (uint32_t k = 0; k < kHookCount; k++) {
        const uint32_t at = base + kNativeHooks[k].link;
        ST32(at, 0xe51ff004u);  // ldr pc, [pc, #-4]
        ST32(at + 4, uint32_t(uintptr_t(rt_native_hook_gates)) + 8 * k);
    }
    for (const KuserFixup &k : kKuserFixups) ST32(base + k.link, kuser_address(k.helper));
    log("native: %u hooks, %u kernel helper references (%.2f s)", kHookCount, uint32_t(sizeof(kKuserFixups) / sizeof(kKuserFixups[0])), secs());

    const bool emulator = native_emulator();
    make_executable(base, base + text_end, emulator);
    make_executable(base + stub_base, base + kTrampolineBase + kTrampolineSize, emulator);
    // what is in memory once loaded, patched and hooked: the same on every machine for one build,
    // so a console's log can be compared with Azahar's (a mismatch: the loading went differently)
    const auto crc = [](uint32_t lo, uint32_t hi) {
        return uint32_t(crc32(0, reinterpret_cast<const Bytef *>(uintptr_t(lo)), hi - lo));
    };
    log("native: code crc %08x (text), %08x (data), %08x (stubs and trampolines)", crc(base, base + text_end),
        crc(base + text_end, lib.end), crc(base + stub_base, base + kTrampolineBase + kTrampolineSize));
    if (!emulator) {
        on_each_core(unaligned_here, "the unaligned-access setting");
        flush_caches_all_cores();
    }
    lib.base = base;
    return lib;
}

}  // namespace rt
