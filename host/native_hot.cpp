// Native (host) reimplementations of hot game functions.
//
// Each entry point has the signature arm2c.py's OVERRIDES expect:
//   replace: extern "C" void fn(Cpu *cpu)
//   wrap:    extern "C" void fn(Cpu *cpu, uint32_t entry, GuestFn original)
// The wrap form is used wherever an original exists, so every function keeps a fallback
// (NOVA3_NATIVE_OFF) and a self-check (NOVA3_NATIVE_CHECK) that runs the recompiled original
// and the native code on the same inputs and logs the first differences.
//
//   NOVA3_NATIVE_CHECK=all | 1 | skin,particle,anim,ttf    compare native against the original
//   NOVA3_NATIVE_OFF=all | 1 | skin,particle,anim,ttf      use the original (recompiled) code
//
// Floating point: the guest uses single-precision VFP without flush-to-zero; the recompiled
// code evaluates every VFP instruction as one rounded float operation (vmla = mul, then add;
// built with -ffp-contract=off). The code here performs the same operations in the same
// order, so results are bit-identical. SIMD only runs independent lanes (x/y/z of one vertex)
// side by side, never reassociates. THIS FILE MUST NOT BE BUILT WITH FP CONTRACTION (fused
// multiply-add): the pragmas below turn it off; also pass -ffp-contract=off (GEN_FLAGS).
//
// Layouts and semantics are documented next to each function (offsets from the ARM code of
// libGloftNAHM.so 1.0.1d; link addresses).
#include "runtime.h"
#include "tls.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#define NH_SSE2 1
#include <emmintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#define NH_NEON 1
#include <arm_neon.h>
#endif

