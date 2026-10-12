// libm, time, environment/process, signals, setjmp/longjmp, dl*, networking (offline),
// Android NDK (log, sensors, looper) and placeholders for OpenSL ES.
#include "os.h"
#include "tls.h"
#include "runtime.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <csetjmp>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace rt {

static const LoadedLib *g_lib;
void set_main_library(const LoadedLib *lib) { g_lib = lib; }
// The image's own ELF hash table (DT_HASH): buckets, then chains, over the symbol table.
uint32_t lib_export(const LoadedLib &lib, const char *name) {
    if (!lib.hash) return 0;
    {
        uint32_t h = 0;
        for (const char *p = name; *p; p++) {
            h = (h << 4) + uint8_t(*p);
            const uint32_t g = h & 0xf0000000u;
            h = (h ^ (g >> 24)) & ~g;
        }
        const uint32_t nbucket = LD32(lib.hash), nchain = LD32(lib.hash + 4), chain = lib.hash + 8 + 4 * nbucket;
        for (uint32_t i = LD32(lib.hash + 8 + 4 * (h % nbucket)); i && i < nchain; i = LD32(chain + 4 * i)) {
            const uint32_t sym = lib.dynsym + 16 * i;
            if (LD16(sym + 14) && !std::strcmp(name, str(lib.dynstr + LD32(sym)))) return g_load_base + LD32(sym + 4);
        }
        return 0;
    }
}
uint32_t export_address(const char *name) { return g_lib ? lib_export(*g_lib, name) : 0; }

