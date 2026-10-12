/* Runtime contract for code emitted by recomp/arm2c.py.
 *
 * Guest memory is a flat 4 GiB window at g_mem; a guest address is a uint32_t
 * offset into it. Guest code keeps registers in C locals; the prologue and the call /
 * return macros move them between those locals and the Cpu struct. At a call the struct
 * holds what the callee may read, at a return what the function may have changed (see
 * the _X macros); host code (rt_dispatch, imports, hooks) always sees the full
 * architectural state.
 */
#pragma once
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS 1  /* GW_STRCPY / GW_STRNCPY: the guest's own strcpy, strncpy */
#endif
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union VRegs {             /* VFP/NEON register file: S(2n) is the low half of D(n) */
    double d[32];
    float s[64];
    uint32_t w[64];
    uint64_t x[32];
} VRegs;

typedef struct Cpu {
    uint32_t r[16];
    uint32_t n, z, c, v;          /* APSR flags, each 0 or 1 */
    uint32_t fpscr;
    uint32_t ret_pc;              /* where the last guest return actually went */
    uint32_t ex_addr;             /* exclusive monitor (ldrex/strex) */
    uint64_t ex_val;
    VRegs vr;
    void *host_thread;            /* owned by the runtime */
} Cpu;

typedef void (*GuestFn)(Cpu *cpu, uint32_t entry);
typedef void (*ImportFn)(Cpu *cpu);

typedef struct FuncEntry {
    uint32_t guest;               /* guest address (load base applied) */
    GuestFn fn;
    uint32_t link;                /* link-time address, passed as `entry` */
} FuncEntry;

extern uint8_t *g_mem;
#ifdef __3DS__
extern uint32_t g_load_base;      /* the native loader's choice (runtime/native_3ds.cpp); g_mem is 0 */
#else
extern const uint32_t g_load_base;
#endif
extern const FuncEntry g_funcs[];
extern const uint32_t g_func_count;
extern const char *const g_import_names[];
extern const uint32_t g_import_count;
extern ImportFn g_import_fns[];

/* An indirect call site's cache (RT_ICALL below) */
#if UINTPTR_MAX == 0xffffffffu
typedef const FuncEntry *RtIc;
#else
typedef uint64_t RtIc;
#endif
void rt_dispatch(Cpu *cpu, uint32_t target);
void rt_dispatch_ic(Cpu *cpu, uint32_t target, RtIc *ic);
void rt_call_import(Cpu *cpu, uint32_t index);
void rt_unimplemented(Cpu *cpu, uint32_t addr, uint32_t word);
void rt_unreachable(Cpu *cpu, uint32_t addr);
void rt_trap(Cpu *cpu, uint32_t addr, const char *why);

/* Indirect calls reload only what AAPCS lets a callee change (r0-r3, r12, lr, the flags).
 * The dispatch table calls a target the analysis cannot show keeps r4-r11 and sp through
 * this check (table.c, arm2c.py Flow.run). */
static inline void rt_aapcs_checked(Cpu *cpu, GuestFn fn, uint32_t link) {
    uint32_t keep[8], sp = cpu->r[13];
    memcpy(keep, &cpu->r[4], sizeof keep);
    fn(cpu, link);
    if (memcmp(keep, &cpu->r[4], sizeof keep) || sp != cpu->r[13])
        rt_trap(cpu, link, "indirect call target changed r4-r11 or sp");
}

/* ---- memory ------------------------------------------------------------- */
/* Guest memory holds the ARM's little-endian bytes. A big-endian host (the GameCube's Gekko)
 * swaps every multi-byte access (GCC turns a swapped load / store into lwbrx / stwbrx), and
 * the VFP register file's single-precision views are indexed through VS(): S(2n) is the low
 * half of D(n), which a big-endian union keeps in its second word. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define RECOMP_BIG_ENDIAN 1
#define LE16(v) __builtin_bswap16(v)
#define LE32(v) __builtin_bswap32(v)
#define LE64(v) __builtin_bswap64(v)
#define VS(n) ((n) ^ 1)
#else
#define LE16(v) (v)
#define LE32(v) (v)
#define LE64(v) (v)
#define VS(n) (n)
#endif
#ifdef __GAMECUBE__
/* Guest addresses are host addresses (the pager maps them, runtime/gc_pager.c): the address
 * itself is the pointer, with no base (and no arithmetic on a null g_mem). */