namespace nh {

// ---- configuration -------------------------------------------------------------------
enum : unsigned { kSkin = 1, kParticle = 2, kAnim = 4, kTtf = 8, kAll = 0xffffffffu };

unsigned parse_mask(const char *name) {
    const char *v = std::getenv(name);
    if (!v || !*v || !std::strcmp(v, "0")) return 0;
    if (!std::strcmp(v, "1") || !std::strcmp(v, "all")) return kAll;
    unsigned m = 0;
    struct { const char *n; unsigned b; } names[] = {{"skin", kSkin}, {"particle", kParticle}, {"anim", kAnim}, {"ttf", kTtf}};
    for (auto &e : names)
        if (std::strstr(v, e.n)) m |= e.b;
    return m;
}
struct Config {
    unsigned check, off;
    Config() : check(parse_mask("NOVA3_NATIVE_CHECK")), off(parse_mask("NOVA3_NATIVE_OFF")) {
        if (check || off) rt::log("native_hot: check mask %x, off mask %x", check, off);
    }
};
const Config &cfg() { static Config c; return c; }
inline bool checking(unsigned f) { return (cfg().check & f) != 0; }
inline bool disabled(unsigned f) { return (cfg().off & f) != 0; }

// ---- check bookkeeping -----------------------------------------------------------------
struct CheckStats {
    const char *name;
    uint64_t calls = 0, bad = 0;
    explicit CheckStats(const char *n) : name(n) {}
    // returns whether this mismatch should be printed in detail
    bool mismatch() {
        bad++;
        return bad <= 20 || (bad & (bad - 1)) == 0;
    }
    void done() {
        calls++;
        if ((calls & 0x3fff) == 0)
            rt::log("native check %s: %llu calls, %llu with differences", name, (unsigned long long)calls,
                    (unsigned long long)bad);
    }
};

// Compare two byte ranges word by word; log the first differing words.
// Two NaNs count as equal: when both operands of an operation are NaN, x86 and ARM return
// the payload of whichever operand is "first", which depends on register allocation in the
// compiled C (the recompiled code does not define it either).
inline bool nan_word(uint32_t w) { return (w & 0x7f800000u) == 0x7f800000u && (w & 0x007fffffu); }
bool compare_words(CheckStats &st, const char *what, uint32_t guest_addr, const uint8_t *orig, const uint8_t *nat,
                   uint32_t size) {
    if (!std::memcmp(orig, nat, size)) return true;
    bool differ = false;
    for (uint32_t o = 0; o < size && !differ; o += 4) {
        uint32_t a = 0, b = 0;
        std::memcpy(&a, orig + o, size - o < 4 ? size - o : 4);
        std::memcpy(&b, nat + o, size - o < 4 ? size - o : 4);
        differ = a != b && !(nan_word(a) && nan_word(b));
    }
    if (!differ) return true;
    if (st.mismatch()) {
        int shown = 0;
        for (uint32_t o = 0; o + 4 <= size && shown < 6; o += 4) {
            uint32_t a, b;
            std::memcpy(&a, orig + o, 4);
            std::memcpy(&b, nat + o, 4);
            if (a == b || (nan_word(a) && nan_word(b))) continue;
            float fa, fb;
            std::memcpy(&fa, &a, 4);
            std::memcpy(&fb, &b, 4);
            rt::log("native check %s: %s @%08x+%x: original %08x (%g) native %08x (%g)", st.name, what, guest_addr, o,
                    a, fa, b, fb);
            shown++;
        }
        if (!shown) rt::log("native check %s: %s @%08x: byte difference", st.name, what, guest_addr);
    }
    return false;
}

// ---- guest memory / calls --------------------------------------------------------------
// (LDF / STF by guest address: recomp.h.) Through host pointers into guest memory, in its
// little-endian order:
inline uint32_t ldw(const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return LE32(v); }
inline void stw(uint8_t *p, uint32_t v) { v = LE32(v); std::memcpy(p, &v, 4); }
inline uint16_t ldh(const uint8_t *p) { uint16_t v; std::memcpy(&v, p, 2); return LE16(v); }
inline float ldf(const uint8_t *p) { uint32_t v = ldw(p); float f; std::memcpy(&f, &v, 4); return f; }
inline void stf(uint8_t *p, float f) { uint32_t v; std::memcpy(&v, &f, 4); stw(p, v); }
inline uint32_t fbits(float f) { uint32_t w; std::memcpy(&w, &f, 4); return w; }
inline float bitsf(uint32_t w) { float f; std::memcpy(&f, &w, 4); return f; }

// A guest call made from a function that replaces a guest function: AAPCS lets the callee
// clobber r0-r3, r12, lr and flags, which is exactly what the replaced function itself was
// allowed to do; lr (the replaced function's return address, needed by the override stub)
// and sp are restored by GuestFrame. Callee-saved registers are preserved by the callee.
struct GuestFrame {
    Cpu *cpu;
    uint32_t sp0, lr0;
    GuestFrame(Cpu *c, uint32_t bytes) : cpu(c), sp0(c->r[13]), lr0(c->r[14]) { c->r[13] = (sp0 - bytes) & ~7u; }
    ~GuestFrame() {
        cpu->r[13] = sp0;
        cpu->r[14] = lr0;
    }
    uint32_t sp() const { return cpu->r[13]; }
    uint32_t call(uint32_t fn, uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0) {
        cpu->r[0] = a0;
        cpu->r[1] = a1;
        cpu->r[2] = a2;
        cpu->r[3] = a3;
        cpu->r[14] = rt::kReturnSentinel;
        rt_dispatch(cpu, fn);
        return cpu->r[0];
    }
};

// =========================================================================================
// 1. Software skinning: glitch::collada::detail::CSoftwareSkinTechnique::skin (0x516504)
// =========================================================================================
// skin() zeroes the output streams, then for every bone b (entries {int count; pair *w} of
// 8 bytes, matrices 4x4 column-major of 64 bytes) runs an inner loop over
// std::pair<float weight, uint16 vertex>: out[v] += w * (M * in[v]) for positions (with
// translation) and normals / tangents / binormals (3x3 part). It has three layouts (P, PN,
// PNTB) and, per bone, a NEON kernel chosen by a byte flag at link 0x18be170 that
// CSoftwareSkinTechnique::init sets from android_getCpuFeatures() & NEON (only init and skin
// reference it). The runtime reports no NEON, so the recompiled C loops run.
//
// Native path: the skin wrapper sets the flag, and the four NEON kernels (NEON assembly the
// recompiler cannot translate) are replaced by the host kernels below, which perform exactly
// the C loops' arithmetic:
//   pos_k  = ((x*m[k] + y*m[4+k]) + m[12+k]) + z*m[8+k];   out_k = out_k + pos_k * w
//   rot_k  =  (x*m[k] + y*m[4+k]) + z*m[8+k];               out_k = out_k + rot_k * w
// (k = 0..2, each step one rounded float op; VFP vmla = round(mul) then round(add)), per
// pair in order, streams in the order P, N, T, B, each "load in, load out, store out" like
// the C loop. Everything else in skin (setup, map/unmap, zeroing, frame stamp) stays guest code.
//
// Kernel arguments (call sites 0x517348, 0x517810, 0x5178b0, 0x517904):
//   neonSkinP   (n, M, W, sInP, inP, sOutP, outP)
//   neonSkinPN  (n, M, W, sInP, inP, sInN, inN, sOutP, outP, sOutN, outN)
//   neonSkinPNTB(n, M, W, sInP, inP, sInN, inN, sInT, inT, sInB, inB,
//                sOutP, outP, sOutN, outN, sOutT, outT, sOutB, outB)
//   neonSkinPNTB_stride_optim(n, M, W, sIn, inP, inN, inT, inB, sOut, outP, outN, outT, outB)
// Strides are vertex strides in bytes (u16 in the stream descriptors); n > 0 is checked by
// the caller.
constexpr uint32_t kSkinNeonFlag = 0x18be170;

RT_THREAD_LOCAL(std::vector<uint32_t> *, t_skin_log);  // check mode: output addresses written

struct SkinArgs {
    int count;
    uint32_t m, w;
    int streams;  // 1, 2 or 4
    uint32_t in[4], sin[4], out[4], sout[4];
};

#if NH_SSE2
inline __m128 load3(const uint8_t *p) {
    __m128 xy = _mm_castpd_ps(_mm_load_sd(reinterpret_cast<const double *>(p)));
    __m128 z = _mm_load_ss(reinterpret_cast<const float *>(p + 8));
    return _mm_movelh_ps(xy, z);
}
inline void store3(uint8_t *p, __m128 v) {
    _mm_store_sd(reinterpret_cast<double *>(p), _mm_castps_pd(v));
    _mm_store_ss(reinterpret_cast<float *>(p + 8), _mm_movehl_ps(v, v));
}
#endif

template <int NS>
void skin_kernel(const SkinArgs &a) {
    const uint8_t *wp = HOST(a.w);
    const uint8_t *mp = HOST(a.m);
    std::vector<uint32_t> *log = RT_TLS(t_skin_log);
#if NH_SSE2
    const __m128 c0 = _mm_loadu_ps(reinterpret_cast<const float *>(mp));
    const __m128 c1 = _mm_loadu_ps(reinterpret_cast<const float *>(mp + 16));
    const __m128 c2 = _mm_loadu_ps(reinterpret_cast<const float *>(mp + 32));
    const __m128 c3 = _mm_loadu_ps(reinterpret_cast<const float *>(mp + 48));
    for (int i = 0; i < a.count; i++, wp += 8) {
        const __m128 w = _mm_set1_ps(ldf(wp));
        const uint16_t v = ldh(wp + 4);
        for (int s = 0; s < NS; s++) {
            const uint8_t *in = HOST(a.in[s] + v * a.sin[s]);
            uint32_t oa = a.out[s] + v * a.sout[s];
            uint8_t *out = HOST(oa);
            __m128 p = load3(in);
            __m128 x = _mm_shuffle_ps(p, p, 0x00), y = _mm_shuffle_ps(p, p, 0x55), z = _mm_shuffle_ps(p, p, 0xaa);
            __m128 t = _mm_add_ps(_mm_mul_ps(x, c0), _mm_mul_ps(y, c1));
            if (s == 0) t = _mm_add_ps(t, c3);
            t = _mm_add_ps(t, _mm_mul_ps(z, c2));
            store3(out, _mm_add_ps(load3(out), _mm_mul_ps(t, w)));
            if (log) log->push_back(oa);
        }
    }
#elif NH_NEON
    const float32x4_t c0 = vld1q_f32(reinterpret_cast<const float *>(mp));
    const float32x4_t c1 = vld1q_f32(reinterpret_cast<const float *>(mp + 16));
    const float32x4_t c2 = vld1q_f32(reinterpret_cast<const float *>(mp + 32));
    const float32x4_t c3 = vld1q_f32(reinterpret_cast<const float *>(mp + 48));
    for (int i = 0; i < a.count; i++, wp += 8) {
        const float w = ldf(wp);
        const uint16_t v = ldh(wp + 4);
        for (int s = 0; s < NS; s++) {
            const uint8_t *in = HOST(a.in[s] + v * a.sin[s]);
            uint32_t oa = a.out[s] + v * a.sout[s];
            uint8_t *out = HOST(oa);
            float32x4_t t = vaddq_f32(vmulq_n_f32(c0, ldf(in)), vmulq_n_f32(c1, ldf(in + 4)));
            if (s == 0) t = vaddq_f32(t, c3);
            t = vaddq_f32(t, vmulq_n_f32(c2, ldf(in + 8)));
            float32x2_t oxy = vld1_f32(reinterpret_cast<const float *>(out));
            float oz = ldf(out + 8);
            float32x4_t tw = vmulq_n_f32(t, w);
            vst1_f32(reinterpret_cast<float *>(out), vadd_f32(oxy, vget_low_f32(tw)));
            stf(out + 8, oz + vgetq_lane_f32(tw, 2));
            if (log) log->push_back(oa);
        }
    }
#else
    float m[16];
    for (int k = 0; k < 16; k++) m[k] = ldf(mp + 4 * k);
    for (int i = 0; i < a.count; i++, wp += 8) {
        const float w = ldf(wp);
        const uint16_t v = ldh(wp + 4);
        for (int s = 0; s < NS; s++) {
            const uint8_t *in = HOST(a.in[s] + v * a.sin[s]);
            uint32_t oa = a.out[s] + v * a.sout[s];
            uint8_t *out = HOST(oa);
            float x = ldf(in), y = ldf(in + 4), z = ldf(in + 8);
            float t[3];
            for (int k = 0; k < 3; k++) {
                float xm = x * m[k];
                float ym = y * m[4 + k];
                float r = xm + ym;
                if (s == 0) r = r + m[12 + k];
                float zm = z * m[8 + k];
                t[k] = r + zm;
            }
            for (int k = 0; k < 3; k++) {
                float tw = t[k] * w;
                stf(out + 4 * k, ldf(out + 4 * k) + tw);
            }
            if (log) log->push_back(oa);
        }
    }
#endif
}

void run_skin(const SkinArgs &a) {
    if (a.count <= 0) return;
    switch (a.streams) {
    case 1: skin_kernel<1>(a); break;
    case 2: skin_kernel<2>(a); break;
    default: skin_kernel<4>(a); break;
    }
}

// Argument word i of the current call (0-3 registers, then the stack).
inline uint32_t arg(Cpu *cpu, int i) { return i < 4 ? cpu->r[i] : LD32(cpu->r[13] + (i - 4) * 4); }

CheckStats g_skin_stats("skin");

}  // namespace nh