namespace {

using u32 = uint32_t;

// ---- math (softfp: arguments/results in core registers, handled by thunk) ---------------
#define M1(name, T) T m_##name(T x) { return ::name(x); }
#define M2(name, T) T m_##name(T x, T y) { return ::name(x, y); }
M1(acos, double) M1(asin, double) M1(atan, double) M1(ceil, double) M1(cos, double) M1(cosh, double)
M1(exp, double) M1(floor, double) M1(log, double) M1(log10, double) M1(sin, double) M1(sinh, double)
M1(tan, double) M1(tanh, double)
M2(atan2, double) M2(fmod, double) M2(pow, double)
M1(acosf, float) M1(asinf, float) M1(atanf, float) M1(ceilf, float) M1(cosf, float) M1(expf, float)
M1(floorf, float) M1(log10f, float) M1(logf, float) M1(sinf, float) M1(tanf, float)
M2(atan2f, float) M2(fmodf, float) M2(powf, float)
double m_ldexp(double x, int e) { return std::ldexp(x, e); }
int32_t m_lrintf(float x) { return int32_t(std::lrintf(x)); }
double m_frexp(double x, u32 e) { int k; double r = std::frexp(x, &k); ST32(e, u32(k)); return r; }
double m_modf(double x, u32 ip) { double i; double r = std::modf(x, &i); uint64_t b; std::memcpy(&b, &i, 8); ST64(ip, b); return r; }

// ---- time ---------------------------------------------------------------------------------
using os::monotonic_ns;
using os::realtime_ns;
using os::sleep_ns;

int t_gettimeofday(u32 tv, u32) {
    int64_t ns = realtime_ns();
    if (tv) { ST32(tv, u32(ns / 1000000000)); ST32(tv + 4, u32(ns / 1000 % 1000000)); }
    return 0;
}
int t_clock_gettime(int id, u32 ts) {
    int64_t ns = id == 0 ? realtime_ns() : monotonic_ns();
    ST32(ts, u32(ns / 1000000000));
    ST32(ts + 4, u32(ns % 1000000000));
    return 0;
}
int32_t t_time(u32 out) {
    int32_t s = int32_t(realtime_ns() / 1000000000);
    if (out) ST32(out, u32(s));
    return s;
}
int t_nanosleep(u32 req, u32 rem) {
    sleep_ns(int64_t(int32_t(LD32(req))) * 1000000000 + LD32(req + 4));
    if (rem) { ST32(rem, 0); ST32(rem + 4, 0); }
    return 0;
}
int t_usleep(u32 us) { sleep_ns(int64_t(us) * 1000); return 0; }
u32 t_sleep(u32 s) { sleep_ns(int64_t(s) * 1000000000); return 0; }
int32_t t_clock() { return int32_t(os::process_cpu_ns() / 1000); }  // CLOCKS_PER_SEC = 1000000
int32_t t_times(u32 buf) {
    int32_t ticks = int32_t(monotonic_ns() / 10000000);  // 100 Hz
    if (buf) { u32 cpu = u32(t_clock() / 10000); ST32(buf, cpu); ST32(buf + 4, 0); ST32(buf + 8, 0); ST32(buf + 12, 0); }
    return ticks;
}

// bionic struct tm: 9 ints, long tm_gmtoff, const char *tm_zone (44 bytes)
void to_guest_tm(u32 g, const std::tm &t, int32_t gmtoff) {
    int v[9] = {t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst};
    for (int i = 0; i < 9; i++) ST32(g + 4 * i, u32(v[i]));
    ST32(g + 36, u32(gmtoff));
    static u32 utc = gstrdup("UTC"), local = gstrdup("LOCAL");
    ST32(g + 40, gmtoff ? local : utc);
}
std::tm from_guest_tm(u32 g) {
    std::tm t{};
    int *v[9] = {&t.tm_sec, &t.tm_min, &t.tm_hour, &t.tm_mday, &t.tm_mon, &t.tm_year, &t.tm_wday, &t.tm_yday, &t.tm_isdst};
    for (int i = 0; i < 9; i++) *v[i] = int(LD32(g + 4 * i));
    return t;
}
u32 tm_buf() {
    static RT_THREAD_LOCAL(u32, b_slots);
    u32 &b = RT_TLS(b_slots);
    if (!b) b = gcalloc(1, 48);
    return b;
}
u32 t_localtime_r(u32 tp, u32 out) {
    time_t t = time_t(int32_t(LD32(tp)));
    std::tm lt;
    int32_t offset = 0;
    if (!os::local_time(t, lt, offset)) return 0;
    to_guest_tm(out, lt, offset);
    return out;
}
u32 t_gmtime_r(u32 tp, u32 out) {
    time_t t = time_t(int32_t(LD32(tp)));
    std::tm gt;
    if (!os::utc_time(t, gt)) return 0;
    to_guest_tm(out, gt, 0);
    return out;
}
u32 t_localtime(u32 tp) { return t_localtime_r(tp, tm_buf()); }
u32 t_gmtime(u32 tp) { return t_gmtime_r(tp, tm_buf()); }
int32_t t_mktime(u32 g) {
    std::tm t = from_guest_tm(g);
    time_t r = std::mktime(&t);
    std::tm lt;
    int32_t offset = 0;
    os::local_time(r, lt, offset);
    to_guest_tm(g, t, offset);
    return int32_t(r);
}
double t_difftime(int32_t a, int32_t b) { return double(a) - double(b); }
u32 t_asctime(u32 g) {
    static u32 buf = gmalloc(64);
    std::tm t = from_guest_tm(g);
    std::strftime(ptr(buf), 64, "%a %b %e %H:%M:%S %Y\n", &t);
    return buf;
}
u32 t_strftime(u32 buf, u32 max, u32 fmt, u32 g) {
    std::tm t = from_guest_tm(g);
    return u32(std::strftime(ptr(buf), max, str(fmt), &t));
}
u32 t_wcsftime(u32 buf, u32 max, u32, u32) { if (max) ST32(buf, 0); return 0; }
u32 t_strptime(u32 s, u32 fmt, u32 g) {
    // Minimal: numeric fields %Y %m %d %H %M %S %y and literals.
    std::tm t = from_guest_tm(g);
    const char *p = str(s);
    for (const char *f = str(fmt); *f; f++) {
        if (*f != '%') {
            if (std::isspace(uint8_t(*f))) { while (std::isspace(uint8_t(*p))) p++; continue; }
            if (*p != *f) return 0;
            p++;
            continue;
        }
        char c = *++f;
        char *end;
        long v = std::strtol(p, &end, 10);
        if (end == p) return 0;
        switch (c) {
        case 'Y': t.tm_year = int(v) - 1900; break;
        case 'y': t.tm_year = int(v < 69 ? v + 100 : v); break;
        case 'm': t.tm_mon = int(v) - 1; break;
        case 'd': t.tm_mday = int(v); break;
        case 'H': t.tm_hour = int(v); break;
        case 'M': t.tm_min = int(v); break;
        case 'S': t.tm_sec = int(v); break;
        default: log("strptime: unsupported %%%c", c); return 0;
        }
        p = end;
    }
    to_guest_tm(g, t, int32_t(LD32(g + 36)));
    return gaddr(p);
}

// ---- environment / process --------------------------------------------------------------------
std::unordered_map<std::string, u32> &env_strings() {
    static std::unordered_map<std::string, u32> m;
    return m;
}
u32 e_getenv(u32 name) {
    static const std::unordered_map<std::string, std::string> env = {
        {"HOME", "/data/data/com.gameloft.android.ANMP.GloftNAHM"},
        {"EXTERNAL_STORAGE", "/sdcard"},
        {"ANDROID_ROOT", "/system"},
        {"ANDROID_DATA", "/data"},
        {"TMPDIR", "/tmp"},
        {"PATH", "/system/bin"},
    };
    auto it = env.find(str(name));
    if (it == env.end()) return 0;
    auto &cache = env_strings();
    auto c = cache.find(it->first);
    if (c != cache.end()) return c->second;
    return cache[it->first] = gstrdup(it->second.c_str());
}
u32 e_setlocale(int, u32) {
    static u32 c = gstrdup("C");
    return c;
}
int32_t e_sysconf(int name) {
    switch (name) {
    case 0x06: return 100;                               // _SC_CLK_TCK
    case 0x0b: return 1024;                              // _SC_OPEN_MAX
    case 0x27: case 0x28: return 4096;                   // _SC_PAGESIZE / _SC_PAGE_SIZE
    case 0x60: case 0x61: return low_end_device() ? 1 : 4;  // _SC_NPROCESSORS_CONF / _ONLN (the CPU profile)
    case 0x62: return low_end_device() ? 65536 : 524288;    // _SC_PHYS_PAGES (256 MiB / 2 GiB)
    case 0x63: return low_end_device() ? 32768 : 262144;    // _SC_AVPHYS_PAGES
    }
    log("sysconf(%#x) unsupported", name);
    set_errno(kEINVAL);
    return -1;
}
int e_getpid() { return int(os::process_id()); }
u32 e_getuid() { return 10123; }
int e_zero() { return 0; }
int e_setid(u32) { return 0; }
u32 e_getpw(u32) { set_errno(kENOENT); return 0; }
int e_gethostname(u32 buf, u32 len) { std::strncpy(ptr(buf), "localhost", len); return 0; }
int e_kill(int, int) { return 0; }
int e_raise(int sig) {
    if (sig == 6) fatal("guest raise(SIGABRT)");
    log("guest raise(%d) ignored", sig);
    return 0;
}
u32 e_bsd_signal(int, u32) { return 0; }
int e_sigaction(int, u32, u32 old) { if (old) std::memset(ptr(old), 0, 16); return 0; }
int e_enosys() { set_errno(kENOSYS); return -1; }
int e_waitpid(int, u32, int) { set_errno(kECHILD); return -1; }
u32 e_popen(u32, u32) { set_errno(kENOSYS); return 0; }
void e_exit(int code) {
    std::fflush(nullptr);
    log("guest exit(%d)", code);
    os::exit_process(code);  // no static destructors under the guest threads still going
}
}  // namespace
// The guest's callers at an abort(): return addresses on the guest stack (words that point
// just after a BL / BLX in the game library), as link addresses for tools/disasm.py.
void log_guest_callers(Cpu *cpu) {
    if (!g_lib) return;
    std::string out;
    int found = 0;
    const u32 top = guest_stack_top(cpu);
    for (u32 sp = cpu->r[13], end = top ? top : sp; sp < end && found < 24; sp += 4) {
        const u32 w = LD32(sp) & ~1u;
        if (w < g_lib->base + 4 || w >= g_lib->end) continue;
        const u32 insn = LD32(w - 4);
        if ((insn & 0x0f000000u) != 0x0b000000u && (insn & 0xfe000000u) != 0xfa000000u &&
            (insn & 0x0ffffff0u) != 0x012fff30u)
            continue;
        char buf[16];
        std::snprintf(buf, sizeof buf, " %08x", w - g_load_base);
        out += buf;
        found++;
    }
    log("guest callers (stack scan):%s", out.c_str());
}
namespace {
void e_abort(Cpu *cpu) {
    log_guest_callers(cpu);
    fatal("guest abort() (lr=%08x, link %08x)", cpu->r[14], cpu->r[14] - g_load_base);
}
void e_assert2(u32 file, int line, u32 func, u32 expr) { fatal("guest assertion failed: %s (%s:%d %s)", str(expr), str(file), line, str(func)); }
void e_stack_chk_fail(Cpu *cpu) { fatal("guest stack smashing detected (lr=%08x)", cpu->r[14]); }
uint32_t init_stack_guard(const char *) { u32 a = gcalloc(1, 8); ST32(a, 0x5a17c0deu); return a; }
int e_atexit(u32, u32, u32) { return 0; }  // static destructors are not run at exit
void e_nop() {}
void e_syscall(Cpu *c) {
    switch (c->r[0]) {
    case 224: c->r[0] = current_thread_id(); return;  // gettid
    }
    log("syscall(%u) unsupported", c->r[0]);
    set_errno(kENOSYS);
    c->r[0] = u32(-1);
}

// ---- setjmp / longjmp ---------------------------------------------------------------------------
// Guest jmp_buf (256 bytes): r4-r11, sp, lr | d8-d15 | magic | C++ catch-frame chain |
// address of the host jmp_buf (on the host stack of the function that called setjmp).
constexpr u32 kJbMagic = 0x6e6f7633u, kJbHost = 128;

}  // namespace
}  // namespace rt