#define HOST(a) ((uint8_t *)(uintptr_t)(uint32_t)(a))
#define GM_ADDR(m, a) ((uint8_t *)(uintptr_t)(uint32_t)(a))
#else
#define HOST(a) (g_mem + (uint32_t)(a))
#define GM_ADDR(m, a) ((m) + (a))
#endif
static inline uint8_t LD8(uint32_t a) { return *HOST(a); }
static inline uint16_t LD16(uint32_t a) { uint16_t v; memcpy(&v, HOST(a), 2); return LE16(v); }
static inline uint32_t LD32(uint32_t a) { uint32_t v; memcpy(&v, HOST(a), 4); return LE32(v); }
static inline uint64_t LD64(uint32_t a) { uint64_t v; memcpy(&v, HOST(a), 8); return LE64(v); }
static inline void ST8(uint32_t a, uint8_t v) { *HOST(a) = v; }
static inline void ST16(uint32_t a, uint32_t v) { uint16_t t = LE16((uint16_t)v); memcpy(HOST(a), &t, 2); }
static inline void ST32(uint32_t a, uint32_t v) { v = LE32(v); memcpy(HOST(a), &v, 4); }
static inline void ST64(uint32_t a, uint64_t v) { v = LE64(v); memcpy(HOST(a), &v, 8); }
/* Floats in guest memory (host hooks and native code; the translated code moves their bits) */
static inline float LDF(uint32_t a) { uint32_t v = LD32(a); float f; memcpy(&f, &v, 4); return f; }
static inline void STF(uint32_t a, float f) { uint32_t v; memcpy(&v, &f, 4); ST32(a, v); }
static inline void LDFV(uint32_t a, float *v, int n) { for (int i = 0; i < n; i++) v[i] = LDF(a + 4u * (uint32_t)i); }
static inline void STFV(uint32_t a, const float *v, int n) { for (int i = 0; i < n; i++) STF(a + 4u * (uint32_t)i, v[i]); }

/* Generated code: the memory base comes from a local of each function (PROLOGUE's gm). The
 * global g_mem would be reloaded after every guest store (a byte store may alias anything);
 * a local whose address is never taken stays in a register. */
static inline uint16_t GM_LD16(uint8_t *m, uint32_t a) { uint16_t v; memcpy(&v, GM_ADDR(m, a), 2); return LE16(v); }
static inline uint32_t GM_LD32(uint8_t *m, uint32_t a) { uint32_t v; memcpy(&v, GM_ADDR(m, a), 4); return LE32(v); }
static inline uint64_t GM_LD64(uint8_t *m, uint32_t a) { uint64_t v; memcpy(&v, GM_ADDR(m, a), 8); return LE64(v); }
static inline void GM_ST16(uint8_t *m, uint32_t a, uint32_t v) { uint16_t t = LE16((uint16_t)v); memcpy(GM_ADDR(m, a), &t, 2); }
static inline void GM_ST32(uint8_t *m, uint32_t a, uint32_t v) { v = LE32(v); memcpy(GM_ADDR(m, a), &v, 4); }
static inline void GM_ST64(uint8_t *m, uint32_t a, uint64_t v) { v = LE64(v); memcpy(GM_ADDR(m, a), &v, 8); }
#ifdef RECOMP_GENERATED
#define LD8(a) (*GM_ADDR(gm, (uint32_t)(a)))
#define LD16(a) GM_LD16(gm, (uint32_t)(a))
#define LD32(a) GM_LD32(gm, (uint32_t)(a))
#define LD64(a) GM_LD64(gm, (uint32_t)(a))
#define ST8(a, v) (*GM_ADDR(gm, (uint32_t)(a)) = (uint8_t)(v))
#define ST16(a, v) GM_ST16(gm, (uint32_t)(a), (v))
#define ST32(a, v) GM_ST32(gm, (uint32_t)(a), (v))
#define ST64(a, v) GM_ST64(gm, (uint32_t)(a), (v))
#ifdef __GAMECUBE__
#define GM_DECL
#define gm ((uint8_t *)0) /* unused: GM_ADDR takes the address itself */
#else
#define GM_DECL uint8_t *const gm = g_mem; (void)gm;
#endif
#else
#define GM_DECL
#endif