using namespace nh;

// glitch::collada::detail::neonSkinP(int, const float*, const std::pair<float,unsigned short>*, int, const float*, int, const float*)
extern "C" void pc_native_skin_p(Cpu *cpu) {
    SkinArgs a{};
    a.count = int(arg(cpu, 0)), a.m = arg(cpu, 1), a.w = arg(cpu, 2), a.streams = 1;
    a.sin[0] = arg(cpu, 3), a.in[0] = arg(cpu, 4), a.sout[0] = arg(cpu, 5), a.out[0] = arg(cpu, 6);
    run_skin(a);
}

// glitch::collada::detail::neonSkinPN(int, M, W, sInP, inP, sInN, inN, sOutP, outP, sOutN, outN)
extern "C" void pc_native_skin_pn(Cpu *cpu) {
    SkinArgs a{};
    a.count = int(arg(cpu, 0)), a.m = arg(cpu, 1), a.w = arg(cpu, 2), a.streams = 2;
    for (int s = 0; s < 2; s++) {
        a.sin[s] = arg(cpu, 3 + 2 * s), a.in[s] = arg(cpu, 4 + 2 * s);
        a.sout[s] = arg(cpu, 7 + 2 * s), a.out[s] = arg(cpu, 8 + 2 * s);
    }
    run_skin(a);
}