extern "C" jmp_buf *rt_setjmp_prepare(Cpu *cpu, void *host_buf) {
    uint32_t jb = cpu->r[0];
    for (int i = 0; i < 10; i++) ST32(jb + 4 * i, cpu->r[4 + i]);
    for (int i = 0; i < 8; i++) ST64(jb + 40 + 8 * i, cpu->vr.x[8 + i]);
    ST32(jb + 104, rt::kJbMagic);
    RtEhFrame *head = rt::eh_head();  // longjmp restores the C++ catch-frame chain
    std::memcpy(g_mem + jb + 112, &head, sizeof head);
    std::memcpy(g_mem + jb + rt::kJbHost, &host_buf, sizeof host_buf);
    return static_cast<jmp_buf *>(host_buf);
}

namespace rt {
namespace {

void e_longjmp(Cpu *cpu, u32 jb, int val) {
    if (LD32(jb + 104) != kJbMagic) fatal("longjmp to a jmp_buf that was not set (%08x)", jb);
    for (int i = 0; i < 10; i++) cpu->r[4 + i] = LD32(jb + 4 * i);
    for (int i = 0; i < 8; i++) cpu->vr.x[8 + i] = LD64(jb + 40 + 8 * i);
    cpu->r[0] = val ? u32(val) : 1;
    RtEhFrame *head;
    std::memcpy(&head, g_mem + jb + 112, sizeof head);
    set_eh_head(head);
    void *host_buf;
    std::memcpy(&host_buf, g_mem + jb + kJbHost, sizeof host_buf);
    longjmp(*static_cast<jmp_buf *>(host_buf), 1);
}

// ---- dynamic loading ------------------------------------------------------------------------
constexpr u32 kSelfHandle = 0x1, kSystemHandle = 0x2;
u32 d_dlopen(u32 name, int) {
    const char *n = str(name);
    if (!n || std::strstr(n, "GloftNAHM")) return kSelfHandle;
    return kSystemHandle;
}
u32 d_dlsym(u32 handle, u32 name) {
    const char *n = str(name);
    if (handle == kSelfHandle && g_lib)
        if (u32 a = lib_export(*g_lib, n)) return a;
    if (u32 a = import_function_address(n, false)) return a;
    log("dlsym: %s not available", n);
    return 0;
}
u32 d_dlerror() {
    static u32 msg = gstrdup("symbol not found");
    return msg;
}
int d_dlclose(u32) { return 0; }
int d_dladdr(u32, u32) { return 0; }
u32 d_find_exidx(u32 pc, u32 count) {
    if (!g_lib) return 0;
    if (pc >= g_lib->moved && pc < g_lib->moved_end) {
        ST32(count, g_lib->moved_exidx_count);
        return g_lib->moved_exidx;
    }
    ST32(count, g_lib->exidx_count);
    return g_lib->exidx;
}

// ---- Android NDK ----------------------------------------------------------------------------------
int g_log_level = 3;  // print DEBUG and above
void android_log(int prio, const char *tag, const std::string &msg) {
    if (prio < g_log_level) return;
    static const char lv[] = "??VDIWEFS";
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    std::fprintf(stderr, "%c/%s: %s%s", lv[prio < 9 ? prio : 0], tag ? tag : "", msg.c_str(),
                 msg.empty() || msg.back() != '\n' ? "\n" : "");
}
void a_log_print(Cpu *c) { Args a(c, 3); android_log(int(c->r[0]), str(c->r[1]), guest_format(str(c->r[2]), a)); c->r[0] = 1; }
void a_log_vprint(Cpu *c) { Args a = Args::va_list(c, c->r[3]); android_log(int(c->r[0]), str(c->r[1]), guest_format(str(c->r[2]), a)); c->r[0] = 1; }

u32 fake_object() { return gcalloc(1, 64); }
u32 a_sensor_manager() { static u32 m = fake_object(); return m; }
u32 a_default_sensor(u32, int) { return 0; }  // no accelerometer/gyroscope on PC
u32 a_create_queue(u32, u32, int, u32, u32) { return fake_object(); }
int a_sensor_fail(u32, u32) { return -1; }
int a_set_rate(u32, u32, int32_t) { return -1; }
int a_get_events(u32, u32, u32) { return 0; }
u32 a_looper() {
    static RT_THREAD_LOCAL(u32, looper);
    u32 &l = RT_TLS(looper);
    if (!l) l = fake_object();
    return l;
}
u32 a_looper_prepare(int) { return a_looper(); }

// ---- OpenSL ES (implemented in the audio step) ------------------------------------------------------
u32 sl_create_engine(u32 engine, u32, u32, u32, u32, u32) {
    if (engine) ST32(engine, 0);
    log("slCreateEngine: audio not implemented yet");
    return 12;  // SL_RESULT_FEATURE_UNSUPPORTED
}
uint32_t init_sl_iid(const char *name) {
    // `const SLInterfaceID SL_IID_x`: pointer variable -> unique 16-byte id
    u32 var = gcalloc(1, 24);
    ST32(var, var + 8);
    uint32_t h = 2166136261u;
    for (const char *p = name; *p; p++) h = (h ^ uint8_t(*p)) * 16777619u;
    for (int i = 0; i < 4; i++) ST32(var + 8 + 4 * i, h * (i + 1));
    return var;
}

}  // namespace

RT_IMPORT(acos, m_acos) RT_IMPORT(asin, m_asin) RT_IMPORT(atan, m_atan) RT_IMPORT(ceil, m_ceil)
RT_IMPORT(cos, m_cos) RT_IMPORT(cosh, m_cosh) RT_IMPORT(exp, m_exp) RT_IMPORT(floor, m_floor)
RT_IMPORT(log, m_log) RT_IMPORT(log10, m_log10) RT_IMPORT(sin, m_sin) RT_IMPORT(sinh, m_sinh)
RT_IMPORT(tan, m_tan) RT_IMPORT(tanh, m_tanh) RT_IMPORT(atan2, m_atan2) RT_IMPORT(fmod, m_fmod)
RT_IMPORT(pow, m_pow) RT_IMPORT(acosf, m_acosf) RT_IMPORT(asinf, m_asinf) RT_IMPORT(atanf, m_atanf)
RT_IMPORT(ceilf, m_ceilf) RT_IMPORT(cosf, m_cosf) RT_IMPORT(expf, m_expf) RT_IMPORT(floorf, m_floorf)
RT_IMPORT(log10f, m_log10f) RT_IMPORT(logf, m_logf) RT_IMPORT(sinf, m_sinf) RT_IMPORT(tanf, m_tanf)
RT_IMPORT(atan2f, m_atan2f) RT_IMPORT(fmodf, m_fmodf) RT_IMPORT(powf, m_powf) RT_IMPORT(ldexp, m_ldexp)
RT_IMPORT(lrintf, m_lrintf) RT_IMPORT(frexp, m_frexp) RT_IMPORT(modf, m_modf)

RT_IMPORT(gettimeofday, t_gettimeofday)
RT_IMPORT(clock_gettime, t_clock_gettime)
RT_IMPORT(time, t_time)
RT_IMPORT(nanosleep, t_nanosleep)
RT_IMPORT(usleep, t_usleep)
RT_IMPORT(sleep, t_sleep)
RT_IMPORT(clock, t_clock)
RT_IMPORT(times, t_times)
RT_IMPORT(localtime_r, t_localtime_r)
RT_IMPORT(gmtime_r, t_gmtime_r)
RT_IMPORT(localtime, t_localtime)
RT_IMPORT(gmtime, t_gmtime)
RT_IMPORT(mktime, t_mktime)
RT_IMPORT(difftime, t_difftime)
RT_IMPORT(asctime, t_asctime)
RT_IMPORT(strftime, t_strftime)
RT_IMPORT(wcsftime, t_wcsftime)
RT_IMPORT(strptime, t_strptime)

RT_IMPORT(getenv, e_getenv)
RT_IMPORT(setlocale, e_setlocale)
RT_IMPORT(sysconf, e_sysconf)
RT_IMPORT(getpid, e_getpid)
RT_IMPORT(getuid, e_getuid) RT_IMPORT(geteuid, e_getuid) RT_IMPORT(getgid, e_getuid) RT_IMPORT(getegid, e_getuid)
RT_IMPORT(setuid, e_setid) RT_IMPORT(setgid, e_setid)
RT_IMPORT(getpwuid, e_getpw) RT_IMPORT(getpwnam, e_getpw)
RT_IMPORT(gethostname, e_gethostname)
RT_IMPORT(kill, e_kill)
RT_IMPORT(raise, e_raise)
RT_IMPORT(bsd_signal, e_bsd_signal)
RT_IMPORT(sigaction, e_sigaction)
RT_IMPORT(fork, e_enosys)
RT_IMPORT_RAW(execle, [](Cpu *c) { set_errno(kENOSYS); c->r[0] = u32(-1); })
RT_IMPORT(waitpid, e_waitpid)
RT_IMPORT(popen, e_popen)
RT_IMPORT(pclose, e_enosys)
RT_IMPORT(system, e_enosys)
RT_IMPORT(exit, e_exit)
RT_IMPORT(abort, e_abort)
RT_IMPORT(__assert2, e_assert2)
RT_IMPORT(__stack_chk_fail, e_stack_chk_fail)
RT_DATA(__stack_chk_guard, init_stack_guard)
RT_IMPORT(__aeabi_atexit, e_atexit)
RT_IMPORT(__google_potentially_blocking_region_begin, e_nop)
RT_IMPORT(__google_potentially_blocking_region_end, e_nop)
RT_IMPORT_RAW(syscall, e_syscall)
RT_IMPORT(setjmp, e_zero)
RT_IMPORT(longjmp, e_longjmp)

RT_IMPORT(dlopen, d_dlopen)
RT_IMPORT(dlsym, d_dlsym)
RT_IMPORT(dlerror, d_dlerror)
RT_IMPORT(dlclose, d_dlclose)
RT_IMPORT(dladdr, d_dladdr)
RT_IMPORT(__gnu_Unwind_Find_exidx, d_find_exidx)


RT_IMPORT_RAW(__android_log_print, a_log_print)
RT_IMPORT_RAW(__android_log_vprint, a_log_vprint)
RT_IMPORT(ASensorManager_getInstance, a_sensor_manager)
RT_IMPORT(ASensorManager_getDefaultSensor, a_default_sensor)
RT_IMPORT(ASensorManager_createEventQueue, a_create_queue)
RT_IMPORT(ASensorEventQueue_enableSensor, a_sensor_fail)
RT_IMPORT(ASensorEventQueue_disableSensor, a_sensor_fail)
RT_IMPORT(ASensorEventQueue_setEventRate, a_set_rate)
RT_IMPORT(ASensorEventQueue_getEvents, a_get_events)
RT_IMPORT(ALooper_forThread, a_looper)
RT_IMPORT(ALooper_prepare, a_looper_prepare)

RT_IMPORT(slCreateEngine, sl_create_engine)
RT_DATA(SL_IID_ENGINE, init_sl_iid)
RT_DATA(SL_IID_PLAY, init_sl_iid)
RT_DATA(SL_IID_RECORD, init_sl_iid)
RT_DATA(SL_IID_BUFFERQUEUE, init_sl_iid)
RT_DATA(SL_IID_ANDROIDSIMPLEBUFFERQUEUE, init_sl_iid)

}  // namespace rt
