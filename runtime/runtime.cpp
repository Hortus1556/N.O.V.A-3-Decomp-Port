// Core runtime: guest address space, ELF loading, imports, indirect-call dispatch, traps.
#include "runtime.h"
#include "tls.h"

#include "os.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#ifdef __3DS__
#include <unistd.h>  // fsync (the log)
#endif

uint8_t *g_mem;
// Import slots: [0, g_import_count) are the PLT imports the recompiled code calls by
// index; further slots are created on demand (dlsym, data-free function lookups).
ImportFn g_import_fns[kMaxImportSlots];

namespace rt {

static uint32_t g_brk;  // next free guest address for map_region
static const LoadedLib *g_crash_lib;  // the game library (crash reports, guest_mapped)
static std::mutex g_brk_lock;

// One report at a time: the first failing thread prints, the others wait to be killed.
std::mutex &report_lock() {
    static std::mutex m;
    return m;
}

static void (*g_fatal_notify)(const char *message);
void set_fatal_notify(void (*fn)(const char *message)) { g_fatal_notify = fn; }

[[noreturn]] void fatal(const char *fmt, ...) {
    log_flush();
    report_lock().lock();
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    const uint32_t entry = guest_thread_entry();
    if (entry) std::fprintf(stderr, "[thread %llu, guest thread from %08x] FATAL: %s\n", (unsigned long long)os::thread_id(), entry - g_load_base, msg);
    else std::fprintf(stderr, "[thread %llu, game thread] FATAL: %s\n", (unsigned long long)os::thread_id(), msg);
    os::print_current_stack();
    std::fflush(stderr);
    if (g_fatal_notify) g_fatal_notify(msg);
    os::exit_process(3);
}

// Logging is asynchronous: rt::log appends the line to a buffer, and a background thread
// writes the buffer out every 100 ms. A write per line stalled whichever thread logged: the
// game thread, and Vox's mixer, which logs from inside its locks (the sound broke up). On the
// Switch every line was an SD card write of milliseconds. Fatal errors, crashes and exits
// flush first (log_flush), so nothing is lost.
// On the 3DS the log's size on the SD card changes only when FS flushes the file (fsync): a
// console that froze hard and was switched off kept nothing after the last one. The writer
// flushes it at most once a second, log_flush every time.
namespace {
std::mutex g_log_lock;    // the pending text
std::mutex g_write_lock;  // writing it out: one writer at a time, in order
std::string g_log_buf;
constexpr size_t kLogMax = size_t(4) << 20;  // pending text kept at most (then lines are dropped)

void write_pending(bool sync) {
    std::string out;
    {
        std::lock_guard<std::mutex> lock(g_log_lock);
        out.swap(g_log_buf);
    }
    static bool unsynced = false;
    if (!out.empty()) {
        std::fwrite(out.data(), 1, out.size(), stderr);
        std::fflush(stderr);
        unsynced = true;
    }
#ifdef __3DS__
    static int64_t last_sync;
    const int64_t now = os::monotonic_ns();
    if (unsynced && (sync || now - last_sync >= 1'000'000'000)) {
        fsync(fileno(stderr));
        unsynced = false;
        last_sync = now;
    }
#else
    (void)sync;
#endif
}
}  // namespace

void log_flush() {
    // from crash paths too: never wait long for a thread that may be stuck mid-write
    for (int i = 0; i < 200; i++) {
        if (g_write_lock.try_lock()) {
            write_pending(true);
            g_write_lock.unlock();
            return;
        }
        os::sleep_ns(1'000'000);
    }
}

void log(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    n = std::clamp(n, 0, int(sizeof buf) - 2);
    buf[n] = '\n';
    static std::atomic<bool> started{false};
    if (!started.exchange(true))
        os::start_background([](void *) {  // a plain sleep: wakes every 100 ms to write
            for (;;) {
                os::sleep_ns(100'000'000);
                std::lock_guard<std::mutex> write(g_write_lock);
                write_pending(false);
            }
        }, nullptr);
    std::lock_guard<std::mutex> lock(g_log_lock);
    if (g_log_buf.size() < kLogMax) g_log_buf.append(buf, size_t(n) + 1);
}

#ifndef __3DS__  // the 3DS's memory, loader, dispatch and host -> game calls: runtime/native_3ds.cpp
void init_memory() {
#ifdef __GAMECUBE__
    // guest addresses are host addresses, mapped page by page by the pager (os_gc.cpp): g_mem
    // stays 0 and commit() maps zero pages
    static bool done;
    if (done) return;
    done = true;
#else
    if (g_mem) return;
    // Reserve the whole 32-bit guest space; regions are committed on demand.
    g_mem = static_cast<uint8_t *>(os::reserve(size_t(1) << 32));
    if (!g_mem) fatal("cannot reserve 4 GiB guest address space");
#endif
    g_brk = kRegionBase;
    // ARM vector page: __kuser_helper_version at 0xffff0ffc (5 = cmpxchg64 available)
    commit(0xffff0000u, 0x1000);
    ST32(0xffff0ffcu, 5);
}

void commit(uint32_t addr, uint32_t size) {
    uint32_t lo = addr & ~0xfffu;
    uint64_t hi = (uint64_t(addr) + size + 0xfff) & ~uint64_t(0xfff);
    if (!os::commit(g_mem + lo, size_t(hi - lo))) fatal("commit %08x+%x failed", addr, size);
}

bool guest_mapped(uint32_t addr) {
    {
        std::lock_guard<std::mutex> lock(g_brk_lock);
        if (addr >= kRegionBase && addr < g_brk) return true;
    }
    if (g_crash_lib && addr >= g_crash_lib->base && addr < g_crash_lib->end) return true;
    return heap_committed(addr) || addr >= 0xffff0000u;
}

uint32_t map_region(uint32_t size) {
    std::lock_guard<std::mutex> lock(g_brk_lock);
    size = (size + 0xffff) & ~0xffffu;
    uint32_t a = g_brk;
    if (uint64_t(a) + size > g_load_base) fatal("guest region space exhausted");
    commit(a, size);
    g_brk += size;
    return a;
}
#endif

// ---- import registry ------------------------------------------------------------

struct ImportTable {
    std::unordered_map<std::string, ImportFn> impls;
    std::unordered_map<std::string, const void *> directs;  // the 3DS: direct_entry (runtime.h)
    std::unordered_map<std::string, DataInit> data_inits;
    std::unordered_map<std::string, uint32_t> data_addrs;
    std::unordered_map<std::string, uint32_t> slots;  // name -> slot index
    std::vector<std::string> slot_names;
    std::mutex lock;
};
static ImportTable &imports() {
    static ImportTable t;
    return t;
}

void register_import(const char *name, ImportFn fn, const void *direct) {
    imports().impls[name] = fn;
    if (direct) imports().directs[name] = direct;
}
const void *import_direct(const char *name) {
    auto &t = imports();
    std::lock_guard<std::mutex> lock(t.lock);
    auto it = t.directs.find(name);
    return it == t.directs.end() ? nullptr : it->second;
}

void set_import(const char *name, ImportFn fn) {
    auto &t = imports();
    std::lock_guard<std::mutex> lock(t.lock);
    t.impls[name] = fn;
    t.directs.erase(name);  // (bound at load time: a replacement goes through the gate)
    auto it = t.slots.find(name);
    if (it != t.slots.end()) g_import_fns[it->second] = fn;
}
void register_data_import(const char *name, DataInit init) { imports().data_inits[name] = init; }

static RT_THREAD_LOCAL(uint32_t, t_last_import);
static void unresolved_import(Cpu *) {
    auto &t = imports();
    fatal("unimplemented import called: %s",
          RT_TLS(t_last_import) < t.slot_names.size() ? t.slot_names[RT_TLS(t_last_import)].c_str() : "?");
}

// the address the game calls an import slot at: a fake one the recompiled code dispatches on the
// PC, the slot's gate on the 3DS (native code)
static uint32_t import_address(uint32_t slot) {
#ifdef __3DS__
    return native_gate_address(slot);
#else
    return kImportBase + slot * 4;
#endif
}

static void init_static_slots() {
    auto &t = imports();
    if (!t.slot_names.empty()) return;
    std::string missing;
    for (uint32_t i = 0; i < g_import_count; i++) {
        t.slot_names.push_back(g_import_names[i]);
        t.slots[g_import_names[i]] = i;
        auto it = t.impls.find(g_import_names[i]);
        g_import_fns[i] = it != t.impls.end() ? it->second : unresolved_import;
        if (it == t.impls.end()) missing += std::string(" ") + g_import_names[i];
    }
    if (!missing.empty()) log("imports without an implementation (trap if called):%s", missing.c_str());
}

uint32_t import_function_address(const char *name, bool create) {
    auto &t = imports();
    std::lock_guard<std::mutex> lock(t.lock);
    init_static_slots();
    auto it = t.slots.find(name);
    if (it != t.slots.end()) return import_address(it->second);
    auto impl = t.impls.find(name);
    if (impl == t.impls.end() && !create) return 0;
    uint32_t i = uint32_t(t.slot_names.size());
    if (i >= kMaxImportSlots) fatal("import slot table full");
    t.slot_names.push_back(name);
    t.slots[name] = i;
    g_import_fns[i] = impl != t.impls.end() ? impl->second : unresolved_import;
    return import_address(i);
}

uint32_t data_import_address(const char *name) {
    auto &t = imports();
    std::lock_guard<std::mutex> lock(t.lock);
    auto it = t.data_addrs.find(name);
    if (it != t.data_addrs.end()) return it->second;
    auto init = t.data_inits.find(name);
    if (init == t.data_inits.end()) return 0;
    uint32_t a = init->second(name);
    t.data_addrs[name] = a;
    return a;
}

static uint32_t resolve_symbol(const char *name) {
    if (uint32_t a = data_import_address(name)) return a;
    return import_function_address(name, true);  // unimplemented ones trap when called
}

// ---- ELF loading --------------------------------------------------------------------

#ifndef __3DS__
LoadedLib load_library(const char *path) {
    // Segments go straight to guest memory; the tables (dynamic section, symbols, relocations)
    // are then read there. A read-only segment is mapped from the file where the system can page
    // it in (os::map_file: the GameCube, whose 24 MB could not hold the library's 25 MB): its
    // pages load when first touched (the game reads 0.6 MB of it, docs/gamecube.md).
    std::ifstream f(path, std::ios::binary);
    if (!f) fatal("cannot open %s", path);
    uint8_t head[4096] = {};
    f.read(reinterpret_cast<char *>(head), sizeof head);
    auto h32 = [&](size_t o) { uint32_t v; std::memcpy(&v, head + o, 4); return uint32_t(LE32(v)); };
    auto h16 = [&](size_t o) { uint16_t v; std::memcpy(&v, head + o, 2); return uint16_t(LE16(v)); };
    if (std::memcmp(head, "\x7f" "ELF\x01\x01", 6)) fatal("%s: not an ELF32 LE file", path);

    {
        std::lock_guard<std::mutex> lock(imports().lock);
        init_static_slots();
    }
    LoadedLib lib{};
    uint32_t phoff = h32(0x1c), phnum = h16(0x2c), dyn_va = 0, dyn_size = 0;
    if (phoff + phnum * 32 > sizeof head) fatal("%s: program headers past the first page", path);
    for (uint32_t i = 0; i < phnum; i++) {
        size_t p = phoff + i * 32;
        uint32_t type = h32(p), off = h32(p + 4), va = h32(p + 8), filesz = h32(p + 16), memsz = h32(p + 20),
                 flags = h32(p + 24);
        if (type == 1) {  // PT_LOAD
            const bool mapped = !(flags & 2) && filesz == memsz && os::map_file(g_load_base + va, memsz, path, off);
            if (!mapped) {
                commit(g_load_base + va, memsz);
                f.clear();
                f.seekg(std::streamoff(off));
                f.read(ptr(g_load_base + va), std::streamsize(filesz));
                if (uint32_t(f.gcount()) != filesz) fatal("%s: short read of a segment", path);
            }
            lib.end = std::max(lib.end, g_load_base + va + memsz);
        } else if (type == 2) {
            dyn_va = va;
            dyn_size = filesz;
        } else if (type == 0x70000001) {  // PT_ARM_EXIDX
            lib.exidx = g_load_base + va;
            lib.exidx_count = memsz / 8;
        }
    }
    auto u32 = [&](uint32_t va) { return LD32(g_load_base + va); };
    auto u16 = [&](uint32_t va) { return uint16_t(LD16(g_load_base + va)); };
    uint32_t rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0, symtab = 0, strtab = 0;
    for (uint32_t o = dyn_va; o < dyn_va + dyn_size; o += 8) {
        int32_t tag = int32_t(u32(o));
        uint32_t val = u32(o + 4);
        switch (tag) {
        case 17: rel = val; break;
        case 18: relsz = val; break;
        case 23: jmprel = val; break;
        case 2: pltrelsz = val; break;
        case 6: symtab = val; break;
        case 5: strtab = val; break;
        case 4: lib.hash = g_load_base + val; break;
        case 25: lib.init_array = g_load_base + val; break;
        case 27: lib.init_array_count = val / 4; break;
        }
    }
    auto sym_name = [&](uint32_t idx) { return str(g_load_base + strtab + u32(symtab + idx * 16)); };
    auto sym_value = [&](uint32_t idx) { return u32(symtab + idx * 16 + 4); };
    auto sym_shndx = [&](uint32_t idx) { return u16(symtab + idx * 16 + 14); };

    auto apply = [&](uint32_t table, uint32_t size) {
        for (uint32_t o = table; o < table + size; o += 8) {
            uint32_t where = g_load_base + u32(o), info = u32(o + 4), type = info & 0xff, si = info >> 8;
            switch (type) {
            case 23:  // R_ARM_RELATIVE
                ST32(where, LD32(where) + g_load_base);
                break;
            case 2:    // R_ARM_ABS32
            case 21:   // R_ARM_GLOB_DAT
            case 22: { // R_ARM_JUMP_SLOT
                uint32_t addend = type == 2 ? LD32(where) : 0;
                uint32_t value = sym_shndx(si) ? g_load_base + sym_value(si) : resolve_symbol(sym_name(si));
                ST32(where, value + addend);
                break;
            }
            default:
                fatal("unsupported relocation type %u", type);
            }
        }
    };
    apply(rel, relsz);
    apply(jmprel, pltrelsz);

    // dlsym on our own library: looked up through its ELF hash table (lib_export)
    lib.dynsym = g_load_base + symtab;
    lib.dynstr = g_load_base + strtab;
    lib.base = g_load_base;
    return lib;
}
#endif

}  // namespace rt

// ---- dispatch -------------------------------------------------------------------
#ifndef __3DS__

// Linux ARM kernel user helpers (fixed addresses in the vector page), used by Thumb-1
// code (boost.thread atomics) that has no ldrex/strex.
static void kuser_helper(Cpu *cpu, uint32_t target) {
    switch (target) {
    case 0xffff0fa0u:  // __kuser_memory_barrier
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return;
    case 0xffff0fc0u: {  // __kuser_cmpxchg(oldval, newval, ptr): r0 = 0 and C set on success
        bool ok = GUEST_CAS32(cpu->r[2], cpu->r[0], cpu->r[1]);
        cpu->r[0] = ok ? 0 : 1;
        cpu->c = cpu->z = ok;
        return;
    }
    case 0xffff0fe0u:  // __kuser_get_tls
        cpu->r[0] = rt::current_tls();
        return;
    case 0xffff0f60u: {  // __kuser_cmpxchg64(*oldval, *newval, ptr)
        bool ok = GUEST_CAS64(cpu->r[2], LD64(cpu->r[0]), LD64(cpu->r[1]));
        cpu->r[0] = ok ? 0 : 1;
        cpu->c = cpu->z = ok;
        return;
    }
    }
    rt::fatal("unknown kernel user helper %08x called, lr=%08x", target, cpu->r[14]);
}

namespace {
#ifdef __GAMECUBE__
// Indirect calls whose call site's cache missed. The GameCube's MEM1 has no room for a hash
// (1 MB): the entry points (g_funcs, sorted by address) are indexed by 1 KB of guest code, each
// bucket the first entry at or after its start (80 KB; about two entries a bucket).
const FuncEntry *find_entry(uint32_t target) {
    static const struct Index {
        uint32_t lo = 0, count = 0;
        std::vector<uint32_t> first;
        Index() {
            for (uint32_t i = 1; i < g_func_count; i++)
                if (g_funcs[i].guest <= g_funcs[i - 1].guest) rt::fatal("g_funcs is not sorted (entry %u)", i);
            if (!g_func_count) return;
            lo = g_funcs[0].guest & ~1023u, count = ((g_funcs[g_func_count - 1].guest - lo) >> 10) + 1;
            first.resize(count);
            uint32_t i = 0;
            for (uint32_t b = 0; b < count; b++) {
                while (i < g_func_count && g_funcs[i].guest < lo + (b << 10)) i++;
                first[b] = i;
            }
        }
    } index;
    const uint32_t b = (target - index.lo) >> 10;
    if (target < index.lo || b >= index.count) return nullptr;
    for (uint32_t i = index.first[b]; i < g_func_count && g_funcs[i].guest <= target; i++)
        if (g_funcs[i].guest == target) return &g_funcs[i];
    return nullptr;
}
#else
// Indirect calls (virtual functions, function pointers) are frequent: an open-addressed hash
// of the guest entry points, built once, instead of a binary search over all of them.
const FuncEntry *find_entry(uint32_t target) {
    struct Slot { uint32_t guest; const FuncEntry *f; };
    static const struct Table {
        std::vector<Slot> slots;
        uint32_t shift = 0;
        Table() {
            uint32_t bits = 1;
            while ((1u << bits) < g_func_count * 2) bits++;
            slots.assign(size_t(1) << bits, Slot{0, nullptr});
            shift = 32 - bits;
            for (uint32_t i = 0; i < g_func_count; i++) {
                uint32_t h = (g_funcs[i].guest * 0x9e3779b1u) >> shift;
                while (slots[h].f) h = (h + 1) & uint32_t(slots.size() - 1);
                slots[h] = Slot{g_funcs[i].guest, &g_funcs[i]};
            }
        }
    } table;
    const uint32_t mask = uint32_t(table.slots.size() - 1);
    for (uint32_t h = (target * 0x9e3779b1u) >> table.shift;; h = (h + 1) & mask) {
        const Slot &s = table.slots[h];
        if (s.guest == target && s.f) return s.f;
        if (!s.f) return nullptr;
    }
}
#endif

// kernel helpers and import slots; false: a guest function
bool dispatch_special(Cpu *cpu, uint32_t target) {
    using namespace rt;
    if (target >= 0xffff0f00u) {
        kuser_helper(cpu, target);
        cpu->ret_pc = cpu->r[14];
        return true;
    }
    if (target >= kImportBase && target < kImportBase + kMaxImportSlots * 4) {
        call_import(cpu, (target - kImportBase) / 4);
        cpu->ret_pc = cpu->r[14];
        return true;
    }
    return false;
}
}  // namespace

extern "C" void rt_dispatch(Cpu *cpu, uint32_t target) {
    if (dispatch_special(cpu, target)) return;
    if (const FuncEntry *f = find_entry(target)) {
        f->fn(cpu, f->link);
        return;
    }
    rt::log_guest_callers(cpu);
    rt::fatal("indirect call to unknown guest address %08x (link %08x), lr=%08x", target, target - g_load_base, cpu->r[14]);
}

// The generated code's per-call-site cache (recomp.h RT_ICALL) missed: look the target up and
// remember it there. One 64-bit word holds the guest address and its g_funcs index + 1, so a
// thread reading the site while another writes it sees a consistent pair.
extern "C" void rt_dispatch_ic(Cpu *cpu, uint32_t target, RtIc *ic) {
    if (dispatch_special(cpu, target)) return;
    if (const FuncEntry *f = find_entry(target)) {
#if UINTPTR_MAX == 0xffffffffu
        __atomic_store_n(ic, f, __ATOMIC_RELAXED);
#else
        __atomic_store_n(ic, uint64_t(f - g_funcs + 1) << 32 | target, __ATOMIC_RELAXED);
#endif
        f->fn(cpu, f->link);
        return;
    }
    rt::log_guest_callers(cpu);
    rt::fatal("indirect call to unknown guest address %08x (link %08x), lr=%08x", target, target - g_load_base, cpu->r[14]);
}
#endif

namespace rt {
// Performance counters: the game thread's time in imports by kind (GL calls: the driver; file
// I/O; waits: locks, condition variables, sleeps, socket polls; memory allocation). Only the
// game thread's calls count, and cheap frequent ones (memcpy, string functions) are not timed.
PerfTimes g_perf_frame, g_perf_report;  // game thread only

const char *perf_kind_name(int kind) {
    static const char *const names[] = {"GL", "file I/O", "waits", "alloc"};
    return kind >= 0 && kind < kPerfKinds ? names[kind] : "?";
}
// On the 3DS the clock is a kernel call (svcGetSystemTick): the GL calls, hundreds a frame, are
// timed in one frame of every kGlTimedEvery and the reports scale that frame's time up.
#ifdef __3DS__
constexpr uint32_t kGlTimedEvery = 8;
#else
constexpr uint32_t kGlTimedEvery = 1;
#endif
uint32_t g_perf_frames;  // frames ended: GL calls are timed while it is a multiple of kGlTimedEvery
void (*g_gl_timing_hook)(bool timed);  // the 3DS: told before a timed frame and after it
PerfTimes perf_frame_end() {
    PerfTimes f = g_perf_frame;
    f.gl_timed = g_perf_frames++ % kGlTimedEvery == 0;
    if (g_gl_timing_hook && g_perf_frames % kGlTimedEvery < 2) g_gl_timing_hook(g_perf_frames % kGlTimedEvery == 0);
    for (int k = 0; k < kPerfKinds; k++) {
        g_perf_report.ns[k] += k == kPerfGl ? (f.gl_timed ? f.ns[k] * kGlTimedEvery : 0) : f.ns[k];
        g_perf_report.calls[k] += f.calls[k];
    }
    g_perf_report.imports += f.imports;
    g_perf_frame = PerfTimes{};
    return f;
}
void perf_add(PerfKind kind, int64_t ticks) {
    g_perf_frame.ns[kind] += ticks;
    g_perf_frame.calls[kind]++;
}
void perf_scale(PerfTimes &t, double ns_per_tick) {
    for (int64_t &v : t.ns) v = int64_t(double(v) * ns_per_tick);
}
PerfTimes perf_take() {
    PerfTimes r = g_perf_report;
    g_perf_report = PerfTimes{};
    return r;
}

int import_kind(const char *n) {
    if (!n) return -1;
    if (n[0] == 'g' && n[1] == 'l' && n[2] >= 'A' && n[2] <= 'Z') return kPerfGl;
    static const char *const file[] = {"fopen", "fread", "fwrite", "fclose", "fseek", "ftell", "fgets", "fflush", "open",
                                       "read", "write", "close", "lseek", "pread", "stat", "fstat", "lstat", "access",
                                       "opendir", "readdir", "closedir", "mkdir", "unlink", "rename", "remove", "fstatfs",
                                       "statfs", "fsync", "ftruncate", "AAssetManager_open", "AAsset_read"};
    static const char *const wait[] = {"pthread_mutex_lock", "pthread_cond_wait", "pthread_cond_timedwait", "usleep",
                                       "nanosleep", "sleep", "sem_wait", "sem_timedwait", "select", "poll", "epoll_wait",
                                       "sched_yield", "pthread_join", "pthread_rwlock_rdlock", "pthread_rwlock_wrlock"};
    static const char *const alloc[] = {"malloc", "free", "calloc", "realloc", "memalign", "posix_memalign", "_Znwj",
                                        "_Znaj", "_ZdlPv", "_ZdaPv", "dlmalloc", "dlfree"};
    for (const char *f : file) if (!std::strcmp(n, f)) return kPerfFile;
    for (const char *f : wait) if (!std::strcmp(n, f)) return kPerfWait;
    for (const char *f : alloc) if (!std::strcmp(n, f)) return kPerfAlloc;
    return -1;
}

// every import passes here: a flat table and a cached pointer, no checks beyond that
int8_t *g_import_kinds;   // per import slot: its PerfKind, or -1 (not timed)
Cpu *g_game_cpu;          // the game thread's Cpu, once it exists
uint32_t *g_import_counts;  // NOVA3_IMPORT_PROFILE=1: the game thread's calls per import slot
void call_import(Cpu *cpu, uint32_t i) {
    RT_TLS(t_last_import) = i;
    if (cpu != g_game_cpu) {
        if (!g_game_cpu && (g_game_cpu = main_cpu()) != nullptr && !g_import_kinds) {
            // every slot: the runtime adds imports beyond the generated ones (g_import_count)
            auto *k = new int8_t[kMaxImportSlots];
            for (uint32_t n = 0; n < kMaxImportSlots; n++) k[n] = n < g_import_count ? int8_t(import_kind(g_import_names[n])) : -1;
            if (const char *v = std::getenv("NOVA3_IMPORT_PROFILE"); v && *v == '1') g_import_counts = new uint32_t[kMaxImportSlots]();
            g_import_kinds = k;
        }
        if (cpu != g_game_cpu || !g_import_kinds) {
            g_import_fns[i](cpu);
            return;
        }
    }
    g_perf_frame.imports++;
    if (g_import_counts) g_import_counts[i]++;
    const int kind = g_import_kinds[i];
    if (kind < 0 || (kind == kPerfGl && g_perf_frames % kGlTimedEvery)) {
        g_import_fns[i](cpu);
        return;
    }
    const uint64_t t = os::ticks();  // ticks here (cheap), converted to ns by the report
    g_import_fns[i](cpu);
    g_perf_frame.ns[kind] += int64_t(os::ticks() - t);
    g_perf_frame.calls[kind]++;
}
}  // namespace rt

namespace rt {
// NOVA3_IMPORT_PROFILE=1: with each perf report, the game thread's imports per frame and the
// busiest ones (what the per-call cost of the import path adds up to)
void import_profile_log(int frames) {
    if (!g_import_counts || frames <= 0) return;
    std::vector<std::pair<uint32_t, uint32_t>> top;  // (calls, slot)
    uint64_t total = 0;
    for (uint32_t i = 0; i < kMaxImportSlots; i++)
        if (g_import_counts[i]) {
            top.push_back({g_import_counts[i], i});
            total += g_import_counts[i];
            g_import_counts[i] = 0;
        }
    std::sort(top.begin(), top.end(), std::greater<>());
    std::string text;
    {
        auto &t = imports();
        std::lock_guard<std::mutex> lock(t.lock);
        for (size_t k = 0; k < top.size() && k < 24; k++) {
            char item[96];
            std::snprintf(item, sizeof item, "%s%s %.0f", k ? ", " : "",
                          top[k].second < t.slot_names.size() ? t.slot_names[top[k].second].c_str() : "?", top[k].first / double(frames));
            text += item;
        }
    }
    log("imports per frame: %.0f: %s", total / double(frames), text.c_str());
}
}  // namespace rt

extern "C" void rt_call_import(Cpu *cpu, uint32_t i) {
    rt::call_import(cpu, i);
    cpu->ret_pc = cpu->r[14];
}

extern "C" void rt_unimplemented(Cpu *cpu, uint32_t addr, uint32_t word) {
    rt::fatal("unimplemented instruction %08x at %08x (link %08x), lr=%08x", word, addr, addr - g_load_base, cpu->r[14]);
}
extern "C" void rt_unreachable(Cpu *cpu, uint32_t addr) {
    (void)cpu;
    rt::fatal("jump table index out of range at %08x (link %08x)", addr, addr - g_load_base);
}
extern "C" void rt_trap(Cpu *cpu, uint32_t addr, const char *why) {
    rt::fatal("guest trap '%s' at %08x, lr=%08x", why, addr, cpu->r[14]);
}

namespace rt {

#ifndef __3DS__
uint32_t call(Cpu *cpu, uint32_t guest_fn, std::initializer_list<uint32_t> args) {
    uint32_t i = 0, stack_args = args.size() > 4 ? uint32_t(args.size() - 4) : 0;
    uint32_t frame = (stack_args * 4 + 7) & ~7u;
    cpu->r[13] -= frame;
    for (uint32_t a : args) {
        if (i < 4) cpu->r[i] = a;
        else ST32(cpu->r[13] + (i - 4) * 4, a);
        i++;
    }
    cpu->r[14] = kReturnSentinel;
    rt_dispatch(cpu, guest_fn);
    if ((cpu->ret_pc ^ kReturnSentinel) & ~1u)
        log("warning: guest call %08x returned to %08x instead of the caller", guest_fn, cpu->ret_pc);
    cpu->r[13] += frame;
    return cpu->r[0];
}

uint32_t call_nested(Cpu *cpu, uint32_t guest_fn, std::initializer_list<uint32_t> args) {
    // Host code running inside an import calls back into the guest: keep the import's
    // register state (its arguments, return address, flags) intact around the call.
    Cpu saved = *cpu;
    cpu->r[13] = (cpu->r[13] - 64) & ~7u;  // keep clear of the import's stack arguments
    uint32_t ret = call(cpu, guest_fn, args);
    uint32_t ex_addr = cpu->ex_addr;
    *cpu = saved;
    cpu->ex_addr = ex_addr;
    return ret;
}
#endif

// ---- crash reporting -----------------------------------------------------------------
// Host frames of recompiled code are named f_<link addr> / t_<link addr>, and every
// generated statement ends in /*@<guest addr>*/, so a symbolized host stack walk
// yields the guest call chain down to the faulting instruction.

void set_crash_library(const LoadedLib *lib) { g_crash_lib = lib; }

std::string guest_symbol(uint32_t link) {
    if (!g_crash_lib) return "";
    static std::vector<std::pair<uint32_t, const char *>> sorted;
    if (sorted.empty()) {
        if (const LoadedLib &l = *g_crash_lib; l.hash)
            for (uint32_t i = 1, n = LD32(l.hash + 4); i < n; i++)
                if (const uint32_t sym = l.dynsym + 16 * i; LD16(sym + 14) && LD8(l.dynstr + LD32(sym)))
                    sorted.push_back({LD32(sym + 4) & ~1u, str(l.dynstr + LD32(sym))});
        std::sort(sorted.begin(), sorted.end());
    }
    auto it = std::upper_bound(sorted.begin(), sorted.end(), std::make_pair(link, (const char *)nullptr),
                               [](auto &a, auto &b) { return a.first < b.first; });
    if (it == sorted.begin()) return "";
    --it;
    char off[32];
    std::snprintf(off, sizeof off, "+0x%x", link - it->first);
    return std::string(it->second) + (link == it->first ? "" : off);
}

static std::string source_line(const char *file, unsigned line) {
    static std::unordered_map<std::string, std::vector<std::string>> cache;
    auto &lines = cache[file];
    if (lines.empty()) {
        std::ifstream f(file);
        for (std::string l; std::getline(f, l);) lines.push_back(std::move(l));
    }
    return line >= 1 && line <= lines.size() ? lines[line - 1] : "";
}

void print_frame(int i, const char *symbol, const char *file, unsigned line) {
    std::string name = symbol ? symbol : "?";
    if (name.size() == 10 && (name[0] == 'f' || name[0] == 't') && name[1] == '_') {
        uint32_t fn = uint32_t(std::strtoul(name.c_str() + 2, nullptr, 16));
        std::string insn;
        if (file) {
            std::string src = source_line(file, line);
            size_t at = src.rfind("/*@");
            if (at != std::string::npos) insn = src.substr(at + 3, src.find("*/", at) - at - 3);
        }
        std::fprintf(stderr, "  #%-2d guest %s %08x %s%s%s\n", i, name[0] == 't' ? "thumb" : "arm", fn,
                     guest_symbol(fn).c_str(), insn.empty() ? "" : "  @ insn ", insn.c_str());
    } else {
        std::fprintf(stderr, "  #%-2d host  %s", i, name.c_str());
        if (file) std::fprintf(stderr, "  (%s:%u)", file, line);
        std::fputc('\n', stderr);
    }
}

void report_fault(const char *what, const void *fault_addr, int access) {
    (void)what;
    if (fault_addr) {
        auto *host = static_cast<const uint8_t *>(fault_addr);
        const char *kind = access == 1 ? "write" : access == 0 ? "read" : "access";
        if (host >= g_mem && host < g_mem + (1ull << 32))
            std::fprintf(stderr, "guest %s fault at %08x\n", kind, uint32_t(host - g_mem));
        else
            std::fprintf(stderr, "host %s fault at %p\n", kind, fault_addr);
    }
    auto &t = imports();
    if (RT_TLS(t_last_import) < t.slot_names.size()) std::fprintf(stderr, "last import called: %s\n", t.slot_names[RT_TLS(t_last_import)].c_str());
    if (Cpu *cpu = current_cpu()) {
        std::fprintf(stderr, "guest registers at last call boundary:\n");
        for (int i = 0; i < 16; i++)
            std::fprintf(stderr, "  r%-2d %08x%s", i, cpu->r[i], i % 4 == 3 ? "\n" : "");
        std::fprintf(stderr, "  lr link address %08x\n", cpu->r[14] - g_load_base);
    }
}

// Hang watchdog: the OS layer samples this counter and prints the game thread's stack
// when it stops advancing.
static std::atomic<uint64_t> g_frame_counter{0};
void watchdog_frame() {
    os::working_set_frame(g_frame_counter++);
    gl_trace_frame();
}
uint64_t watchdog_frames() { return g_frame_counter; }
void start_watchdog(int seconds) { os::start_watchdog(seconds); }
void install_crash_handler() { os::install_crash_handler(); }

#ifndef __3DS__
Cpu *new_cpu(uint32_t stack_size) {
    auto *cpu = static_cast<Cpu *>(std::calloc(1, sizeof(Cpu)));
    uint32_t stack = map_region(stack_size);
    cpu->r[13] = stack + stack_size - 16;
    cpu->ex_addr = ~0u;
    note_guest_stack(cpu, stack + stack_size);
    return cpu;
}
#endif

static std::mutex g_stacks_lock;
static std::unordered_map<const Cpu *, uint32_t> g_stack_tops;
void note_guest_stack(const Cpu *cpu, uint32_t top) {
    std::lock_guard<std::mutex> lock(g_stacks_lock);
    if (top) g_stack_tops[cpu] = top;
    else g_stack_tops.erase(cpu);
}
uint32_t guest_stack_top(const Cpu *cpu) {
    std::lock_guard<std::mutex> lock(g_stacks_lock);
    auto it = g_stack_tops.find(cpu);
    return it == g_stack_tops.end() ? 0 : it->second;
}

}  // namespace rt