// glitch::collada::detail::neonSkinPNTB(int, M, W, 4 x (sIn, in), 4 x (sOut, out))
extern "C" void pc_native_skin_pntb(Cpu *cpu) {
    SkinArgs a{};
    a.count = int(arg(cpu, 0)), a.m = arg(cpu, 1), a.w = arg(cpu, 2), a.streams = 4;
    for (int s = 0; s < 4; s++) {
        a.sin[s] = arg(cpu, 3 + 2 * s), a.in[s] = arg(cpu, 4 + 2 * s);
        a.sout[s] = arg(cpu, 11 + 2 * s), a.out[s] = arg(cpu, 12 + 2 * s);
    }
    run_skin(a);
}

// glitch::collada::detail::neonSkinPNTB_stride_optim(int, M, W, sIn, inP, inN, inT, inB, sOut, outP, outN, outT, outB)
extern "C" void pc_native_skin_pntb_so(Cpu *cpu) {
    SkinArgs a{};
    a.count = int(arg(cpu, 0)), a.m = arg(cpu, 1), a.w = arg(cpu, 2), a.streams = 4;
    uint32_t sin = arg(cpu, 3), sout = arg(cpu, 8);
    for (int s = 0; s < 4; s++) {
        a.sin[s] = sin, a.in[s] = arg(cpu, 4 + s);
        a.sout[s] = sout, a.out[s] = arg(cpu, 9 + s);
    }
    run_skin(a);
}