/* Atomic compare-and-swap on guest memory, values in the guest's order: 1 if it stored. */
#define LE8(v) (v)
#define GUEST_CAS_IMPL(bits, T)                                                                     static inline int GUEST_CAS##bits(uint32_t a, T expected, T desired) {                              expected = LE##bits(expected);                                                                  return __atomic_compare_exchange_n((T *)HOST(a), &expected, LE##bits(desired), 0,                                                  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);                     }
GUEST_CAS_IMPL(8, uint8_t)
GUEST_CAS_IMPL(16, uint16_t)
GUEST_CAS_IMPL(32, uint32_t)
#ifdef __GAMECUBE__
/* the Gekko has no 64-bit compare and swap; one core: interrupts off around the compare */
static inline int GUEST_CAS64(uint32_t a, uint64_t expected, uint64_t desired) {
    uint32_t msr, off;
    __asm__ volatile("mfmsr %0; rlwinm %1,%0,0,17,15; mtmsr %1" : "=r"(msr), "=&r"(off) : : "memory");
    uint64_t now;
    memcpy(&now, HOST(a), 8);
    const int ok = now == LE64(expected);
    if (ok) { desired = LE64(desired); memcpy(HOST(a), &desired, 8); }
    __asm__ volatile("mtmsr %0" : : "r"(msr) : "memory");
    return ok;
}
#else
GUEST_CAS_IMPL(64, uint64_t)
#endif

/* Exclusive store succeeds (returns 0) only if memory still holds what ldrex saw. */
#define STREX_IMPL(bits, T)                                                                         static inline uint32_t STREX##bits(Cpu *cpu, uint32_t a, uint64_t val) {                            if (cpu->ex_addr != a) return 1;                                                                 cpu->ex_addr = ~0u;                                                                              return GUEST_CAS##bits(a, (T)cpu->ex_val, (T)val) ? 0 : 1;                                   }
STREX_IMPL(8, uint8_t)
STREX_IMPL(16, uint16_t)
STREX_IMPL(32, uint32_t)
STREX_IMPL(64, uint64_t)
#define BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)

/* ---- integer helpers ----------------------------------------------------- */
static inline uint32_t ROR(uint32_t v, unsigned n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }
static inline uint32_t CLZ(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32; }
static inline uint32_t BSWAP32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t RBIT(uint32_t v) {
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0f0f0f0fu) | ((v & 0x0f0f0f0fu) << 4);
    return BSWAP32(v);
}
static inline uint32_t SDIV(uint32_t a, uint32_t b) {
    if (!b) return 0;
    if (a == 0x80000000u && b == 0xffffffffu) return a;
    return (uint32_t)((int32_t)a / (int32_t)b);
}
static inline uint32_t UDIV(uint32_t a, uint32_t b) { return b ? a / b : 0; }
static inline uint32_t SSAT(int32_t v, unsigned bits) {
    int32_t hi = (int32_t)((1u << (bits - 1)) - 1), lo = -hi - 1;
    return (uint32_t)(v > hi ? hi : v < lo ? lo : v);
}
static inline uint32_t USAT(int32_t v, unsigned bits) {
    int64_t hi = (int64_t)(bits >= 32 ? 0xffffffffu : (1u << bits) - 1);
    return (uint32_t)(v < 0 ? 0 : v > hi ? hi : v);
}
static inline uint32_t QADD(uint32_t a, uint32_t b) {
    int64_t r = (int64_t)(int32_t)a + (int32_t)b;
    return (uint32_t)(r > INT32_MAX ? INT32_MAX : r < INT32_MIN ? INT32_MIN : r);
}
static inline uint32_t QSUB(uint32_t a, uint32_t b) {
    int64_t r = (int64_t)(int32_t)a - (int32_t)b;
    return (uint32_t)(r > INT32_MAX ? INT32_MAX : r < INT32_MIN ? INT32_MIN : r);
}
static inline uint32_t QDADD(uint32_t a, uint32_t b) { return QADD(a, QADD(b, b)); }
static inline uint32_t QDSUB(uint32_t a, uint32_t b) { return QSUB(a, QADD(b, b)); }

