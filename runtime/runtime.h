// Host-side runtime API (C++). Generated guest code only sees recomp.h.
#pragma once
#include "recomp.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>

constexpr uint32_t kMaxImportSlots = 2048;

namespace rt {

// ---- guest address-space layout ---------------------------------------------------
constexpr uint32_t kRegionBase = 0x08000000u;   // map_region(): stacks, small fixed areas
// g_load_base (0x10000000): the game library
constexpr uint32_t kHeapBase = 0x20000000u;     // guest malloc heap
#ifdef __GAMECUBE__
constexpr uint32_t kHeapEnd = 0x80000000u;      // above: the host's MEM1 mappings (docs/gamecube.md)
#else
constexpr uint32_t kHeapEnd = 0xf0000000u;
#endif
constexpr uint32_t kImportBase = 0xfff00000u;   // fake addresses of imported functions
constexpr uint32_t kReturnSentinel = 0xfffffff0u;
#ifdef __3DS__
// The New 3DS runs the game's own code (runtime/native_3ds.cpp, docs/3ds.md): the library sits at
// the start of the regular heap, its ARMv6K patch stubs and hook trampolines right after it, and
// newlib's heap (the guest heap too) begins past this reserve (runtime/os_3ds.cpp).
constexpr uint32_t kNativeLoadBase = 0x08000000u;
constexpr uint32_t kNativeImageReserve = 0x01c00000u;  // 28 MB
uint32_t native_gate_address(uint32_t slot);  // the import slot's assembly gate (native_3ds_gates.S)
bool native_emulator();                       // running in Citra / Azahar
// NOVA3_LOAD_PROFILE=1: samples what the game thread does during its long frames (loads) and logs
// it once frames run again (native_3ds.cpp); call on the game thread
void native_start_load_profile();
#endif

struct LoadedLib {
    uint32_t base = 0, end = 0;
    uint32_t init_array = 0, init_array_count = 0;
    uint32_t exidx = 0, exidx_count = 0;
    // The 3DS: code moved out of the image (tools/v6patch.py's Thumb island), [moved, moved_end),
    // and its own unwind table
    uint32_t moved = 0, moved_end = 0, moved_exidx = 0, moved_exidx_count = 0;
    // Exported names are looked up in the loaded image's own symbol table through its ELF hash
    // table (DT_HASH; guest addresses): the 40,000 names copied into a map took ~5 MB of the host
    // heap, more than the GameCube's host has.
    uint32_t dynsym = 0, dynstr = 0, hash = 0;
};
uint32_t lib_export(const LoadedLib &lib, const char *name);  // an exported symbol's address, or 0

using DataInit = uint32_t (*)(const char *name);  // returns the guest address of the object

[[noreturn]] void fatal(const char *fmt, ...);
void set_fatal_notify(void (*fn)(const char *message));  // e.g. show the message to the user before exiting
void log(const char *fmt, ...);  // asynchronous: a background thread writes the lines out
void log_flush();
void gl_stats_log(int frames);
void gl_memory(uint64_t &textures, uint64_t &textures_3ds, uint64_t &targets, uint64_t &buffers);  // live GL memory    // the GL calls per frame since the last report
void gl_set_shader_cache(const std::string &dir);  // program binaries saved there (before the first link)
// Without program binaries (the Switch): compile one program of earlier runs ahead into the
// program pool (call while the game idles in its menus); false once none are left.
bool gl_prewarm_step();
void call_import(Cpu *cpu, uint32_t index);  // an import (the game thread's are timed by kind)
Cpu *main_cpu();                  // the game thread's Cpu

// Performance counters for the log: the game thread's time in imports, by kind, accumulated
// per frame (perf_frame_end) and per report (perf_take).
enum PerfKind { kPerfGl, kPerfFile, kPerfWait, kPerfAlloc, kPerfKinds };
struct PerfTimes {
    int64_t ns[kPerfKinds] = {};   // in os::ticks() until perf_scale() converts them
    uint32_t calls[kPerfKinds] = {};
    uint64_t imports = 0;  // every import call of the game thread
    bool gl_timed = true;  // perf_frame_end: whether this frame's GL calls were timed (1 in 8 on the 3DS)
};
PerfTimes perf_frame_end();       // this frame's times (and adds them to the report's)
PerfTimes perf_take();            // the report's times since the last perf_take
const char *perf_kind_name(int kind);
void perf_add(PerfKind kind, int64_t ticks);  // game thread: time spent outside the import calls
void import_profile_log(int frames);  // NOVA3_IMPORT_PROFILE=1: the game thread's busiest imports
// The 3DS: called at the end of a frame with whether the next one is timed (GL calls are timed in
// 1 frame of 8): native_3ds.cpp points the game's GL imports at their gates for it, else straight
// at the functions.
extern void (*g_gl_timing_hook)(bool timed);
void perf_scale(PerfTimes &t, double ns_per_tick);  // ticks -> ns                 // write out what is pending (crashes, exits)
void init_memory();
void install_crash_handler();
void start_watchdog(int seconds);  // this thread: print its stack when no frame completes for `seconds`
void watchdog_frame();                                // call once per completed frame
void set_crash_library(const LoadedLib *lib);
std::string guest_symbol(uint32_t link);  // nearest exported symbol (+offset) for a link address  // guest symbol names in crash reports
void commit(uint32_t addr, uint32_t size);
bool guest_mapped(uint32_t addr);    // committed guest memory (map regions, the library, heap)
bool heap_committed(uint32_t addr);  // heap.cpp
void heap_usage(uint64_t &live, uint64_t &peak, uint64_t &span);  // guest heap bytes in use, most at once, bump extent
uint32_t map_region(uint32_t size);
LoadedLib load_library(const char *path);
void register_import(const char *name, ImportFn fn, const void *direct = nullptr);
// The 3DS: the typed host function behind an import, when the game's code can call it directly
// (direct_entry below), else nullptr.
const void *import_direct(const char *name);
void set_import(const char *name, ImportFn fn);  // replace an implementation at runtime
void install_null_gl();                            // gles_null.cpp: headless renderer
using GlLoader = void *(*)(const char *name);
bool gl_load(GlLoader loader);
void gl_set_anisotropy(float level);
void gl_trace_frame();  // a frame is complete (NOVA3_GLTRACE: gles.cpp)
// Texture uploads (host/texpack.cpp): seen before they reach GL; returning true means the call
// was handled (a replacement uploaded instead, or the call dropped). Game thread, context current.
struct GlTextureHooks {
    bool (*tex_image)(uint32_t target, int level, int internal_format, int width, int height, uint32_t format,
                      uint32_t type, const void *pixels) = nullptr;
    bool (*generate_mipmap)(uint32_t target) = nullptr;
};
void gl_set_texture_hooks(const GlTextureHooks &hooks);               // before gl_load: 1 = off, up to 16                     // gles.cpp: resolve GLES2 entry points (context current)
bool gl_screenshot(const char *path, int width, int height);  // PNG of the back buffer
void register_data_import(const char *name, DataInit init);
uint32_t import_function_address(const char *name, bool create);
uint32_t data_import_address(const char *name);
void set_main_library(const LoadedLib *lib);
uint32_t export_address(const char *name);  // guest address of a symbol exported by the game library
RtEhFrame *eh_head();             // eh.cpp: per-thread C++ catch-frame chain
void set_eh_head(RtEhFrame *f);  // for dlsym / unwinder lookups
Cpu *new_cpu(uint32_t stack_size = 1u << 20);
// Call a guest function with AAPCS integer arguments; returns r0.
uint32_t call(Cpu *cpu, uint32_t guest_fn, std::initializer_list<uint32_t> args);
// Same, from host code that is itself running inside an import (preserves cpu state).
uint32_t call_nested(Cpu *cpu, uint32_t guest_fn, std::initializer_list<uint32_t> args);

inline uint32_t sym(uint32_t link_addr) { return g_load_base + link_addr; }

// ---- guest pointers ----------------------------------------------------------------
template <class T = char>
inline T *ptr(uint32_t a) { return reinterpret_cast<T *>(HOST(a)); }
inline const char *str(uint32_t a) { return a ? reinterpret_cast<const char *>(HOST(a)) : nullptr; }
inline uint32_t gaddr(const void *host) { return uint32_t(reinterpret_cast<uintptr_t>(host) - reinterpret_cast<uintptr_t>(g_mem)); }

// ---- heap (heap.cpp) -------------------------------------------------------------------
uint32_t gmalloc(uint32_t size);
uint32_t gcalloc(uint32_t n, uint32_t size);
uint32_t grealloc(uint32_t p, uint32_t size);
void gfree(uint32_t p);
uint32_t gstrdup(const char *s);  // guest copy of a host string

// ---- threads (thread.cpp) ------------------------------------------------------------
Cpu *init_main_thread();
// The top of a guest thread's stack (crash reports scan up to it); 0 if unknown.
void note_guest_stack(const Cpu *cpu, uint32_t top);
uint32_t guest_thread_entry();  // the current guest thread's start routine (0: the game thread)
void log_guest_callers(Cpu *cpu);  // return addresses on a guest stack (crash and hang reports)
void log_game_thread_callers();
void report_threads(FILE *out);  // thread.cpp: each guest thread, the mutex it waits for and its holder, its callers
// Guest threads: running, and ended but not yet joined (the periodic memory log line).
void thread_counts(int &running, int &unjoined);
uint32_t guest_stack_top(const Cpu *cpu);
Cpu *current_cpu();
void set_errno(int e);  // Linux errno values
int get_errno();
uint32_t current_tls();  // guest TLS block of the calling thread
uint32_t current_thread_id();

// ---- filesystem (fs.cpp) ---------------------------------------------------------------
struct FsConfig {
    std::string data_dir;   // host directory backing the guest filesystem sandbox
    std::string save_dir;   // host directory for the app's own data, /data/data/<package> without
                            // cache/ (the save games); empty: inside data_dir like the rest
    std::string obb_dir;    // host directory containing main/patch OBBs
    std::string apk_path;   // the game's APK (guest: /data/app/<package>-1.apk)
    std::string package = "com.gameloft.android.ANMP.GloftNAHM";
};
void init_fs(const FsConfig &cfg);
// The device the game is told it runs on picks its quality profiles (GameOptions.json): on the
// 3DS and the GameCube, and on a PC with NOVA3_DEVICE=3ds to measure it, a 256 MB phone with one 804 MHz core
// and an Adreno 200 (low shaders, no post effects, smaller textures, texture streaming).
bool low_end_device();
std::string host_path(const char *guest_path);  // guest absolute/relative path -> host path
// The file system requests behind the game's file calls so far (libc_io.cpp; times in os::ticks),
// and on the 3DS the packed files' blocks unpacked
struct IoStats { uint64_t read_bytes, reads, read_ticks, opens, open_ticks, stats, stat_ticks, unpacked_bytes, unpack_ticks; };
void io_stats(IoStats &s);
int io_now();  // the request the game thread waits for: 0 none, 1 read, 2 open, 3 stat, 4 unpacking
#ifdef __3DS__
void io_boot_done();  // the title menu runs: the archives' mount index is freed (libc_io.cpp)
#endif

// ---- sockets (net.cpp) sharing the guest fd space with files (libc_io.cpp) ------------------------
int io_alloc_socket_fd();
bool io_is_socket(int fd);
int io_poll_fd(int fd, int events);  // POLLIN(1)/POLLOUT(4) readiness of a non-socket fd
int net_recv(int fd, uint32_t buf, uint32_t n, int flags);
int net_send(int fd, uint32_t buf, uint32_t n, int flags);
void net_close(int fd);
void net_set_nonblock(int fd, bool on);
int net_ioctl(int fd, uint32_t req, uint32_t arg);
int net_poll(int fd, int events);  // POLLIN(1)/POLLOUT(4)/POLLERR(8)/POLLHUP(0x10) of a socket, no wait
// Before the game starts: IPv4 addresses ("10.147.17.5, ...") that also get every broadcast
// (local multiplayer's discover packet), for networks that do not pass broadcasts.
void net_set_peers(const std::string &list);

struct Args;
std::string guest_format(const char *fmt, Args &args);  // printf with guest arguments (libc_io.cpp)

// ---- import argument marshalling (AAPCS softfp, as used by armeabi-v7a) ------------------
// Integer/pointer args in r0-r3 then stack; 64-bit values and doubles take an even
// register pair (or 8-aligned stack slot). Floats travel in core registers.
struct Args {
    Cpu *cpu;
    uint32_t i = 0;  // next argument word (0-3 registers, 4+ stack)
    uint32_t base = 0;  // != 0: read from a va_list memory area instead
    explicit Args(Cpu *c, uint32_t first = 0) : cpu(c), i(first) {}
    static Args va_list(Cpu *c, uint32_t ap) { Args a(c); a.base = ap; return a; }
    uint32_t word() {
        uint32_t v = base ? LD32(base + i * 4) : i < 4 ? cpu->r[i] : LD32(cpu->r[13] + (i - 4) * 4);
        i++;
        return v;
    }
    uint64_t dword() {
        if (base) { if ((base + i * 4) & 7) i++; } else if (i & 1) i++;
        uint64_t lo = word();
        return lo | uint64_t(word()) << 32;
    }
    template <class T>
    T get() {
        if constexpr (std::is_same_v<T, float>) { uint32_t w = word(); float f; std::memcpy(&f, &w, 4); return f; }
        else if constexpr (std::is_same_v<T, double>) { uint64_t w = dword(); double d; std::memcpy(&d, &w, 8); return d; }
        else if constexpr (sizeof(T) == 8) return T(dword());
        else return T(word());
    }
};

template <class T>
inline void set_return(Cpu *cpu, T v) {
    if constexpr (std::is_same_v<T, float>) std::memcpy(&cpu->r[0], &v, 4);
    else if constexpr (std::is_same_v<T, double>) { uint64_t w; std::memcpy(&w, &v, 8); cpu->r[0] = uint32_t(w); cpu->r[1] = uint32_t(w >> 32); }
    else if constexpr (sizeof(T) == 8) { cpu->r[0] = uint32_t(uint64_t(v)); cpu->r[1] = uint32_t(uint64_t(v) >> 32); }
    else if constexpr (std::is_same_v<T, bool>) cpu->r[0] = v ? 1 : 0;
    else cpu->r[0] = uint32_t(v);
}

template <class F> struct FnTraits;
template <class R, class... A> struct FnTraits<R (*)(A...)> {
    using Ret = R;
    using ArgsTuple = std::tuple<A...>;
};
template <class R, class... A> struct FnTraits<R (*)(Cpu *, A...)> {
    using Ret = R;
    using ArgsTuple = std::tuple<A...>;
    static constexpr bool wants_cpu = true;
};

// thunk<f>: adapts a typed host function `R f(A...)` (optionally `R f(Cpu*, A...)`) to ImportFn.
template <auto F>
void thunk(Cpu *cpu) {
    using Tr = FnTraits<decltype(F)>;
    Args args(cpu);
    auto unpack = [&]<class... A>(std::tuple<A...> *) { return std::tuple<A...>{args.get<A>()...}; };  // braced: left-to-right
    auto values = unpack(static_cast<typename Tr::ArgsTuple *>(nullptr));
    auto invoke = [&](auto &&...a) {
        if constexpr (requires { Tr::wants_cpu; }) return F(cpu, a...);
        else return F(a...);
    };
    if constexpr (std::is_void_v<typename Tr::Ret>) std::apply(invoke, values);
    else set_return(cpu, std::apply(invoke, values));
}

// The 3DS runs the game's own code: a typed import whose arguments and result all travel in core
// registers as plain words (integers, enums, pointers: guest addresses are host addresses there)
// can be called by the game directly, with no Cpu (runtime/native_3ds.cpp decides which are).
template <class T>
constexpr bool kWordArg = (std::is_integral_v<T> || std::is_enum_v<T> || std::is_pointer_v<T>) && sizeof(T) <= 4 &&
                          !std::is_same_v<T, bool>;
template <auto F>
constexpr const void *direct_entry() {
#ifdef __3DS__
    using Tr = FnTraits<decltype(F)>;
    if constexpr (requires { Tr::wants_cpu; }) {
        return nullptr;
    } else {
        constexpr bool args = []<class... A>(std::tuple<A...> *) { return (kWordArg<A> && ...); }(
            static_cast<typename Tr::ArgsTuple *>(nullptr));
        constexpr bool ret = [] {
            if constexpr (std::is_void_v<typename Tr::Ret>) return true;
            else return kWordArg<typename Tr::Ret>;
        }();
        if constexpr (args && ret) return reinterpret_cast<const void *>(F);
        else return nullptr;
    }
#else
    return nullptr;
#endif
}

struct ImportRegistrar {
    ImportRegistrar(const char *name, ImportFn fn, const void *direct = nullptr) { register_import(name, fn, direct); }
};
struct DataRegistrar {
    DataRegistrar(const char *name, DataInit init) { register_data_import(name, init); }
};

}  // namespace rt