// CSoftwareSkinTechnique::skin(unsigned, SSkinBuffer &, CMeshBuffer *): r0 this, r1 index,
// r2 SSkinBuffer (+0x18: frame stamp; skin returns early when it equals the global stamp and
// sets it at the end), r3 mesh buffer.
// Check mode: run skin with the native kernels (logging every output vertex written), save
// those outputs, restore the stamp and registers, run skin again with the flag cleared (the
// recompiled C loops) and compare. The game continues with the C loops' results.
extern "C" void pc_native_skin(Cpu *cpu, uint32_t entry, GuestFn original) {
    uint8_t *flag = HOST(rt::sym(kSkinNeonFlag));
    if (disabled(kSkin)) {
        *flag = 0;
        original(cpu, entry);
        return;
    }
    if (!checking(kSkin)) {
        *flag = 1;
        original(cpu, entry);
        return;
    }
    const Cpu saved = *cpu;
    const uint32_t stamp_addr = cpu->r[2] + 0x18, stamp = LD32(stamp_addr);
    std::vector<uint32_t> written;
    RT_TLS(t_skin_log) = &written;
    *flag = 1;
    original(cpu, entry);
    RT_TLS(t_skin_log) = nullptr;
    if (written.empty()) return;  // early return or nothing skinned
    std::vector<uint8_t> native(written.size() * 12);
    for (size_t i = 0; i < written.size(); i++) std::memcpy(&native[i * 12], HOST(written[i]), 12);
    *cpu = saved;
    ST32(stamp_addr, stamp);
    *flag = 0;
    original(cpu, entry);
    *flag = 1;
    bool ok = true;
    for (size_t i = 0; i < written.size() && ok; i++)
        ok = compare_words(g_skin_stats, "skinned vertex (original vs native)", written[i], HOST(written[i]),
                           &native[i * 12], 12);
    g_skin_stats.done();
}

// =========================================================================================
// 2. spark particle animations: CPSAnim_{Color,Size,Dir}::Compute(float t, vecN &out, vecN &init)
// =========================================================================================
// this: +0 int count, +4 float *times, +8 (Color, Dir) / +0x10 (Size) vecN *values.
//   count <= 0: out = init
//   values[0] = init; if count != 1: values[1] = init
//   i = largest index in [count-1 .. 0] with times[i] <= t (ordered compare), else -1
//   i == count-1: out = values[count-1]
//   else: values[0] = init (again); t0 = times[i], t1 = times[i+1] (i = -1 reads times[-1] /
//         values[-1] like the original)
//         Color, Dir: q = (1/(t1-t0)) * (v1-v0);  Size: q = (v1-v0) / (t1-t0)
//         out = q; out = out * (t - t0); out = out + v0
// Color 0x9c0ac8 (vector4d), Size 0x9c39bc (vector2d), Dir 0x9c2880 (vector3d).
namespace nh {

template <int N, uint32_t kValues, bool kRecip>
void anim_compute(uint32_t self, float t, uint8_t *out, const uint8_t *init) {
    const int32_t count = int32_t(LD32(self));
    if (count <= 0) {
        for (int k = 0; k < N; k++) stw(out + 4 * k, ldw(init + 4 * k));
        return;
    }
    const uint32_t vals = LD32(self + kValues);
    for (int k = 0; k < N; k++) ST32(vals + 4 * k, ldw(init + 4 * k));
    if (count != 1)
        for (int k = 0; k < N; k++) ST32(vals + 4 * N + 4 * k, ldw(init + 4 * k));
    const uint32_t times = LD32(self + 4);
    int32_t i = count - 1;
    for (; i >= 0; i--)
        if (LDF(times + 4 * uint32_t(i)) <= t) break;
    if (i == count - 1) {
        const uint32_t v = vals + uint32_t(i) * 4 * N;
        for (int k = 0; k < N; k++) stw(out + 4 * k, LD32(v + 4 * k));
        return;
    }
    for (int k = 0; k < N; k++) ST32(vals + 4 * k, ldw(init + 4 * k));
    const uint32_t v0 = vals + uint32_t(i) * 4 * N, v1 = v0 + 4 * N;
    const uint32_t t0a = times + 4 * uint32_t(i);
    const float dt = LDF(t0a + 4) - LDF(t0a);
    float q[N];
    if (kRecip) {
        const float inv = 1.0f / dt;
        for (int k = 0; k < N; k++) q[k] = inv * (LDF(v1 + 4 * k) - LDF(v0 + 4 * k));
    } else {
        for (int k = 0; k < N; k++) q[k] = (LDF(v1 + 4 * k) - LDF(v0 + 4 * k)) / dt;
    }
    for (int k = 0; k < N; k++) stf(out + 4 * k, q[k]);
    const float s = t - LDF(t0a);
    float r[N];
    for (int k = 0; k < N; k++) r[k] = (N == 4 ? ldf(out + 4 * k) : q[k]) * s;  // Color re-reads out
    for (int k = 0; k < N; k++) stf(out + 4 * k, r[k]);
    for (int k = 0; k < N; k++) stf(out + 4 * k, r[k] + LDF(v0 + 4 * k));
}

constexpr auto color_compute = anim_compute<4, 8, true>;
constexpr auto size_compute = anim_compute<2, 0x10, false>;
constexpr auto dir_compute = anim_compute<3, 8, true>;

CheckStats g_anim_stats("anim");

template <int N, uint32_t kValues, bool kRecip>
void anim_wrap(Cpu *cpu, uint32_t entry, GuestFn original) {
    if (disabled(kAnim)) {
        original(cpu, entry);
        return;
    }
    const uint32_t self = cpu->r[0], out = cpu->r[2], init = cpu->r[3];
    const float t = bitsf(cpu->r[1]);
    if (!checking(kAnim)) {
        anim_compute<N, kValues, kRecip>(self, t, HOST(out), HOST(init));
        cpu->ret_pc = cpu->r[14];
        return;
    }
    // inputs it writes: out (N words), values[0..1] (2N words)
    const uint32_t vals = LD32(self + kValues);
    uint8_t out0[16], vals0[32], out1[16], vals1[32];
    std::memcpy(out0, HOST(out), 4 * N);
    std::memcpy(vals0, HOST(vals), 8 * N);
    const Cpu saved = *cpu;
    original(cpu, entry);
    std::memcpy(out1, HOST(out), 4 * N);
    std::memcpy(vals1, HOST(vals), 8 * N);
    std::memcpy(HOST(out), out0, 4 * N);
    std::memcpy(HOST(vals), vals0, 8 * N);
    const Cpu after = *cpu;
    *cpu = saved;
    anim_compute<N, kValues, kRecip>(self, t, HOST(out), HOST(init));
    if (compare_words(g_anim_stats, "out", out, out1, HOST(out), 4 * N))
        compare_words(g_anim_stats, "values", vals, vals1, HOST(vals), 8 * N);
    std::memcpy(HOST(out), out1, 4 * N);  // continue with the original's results
    std::memcpy(HOST(vals), vals1, 8 * N);
    *cpu = after;
    g_anim_stats.done();
}

}  // namespace nh