/* Register-specified shifts (amount = low byte of rs); carry-out to *co. */
static inline uint32_t SH_LSL_R(uint32_t v, uint32_t s, uint32_t *co) {
    s &= 0xff;
    if (s == 0) return v;
    if (s < 32) { *co = (v >> (32 - s)) & 1; return v << s; }
    *co = s == 32 ? v & 1 : 0;
    return 0;
}
static inline uint32_t SH_LSR_R(uint32_t v, uint32_t s, uint32_t *co) {
    s &= 0xff;
    if (s == 0) return v;
    if (s < 32) { *co = (v >> (s - 1)) & 1; return v >> s; }
    *co = s == 32 ? v >> 31 : 0;
    return 0;
}
static inline uint32_t SH_ASR_R(uint32_t v, uint32_t s, uint32_t *co) {
    s &= 0xff;
    if (s == 0) return v;
    if (s < 32) { *co = (v >> (s - 1)) & 1; return (uint32_t)((int32_t)v >> s); }
    *co = v >> 31;
    return (uint32_t)((int32_t)v >> 31);
}
static inline uint32_t SH_ROR_R(uint32_t v, uint32_t s, uint32_t *co) {
    s &= 0xff;
    if (s == 0) return v;
    s &= 31;
    if (s == 0) { *co = v >> 31; return v; }
    *co = (v >> (s - 1)) & 1;
    return ROR(v, s);
}

/* ---- VFP helpers ----------------------------------------------------------- */
static inline float VFP_IMM_S(uint32_t imm8) {
    uint32_t sign = imm8 >> 7, b6 = (imm8 >> 6) & 1;
    uint32_t bits = (sign << 31) | ((b6 ? 0x1fu : 0x20u) << 25) | ((imm8 & 0x3f) << 19);
    float f; memcpy(&f, &bits, 4); return f;
}
static inline double VFP_IMM_D(uint32_t imm8) {
    uint64_t sign = imm8 >> 7, b6 = (imm8 >> 6) & 1;
    uint64_t bits = (sign << 63) | ((b6 ? 0xffull : 0x100ull) << 54) | ((uint64_t)(imm8 & 0x3f) << 48);
    double d; memcpy(&d, &bits, 8); return d;
}
/* vcmp: sets FPSCR NZCV (lt=1000, eq=0110, gt=0010, unordered=0011). */
static inline uint32_t VFP_CMP(uint32_t fpscr, double a, double b) {
    uint32_t f = a < b ? 0x8u : a == b ? 0x6u : a > b ? 0x2u : 0x3u;
    return (fpscr & 0x0fffffffu) | (f << 28);
}
static inline uint32_t CVT_S32_RZ(double v) {
    if (v != v) return 0;
    if (v >= 2147483647.0) return 0x7fffffffu;
    if (v <= -2147483648.0) return 0x80000000u;
    return (uint32_t)(int32_t)v;
}
static inline uint32_t CVT_U32_RZ(double v) {
    if (v != v || v <= 0.0) return 0;
    if (v >= 4294967295.0) return 0xffffffffu;
    return (uint32_t)v;
}
static inline uint32_t CVT_S16_RZ(double v) {
    if (v != v) return 0;
    if (v >= 32767.0) return 0x7fffu;
    if (v <= -32768.0) return 0xffff8000u;
    return (uint32_t)(int32_t)v;
}
static inline uint32_t CVT_U16_RZ(double v) {
    if (v != v || v <= 0.0) return 0;
    if (v >= 65535.0) return 0xffffu;
    return (uint32_t)v;
}
static inline uint32_t CVT_S32_RN(double v) { return CVT_S32_RZ(nearbyint(v)); }
static inline uint32_t CVT_U32_RN(double v) { return CVT_U32_RZ(nearbyint(v)); }
#define FABS(x) fabs(x)
#define SQRT(x) sqrt(x)

/* ---- imports compiled in place ---------------------------------------------- */
/* arm2c.py's INLINE_IMPORTS: calls to these imports become plain C at the call site, the
 * same host calls as the runtime's implementations (runtime/libc_string.cpp, libc_misc.cpp:
 * guest pointers are gm + address, floats and doubles travel in core registers, AAPCS
 * softfp), with no trip through the import table. GW_ names write guest memory (arm2c.py's
 * register-flow analysis treats them as stores through a pointer). */
#ifdef RECOMP_GENERATED
#if defined(__GNUC__) || defined(__clang__)
/* keeps the compiler from folding a call with a constant argument at build time, which could
 * round differently from the host library the runtime calls */