#define RT_CAT2(a, b) a##b
#define RT_CAT(a, b) RT_CAT2(a, b)
// Typed import: RT_IMPORT(strlen, my_strlen)
#define RT_IMPORT(name, fn) static rt::ImportRegistrar RT_CAT(reg_, __COUNTER__)(#name, &rt::thunk<fn>, rt::direct_entry<fn>());
// Raw import taking the Cpu directly (variadics etc.)
#define RT_IMPORT_RAW(name, fn) static rt::ImportRegistrar RT_CAT(reg_, __COUNTER__)(#name, fn);
#define RT_DATA(name, init) static rt::DataRegistrar RT_CAT(dreg_, __COUNTER__)(#name, init);

// Linux errno values (bionic)
enum : int {
    kEPERM = 1, kENOENT = 2, kEINTR = 4, kEIO = 5, kEBADF = 9, kECHILD = 10, kEAGAIN = 11, kENOMEM = 12,
    kEACCES = 13, kEBUSY = 16, kEEXIST = 17, kENOTDIR = 20, kEISDIR = 21, kEINVAL = 22, kEMFILE = 24,
    kENOSPC = 28, kESPIPE = 29, kERANGE = 34, kEDEADLK = 35, kENOSYS = 38, kENOTEMPTY = 39,
    kENOTSOCK = 88, kEAFNOSUPPORT = 97, kENETDOWN = 100, kENETUNREACH = 101, kECONNREFUSED = 111,
    kETIMEDOUT = 110,
};