extern "C" void pc_native_anim_color(Cpu *cpu, uint32_t entry, GuestFn original) { anim_wrap<4, 8, true>(cpu, entry, original); }
extern "C" void pc_native_anim_size(Cpu *cpu, uint32_t entry, GuestFn original) { anim_wrap<2, 0x10, false>(cpu, entry, original); }
extern "C" void pc_native_anim_dir(Cpu *cpu, uint32_t entry, GuestFn original) { anim_wrap<3, 8, true>(cpu, entry, original); }

// =========================================================================================
// 3. spark::CParticle::Update(float dt) (0x9ba9fc)
// =========================================================================================
// CParticle: +0x58 vec3 pos, +0x64 vec3 previous pos, +0x70 vec3 velocity, +0x7c u8[4]
// color, +0x80 u8[4] initial color, +0x84 vec3 initial dir, +0xa4/+0xa8 size,
// +0xac/+0xb0 initial size, +0xb8 u8 alive, +0xc0 CEmitterInstance *, +0xc4 float age,
// +0xc8 float lifetime, +0xcc object notified on update when +0xd0 (type) == 6 (vtable
// +0xa0, (obj, &pos)), +0xd4 float (CSInfluence output).
// CEmitterInstance: +0x104 u8 cull-behind-camera, +0x350 vec3 bbox min, +0x35c vec3 bbox max,
// +0x428 CPSAnim_Color*, +0x42c CPSAnim_Size*, +0x430 CPSAnim_Dir*, +0x434
// CPSAnim_CSInfluence*, +0x456 u8, +0x43c intrusive list of influences (node: +0 next,
// +8 object; object vtable +0x1c: (obj, float dt, float ratio, CParticle*)).
//   age += dt (stored); age < 0: alive = 0, return
//   ratio = age / lifetime; prev = pos
//   color anim: out = 0, init = float(initial color bytes); color[k] = (u8)cvt_u32_rz(out[k])
//   size anim:  out = 0, init = initial size; size = out
//   dir anim:   out = 0, init = +0x84; velocity = out
//   CSInfluence: em+0x456 ? +0xd4 = 0 : Compute(anim, ratio, this+0xd4, 0, 0)  (guest)
//   each influence: vcall(obj, dt, ratio, this)                                  (guest)
//   age > lifetime, or (em+0x104 && IsBehindCamera(em, &pos)): alive = 0, then if type == 6
//     notify; return
//   pos += (dt * 1000) * velocity (each component: pos + f*v); grow the emitter bbox
//   (max: >, min: <, ordered compares); type == 6: notify
namespace nh {

constexpr uint32_t kCSInfluenceCompute = 0x9c18b8;
constexpr uint32_t kIsBehindCamera = 0xa01d40;

void particle_update(Cpu *cpu, uint32_t self, uint32_t dt_bits) {
    GuestFrame fr(cpu, 0x40);
    const float dt = bitsf(dt_bits);
    const float age = dt + LDF(self + 0xc4);
    STF(self + 0xc4, age);
    if (age < 0) {
        ST8(self + 0xb8, 0);
        return;
    }
    uint32_t em = LD32(self + 0xc0);
    const float ratio = age / LDF(self + 0xc8);
    const uint32_t ratio_bits = fbits(ratio);
    {
        uint32_t a = LD32(self + 0x58), b = LD32(self + 0x5c), c = LD32(self + 0x60);
        ST32(self + 0x64, a), ST32(self + 0x68, b), ST32(self + 0x6c, c);
    }
    if (uint32_t anim = LD32(em + 0x428)) {
        uint8_t out[16] = {}, init[16];
        for (int k = 0; k < 4; k++) stf(init + 4 * k, float(uint32_t(LD8(self + 0x80 + k))));
        color_compute(anim, ratio, out, init);
        em = LD32(self + 0xc0);
        for (int k = 0; k < 4; k++) ST8(self + 0x7c + k, uint8_t(CVT_U32_RZ(double(ldf(out + 4 * k)))));
    }
    if (uint32_t anim = LD32(em + 0x42c)) {
        uint8_t out[8] = {}, init[8];
        stw(init, LD32(self + 0xac));
        stw(init + 4, LD32(self + 0xb0));
        size_compute(anim, ratio, out, init);
        em = LD32(self + 0xc0);
        ST32(self + 0xa4, ldw(out));
        ST32(self + 0xa8, ldw(out + 4));
    }
    if (uint32_t anim = LD32(em + 0x430)) {
        uint8_t out[12] = {};
        dir_compute(anim, ratio, out, HOST(self + 0x84));
        em = LD32(self + 0xc0);
        ST32(self + 0x70, ldw(out)), ST32(self + 0x74, ldw(out + 4)), ST32(self + 0x78, ldw(out + 8));
    }
    if (uint32_t anim = LD32(em + 0x434)) {
        if (LD8(em + 0x456)) {
            ST32(self + 0xd4, 0);
        } else {
            ST32(fr.sp(), 0);  // 5th argument (stack)
            fr.call(rt::sym(kCSInfluenceCompute), anim, ratio_bits, self + 0xd4, 0);
            em = LD32(self + 0xc0);
        }
    }
    for (uint32_t node = LD32(em + 0x43c); node != em + 0x43c; node = LD32(node)) {
        uint32_t obj = LD32(node + 8);
        fr.call(LD32(LD32(obj) + 0x1c), obj, dt_bits, ratio_bits, self);
        em = LD32(self + 0xc0);
    }
    bool dead = LDF(self + 0xc4) > LDF(self + 0xc8);
    if (!dead && LD8(em + 0x104)) {
        dead = fr.call(rt::sym(kIsBehindCamera), em, self + 0x58) != 0;
        if (!dead) em = LD32(self + 0xc0);
    }
    uint32_t type;
    if (dead) {
        type = LD32(self + 0xd0);
        ST8(self + 0xb8, 0);
    } else {
        const float f = dt * 1000.0f;
        const float px = LDF(self + 0x58) + f * LDF(self + 0x70);
        const float py = LDF(self + 0x5c) + f * LDF(self + 0x74);
        const float pz = LDF(self + 0x60) + f * LDF(self + 0x78);
        type = LD32(self + 0xd0);
        STF(self + 0x58, px), STF(self + 0x5c, py), STF(self + 0x60, pz);
        if (px > LDF(em + 0x35c)) STF(em + 0x35c, px);
        if (py > LDF(em + 0x360)) STF(em + 0x360, py);
        if (pz > LDF(em + 0x364)) STF(em + 0x364, pz);
        if (px < LDF(em + 0x350)) STF(em + 0x350, px);
        if (py < LDF(em + 0x354)) STF(em + 0x354, py);
        if (pz < LDF(em + 0x358)) STF(em + 0x358, pz);
    }
    if (type == 6)
        if (uint32_t obj = LD32(self + 0xcc)) fr.call(LD32(LD32(obj) + 0xa0), obj, self + 0x58);
}

CheckStats g_particle_stats("particle");

}  // namespace nh