#define IM_OPAQUE(x) __asm__("" : "+r"(x))
#else
#define IM_OPAQUE(x) ((void)0)
#endif
static inline float IM_U2F(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t IM_F2U(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline double IM_U2D(uint32_t lo, uint32_t hi) { uint64_t u = (uint64_t)hi << 32 | lo; double d; memcpy(&d, &u, 8); return d; }
static inline uint64_t IM_D2U(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
#define IM_F1(f) static inline uint32_t IM_##f(uint32_t a) { IM_OPAQUE(a); return IM_F2U(f(IM_U2F(a))); }
#define IM_F2(f) static inline uint32_t IM_##f(uint32_t a, uint32_t b) { IM_OPAQUE(a); IM_OPAQUE(b); return IM_F2U(f(IM_U2F(a), IM_U2F(b))); }
#define IM_D1(f) static inline uint64_t IM_##f(uint32_t a, uint32_t b) { IM_OPAQUE(a); IM_OPAQUE(b); return IM_D2U(f(IM_U2D(a, b))); }
#define IM_D2(f) static inline uint64_t IM_##f(uint32_t a, uint32_t b, uint32_t c, uint32_t d) \
    { IM_OPAQUE(a); IM_OPAQUE(b); IM_OPAQUE(c); IM_OPAQUE(d); return IM_D2U(f(IM_U2D(a, b), IM_U2D(c, d))); }
IM_F1(acosf) IM_F1(asinf) IM_F1(atanf) IM_F1(ceilf) IM_F1(cosf) IM_F1(expf) IM_F1(floorf) IM_F1(log10f)
IM_F1(logf) IM_F1(sinf) IM_F1(tanf)
IM_F2(atan2f) IM_F2(fmodf) IM_F2(powf)
IM_D1(acos) IM_D1(asin) IM_D1(atan) IM_D1(ceil) IM_D1(cos) IM_D1(cosh) IM_D1(exp) IM_D1(floor) IM_D1(log)
IM_D1(log10) IM_D1(sin) IM_D1(sinh) IM_D1(tan) IM_D1(tanh)
IM_D2(atan2) IM_D2(fmod) IM_D2(pow)
#define GR_STRSTR(h, n) IM_gaddr(gm, strstr((const char *)GM_ADDR(gm, (uint32_t)(h)), (const char *)GM_ADDR(gm, (uint32_t)(n))))
#define GR_ISALNUM(c) ((uint32_t)((int32_t)(c) >= 0 && (int32_t)(c) < 128 &&                                   (((c) | 32) - 'a' < 26u || (c) - '0' < 10u)))
/* The game's most frequent thread calls (runtime/thread.cpp): no register state of their own,
 * so called directly; a lock that has to wait is timed for the performance log. */
uint32_t rt_im_getspecific(uint32_t key);
uint32_t rt_im_self(void);
uint32_t rt_im_mutex_lock(uint32_t m);
uint32_t rt_im_mutex_unlock(uint32_t m);
#define GW_MEMCPY(d, s, n) memcpy(GM_ADDR(gm, (uint32_t)(d)), GM_ADDR(gm, (uint32_t)(s)), (uint32_t)(n))
#define GW_MEMMOVE(d, s, n) memmove(GM_ADDR(gm, (uint32_t)(d)), GM_ADDR(gm, (uint32_t)(s)), (uint32_t)(n))
#define GW_MEMSET(d, c, n) memset(GM_ADDR(gm, (uint32_t)(d)), (int)(c), (uint32_t)(n))
#define GW_WMEMCPY(d, s, n) memcpy(GM_ADDR(gm, (uint32_t)(d)), GM_ADDR(gm, (uint32_t)(s)), (size_t)(uint32_t)(n) * 4)
#define GW_STRCPY(d, s) strcpy((char *)GM_ADDR(gm, (uint32_t)(d)), (const char *)GM_ADDR(gm, (uint32_t)(s)))
#define GW_STRNCPY(d, s, n) strncpy((char *)GM_ADDR(gm, (uint32_t)(d)), (const char *)GM_ADDR(gm, (uint32_t)(s)), (uint32_t)(n))
#define GR_MEMCMP(a, b, n) ((uint32_t)memcmp(GM_ADDR(gm, (uint32_t)(a)), GM_ADDR(gm, (uint32_t)(b)), (uint32_t)(n)))
#define GR_STRLEN(s) ((uint32_t)strlen((const char *)GM_ADDR(gm, (uint32_t)(s))))
#define GR_STRCMP(a, b) ((uint32_t)strcmp((const char *)GM_ADDR(gm, (uint32_t)(a)), (const char *)GM_ADDR(gm, (uint32_t)(b))))
#define GR_STRNCMP(a, b, n) ((uint32_t)strncmp((const char *)GM_ADDR(gm, (uint32_t)(a)), (const char *)GM_ADDR(gm, (uint32_t)(b)), (uint32_t)(n)))
static inline uint32_t IM_gaddr(const uint8_t *m, const void *p) { return p ? (uint32_t)((uintptr_t)p - (uintptr_t)m) : 0; }
#define GR_STRCHR(s, c) IM_gaddr(gm, strchr((const char *)GM_ADDR(gm, (uint32_t)(s)), (int)(c)))
#define GR_STRRCHR(s, c) IM_gaddr(gm, strrchr((const char *)GM_ADDR(gm, (uint32_t)(s)), (int)(c)))
#define GR_MEMCHR(s, c, n) IM_gaddr(gm, memchr(GM_ADDR(gm, (uint32_t)(s)), (int)(c), (uint32_t)(n)))
/* bionic's C locale: ASCII folding, as runtime/libc_string.cpp casecmp */
static inline uint32_t IM_casecmp(const uint8_t *a, const uint8_t *b) {
    for (;; a++, b++) {
        int ca = *a, cb = *b;
        ca += ((unsigned)(ca - 'A') < 26u) << 5;
        cb += ((unsigned)(cb - 'A') < 26u) << 5;
        if (ca != cb || !ca) return (uint32_t)(ca - cb);
    }
}
#define GR_STRCASECMP(a, b) IM_casecmp(GM_ADDR(gm, (uint32_t)(a)), GM_ADDR(gm, (uint32_t)(b)))
#define GR_TOLOWER(c) ((int32_t)(c) >= 'A' && (int32_t)(c) <= 'Z' ? (uint32_t)(c) + 32 : (uint32_t)(c))
static inline uint32_t IM_wcslen(const uint8_t *m, uint32_t s) { uint32_t n = 0; while (GM_LD32((uint8_t *)m, s + 4 * n)) n++; return n; }
#define GR_WCSLEN(s) IM_wcslen(gm, (uint32_t)(s))
#endif

/* ---- function plumbing ------------------------------------------------------ */
#define LOCALS                                                                           \
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, sp, lr;               \
    uint32_t n, z, c, v
#define PROLOGUE                                                                         \
    LOCALS;                                                                               \
    GM_DECL                                                                               \
    VDECL                                                                                 \
    RELOAD;                                                                               \
    (void)entry
/* No-ops kept for code generated by the earlier arm2c.py, which held VFP registers in
 * locals and redefined these around each function; current output never uses them. */
#define VDECL
#define VSPILL ((void)0)
#define VRELOAD ((void)0)
#define SPILL                                                                            \
    (cpu->r[0] = r0, cpu->r[1] = r1, cpu->r[2] = r2, cpu->r[3] = r3, cpu->r[4] = r4,      \
     cpu->r[5] = r5, cpu->r[6] = r6, cpu->r[7] = r7, cpu->r[8] = r8, cpu->r[9] = r9,      \
     cpu->r[10] = r10, cpu->r[11] = r11, cpu->r[12] = r12, cpu->r[13] = sp,               \
     cpu->r[14] = lr, cpu->n = n, cpu->z = z, cpu->c = c, cpu->v = v, VSPILL)
#define RELOAD                                                                           \
    (r0 = cpu->r[0], r1 = cpu->r[1], r2 = cpu->r[2], r3 = cpu->r[3], r4 = cpu->r[4],      \
     r5 = cpu->r[5], r6 = cpu->r[6], r7 = cpu->r[7], r8 = cpu->r[8], r9 = cpu->r[9],      \
     r10 = cpu->r[10], r11 = cpu->r[11], r12 = cpu->r[12], sp = cpu->r[13],               \
     lr = cpu->r[14], n = cpu->n, z = cpu->z, c = cpu->c, v = cpu->v, VRELOAD)
/* Returns record their real target in cpu->ret_pc. A call site that knows its return
 * address R checks it afterwards: if the callee returned elsewhere (e.g. libgcc's
 * `bl local_label` helpers that pop the *caller's* frame), the caller returns too and
 * the chain unwinds to the frame that matches. Tail transfers keep the callee's ret_pc.
 * FRAME_EXIT is redefined around functions that own C++ catch frames. */
#define FRAME_EXIT ((void)0)
#define RET_TO(x) do { cpu->ret_pc = (x); SPILL; FRAME_EXIT; return; } while (0)
#define RET RET_TO(lr)
#define RET_PROPAGATE do { SPILL; FRAME_EXIT; return; } while (0)
#define RET_CHECK(R) do { if ((cpu->ret_pc ^ (uint32_t)(R)) & ~1u) RET_PROPAGATE; } while (0)
#define CALL(f) do { SPILL; f(cpu, 0); RELOAD; } while (0)
/* Indirect calls go through a cache per call site, so the usual single-target site calls the
 * function without the lookup (rt_dispatch_ic fills it). 64-bit hosts: the last target's guest
 * address and its g_funcs index + 1 in one 64-bit word (one atomic load reads a consistent pair).
 * 32-bit hosts (the GameCube, no 64-bit atomics): the last target's g_funcs entry, whose own guest
 * address is compared. */
#if UINTPTR_MAX == 0xffffffffu
static inline void RT_ICALL(Cpu *cpu, uint32_t t, RtIc *ic) {
    const FuncEntry *f = __atomic_load_n(ic, __ATOMIC_RELAXED);
    if (f && f->guest == t) {
        f->fn(cpu, f->link);
        return;
    }
    rt_dispatch_ic(cpu, t, ic);
}
#else
static inline void RT_ICALL(Cpu *cpu, uint32_t t, RtIc *ic) {
    uint64_t e = __atomic_load_n(ic, __ATOMIC_RELAXED);
    if ((uint32_t)e == t && (e >> 32)) {
        const FuncEntry *f = &g_funcs[(e >> 32) - 1];
        f->fn(cpu, f->link);
        return;
    }
    rt_dispatch_ic(cpu, t, ic);
}
#endif
#define CALLI(t) do { uint32_t t_ = (t); static RtIc ic_; SPILL; RT_ICALL(cpu, t_, &ic_); RELOAD; } while (0)
#define CALL_RET(f, R) do { CALL(f); RET_CHECK(R); } while (0)
#define CALLI_RET(t, R) do { CALLI(t); RET_CHECK(R); } while (0)
#define TAIL_CALL(f) do { SPILL; FRAME_EXIT; f(cpu, 0); return; } while (0)
#define TAIL_CALLI(t) do { uint32_t t_ = (t); static RtIc ic_; SPILL; FRAME_EXIT; RT_ICALL(cpu, t_, &ic_); return; } while (0)
#define IMPORT(i) do { SPILL; rt_call_import(cpu, i); RELOAD; } while (0)

/* The macros above move every register; Thumb code and functions arm2c.py does not analyse
 * use them. Everything else uses the _X forms, which take the lists arm2c.py's register-flow
 * analysis computed for that site: s = registers to store into the Cpu struct before,
 * l = registers to load after, p = registers to store when a callee returned elsewhere,
 * each written as SV(r4) SV(lr) ... / LV(r0) ... (an empty list moves nothing). Locals the
 * prologue does not load are written before they are read or stored. */
#define CPU_r0 cpu->r[0]
#define CPU_r1 cpu->r[1]
#define CPU_r2 cpu->r[2]
#define CPU_r3 cpu->r[3]
#define CPU_r4 cpu->r[4]
#define CPU_r5 cpu->r[5]
#define CPU_r6 cpu->r[6]
#define CPU_r7 cpu->r[7]
#define CPU_r8 cpu->r[8]
#define CPU_r9 cpu->r[9]
#define CPU_r10 cpu->r[10]
#define CPU_r11 cpu->r[11]
#define CPU_r12 cpu->r[12]
#define CPU_sp cpu->r[13]
#define CPU_lr cpu->r[14]
#define CPU_n cpu->n
#define CPU_z cpu->z
#define CPU_c cpu->c
#define CPU_v cpu->v
#define SV(x) CPU_##x = x;
#define LV(x) x = CPU_##x;
#define PROLOGUE_X(l) LOCALS; GM_DECL l (void)entry
#define RET_CHECK_X(R, p) if ((cpu->ret_pc ^ (uint32_t)(R)) & ~1u) { p FRAME_EXIT; return; }
#define CALL_X(f, R, s, l, p) do { s f(cpu, 0); l RET_CHECK_X(R, p) } while (0)
/* A call to an entry the analysis shows always returns to its lr (and so to R): no check. */
#define CALLN_X(f, s, l) do { s f(cpu, 0); l } while (0)
#define CALLI_X(t, R, s, l, p) do { uint32_t t_ = (t); static RtIc ic_; s RT_ICALL(cpu, t_, &ic_); l RET_CHECK_X(R, p) } while (0)
#define TAIL_X(f, s) do { s FRAME_EXIT; f(cpu, 0); return; } while (0)
#define TAILI_X(t, s) do { uint32_t t_ = (t); static RtIc ic_; s FRAME_EXIT; RT_ICALL(cpu, t_, &ic_); return; } while (0)
#define IMPORT_X(i, s, l) do { s rt_call_import(cpu, i); l } while (0)
#define RET_X(s) do { cpu->ret_pc = lr; s FRAME_EXIT; return; } while (0)
#define RET_TO_X(x, s) do { cpu->ret_pc = (x); s FRAME_EXIT; return; } while (0)
/* After a call that does not return. The fatal paths of the _X code end the C function too,
 * so that no C path leads from them into code that was emitted for other paths only. */
#define NORETURN_X(a) do { rt_trap(cpu, a, "returned from a noreturn call"); FRAME_EXIT; return; } while (0)
#define TRAP_X(t) do { t; FRAME_EXIT; return; } while (0)
/* C++ exceptions: functions owning landing pads register a catch frame; the runtime's
 * restore_core_regs replacement (runtime/eh.cpp) longjmps to it with the landing pad
 * as `entry`, which the entry switch then dispatches. */
typedef struct RtEhFrame {
    struct RtEhFrame *prev;
    uint32_t lo, hi;      /* link-time range of the C function */
    uint32_t entry_sp;    /* guest sp on entry: distinguishes recursive activations */
    uint32_t target;      /* landing pad (link address) after a jump */
    jmp_buf jb;           /* C library setjmp (see GUEST_SETJMP for why not the builtin) */
} RtEhFrame;
jmp_buf *rt_eh_push(Cpu *cpu, RtEhFrame *f, uint32_t lo, uint32_t hi);
void rt_eh_pop(RtEhFrame *f);
#define EH_BEGIN(lo, hi) \
    RtEhFrame eh_; \
    if (setjmp(*rt_eh_push(cpu, &eh_, lo, hi))) { RELOAD; entry = eh_.target; }

/* setjmp: a host jmp_buf on this function's host stack (alloca: it lives until the function
 * returns), its address kept in the guest jmp_buf; longjmp (libc_misc.cpp) restores the guest
 * callee-saved registers and longjmp()s back here. The C library's setjmp/longjmp, not
 * __builtin_setjmp: with clang on Win64 the builtin pair restores a frame pointer offset
 * from the real one whenever the function's frame pointer is not at its frame base (e.g.
 * when it saves xmm6-15), and the resumed code then reads its spilled values from the wrong
 * slots. */
jmp_buf *rt_setjmp_prepare(Cpu *cpu, void *host_buf);
#define GUEST_SETJMP() do { SPILL; if (setjmp(*rt_setjmp_prepare(cpu, __builtin_alloca(sizeof(jmp_buf)))) == 0) cpu->r[0] = 0; RELOAD; } while (0)
#define SETJMP_X(s, l) do { s if (setjmp(*rt_setjmp_prepare(cpu, __builtin_alloca(sizeof(jmp_buf)))) == 0) cpu->r[0] = 0; l } while (0)
#define UNIMPL(a, w) do { SPILL; rt_unimplemented(cpu, a, w); RELOAD; } while (0)
#define UNREACHABLE(a) do { SPILL; rt_unreachable(cpu, a); } while (0)
#define UNIMPL_X(a, w, s) do { s rt_unimplemented(cpu, a, w); FRAME_EXIT; return; } while (0)
#define UNREACHABLE_X(a) do { rt_unreachable(cpu, a); FRAME_EXIT; return; } while (0)
#define TRAP_BKPT() rt_trap(cpu, 0, "bkpt")
#define TRAP_UDF(a) rt_trap(cpu, a, "udf")

#ifdef __cplusplus
}
#endif