// Check mode runs the original first, then the native code from the same particle / emitter
// bbox state. Influence callbacks and the type-6 notification run twice (side effects
// outside the particle, e.g. random number draws, are not undone).
extern "C" void pc_native_particle_update(Cpu *cpu, uint32_t entry, GuestFn original) {
    if (disabled(kParticle)) {
        original(cpu, entry);
        return;
    }
    const uint32_t self = cpu->r[0], dt = cpu->r[1];
    if (!checking(kParticle)) {
        particle_update(cpu, self, dt);
        cpu->ret_pc = cpu->r[14];
        return;
    }
    const uint32_t em = LD32(self + 0xc0);
    uint8_t p0[0xd8], b0[0x18], p1[0xd8], b1[0x18];
    std::memcpy(p0, HOST(self), sizeof p0);
    std::memcpy(b0, HOST(em + 0x350), sizeof b0);
    const Cpu saved = *cpu;
    original(cpu, entry);
    const Cpu after = *cpu;
    std::memcpy(p1, HOST(self), sizeof p1);
    std::memcpy(b1, HOST(em + 0x350), sizeof b1);
    std::memcpy(HOST(self), p0, sizeof p0);
    std::memcpy(HOST(em + 0x350), b0, sizeof b0);
    *cpu = saved;
    particle_update(cpu, self, dt);
    if (compare_words(g_particle_stats, "particle", self, p1, HOST(self), sizeof p1))
        compare_words(g_particle_stats, "emitter bbox", em + 0x350, b1, HOST(em + 0x350), sizeof b1);
    std::memcpy(HOST(self), p1, sizeof p1);  // continue with the original's results
    std::memcpy(HOST(em + 0x350), b1, sizeof b1);
    *cpu = after;
    g_particle_stats.done();
}

// =========================================================================================
// 4. CTTFFont::CTTFGlyphWrapper::IsCached(int) const (0xf82a0c), operator[](int) const (0xf82918)
// =========================================================================================
// CTTFGlyphWrapper +4 -> std::map<int, CTTFGlyph> (_Rb_tree: header at +4, root at +8;
// node: +8 left, +0xc right, +0x10 key, +0x14 value). lower_bound with signed keys.
// operator[] returns &value, or the address of a function-local static empty CTTFGlyph
// (link 0x18da930, guard 0x18da920); the first call (guard not set) runs the original,
// which constructs it.
namespace nh {
constexpr uint32_t kGlyphStatic = 0x18da930, kGlyphGuard = 0x18da920;

inline uint32_t glyph_lower_bound(uint32_t self, int32_t key, uint32_t &end) {
    const uint32_t tree = LD32(self + 4);
    end = tree + 4;
    uint32_t y = end;
    for (uint32_t x = LD32(tree + 8); x;) {
        if (int32_t(LD32(x + 0x10)) >= key) y = x, x = LD32(x + 8);
        else x = LD32(x + 0xc);
    }
    return y;
}
uint32_t glyph_is_cached(uint32_t self, int32_t key) {
    uint32_t end, y = glyph_lower_bound(self, key, end);
    return y != end && int32_t(LD32(y + 0x10)) <= key;
}
uint32_t glyph_lookup(uint32_t self, int32_t key) {
    uint32_t end, y = glyph_lower_bound(self, key, end);
    if (y == end || int32_t(LD32(y + 0x10)) > key) return rt::sym(kGlyphStatic);
    return y + 0x14;
}
CheckStats g_ttf_stats("ttf");
}  // namespace nh

extern "C" void pc_native_glyph_is_cached(Cpu *cpu, uint32_t entry, GuestFn original) {
    if (disabled(kTtf)) {
        original(cpu, entry);
        return;
    }
    const uint32_t r = glyph_is_cached(cpu->r[0], int32_t(cpu->r[1]));
    if (checking(kTtf)) {
        original(cpu, entry);
        if (cpu->r[0] != r && g_ttf_stats.mismatch())
            rt::log("native check ttf: IsCached(%d) original %u native %u", int32_t(cpu->r[1]), cpu->r[0], r);
        g_ttf_stats.done();
        return;
    }
    cpu->r[0] = r;
    cpu->ret_pc = cpu->r[14];
}

extern "C" void pc_native_glyph_lookup(Cpu *cpu, uint32_t entry, GuestFn original) {
    if (disabled(kTtf) || !(LD32(rt::sym(kGlyphGuard)) & 1)) {
        original(cpu, entry);
        return;
    }
    const uint32_t key = cpu->r[1];
    const uint32_t r = glyph_lookup(cpu->r[0], int32_t(key));
    if (checking(kTtf)) {
        original(cpu, entry);
        if (cpu->r[0] != r && g_ttf_stats.mismatch())
            rt::log("native check ttf: operator[](%d) original %08x native %08x", int32_t(key), cpu->r[0], r);
        g_ttf_stats.done();
        return;
    }
    cpu->r[0] = r;
    cpu->ret_pc = cpu->r[14];
}
