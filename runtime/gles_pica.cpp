// GLES2 on the New 3DS's PICA200, through citro3d (docs/3ds.md, Graphics). runtime/gles.cpp does
// what it does on every platform (guest pointers, skipping repeated state, the uniform cache, the
// program pool) and calls the functions here as its driver: install_pica_gl() hands them to
// gl_load(). The PICA has programmable vertex shaders but a fixed-function fragment stage (six
// texture combiners, three texture units, fog, alpha test), so every GLSL program the game links
// is recognised from its sources and #defines and drawn with a hand-written vertex shader
// (runtime/pica/*.v.pica) and a combiner setup: a "material" (pica_materials.inc). Programs
// without one draw nothing (logged once).
//
// Coordinates: the GL clip space is turned into the PICA's on the CPU, in the position matrix
// the vertex shader gets: depth from [-w, w] to [0, -w] (the depth buffer then holds GL's 0..1),
// and on the screen a quarter turn (its framebuffer is 240x400 or 240x320, portrait). Texture data is
// stored in the PICA's tiled order with GL's first row first; texture coordinates are flipped and
// scaled in the vertex shader instead of the data (ETC1 blocks could not be flipped losslessly),
// which also covers textures padded to a power of two.
//
// Memory: vertex and index buffers, textures and the per-frame ring of client-memory vertex data
// are linear memory. Anything the GPU may still read (the frame in flight) is freed only after
// the next frame's start has waited for the GPU; a buffer or texture changed after a draw in the
// same frame gets new storage (copy on write).
//
// Caches: the GPU reads memory, not the ARM11's data caches (L1, and the New 3DS's L2), so what
// the CPU writes for it is flushed by whoever writes it: buffers and textures at their upload, the
// ring before each submission (ring_flush), each command list by GSP (GX_CMDLIST_FLUSH). citro3d's
// default (flags 0) instead flushes the whole linear heap at every frame's end, 80 MB here. Azahar
// keeps no caches: a missing flush shows only on a console.
#include <3ds.h>
#include <citro3d.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "gl_program.h"
#include "gles_pica.h"

namespace rt {
bool native_emulator();  // runtime/native_3ds.cpp
void log(const char *fmt, ...);
void log_flush();
[[noreturn]] void fatal(const char *fmt, ...);
using GlLoader = void *(*)(const char *name);
bool gl_load(GlLoader loader);
}  // namespace rt

extern "C" {  // runtime/pica/*.v.pica assembled into one DVLB (picasso), then bin2s
extern const u8 pica_shbin[];
extern const u8 pica_shbin_end[];
extern u32 __ctru_linear_heap, __ctru_linear_heap_size;  // (runtime/os_3ds.cpp lays them out)
s32 rt_native_flush_caches_kernel();  // runtime/native_3ds_gates.S (svcBackdoor): this core's L1 cleaned
}

namespace pica {
namespace {

using namespace glprog;  // programs, uniforms, materials (runtime/gl_program.cpp)
using GLenum = uint32_t;
using GLuint = uint32_t;
using GLint = int;  // (int32_t is long on the ARM newlib: std::min / max mismatches)
using GLsizei = int;
using GLfloat = float;
using GLboolean = uint8_t;
using GLbitfield = uint32_t;
using GLintptr = intptr_t;
using GLsizeiptr = intptr_t;

constexpr int kScreenW = 400, kScreenH = 240;  // the top screen; the bottom one is 320x240
constexpr uint32_t kRingSize = 4u << 20;  // client-memory vertex data per frame
// The screen target's transfer to the screen's framebuffer (RGB8, the format of both screens
// while the game draws on them)
constexpr u32 kScreenTransfer = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
                                GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
constexpr int kMaxTex = 1024;

template <class... A> void log_once(const std::string &key, const char *fmt, A... a) {
    static std::set<std::string> seen;
    if (seen.insert(key).second) rt::log(fmt, a...);
}

// ---- statistics ---------------------------------------------------------------------------------
Stats g_stats;

// Where a draw's time goes (the replay's build, PICA_PROFILE): Stats::phase_ticks[n - 1] gets the
// time from the previous mark to mark n: 0 (after the frame start), 1 (target), 5 .. 9 (within
// setup), 2 (vertex data), 3 (textures, fragment state), 4 (issued)
#ifdef PICA_PROFILE
uint64_t g_mark;
#define PROFILE_MARK(n)                    \
    do {                                    \
        const uint64_t t_ = svcGetSystemTick(); \
        if (n) g_stats.phase_ticks[(n) - 1] += t_ - g_mark; \
        g_mark = t_;                            \
    } while (0)
#else
#define PROFILE_MARK(n)
#endif

// ---- linear memory: deferred frees and the client-data ring ------------------------------------
uint32_t g_frame_id = 1;  // bumped when a frame starts (after the GPU finished the previous one)
bool g_gpu_pending = false;  // a frame was submitted and not yet waited for (read_screen)
bool g_in_frame = false;
std::vector<void *> g_free_later;  // released at the next frame's start
bool in_vram(const void *p) { return uintptr_t(p) >= 0x1F000000 && uintptr_t(p) < 0x1F600000; }
void gpu_free(void *p) {
    if (in_vram(p)) vramFree(p);
    else linearFree(p);
}
void free_later(void *p) {
    if (p) g_free_later.push_back(p);
}
struct Ring {
    uint8_t *base = nullptr;
    uint32_t used = 0, flushed = 0;
    std::vector<std::pair<void *, uint32_t>> extra;  // blocks of a full ring, not flushed yet
} g_ring;
// Linear memory for this frame's GPU reads.
uint8_t *ring_alloc(uint32_t size, uint32_t align = 16) {
    uint32_t at = (g_ring.used + align - 1) & ~(align - 1);
    if (at + size <= kRingSize) {
        g_ring.used = at + size;
        return g_ring.base + at;
    }
    // full: a block of its own, flushed with the ring and freed after the frame
    log_once("ring", "pica: client vertex ring full (%u KB), using extra blocks", kRingSize / 1024);
    auto *p = static_cast<uint8_t *>(linearAlloc(size));
    free_later(p);
    if (p) g_ring.extra.push_back({p, size});
    g_stats.ring_overflow += size;
    return p;
}
// CPU writes for the GPU out of the CPU's caches; a refused flush is logged (once per kind).
void gpu_flush(const void *p, u32 n, const char *what) {
    if (const Result rc = GSPGPU_FlushDataCache(p, n); R_FAILED(rc))
        log_once(std::string("flush") + what, "pica: the flush of %s at %p (%lu bytes) failed: %08lx", what, p, (unsigned long)n,
                 (unsigned long)rc);
}
// What the CPU wrote into the ring since the last submission, out of its caches.
void ring_flush() {
    if (g_ring.used > g_ring.flushed) gpu_flush(g_ring.base + g_ring.flushed, g_ring.used - g_ring.flushed, "the vertex ring");
    g_ring.flushed = g_ring.used;
    for (const auto &[p, n] : g_ring.extra) gpu_flush(p, n, "a vertex ring block");
    g_ring.extra.clear();
}
// The commands so far to citro3d's queue (it runs at the frame's end), their data flushed.
void submit_split() {
    ring_flush();
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
}

// ---- objects --------------------------------------------------------------------------------------
struct Texture {
    C3D_Tex tex{};
    bool allocated = false;
    GPU_TEXCOLOR fmt = GPU_RGBA8;
    GLenum gl_format = 0;      // the GL format of level 0 (0x8D64 ETC1)
    int w = 0, h = 0;          // GL size of level 0
    int pw = 0, ph = 0;        // PICA size: powers of two, 8..1024
    int skip = 0;              // GL levels dropped (too large for the PICA)
    int levels = 1;            // PICA levels allocated
    uint32_t have = 0;         // PICA levels uploaded (bits)
    GLenum min_filter = 0x2702, mag_filter = 0x2601, wrap_s = 0x2901, wrap_t = 0x2901;
    bool render_target = false;
    bool vram = false;         // render targets live in VRAM
    int rt_shift = 0;          // render targets: the PICA image is the GL one halved this many times
    uint32_t used_frame = 0;
    uint32_t bytes = 0;
    float su = 1, sv = 1;      // the GL image's share of the PICA texture (padding)
    C3D_RenderTarget *target = nullptr;  // when it is rendered to
};

// A range of a buffer object's vertices whose float positions are stored relative to `c`
// (position_origin): field = the position's offset in a vertex, lo..hi the vertices.
struct Origin {
    uint32_t field, stride, lo, hi;
    float c[3];
};
struct Buffer {
    uint8_t *data = nullptr;
    uint32_t size = 0;
    uint32_t used_frame = 0;
    std::vector<Origin> origins;
};

struct Renderbuffer {
    GLenum format = 0;
    int w = 0, h = 0;
};
struct Framebuffer {
    GLuint color_tex = 0, color_rb = 0, depth_rb = 0, stencil_rb = 0;
    C3D_RenderTarget *target = nullptr;  // built at the first draw
    GLuint target_tex = 0;
};

std::unordered_map<GLuint, std::unique_ptr<Texture>> g_textures;
std::unordered_map<GLuint, Buffer> g_buffers;
std::unordered_map<GLuint, Renderbuffer> g_renderbuffers;
std::unordered_map<GLuint, Framebuffer> g_framebuffers;
GLuint g_next_name = 1;
uint64_t g_tex_bytes, g_buf_bytes, g_vram_bytes;

// ---- GL state -----------------------------------------------------------------------------------
struct AttribState {
    bool enabled = false;
    GLint size = 4;
    GLenum type = 0x1406;
    bool norm = false;
    GLsizei stride = 0;
    uintptr_t pointer = 0;  // client pointer, or offset into `buffer`
    GLuint buffer = 0;
    struct Buffer *buf = nullptr;  // `buffer`'s object (cleared when it is deleted)
    float value[4] = {0, 0, 0, 1};
};
struct State {
    GLuint array_buffer = 0, element_buffer = 0, framebuffer = 0, renderbuffer = 0;
    int unit = 0;
    GLuint tex[8] = {};
    AttribState attr[16];
    bool blend = false, depth_test = false, cull = false, scissor_test = false, stencil_test = false,
         polygon_offset = false, dither = true;
    GLenum blend_src_rgb = 1, blend_dst_rgb = 0, blend_src_a = 1, blend_dst_a = 0;
    GLenum blend_eq_rgb = 0x8006, blend_eq_a = 0x8006;
    float blend_color[4] = {};
    GLenum depth_func = 0x0201;  // LESS
    bool depth_mask = true;
    uint8_t color_mask = 0xF;
    GLenum cull_face = 0x0405, front_face = 0x0901;  // BACK, CCW
    int viewport[4] = {0, 0, kScreenW, kScreenH};
    int scissor[4] = {0, 0, kScreenW, kScreenH};
    float clear_color[4] = {};
    float clear_depth = 1;
    int clear_stencil = 0;
    GLenum stencil_func = 0x0207;  // ALWAYS
    int stencil_ref = 0;
    uint32_t stencil_mask = 0xff, stencil_write = 0xff;
    GLenum stencil_fail = 0x1E00, stencil_zfail = 0x1E00, stencil_zpass = 0x1E00;  // KEEP
    float polygon_factor = 0, polygon_units = 0;
    float depth_near = 0, depth_far = 1;
    int unpack = 4, pack = 4;
    GLenum error = 0;
} g;

Texture *g_texp[8];  // the textures of g.tex (no lookups per draw)
// The bound framebuffer's colour texture (and whether it has depth), looked up when g.framebuffer
// or an attachment changes
GLuint g_fb_name;
Texture *g_fb_target;
bool g_fb_depth;

void set_error(GLenum e) {
    if (!g.error) g.error = e;
}

float g_depth_far = 4096, g_depth_far_next = 0;  // the depth scale (set_fog), fixed within a frame
uint32_t g_tex_version = 1;  // bumped whenever a texture's C3D state changes (params, storage)
// A texture's storage written by the CPU: out of the CPU's caches, and every unit bound again at
// the next draw, which clears the GPU's texture cache (citro3d clears it only when a binding
// changes; the console's PICA keeps texels of an address cached, Azahar has no texture cache).
void tex_written(C3D_Tex &t) {
    if (!in_vram(t.data)) gpu_flush(t.data, C3D_TexCalcTotalSize(t.size, t.maxLevel), "a texture");
    g_tex_version++;
}
void forget_bindings();

// ---- render targets -----------------------------------------------------------------------------
C3D_RenderTarget *g_screen;        // the screen (240x400 or 240x320, rotated): see set_screen
int g_screen_w = kScreenW;         // 400 on the top screen, 320 on the bottom one
bool g_hide_frame = false;         // the frame being drawn is not shown (set_screen)
C3D_Tex g_blank;                   // bound to units without a texture: 8x8 white
C3D_Tex g_ramp;                    // 8x8: red = s, green = t across the texel centres (height_fog)
C3D_RenderTarget *g_drawing_on;    // the target of this frame's current draws
bool g_target_is_screen = true;
int g_target_w = kScreenW, g_target_h = kScreenH, g_target_shift = 0;
uint32_t g_draws_in_list = 0;      // draws since the last submission (a clear in between splits)

void begin_frame() {
    if (g_in_frame) return;
    const uint64_t t0 = svcGetSystemTick();
    // waits for the previous frame's GPU work (and its transfer to the screen). Not
    // C3D_FRAME_SYNCDRAW: that also waits for the next VBlank, inside the game's first GL call
    // of the frame, on top of the host's own frame cap (8-9 ms a frame at the title menu)
    C3D_FrameBegin(0);
    g_gpu_pending = false;
    g_stats.wait_ms += double(svcGetSystemTick() - t0) / CPU_TICKS_PER_MSEC;
    g_in_frame = true;
    g_frame_id++;
    for (void *p : g_free_later) gpu_free(p);
    g_free_later.clear();
    g_ring.used = g_ring.flushed = 0;
    g_drawing_on = nullptr;
    g_draws_in_list = 0;
    if (g_depth_far_next > 0) g_depth_far = g_depth_far_next;  // the farthest plane seen last frame
    g_depth_far_next = 0;
}

// ---- textures: formats and the PICA's tiled layout ----------------------------------------------
// Offset of texel (x, y) in a tiled image `w` texels wide: 8x8 tiles in rows, Morton order inside.
inline uint32_t tiled(uint32_t x, uint32_t y, uint32_t w) {
    static const uint8_t m[8] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
    return ((y >> 3) * (w >> 3) + (x >> 3)) * 64 + (m[x & 7] | m[y & 7] << 1);
}
int bpp(GPU_TEXCOLOR f) {
    switch (f) {
    case GPU_RGBA8: return 32;
    case GPU_RGB8: return 24;
    case GPU_RGBA5551: case GPU_RGB565: case GPU_RGBA4: case GPU_LA8: case GPU_HILO8: return 16;
    case GPU_L8: case GPU_A8: case GPU_LA4: case GPU_ETC1A4: return 8;
    default: return 4;  // L4 A4 ETC1
    }
}
int next_pow2(int v) {
    int p = 8;
    while (p < v) p <<= 1;
    return p;
}
// The PICA format for a GL upload.
GPU_TEXCOLOR pica_format(GLenum format, GLenum type) {
    if (format == 0x8D64) return GPU_ETC1;
    if (type == 0x8363) return GPU_RGB565;
    if (type == 0x8033) return GPU_RGBA4;
    if (type == 0x8034) return GPU_RGBA5551;
    switch (format) {
    case 0x1907: return GPU_RGB565;    // RGB: 16 bits are plenty for the game's pictures (ETC1 elsewhere)
    case 0x1909: return GPU_L8;
    case 0x1906: return GPU_A8;
    case 0x190A: return GPU_LA8;
    default: return GPU_RGBA8;         // RGBA, BGRA
    }
}
void tex_free(Texture &t) {
    if (t.target) {
        C3D_RenderTargetDelete(t.target);
        t.target = nullptr;
    }
    if (t.allocated) {
        if (t.used_frame == g_frame_id) free_later(t.tex.data);  // the GPU may still read it
        else gpu_free(t.tex.data);
        (t.vram ? g_vram_bytes : g_tex_bytes) -= t.bytes;
        t.allocated = false;
        t.tex.data = nullptr;
    }
}
// Applies the GL sampler state to the C3D texture (its param word, mip range).
void tex_params(Texture &t) {
    if (!t.allocated) return;
    g_tex_version++;
    auto filter = [](GLenum f) { return f == 0x2600 || f == 0x2700 || f == 0x2702 ? GPU_NEAREST : GPU_LINEAR; };
    auto wrap = [](GLenum w) { return w == 0x812F ? GPU_CLAMP_TO_EDGE : w == 0x8370 ? GPU_MIRRORED_REPEAT : GPU_REPEAT; };
    C3D_TexSetFilter(&t.tex, filter(t.mag_filter), filter(t.min_filter));
    const bool mip = t.min_filter >= 0x2700 && t.min_filter <= 0x2703;
    C3D_TexSetFilterMipmap(&t.tex, t.min_filter == 0x2702 || t.min_filter == 0x2703 ? GPU_LINEAR : GPU_NEAREST);
    // padded (non power of two) images clamp: GL allows nothing else for them
    const bool padded = t.pw != t.w || t.ph != t.h;
    C3D_TexSetWrap(&t.tex, padded ? GPU_CLAMP_TO_EDGE : wrap(t.wrap_s), padded ? GPU_CLAMP_TO_EDGE : wrap(t.wrap_t));
    int top = 0;
    while (top + 1 < t.levels && (t.have >> (top + 1) & 1)) top++;
    t.tex.minLevel = 0;
    t.tex.maxLevel = uint8_t(mip ? top : 0);
}
// Storage for a texture's PICA levels 0..levels-1 (keeps the uploaded ones when it grows).
bool tex_alloc(Texture &t, GPU_TEXCOLOR fmt, int pw, int ph, int levels, bool vram = false) {
    C3D_Tex old = t.tex;
    const bool had = t.allocated && t.fmt == fmt && t.pw == pw && t.ph == ph;
    const int old_levels = t.levels;
    const uint32_t old_have = t.have;
    C3D_Tex fresh{};
    if (!C3D_TexInitWithParams(&fresh, nullptr, C3D_TexInitParams{uint16_t(pw), uint16_t(ph), uint8_t(levels - 1), fmt, GPU_TEX_2D, vram})) {
        if (vram) return tex_alloc(t, fmt, pw, ph, levels, false);  // VRAM full: linear memory
        log_once("texalloc", "pica: out of linear memory for a %dx%d texture (%.1f MB of textures)", pw, ph, g_tex_bytes / 1048576.0);
        return false;
    }
    if (had) {
        for (int l = 0; l < std::min(old_levels, levels); l++)
            if (old_have >> l & 1) {
                u32 n;
                void *src = C3D_TexGetImagePtr(&old, old.data, l, &n);
                std::memcpy(C3D_Tex2DGetImagePtr(&fresh, l, nullptr), src, n);
            }
    }
    tex_free(t);
    g_tex_version++;
    t.tex = fresh;
    t.allocated = true;
    t.fmt = fmt, t.pw = pw, t.ph = ph, t.levels = levels;
    t.have = had ? old_have & ((1u << levels) - 1) : 0;
    t.bytes = C3D_TexCalcTotalSize(t.tex.size, levels - 1);
    t.vram = in_vram(t.tex.data);
    if (!t.vram) g_tex_bytes += t.bytes;
    else g_vram_bytes += t.bytes;
    g_stats.tex_peak = std::max<uint64_t>(g_stats.tex_peak, g_tex_bytes);
    return true;
}
// The texture is about to be written: new storage if the GPU may still read the old.
void tex_cow(Texture &t) {
    if (!t.allocated || t.used_frame != g_frame_id || t.vram) return;
    C3D_Tex fresh{};
    if (!C3D_TexInitWithParams(&fresh, nullptr, C3D_TexInitParams{uint16_t(t.pw), uint16_t(t.ph), uint8_t(t.levels - 1), t.fmt, GPU_TEX_2D, false}))
        return;
    std::memcpy(fresh.data, t.tex.data, t.bytes);
    fresh.param = t.tex.param, fresh.lodParam = t.tex.lodParam;
    free_later(t.tex.data);
    t.tex = fresh;
    t.used_frame = 0;
    g_stats.tex_cow++;
    g_tex_version++;
}
// Writes one GL level (rows bottom-up in GL terms: row 0 first) into PICA level `pl`.
void tex_store(Texture &t, int pl, int x0, int y0, int w, int h, GLenum format, GLenum type, const uint8_t *src, int unpack) {
    const int lw = std::max(8, t.pw >> pl), lh = std::max(8, t.ph >> pl);
    uint8_t *dst = static_cast<uint8_t *>(C3D_Tex2DGetImagePtr(&t.tex, pl, nullptr));
    if (t.fmt == GPU_ETC1) {
        // 4x4 blocks (8 bytes, big-endian 64-bit words) -> the PICA's: four blocks per 8x8 tile in
        // Z order, each block's word little-endian.
        const int bw = (w + 3) / 4, bh = (h + 3) / 4;
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                const int px = x0 / 4 + bx, py = y0 / 4 + by;
                if (px * 4 >= lw || py * 4 >= lh) continue;
                const uint32_t tile = uint32_t((py >> 1) * (lw >> 3) + (px >> 1));
                uint8_t *o = dst + tile * 32 + uint32_t(((py & 1) << 1 | (px & 1)) * 8);
                const uint8_t *s = src + (size_t(by) * size_t(bw) + size_t(bx)) * 8;
                uint32_t a, b;  // (the byte order reversed: two `rev`s)
                std::memcpy(&a, s + 4, 4);
                std::memcpy(&b, s, 4);
                a = __builtin_bswap32(a), b = __builtin_bswap32(b);
                std::memcpy(o, &a, 4);
                std::memcpy(o + 4, &b, 4);
            }
        return;
    }
    int in_bpp = 4;
    if (type == 0x8363 || type == 0x8033 || type == 0x8034) in_bpp = 2;
    else if (format == 0x1907) in_bpp = 3;
    else if (format == 0x190A) in_bpp = 2;
    else if (format == 0x1906 || format == 0x1909) in_bpp = 1;
    const size_t row = (size_t(w) * size_t(in_bpp) + size_t(unpack) - 1) / size_t(unpack) * size_t(unpack);
    const bool bgra = format == 0x80E1;
    for (int y = 0; y < h; y++) {
        const int py = y0 + y;
        if (py >= lh) break;
        const uint8_t *s = src + size_t(y) * row;
        for (int x = 0; x < w; x++, s += in_bpp) {
            const int px = x0 + x;
            if (px >= lw) break;
            const uint32_t i = tiled(uint32_t(px), uint32_t(py), uint32_t(lw));
            switch (t.fmt) {
            case GPU_RGBA8: {  // memory: A B G R
                uint8_t *o = dst + i * 4;
                if (in_bpp == 4) o[0] = s[3], o[1] = bgra ? s[0] : s[2], o[2] = s[1], o[3] = bgra ? s[2] : s[0];
                else o[0] = 255, o[1] = s[2], o[2] = s[1], o[3] = s[0];
                break;
            }
            case GPU_RGB8: {   // memory: B G R
                uint8_t *o = dst + i * 3;
                o[0] = s[2], o[1] = s[1], o[2] = s[0];
                break;
            }
            case GPU_RGB565:
            case GPU_RGBA4:
            case GPU_RGBA5551: {
                uint16_t v;
                if (in_bpp == 2) std::memcpy(&v, s, 2);  // GL's packed shorts have the PICA's bit order
                else if (t.fmt == GPU_RGB565) v = uint16_t((s[0] >> 3) << 11 | (s[1] >> 2) << 5 | s[2] >> 3);
                else if (t.fmt == GPU_RGBA4) v = uint16_t((s[0] >> 4) << 12 | (s[1] >> 4) << 8 | (s[2] >> 4) << 4 | (in_bpp == 4 ? s[3] >> 4 : 15));
                else v = uint16_t((s[0] >> 3) << 11 | (s[1] >> 3) << 6 | (s[2] >> 3) << 1 | (in_bpp == 4 ? s[3] >> 7 : 1));
                std::memcpy(dst + i * 2, &v, 2);
                break;
            }
            case GPU_LA8: dst[i * 2] = s[1], dst[i * 2 + 1] = s[0]; break;  // memory: A L
            case GPU_L8: case GPU_A8: dst[i] = s[0]; break;
            default: break;
            }
        }
    }
    // A padded image repeats its last column and row into the padding (linear filtering at the edge).
    if (x0 == 0 && y0 == 0 && (w < lw || h < lh) && t.fmt != GPU_ETC1) {
        const int b = bpp(t.fmt) / 8;
        for (int y = 0; y < lh; y++)
            for (int x = (y < h ? w : 0); x < lw; x++) {
                const uint32_t from = tiled(uint32_t(std::min(x, w - 1)), uint32_t(std::min(y, h - 1)), uint32_t(lw));
                std::memcpy(dst + tiled(uint32_t(x), uint32_t(y), uint32_t(lw)) * uint32_t(b), dst + from * uint32_t(b), size_t(b));
            }
    }
}

// ---- buffers ------------------------------------------------------------------------------------
void buf_free(Buffer &b) {
    if (!b.data) return;
    if (b.used_frame == g_frame_id) free_later(b.data);
    else linearFree(b.data);
    g_buf_bytes -= b.size;
    b.data = nullptr;
    b.size = 0;
}

// ---- positions relative to an origin near them (the PICA's float24) -----------------------------
// The console's vertex shader computes in float24 (a 16-bit mantissa); Azahar in float32. The
// levels' static geometry is in world coordinates (x, y about 2000-2300 in chapter 0), and the
// depth the shader computes from them is a sum of terms that large cancelling to a few metres: an
// error of about 0.03 units, which made the floors and the surfaces just above them (a carpet over
// tiles) fight in jagged patches. So a buffer object's float positions are stored relative to the
// centre of the vertices a draw uses, and that centre is folded into the draw's matrices on the CPU
// in float32 (setup: mvp, vz, wz, world, the light position): the GPU multiplies small numbers.
// Ranges that overlap are merged (undone, then done again over their union).
void shift_position(uint8_t *at, const float c[3], float sign) {
    float v[3];
    std::memcpy(v, at, 12);
    for (int k = 0; k < 3; k++) v[k] += sign * c[k];
    std::memcpy(at, v, 12);
}
// The origin of the positions `a` reads for the vertices lo..hi (zero: as the game gave them).
bool native_attrib(const AttribState &a);
const float *position_origin(const AttribState *a, uint32_t lo, uint32_t hi) {
    static const float zero[3] = {0, 0, 0};
    if (!a || !a->enabled || a->type != 0x1406 || a->size != 3) return zero;
    if (!a->buffer) {  // client memory: copied for each draw, recentred in the copy (attrib_source)
        static float c[3];
        const auto *src = reinterpret_cast<const uint8_t *>(a->pointer);
        const uint32_t stride = a->stride ? uint32_t(a->stride) : 12;
        if (!src || !native_attrib(*a)) return zero;
        float mn[3] = {INFINITY, INFINITY, INFINITY}, mx[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (uint32_t v = lo; v <= hi; v++) {
            float q[3];
            std::memcpy(q, src + size_t(v) * stride, 12);
            for (int k = 0; k < 3; k++) mn[k] = std::min(mn[k], q[k]), mx[k] = std::max(mx[k], q[k]);
        }
        float far = 0;
        for (int k = 0; k < 3; k++) {
            c[k] = 0.5f * (mn[k] + mx[k]);
            if (!std::isfinite(c[k])) return zero;
            far = std::max(far, std::fabs(c[k]));
        }
        return far >= 64 ? c : zero;
    }
    Buffer *b = a->buf;
    const uint32_t stride = a->stride ? uint32_t(a->stride) : 12;
    if (!b || !b->data || stride < 12) return zero;
    const uint32_t field = uint32_t(a->pointer) % stride, vlo = lo + uint32_t(a->pointer) / stride, vhi = hi + uint32_t(a->pointer) / stride;
    if (field + uint64_t(vhi) * stride + 12 > b->size) return zero;
    for (const Origin &o : b->origins)
        if (o.field == field && o.stride == stride && o.lo <= vlo && vhi <= o.hi) return o.c;
    bool copied = false;
    auto writable = [&] {  // (draws of this frame still to run read the old storage, with their origins)
        if (copied || b->used_frame != g_frame_id) return;
        auto *fresh = static_cast<uint8_t *>(linearAlloc(b->size));
        if (!fresh) return;
        std::memcpy(fresh, b->data, b->size);
        free_later(b->data);
        b->data = fresh;
        b->used_frame = 0;
        copied = true;
        g_stats.buf_cow++;
    };
    Origin n{field, stride, vlo, vhi, {0, 0, 0}};
    const uint64_t lo_byte = field + uint64_t(vlo) * stride, hi_byte = field + uint64_t(vhi) * stride + 12;
    for (size_t i = 0; i < b->origins.size();) {
        const Origin &o = b->origins[i];
        const uint64_t olo = o.field + uint64_t(o.lo) * o.stride, ohi = o.field + uint64_t(o.hi) * o.stride + 12;
        if (ohi <= lo_byte || hi_byte <= olo) {
            i++;
            continue;
        }
        if (o.field != field || o.stride != stride) {  // the same bytes as positions of another layout
            log_once("originlayout", "pica: a buffer's positions read with two layouts: not recentred");
            return zero;
        }
        writable();
        if (b->used_frame == g_frame_id) return zero;  // (no memory for the copy)
        for (uint32_t v = o.lo; v <= o.hi; v++) shift_position(b->data + o.field + uint64_t(v) * stride, o.c, +1);
        n.lo = std::min(n.lo, o.lo), n.hi = std::max(n.hi, o.hi);
        b->origins.erase(b->origins.begin() + ptrdiff_t(i));
    }
    float mn[3] = {INFINITY, INFINITY, INFINITY}, mx[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (uint32_t v = n.lo; v <= n.hi; v++) {
        float p[3];
        std::memcpy(p, b->data + field + uint64_t(v) * stride, 12);
        for (int k = 0; k < 3; k++) mn[k] = std::min(mn[k], p[k]), mx[k] = std::max(mx[k], p[k]);
    }
    float far = 0;
    for (int k = 0; k < 3; k++) {
        const float c = 0.5f * (mn[k] + mx[k]);
        n.c[k] = std::isfinite(c) ? c : 0;
        far = std::max(far, std::fabs(n.c[k]));
    }
    if (far >= 64) {  // (nearer: float24 already resolves millimetres)
        writable();
        if (b->used_frame == g_frame_id) n.c[0] = n.c[1] = n.c[2] = 0;
        else
            for (uint32_t v = n.lo; v <= n.hi; v++) shift_position(b->data + field + uint64_t(v) * stride, n.c, -1);
    } else {
        n.c[0] = n.c[1] = n.c[2] = 0;
    }
    gpu_flush(b->data + field + uint64_t(n.lo) * stride, (n.hi - n.lo) * stride + 12, "recentred positions");
    b->origins.push_back(n);
    return b->origins.back().c;
}

// ---- materials ----------------------------------------------------------------------------------
// The vertex shaders (one DVLB, runtime/pica/*.v.pica).
DVLB_s *g_dvlb;
// The vertex shaders' uniforms, by name (-1 where a shader has none).
enum Reg { R_MVP, R_TC0A, R_TC0B, R_TC1A, R_TC1B, R_TC2A, R_TC2B, R_CMUL, R_CADD, R_NRM, R_CAR, R_CAG, R_CAB, R_CBR, R_CBG, R_CBB, R_CC, R_LPOS, R_LATT, R_LCOL, R_SCALES, R_WORLD, R_L1POS, R_L2POS, R_L1COL, R_L2COL, R_AMB,
           R_VZ, R_WZ, R_FOG, R_FOGZ, R_FOGZK, R_COUNT };
const char *const kRegNames[R_COUNT] = {"mvp", "tc0a", "tc0b", "tc1a", "tc1b", "tc2a", "tc2b", "cmul", "cadd", "nrm", "cAr", "cAg", "cAb", "cBr", "cBg", "cBb", "cC", "lpos", "latt", "lcol", "scales", "world", "l1pos", "l2pos", "l1col", "l2col", "amb",
                                        "vz", "wz", "fog", "fogz", "fogzk"};
struct VShader {
    shaderProgram_s prog;
    int r[R_COUNT];
    int fogzon;  // generic.v.pica's bool uniform, as C3D_BoolUnifSet takes it (-1 elsewhere)
    int fogzon_sent = -1;
};
std::vector<VShader> g_vshaders;
VShader *g_bound_vs;
bool g_layout_known = false;  // the vertex layout last sent (draw) is still current

// Float uniform writes that skip unchanged registers (each changed one is GPU commands). The
// values live in citro3d's copy (C3D_FVUnif) but are sent by send_state() from g_unif_dirty, not
// by citro3d (whose C3D_UpdateUniforms scans all 96 registers at every draw).
uint32_t g_unif_dirty[3];
int g_debug;  // (set_debug)
void set_fvec(int reg, float x, float y, float z, float w) {
    if (reg < 0) return;
    C3D_FVec &v = C3D_FVUnif[GPU_VERTEX_SHADER][reg];
    if (v.x == x && v.y == y && v.z == z && v.w == w) return;
    if (g_debug & 1) return C3D_FVUnifSet(GPU_VERTEX_SHADER, reg, x, y, z, w);
    v.x = x, v.y = y, v.z = z, v.w = w;
    g_unif_dirty[reg >> 5] |= 1u << (reg & 31);
}
void set_fvecs(int reg, const float (*rows)[4], int n) {
    for (int i = 0; i < n; i++) set_fvec(reg + i, rows[i][0], rows[i][1], rows[i][2], rows[i][3]);
}

int type_size(GLenum t) { return t == 0x1406 || t == 0x140C ? 4 : t == 0x1402 || t == 0x1403 || t == 0x8D61 ? 2 : 1; }
// Scale that turns an attribute's stored values into what GL hands the shader.
float attrib_scale(const AttribState &a) {
    if (!a.norm || !a.enabled) return 1;
    switch (a.type) {
    case 0x1400: return 1.0f / 127;
    case 0x1401: return 1.0f / 255;
    case 0x1402: return 1.0f / 32767;
    case 0x1403: return 1.0f / 65535;
    default: return 1;
    }
}


const float kZero[2][4] = {};

// The current draw's position transform: GL clip space -> PICA (depth, screen rotation).
// `m` is the GL column-major matrix; `scale` the position attribute's normalisation.
void clip_matrix(float out[4][4], const float *m, float scale, int size) {
    float r[4][4];  // GL rows
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++) r[row][col] = m[col * 4 + row] * (col < 3 || size == 4 ? scale : 1.0f);
    for (int col = 0; col < 4; col++) {
        if (g_target_is_screen) {  // a quarter turn: buffer x = GL y, buffer y = -GL x
            out[0][col] = r[1][col];
            out[1][col] = -r[0][col];
        } else {
            out[0][col] = r[0][col];
            out[1][col] = r[1][col];
        }
        out[2][col] = -0.5f * r[2][col] - 0.5f * r[3][col];  // z: [-w, w] -> [0, -w]
        out[3][col] = r[3][col];
    }
}
// Texture coordinate rows for a PICA unit sampling GL texture `t` with coordinates from an
// attribute with normalisation `scale`, after the GL-side transform `x` (s = x0 u + x1 v + x2,
// t = x3 u + x4 v + x5): the flip to the PICA's t (its first row in memory is t = 1, rendered
// images excepted) and the padded image's share.
void uv_rows(float out[2][4], const Texture *t, float scale, const float x[6]) {
    const float su = t ? t->su : 1, sv = t ? t->sv : 1;
    out[0][0] = x[0] * scale * su, out[0][1] = x[1] * scale * su, out[0][2] = 0, out[0][3] = x[2] * su;
    if (t && t->render_target) {
        out[1][0] = x[3] * scale * sv, out[1][1] = x[4] * scale * sv, out[1][2] = 0, out[1][3] = x[5] * sv;
    } else {
        out[1][0] = -x[3] * scale * sv, out[1][1] = -x[4] * scale * sv, out[1][2] = 0, out[1][3] = 1 - x[5] * sv;
    }
}

// Combiner stages: written only when they change (each change is GPU commands).
C3D_TexEnv g_tev[6];
bool g_tev_known[6];
const struct Recipe *g_tev_recipe;  // what the stages were last made from (setup)
uint32_t g_tev_key[4];  // ... the textures present and the constants they use
void set_tev(int i, const C3D_TexEnv &e) {
    if (g_tev_known[i] && std::memcmp(&g_tev[i], &e, sizeof e) == 0) return;
    g_tev[i] = e;
    g_tev_known[i] = true;
    C3D_SetTexEnv(i, &g_tev[i]);
}
C3D_TexEnv tev_stage() {
    C3D_TexEnv e;
    C3D_TexEnvInit(&e);
    return e;
}
uint32_t pack_color(float r, float g, float b, float a) {
    auto c = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return c(r) | c(g) << 8 | c(b) << 16 | c(a) << 24;
}

// Fog: the PICA's fog unit, indexed by the fragment's depth. Perspective draws use W-buffering
// with depth = view distance / g_depth_far, so the table is linear in distance and the game's
// per-vertex linear fog (a factor clamped from the distance) maps onto it exactly.
C3D_FogLut g_fog_lut;
float g_fog_key[5] = {-1};
bool g_fog_on = false;
uint32_t g_fog_color = ~0u;
void set_fog(const Recipe &r, const Program &p) {
    const bool on = r.u_fog_se >= 0 && !r.fogz;  // (FOGZ: both fogs in the combiners)
    if (!on) {
        if (g_fog_on) C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false), g_fog_on = false;
        return;
    }
    const float *se = p.value(r.u_fog_se);
    const float fmax = r.u_fog_max >= 0 ? p.value(r.u_fog_max)[0] : 1.0f;
    const float key[5] = {se[0], se[1], se[2], fmax, g_depth_far + float(r.fog_kind) * 1e9f};
    if (std::memcmp(key, g_fog_key, sizeof key) != 0) {
        std::memcpy(g_fog_key, key, sizeof key);
        float data[256];
        for (int i = 0; i <= 128; i++) {
            const float d = float(i) / 128.0f * g_depth_far;
            float f = r.fog_kind == 0 ? (d - se[0]) * se[2] : (-d - se[0]) / (se[1] - se[0] == 0 ? 1 : se[1] - se[0]);
            f = std::clamp(f, 0.0f, fmax);
            const float v = 1.0f - f;  // the table holds the fragment colour's share
            if (i < 128) data[i] = v;
            if (i > 0) data[i + 127] = v - data[i - 1];
        }
        FogLut_FromArray(&g_fog_lut, data);
        C3D_FogLutBind(&g_fog_lut);
    }
    const float *c = p.value(r.u_fog_color);
    const uint32_t color = pack_color(c[0], c[1], c[2], 1) & 0xffffff;
    if (color != g_fog_color) C3D_FogColor(color), g_fog_color = color;
    if (!g_fog_on) C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false), g_fog_on = true;
}

// What a draw needs from its recipe: shader, inputs, uniforms, textures, combiners.
struct DrawCtx {
    VShader *vs = nullptr;
    int input_loc[8];  // GL attribute location per PICA input register (-1: its constant value)
    int inputs = 0;
    int unit_tex[3] = {-1, -1, -1};  // GL texture unit per PICA unit
    Texture *tex[3] = {};             // what they have bound
    bool alpha_test = false;
    float alpha_ref = 0;
    bool perspective = true;
    float za = 0, zb = 0;  // perspective: GL z_clip = za * z_view + zb (glDepthRangef under W-buffering)
    bool ramp = false;     // unit 1: the fog factors' ramp (height_fog)
    uint32_t vlo = 0, vhi = 0;      // the vertices the draw uses (draw: from its indices)
    float origin[3] = {0, 0, 0};    // its positions' origin (position_origin), folded into the uniforms
};
// A row the shader dots with the position, for positions stored relative to `o`: + row . o in w.
void fold_origin(float row[4], const float o[3]) { row[3] += row[0] * o[0] + row[1] * o[1] + row[2] * o[2]; }
const AttribState *attr_of(int index) { return index >= 0 && index < 16 ? &g.attr[index] : nullptr; }
float attr_scale(int index) {
    const AttribState *a = attr_of(index);
    return a ? attrib_scale(*a) : 1.0f;
}
Texture *unit_texture(const Program &p, const UnitSpec &s) {
    if (s.sampler < 0) return nullptr;
    Texture *t = g_texp[int(p.value(s.sampler)[0]) & 7];
    return t && t->allocated ? t : nullptr;
}
// FOGZ: the vertex shader's fog uniforms (World and WorldViewT change with every object)
void fogz_uniforms(const Program &p, DrawCtx &c, const AttribState *pos) {
    const Recipe &r = *p.recipe;
    const VShader &vs = *c.vs;
    // GLSL's x / 0 is an infinity, clamped: a huge factor does the same
    auto inv = [](float v) { return v != 0 ? 1 / v : 1e12f; };
    const float ps = pos ? attrib_scale(*pos) : 1, pw = pos && pos->enabled && pos->size == 4 ? ps : 1;
    const float *wvt = p.value(r.u_wvt), *wm = p.value(r.u_world), *vi = p.value(r.u_view_inv), *fz = p.value(r.u_fogz);
    const float fmax = r.u_fog_max >= 0 ? p.value(r.u_fog_max)[0] : 1.0f;
    float vz[4] = {wvt[8] * ps, wvt[9] * ps, wvt[10] * ps, wvt[11] * pw}, wz[4] = {wm[2] * ps, wm[6] * ps, wm[10] * ps, wm[14] * pw};
    fold_origin(vz, c.origin), fold_origin(wz, c.origin);
    set_fvec(vs.r[R_VZ], vz[0], vz[1], vz[2], vz[3]);  // dot(WorldViewT[2], pos)
    set_fvec(vs.r[R_WZ], wz[0], wz[1], wz[2], wz[3]);  // (World * pos).z
    if (r.u_fog_se >= 0) {
        const float *se = p.value(r.u_fog_se);
        set_fvec(vs.r[R_FOG], se[0], se[2], fmax, 0);
    } else {
        set_fvec(vs.r[R_FOG], 0, 0, 0, 0);
    }
    const float zinv = inv(fz[1]), zcam = std::clamp((vi[14] - fz[0]) * zinv, 0.0f, fmax);
    set_fvec(vs.r[R_FOGZ], fz[0], zinv, inv(fz[2]), zcam);
    set_fvec(vs.r[R_FOGZK], fmax, r.fogz_alpha ? 1.0f : 0.0f, r.fogz_ramp ? 0.0f : 1.0f, r.fogz_ramp ? 1.0f : 0.0f);
    if (r.fogz_ramp) set_fvecs(vs.r[R_TC1A], kZero, 2), set_fvecs(vs.r[R_TC1B], kZero, 2), c.ramp = true;
}

// The material of the last draw: what its setup depended on, beyond the uniforms that change with
// every object (Recipe::per_draw: not counted in Program::serial).
struct MaterialKey {  // (no padding: built field by field and compared with memcmp)
    const Program *p;
    uint32_t serial, tex_version;
    const VShader *vs;
    Texture *tex[3];
    float depth_far;
    struct {
        uint32_t format;  // 1 | enabled << 1 | normalized << 2 | size << 3 | type << 8 (0: no attribute)
        float value[4];   // a disabled attribute's constant
    } in[5];              // (at most 5 inputs: generic.v.pica's)
};
static_assert(sizeof(MaterialKey) == 32 + 5 * 20, "MaterialKey: padding would be compared");
MaterialKey g_material;
struct {
    int unit_tex[3];
    Texture *tex[3];
    bool alpha_test, ramp, colour_input;
    float alpha_ref;
} g_material_ctx;
void forget_material() { g_material.p = nullptr; }

bool setup(Program &p, DrawCtx &c) {
    const Recipe &r = *p.recipe;
    c.vs = &g_vshaders[std::min<size_t>(r.vs, g_vshaders.size() - 1)];
    VShader &vs = *c.vs;
    if (g_bound_vs != c.vs) {  // (before its uniforms are set)
        C3D_BindProgram(&c.vs->prog);
        g_bound_vs = c.vs;
        g_layout_known = false;
        // citro3d writes the shader's constants (.constf) into their registers behind its own
        // copy of them, which set_fvec compares with: those registers are written next time
        const shaderInstance_s *si = c.vs->prog.vertexShader;
        for (int i = 0; i < si->numFloat24Uniforms; i++) C3D_FVUnif[GPU_VERTEX_SHADER][si->float24Uniforms[i].id & 0x7f].w = NAN;
        c.vs->fogzon_sent = -1;
    }
    if (vs.fogzon >= 0 && vs.fogzon_sent != int(r.fogz)) {
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, vs.fogzon, r.fogz);
        vs.fogzon_sent = r.fogz;
    }
    // inputs
    const bool generic = r.vs == VS_GENERIC;
    if (generic) {
        const int in[5] = {r.a_vertex, r.a_uv[0], r.a_uv[1], r.a_color, r.a_uv[2]};
        c.inputs = 5;
        for (int i = 0; i < 5; i++) c.input_loc[i] = in[i] >= 0 ? p.attribs[size_t(in[i])].loc : -1;
    } else {
        const int in[4] = {r.a_vertex, r.a_uv[0], r.a_normal, r.a_color};
        c.inputs = 4;
        for (int i = 0; i < 4; i++) c.input_loc[i] = in[i] >= 0 ? p.attribs[size_t(in[i])].loc : -1;
    }
    // position
    const AttribState *pos = attr_of(c.input_loc[0]);
    std::memcpy(c.origin, position_origin(pos, c.vlo, c.vhi), sizeof c.origin);
    float mvp[4][4];
    const float *m = p.value(r.u_mvp);
    clip_matrix(mvp, m, pos ? attrib_scale(*pos) : 1.0f, pos && pos->enabled ? pos->size : 4);
    for (auto &row : mvp) fold_origin(row, c.origin);
    set_fvecs(vs.r[R_MVP], mvp, 4);
    c.perspective = !(m[3] == 0 && m[7] == 0 && m[11] == 0);
    if (c.perspective) {  // this projection's far plane, for the next frame's depth scale
        const float wz[3] = {-m[3], -m[7], -m[11]};  // the view's z row (GL: w = -z)
        int k = 0;
        for (int i = 1; i < 3; i++)
            if (std::fabs(wz[i]) > std::fabs(wz[k])) k = i;
        if (wz[k] != 0) {
            const float A = m[k * 4 + 2] / wz[k], B = m[14] - A * -m[15];
            if (A < -1.0f && A > -1.5f) g_depth_far_next = std::max(g_depth_far_next, std::min(B / (1 + A), 100000.0f));
            c.za = A, c.zb = B;
        }
    }
    // the same material as the last draw's (most draws: only the matrices differ): units, colour,
    // lighting, fog and combiners are as that draw set them
    MaterialKey mk;
    mk.p = &p, mk.serial = p.serial, mk.vs = c.vs, mk.tex_version = g_tex_version, mk.depth_far = g_depth_far;
    for (int n = 0; n < 3; n++) mk.tex[n] = unit_texture(p, r.unit[n]);
    for (int i = 0; i < 5; i++) {
        const AttribState *a = i < c.inputs ? attr_of(c.input_loc[i]) : nullptr;
        auto &in = mk.in[i];
        in.format = a ? 1u | uint32_t(a->enabled) << 1 | uint32_t(a->norm) << 2 | uint32_t(a->size) << 3 | a->type << 8 : 0;
        if (a && !a->enabled) std::memcpy(in.value, a->value, sizeof in.value);
        else in.value[0] = in.value[1] = in.value[2] = in.value[3] = 0;
    }
    if (std::memcmp(&mk, &g_material, sizeof mk) == 0) {
        std::memcpy(c.unit_tex, g_material_ctx.unit_tex, sizeof c.unit_tex);
        std::memcpy(c.tex, g_material_ctx.tex, sizeof c.tex);
        c.alpha_test = g_material_ctx.alpha_test, c.alpha_ref = g_material_ctx.alpha_ref, c.ramp = g_material_ctx.ramp;
        if (!g_material_ctx.colour_input) c.input_loc[3] = -1;
        if (r.fogz) fogz_uniforms(p, c, pos);
        g_stats.materials_reused++;
        return true;
    }
    auto done = [&] {
        g_material = mk;
        std::memcpy(g_material_ctx.unit_tex, c.unit_tex, sizeof c.unit_tex);
        std::memcpy(g_material_ctx.tex, c.tex, sizeof c.tex);
        g_material_ctx.alpha_test = c.alpha_test, g_material_ctx.alpha_ref = c.alpha_ref, g_material_ctx.ramp = c.ramp;
        g_material_ctx.colour_input = c.input_loc[3] >= 0;
        return true;
    };
    PROFILE_MARK(5);
    // textures and their coordinates
    const Reg rows_a[3] = {R_TC0A, R_TC1A, R_TC2A}, rows_b[3] = {R_TC0B, R_TC1B, R_TC2B};
    Texture **tex = c.tex;
    for (int n = 0; n < 3; n++) {
        const UnitSpec &s = r.unit[n];
        tex[n] = unit_texture(p, s);
        if (s.sampler < 0) continue;
        c.unit_tex[n] = tex[n] ? int(p.value(s.sampler)[0]) & 7 : -1;
        float x[6] = {1, 0, 0, 0, 1, 0};
        if (s.offset >= 0) x[2] += p.value(s.offset)[0], x[5] += p.value(s.offset)[1];
        if (s.scaleoffset >= 0) {
            const float *so = p.value(s.scaleoffset);
            x[0] = so[0], x[2] = so[2], x[4] = so[1], x[5] = so[3];
        }
        if (s.transform >= 0) {  // (u, v, 1, 0): columns 0, 1 and 2 of the GL matrix
            const float *t = p.value(s.transform);
            x[0] = t[0], x[1] = t[4], x[2] = t[8], x[3] = t[1], x[4] = t[5], x[5] = t[9];
        }
        const int uv_attr = r.a_uv[s.uv];
        float rows[2][4];
        uv_rows(rows, tex[n], attr_scale(uv_attr >= 0 ? p.attribs[size_t(uv_attr)].loc : -1), x);
        if (!generic) {  // their coordinates all come from uv0 (point2: unit 0 only)
            set_fvecs(vs.r[rows_a[n]], rows, 2);
            continue;
        }
        // generic: tcN = a * uv0 + b * (uv1, or uv2 for unit 2)
        const bool from_b = s.uv != 0;
        set_fvecs(vs.r[from_b ? rows_b[n] : rows_a[n]], rows, 2);
        set_fvecs(vs.r[from_b ? rows_a[n] : rows_b[n]], kZero, 2);
    }
    PROFILE_MARK(6);
    // vertex colour: color * cmul + cadd, with the uniform factors folded in
    float k[4] = {1, 1, 1, 1};
    if (r.u_color >= 0) {
        const float *amb = p.value(r.u_color);
        if (r.color_mask & 1) k[0] = amb[0], k[1] = amb[1], k[2] = amb[2];
        if (r.color_mask & 2) k[3] = amb[3];
    }
    if (r.u_fade >= 0) {
        const float f = p.value(r.u_fade)[0];
        k[0] *= f, k[1] *= f, k[2] *= f;
    }
    const AttribState *col = attr_of(c.input_loc[3]);
    if (col && col->enabled) {
        const float s = attrib_scale(*col);
        set_fvec(vs.r[R_CMUL], k[0] * s, k[1] * s, k[2] * s, k[3] * s);
        set_fvec(vs.r[R_CADD], 0, 0, 0, 0);
    } else {  // a constant colour (glVertexAttrib4f, or none)
        const float *v = col ? col->value : nullptr;
        set_fvec(vs.r[R_CMUL], 0, 0, 0, 0);
        set_fvec(vs.r[R_CADD], k[0] * (v ? v[0] : 1), k[1] * (v ? v[1] : 1), k[2] * (v ? v[2] : 1), k[3] * (v ? v[3] : 1));
        if (!generic) c.input_loc[3] = -1;
    }
    if (r.vs == VS_LIT) {
        if (col && col->enabled) set_fvec(vs.r[R_CMUL], 0, 0, 0, attrib_scale(*col));
        const float *wit = p.value(r.u_wit);
        float n[3][4];
        for (int row = 0; row < 3; row++)
            for (int cc = 0; cc < 4; cc++) n[row][cc] = cc < 3 ? wit[cc * 4 + row] : 0;
        set_fvecs(vs.r[R_NRM], n, 3);
        for (int i = 0; i < 7; i++) {
            const float *v = r.u_ird[i] >= 0 ? p.value(r.u_ird[i]) : nullptr;
            set_fvec(vs.r[R_CAR + i], v ? v[0] : 0, v ? v[1] : 0, v ? v[2] : 0, v && i < 6 ? v[3] : 0);
        }
        const float *lp = r.u_lpos >= 0 ? p.value(r.u_lpos) : nullptr, *la = r.u_latt >= 0 ? p.value(r.u_latt) : nullptr;
        const float *lc = r.u_lcol >= 0 ? p.value(r.u_lcol) : nullptr;
        set_fvec(vs.r[R_LPOS], (lp ? lp[0] : 0) - c.origin[0], (lp ? lp[1] : 0) - c.origin[1], (lp ? lp[2] : 0) - c.origin[2], 0);
        set_fvec(vs.r[R_LATT], la ? la[0] : 0, la ? la[1] : 1, la ? la[2] : 0, 0);
        set_fvec(vs.r[R_LCOL], lc ? lc[0] : 0, lc ? lc[1] : 0, lc ? lc[2] : 0, lc ? lc[3] : 0);
        const AttribState *nr = attr_of(c.input_loc[2]);
        set_fvec(vs.r[R_SCALES], nr ? attrib_scale(*nr) : 1, 1, 1, 1);
    }
    if (r.vs == VS_POINT2) {
        if (col && col->enabled) set_fvec(vs.r[R_CMUL], 0, 0, 0, attrib_scale(*col));
        else set_fvec(vs.r[R_CMUL], 0, 0, 0, 0);
        const float *wm = p.value(r.u_world), *wit = p.value(r.u_wit);
        float rows[3][4], n[3][4];
        for (int row = 0; row < 3; row++)
            for (int cc = 0; cc < 4; cc++) rows[row][cc] = wm[cc * 4 + row], n[row][cc] = cc < 3 ? wit[cc * 4 + row] : 0;
        for (auto &row : rows) fold_origin(row, c.origin);
        set_fvecs(vs.r[R_WORLD], rows, 3);
        set_fvecs(vs.r[R_NRM], n, 3);
        auto v4 = [&](int u, int i, float d) { return u >= 0 ? p.value(u)[i] : d; };
        const float i1 = v4(r.u_l1i, 0, 1), i2 = v4(r.u_l2i, 0, 1);
        set_fvec(vs.r[R_L1POS], v4(r.u_l1p, 0, 0), v4(r.u_l1p, 1, 0), v4(r.u_l1p, 2, 0), 0);
        set_fvec(vs.r[R_L2POS], v4(r.u_l2p, 0, 0), v4(r.u_l2p, 1, 0), v4(r.u_l2p, 2, 0), 0);
        set_fvec(vs.r[R_L1COL], v4(r.u_l1c, 0, 0) * i1, v4(r.u_l1c, 1, 0) * i1, v4(r.u_l1c, 2, 0) * i1, 0);
        set_fvec(vs.r[R_L2COL], v4(r.u_l2c, 0, 0) * i2, v4(r.u_l2c, 1, 0) * i2, v4(r.u_l2c, 2, 0) * i2, 0);
        set_fvec(vs.r[R_AMB], v4(r.u_amb, 0, 0), v4(r.u_amb, 1, 0), v4(r.u_amb, 2, 0), 0);
    }
    PROFILE_MARK(7);
    if (r.fogz) fogz_uniforms(p, c, pos);
    if (r.u_alpharef >= 0) c.alpha_test = true, c.alpha_ref = p.value(r.u_alpharef)[0];
    set_fog(r, p);
    PROFILE_MARK(8);
    // combiners: the same as the last draw's when the recipe, the textures present and the
    // constants they use are (most draws)
    uint32_t key[4] = {(tex[0] ? 1u : 0u) | (tex[1] ? 2u : 0u) | (tex[2] ? 4u : 0u), 0, 0, 0};
    if (r.u_factor >= 0) std::memcpy(&key[1], p.value(r.u_factor), 4);
    if (r.u_tint >= 0) key[2] = pack_color(p.value(r.u_tint)[0], p.value(r.u_tint)[1], p.value(r.u_tint)[2], 0);
    if (r.fogz) {
        const float *fc = r.u_fog_color >= 0 ? p.value(r.u_fog_color) : kZero[0], *zc = p.value(r.u_fogz_color);
        key[2] = pack_color(fc[0], fc[1], fc[2], 0), key[3] = pack_color(zc[0], zc[1], zc[2], 0);
    }
    if (&r == g_tev_recipe && std::memcmp(key, g_tev_key, sizeof key) == 0) return done();
    g_tev_recipe = &r;
    std::memcpy(g_tev_key, key, sizeof key);
    // a texture that is not bound reads as its stand-in (a constant)
    auto T = [&](int n) { return tex[n] ? GPU_TEVSRC(GPU_TEXTURE0 + n) : GPU_CONSTANT; };
    auto stand_in = [&](int n) { return pack_color(r.unit[n].rgb, r.unit[n].rgb, r.unit[n].rgb, r.unit[n].a); };
    uint32_t konst = 0xffffffff;
    for (int n = 0; n < 3; n++)
        if (r.unit[n].sampler >= 0 && !tex[n]) konst = stand_in(n);
    C3D_TexEnv env[6];
    for (C3D_TexEnv &e : env) e = tev_stage();
    C3D_TexEnv &s0 = env[0], &s1 = env[1], &s2 = env[2];
    const GPU_TEVSRC alpha_src = r.split_alpha ? T(2) : T(0);
    const GPU_TEVOP_A alpha_op = r.split_alpha ? GPU_TEVOP_A_SRC_R : GPU_TEVOP_A_SRC_ALPHA;
    int stages = 1;
    switch (r.tev) {
    case TEV_TEXVC:
        C3D_TexEnvSrc(&s0, C3D_RGB, T(0), GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(&s0, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvSrc(&s0, C3D_Alpha, alpha_src, GPU_PRIMARY_COLOR);
        C3D_TexEnvOpAlpha(&s0, alpha_op, GPU_TEVOP_A_SRC_ALPHA);
        C3D_TexEnvFunc(&s0, C3D_Alpha, GPU_MODULATE);
        break;
    case TEV_LIGHTMAP:
        C3D_TexEnvSrc(&s0, C3D_RGB, T(0), T(1));
        C3D_TexEnvFunc(&s0, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvScale(&s0, C3D_RGB, GPU_TEVSCALE_2);
        C3D_TexEnvSrc(&s0, C3D_Alpha, alpha_src);
        C3D_TexEnvOpAlpha(&s0, alpha_op);
        C3D_TexEnvFunc(&s0, C3D_Alpha, GPU_REPLACE);
        break;
    case TEV_LIGHTMAP_TEX2:
        C3D_TexEnvSrc(&s0, C3D_Both, T(2), T(0), GPU_PRIMARY_COLOR);  // d2 * r + d * (1 - r) (GLOW: alpha)
        C3D_TexEnvOpRgb(&s0, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, r.tex2_alpha ? GPU_TEVOP_RGB_SRC_ALPHA : GPU_TEVOP_RGB_SRC_R);
        C3D_TexEnvOpAlpha(&s0, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA, r.tex2_alpha ? GPU_TEVOP_A_SRC_ALPHA : GPU_TEVOP_A_SRC_R);
        C3D_TexEnvFunc(&s0, C3D_Both, GPU_INTERPOLATE);
        C3D_TexEnvSrc(&s1, C3D_RGB, GPU_PREVIOUS, T(1));
        C3D_TexEnvFunc(&s1, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvScale(&s1, C3D_RGB, GPU_TEVSCALE_2);
        stages = 2;
        break;
    case TEV_MASK:
        C3D_TexEnvSrc(&s0, C3D_RGB, T(0), T(1));
        C3D_TexEnvFunc(&s0, C3D_RGB, r.lightmap ? GPU_MODULATE : GPU_REPLACE);
        if (r.lightmap) C3D_TexEnvScale(&s0, C3D_RGB, GPU_TEVSCALE_2);
        C3D_TexEnvSrc(&s0, C3D_Alpha, T(2), GPU_PRIMARY_COLOR);
        C3D_TexEnvOpAlpha(&s0, GPU_TEVOP_A_SRC_G, GPU_TEVOP_A_SRC_ALPHA);
        C3D_TexEnvFunc(&s0, C3D_Alpha, GPU_MODULATE);
        break;
    case TEV_VC:
        C3D_TexEnvSrc(&s0, C3D_Both, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(&s0, C3D_Both, GPU_REPLACE);
        break;
    case TEV_TERRAIN:
        // red map x r + green map x g, x lightmap x 2 (the blue map: terrain_blue)
        C3D_TexEnvSrc(&s0, C3D_Both, T(0), GPU_PRIMARY_COLOR);
        C3D_TexEnvOpRgb(&s0, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_R);
        C3D_TexEnvOpAlpha(&s0, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_R);
        C3D_TexEnvFunc(&s0, C3D_Both, GPU_MODULATE);
        C3D_TexEnvSrc(&s1, C3D_Both, T(2), GPU_PRIMARY_COLOR, GPU_PREVIOUS);
        C3D_TexEnvOpRgb(&s1, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_COLOR);
        C3D_TexEnvOpAlpha(&s1, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_G, GPU_TEVOP_A_SRC_ALPHA);
        C3D_TexEnvFunc(&s1, C3D_Both, GPU_MULTIPLY_ADD);
        C3D_TexEnvSrc(&s2, C3D_RGB, GPU_PREVIOUS, T(1));
        C3D_TexEnvFunc(&s2, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvScale(&s2, C3D_RGB, GPU_TEVSCALE_2);
        stages = 3;
        break;
    case TEV_TINTED:
        // stage 0: tint = colour * mask + 1 * (1 - mask) (the constant's alpha is 0: 1 - alpha = 1)
        C3D_TexEnvSrc(&s0, C3D_RGB, GPU_CONSTANT, GPU_CONSTANT, T(1));
        C3D_TexEnvOpRgb(&s0, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA, GPU_TEVOP_RGB_SRC_R);
        C3D_TexEnvFunc(&s0, C3D_RGB, GPU_INTERPOLATE);
        C3D_TexEnvSrc(&s0, C3D_Alpha, alpha_src, GPU_PRIMARY_COLOR);
        C3D_TexEnvOpAlpha(&s0, alpha_op, GPU_TEVOP_A_SRC_ALPHA);
        C3D_TexEnvFunc(&s0, C3D_Alpha, GPU_MODULATE);
        C3D_TexEnvSrc(&s1, C3D_RGB, GPU_PREVIOUS, T(0));  // x texture, then x vertex colour
        C3D_TexEnvFunc(&s1, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvSrc(&s2, C3D_RGB, GPU_PREVIOUS, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(&s2, C3D_RGB, GPU_MODULATE);
        stages = 3;
        break;
    case TEV_TEX2_MUL:
        C3D_TexEnvSrc(&s0, C3D_RGB, T(0), T(1));
        C3D_TexEnvFunc(&s0, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvSrc(&s0, C3D_Alpha, alpha_src, GPU_PRIMARY_COLOR);
        C3D_TexEnvOpAlpha(&s0, alpha_op, GPU_TEVOP_A_SRC_ALPHA);
        C3D_TexEnvFunc(&s0, C3D_Alpha, GPU_MODULATE);
        C3D_TexEnvSrc(&s1, C3D_RGB, GPU_PREVIOUS, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(&s1, C3D_RGB, GPU_MODULATE);
        stages = 2;
        break;
    case TEV_TEX2_MIX: {
        const float f = r.u_factor >= 0 ? p.value(r.u_factor)[0] : 0;
        C3D_TexEnvSrc(&s0, C3D_Both, T(1), T(0), GPU_CONSTANT);
        C3D_TexEnvOpRgb(&s0, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
        C3D_TexEnvFunc(&s0, C3D_Both, GPU_INTERPOLATE);
        konst = (konst & 0x00ffffff) | pack_color(0, 0, 0, f);
        break;
    }
    case TEV_GLOW:
        C3D_TexEnvSrc(&s0, C3D_RGB, T(0), GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(&s0, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvSrc(&s0, C3D_Alpha, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(&s0, C3D_Alpha, GPU_REPLACE);
        C3D_TexEnvSrc(&s1, C3D_RGB, T(0), T(0), GPU_PREVIOUS);  // + d * (1 - d.a)
        C3D_TexEnvOpRgb(&s1, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA, GPU_TEVOP_RGB_SRC_COLOR);
        C3D_TexEnvFunc(&s1, C3D_RGB, GPU_MULTIPLY_ADD);
        stages = 2;
        break;
    }
    // the lit shaders' colour comes at half scale (lit.v.pica, point2.v.pica): doubled where it is used
    if (r.vs != VS_GENERIC) C3D_TexEnvScale(r.tev == TEV_TINTED ? &s2 : &s0, C3D_RGB, GPU_TEVSCALE_2);
    // every stage reads the stand-ins from its own constant
    for (int i = 0; i < stages; i++) C3D_TexEnvColor(&env[i], konst);
    if (r.tev == TEV_TINTED) C3D_TexEnvColor(&s0, key[2]);
    if (r.fogz) {  // the distance fog, then the height fog (height_fog)
        const GPU_TEVSRC f = r.fogz_ramp ? GPU_TEXTURE1 : GPU_PRIMARY_COLOR;
        for (int k = 0; k < 2; k++) {
            const GPU_TEVOP_RGB op = r.fogz_ramp ? (k ? GPU_TEVOP_RGB_SRC_G : GPU_TEVOP_RGB_SRC_R) : k ? GPU_TEVOP_RGB_SRC_B : GPU_TEVOP_RGB_SRC_G;
            C3D_TexEnv &e = env[stages++];
            if (k && r.fogz_additive) {  // rgb x (1 - factor)
                C3D_TexEnvSrc(&e, C3D_RGB, GPU_PREVIOUS, f);
                C3D_TexEnvOpRgb(&e, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB(int(op) + 1));  // (the ONE_MINUS_ of op)
                C3D_TexEnvFunc(&e, C3D_RGB, GPU_MODULATE);
                continue;
            }
            C3D_TexEnvSrc(&e, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, f);
            C3D_TexEnvOpRgb(&e, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, op);
            C3D_TexEnvFunc(&e, C3D_RGB, GPU_INTERPOLATE);
            C3D_TexEnvColor(&e, key[2 + k]);
        }
    }
    for (int i = 0; i < 6; i++) set_tev(i, env[i]);
    return done();
}
// ---- fixed-function state for a draw --------------------------------------------------------------
constexpr float kPolygonStep = 1.0f / 131072;  // glPolygonOffset: depth per unit (W-buffer: 4 cm at 5000)
GPU_TESTFUNC test_func(GLenum f) {
    switch (f) {
    case 0x0200: return GPU_NEVER;
    case 0x0201: return GPU_LESS;
    case 0x0202: return GPU_EQUAL;
    case 0x0203: return GPU_LEQUAL;
    case 0x0204: return GPU_GREATER;
    case 0x0205: return GPU_NOTEQUAL;
    case 0x0206: return GPU_GEQUAL;
    default: return GPU_ALWAYS;
    }
}
GPU_BLENDFACTOR blend_factor(GLenum f) {
    switch (f) {
    case 0: return GPU_ZERO;
    case 1: return GPU_ONE;
    case 0x0300: return GPU_SRC_COLOR;
    case 0x0301: return GPU_ONE_MINUS_SRC_COLOR;
    case 0x0302: return GPU_SRC_ALPHA;
    case 0x0303: return GPU_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return GPU_DST_ALPHA;
    case 0x0305: return GPU_ONE_MINUS_DST_ALPHA;
    case 0x0306: return GPU_DST_COLOR;
    case 0x0307: return GPU_ONE_MINUS_DST_COLOR;
    case 0x0308: return GPU_SRC_ALPHA_SATURATE;
    case 0x8001: return GPU_CONSTANT_COLOR;
    case 0x8002: return GPU_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return GPU_CONSTANT_ALPHA;
    case 0x8004: return GPU_ONE_MINUS_CONSTANT_ALPHA;
    default: return GPU_ONE;
    }
}
GPU_BLENDEQUATION blend_eq(GLenum e) {
    return e == 0x800A ? GPU_BLEND_SUBTRACT : e == 0x800B ? GPU_BLEND_REVERSE_SUBTRACT : GPU_BLEND_ADD;
}
GPU_STENCILOP stencil_op(GLenum o) {
    switch (o) {
    case 0: return GPU_STENCIL_ZERO;
    case 0x1E01: return GPU_STENCIL_REPLACE;
    case 0x1E02: return GPU_STENCIL_INCR;
    case 0x1E03: return GPU_STENCIL_DECR;
    case 0x150A: return GPU_STENCIL_INVERT;
    case 0x8507: return GPU_STENCIL_INCR_WRAP;
    case 0x8508: return GPU_STENCIL_DECR_WRAP;
    default: return GPU_STENCIL_KEEP;
    }
}
// A GL rectangle (x, y up from the bottom left, w, h) on the current target, in its buffer's terms.
void target_rect(const int *r, int &x, int &y, int &w, int &h) {
    if (g_target_is_screen) {  // portrait 240xW: buffer x = GL y, buffer y = W - GL x - w
        x = r[1], y = g_screen_w - r[0] - r[2], w = r[3], h = r[2];
    } else {  // a texture, maybe at a fraction of its GL size
        x = r[0] >> g_target_shift, y = r[1] >> g_target_shift, w = r[2] >> g_target_shift, h = r[3] >> g_target_shift;
    }
}

// Depth buffers for texture targets: one per size, shared (the game gives its targets one depth
// renderbuffer too), in VRAM.
std::map<std::pair<int, int>, void *> g_target_depth;
void *target_depth(int w, int h) {
    void *&d = g_target_depth[{w, h}];
    if (!d) d = vramAlloc(C3D_CalcDepthBufSize(u32(w), u32(h), GPU_RB_DEPTH24_STENCIL8));
    if (!d) log_once("rttdepth", "pica: no VRAM for a %dx%d depth buffer", w, h);
    return d;
}

// The draw's target: the screen or a texture.
bool bind_target() {
    C3D_RenderTarget *t = g_screen;
    g_target_is_screen = true;
    g_target_w = g_screen_w, g_target_h = kScreenH, g_target_shift = 0;
    if (g.framebuffer) {
        if (g.framebuffer != g_fb_name || !g_fb_target) {
            g_fb_target = nullptr;
            auto fb = g_framebuffers.find(g.framebuffer);
            if (fb == g_framebuffers.end() || !fb->second.color_tex) return false;
            auto tex = g_textures.find(fb->second.color_tex);
            if (tex == g_textures.end()) return false;
            g_fb_name = g.framebuffer, g_fb_target = tex->second.get();
            g_fb_depth = fb->second.depth_rb || fb->second.stencil_rb;
        }
        Texture &tx = *g_fb_target;
        if (!tx.allocated) return false;
        if (!tx.target) {
            tx.target = C3D_RenderTargetCreateFromTex(&tx.tex, GPU_TEXFACE_2D, 0, C3D_DEPTHTYPE(-1));
            if (!tx.target) {
                log_once("rtt", "pica: cannot render to a %dx%d texture", tx.pw, tx.ph);
                return false;
            }
            if (g_fb_depth)
                if (void *d = target_depth(tx.pw, tx.ph)) C3D_FrameBufDepth(&tx.target->frameBuf, d, GPU_RB_DEPTH24_STENCIL8);
            tx.render_target = true;
        }
        t = tx.target;
        tx.used_frame = g_frame_id;
        g_target_is_screen = false;
        g_target_w = tx.pw, g_target_h = tx.ph, g_target_shift = tx.rt_shift;
    }
    if (t != g_drawing_on) {
        C3D_FrameDrawOn(t);  // (sets the viewport)
        g_drawing_on = t;
        forget_bindings();
    }
    return true;
}

// ---- drawing --------------------------------------------------------------------------------------
GPU_Primitive_t primitive(GLenum mode) {
    switch (mode) {
    case 4: return GPU_TRIANGLES;
    case 5: return GPU_TRIANGLE_STRIP;
    case 6: return GPU_TRIANGLE_FAN;
    default: return GPU_GEOMETRY_PRIM;  // points and lines: not drawn
    }
}

// Whether the PICA reads an attribute as it is (it lacks unsigned shorts, fixed point, half floats).
bool native_attrib(const AttribState &a) {
    const int ts = type_size(a.type);
    const uint32_t stride = a.stride ? uint32_t(a.stride) : uint32_t(a.size * ts);
    return (a.type == 0x1406 || a.type == 0x1400 || a.type == 0x1401 || a.type == 0x1402) && stride < 256 && (a.pointer & uint32_t(ts - 1)) == 0;
}
// Client-memory vertex data copied for this draw: one copy per interleaved stream.
struct Copies {
    struct Copy {
        const uint8_t *src;
        uint32_t stride;
        const uint8_t *dst;
    } c[8];
    int n = 0;
};
// Attribute data of a draw, vertices base..last (vertex `base` first): the buffer object's
// storage, or this frame's copy of client memory (converted to float for formats the PICA lacks).
// `origin`: client-memory positions are copied relative to it (position_origin), or null.
bool attrib_source(const AttribState &a, uint32_t base, uint32_t last, const uint8_t *&data, uint32_t &stride, GPU_FORMATS &fmt, int &count,
                   Copies &copies, const float *origin = nullptr) {
    const int ts = type_size(a.type);
    const uint32_t elem = uint32_t(a.size * ts);
    stride = a.stride ? uint32_t(a.stride) : elem;
    count = a.size;
    const bool native = native_attrib(a);
    fmt = a.type == 0x1406 ? GPU_FLOAT : a.type == 0x1400 ? GPU_BYTE : a.type == 0x1401 ? GPU_UNSIGNED_BYTE : GPU_SHORT;
    const uint8_t *src;
    if (a.buffer) {
        Buffer *b = a.buf;
        if (!b || !b->data) return false;
        b->used_frame = g_frame_id;
        if (a.pointer + size_t(last) * stride + elem > b->size) return false;
        src = b->data + a.pointer;
        if (native) {
            data = src + size_t(base) * stride;
            return true;
        }
    } else {
        src = reinterpret_cast<const uint8_t *>(a.pointer);
        if (!src) return false;
    }
    const uint32_t n = last - base + 1;
    if (native) {
        for (int i = 0; i < copies.n; i++) {  // another attribute of a stream already copied
            const Copies::Copy &cp = copies.c[i];
            if (cp.stride == stride && src >= cp.src && src + elem <= cp.src + stride) {
                data = cp.dst + (src - cp.src);
                return true;
            }
        }
        // the whole vertices (the stream's other attributes may follow this one)
        uint8_t *dst = ring_alloc(n * stride);
        std::memcpy(dst, src + size_t(base) * stride, size_t(n) * stride);
        if (origin && (origin[0] != 0 || origin[1] != 0 || origin[2] != 0))
            for (uint32_t i = 0; i < n; i++) shift_position(dst + size_t(i) * stride, origin, -1);
        if (copies.n < 8) copies.c[copies.n++] = {src, stride, dst};
        data = dst;
        g_stats.copied_bytes += n * stride;
        return true;
    }
    // unsigned shorts, fixed, half floats, wide strides: to packed floats
    float *dst = reinterpret_cast<float *>(ring_alloc(n * uint32_t(a.size) * 4, 4));
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *v = src + size_t(base + i) * stride;
        for (int k = 0; k < a.size; k++) {
            float f = 0;
            switch (a.type) {
            case 0x1406: std::memcpy(&f, v + 4 * k, 4); break;
            case 0x1400: f = float(int8_t(v[k])); break;
            case 0x1401: f = float(v[k]); break;
            case 0x1402: { int16_t x; std::memcpy(&x, v + 2 * k, 2); f = x; break; }
            case 0x1403: { uint16_t x; std::memcpy(&x, v + 2 * k, 2); f = x; break; }
            case 0x140C: { int32_t x; std::memcpy(&x, v + 4 * k, 4); f = float(x) / 65536.0f; break; }
            case 0x8D61: {  // half float
                uint16_t hb;
                std::memcpy(&hb, v + 2 * k, 2);
                const uint32_t sign = uint32_t(hb >> 15) << 31, e = hb >> 10 & 31, m = hb & 1023;
                const uint32_t bits = e == 0 ? sign : e == 31 ? sign | 0x7f800000 | m << 13 : sign | (e + 112) << 23 | m << 13;
                std::memcpy(&f, &bits, 4);
                break;
            }
            }
            dst[i * uint32_t(a.size) + uint32_t(k)] = f;
        }
    }
    data = reinterpret_cast<const uint8_t *>(dst);
    stride = uint32_t(a.size) * 4;
    fmt = GPU_FLOAT;
    return true;
}
// The PICA's vertex buffers for a draw's attribute streams: attributes interleaved in the same
// vertices become one buffer with a component list (in address order, gaps as padding), which
// is fewer buffers for the GPU to fetch and fewer commands; anything not 4-byte aligned gets a
// buffer of its own.
struct Stream {
    const uint8_t *data;
    uint32_t stride, bytes;
    int loader;
};
bool buffers(C3D_BufInfo &bi, Stream *st, int n) {
    bool used[8] = {};
    for (int i = 0; i < n; i++) {
        if (used[i]) continue;
        // the streams of this one's vertices, by address
        int group[8], g = 0;
        const uint8_t *lo = st[i].data;
        for (int j = i; j < n; j++)
            if (!used[j] && st[j].stride == st[i].stride && st[j].data + st[i].stride > st[i].data && st[j].data < st[i].data + st[i].stride &&
                !((uintptr_t(st[j].data) | st[j].bytes) & 3))
                group[g++] = j, lo = std::min(lo, st[j].data);
        if ((uintptr_t(st[i].data) | st[i].bytes) & 3) group[0] = i, g = 1, lo = st[i].data;
        for (int k = 1; k < g; k++)  // (by address: insertion, at most 8)
            for (int m = k; m > 0 && st[group[m]].data < st[group[m - 1]].data; m--) std::swap(group[m], group[m - 1]);
        uint64_t perm = 0;
        int comps = 0;
        uint32_t at = 0;  // bytes of the vertex described so far
        int taken = 0;
        for (int k = 0; k < g; k++) {
            const Stream &s = st[group[k]];
            const uint32_t off = uint32_t(s.data - lo);
            if (off < at || off + s.bytes > s.stride || comps + int((off - at + 15) / 16) + 1 > 12) break;  // (overlaps: own buffer)
            for (uint32_t pad = off - at; pad; ) {  // 0xC .. 0xF: 4 .. 16 bytes of padding
                const uint32_t p = std::min<uint32_t>(pad, 16);
                perm |= uint64_t(0xB + p / 4) << (4 * comps++);
                pad -= p;
            }
            perm |= uint64_t(s.loader) << (4 * comps++);
            at = off + s.bytes;
            used[group[k]] = true;
            taken++;
        }
        if (!taken) {  // (cannot happen: the first stream always fits)
            used[i] = true;
            perm = uint64_t(st[i].loader), comps = 1, lo = st[i].data;
            at = st[i].bytes;
        }
        // the rest of the vertex as padding: the components must add up to the stride, or the
        // console's PICA freezes (3dbrew GPU/Pitfalls); Azahar reads the stride alone
        const uint32_t rest = st[i].stride - at;
        if (rest % 4 == 0 && at % 4 == 0 && comps + int((rest + 15) / 16) <= 12) {
            for (uint32_t pad = rest; pad;) {
                const uint32_t p = std::min<uint32_t>(pad, 16);
                perm |= uint64_t(0xB + p / 4) << (4 * comps++);
                pad -= p;
            }
        } else {
            g_stats.stride_mismatch++;
            log_once("stride" + std::to_string(st[i].stride) + "/" + std::to_string(at), "pica: a vertex of %u bytes described by %u: the GPU may freeze",
                     st[i].stride, at);
        }
        if (BufInfo_Add(&bi, lo, ptrdiff_t(st[i].stride), comps, perm) < 0) return false;
        if (!used[i]) i--;  // (the first stream did not fit its group: again, on its own)
    }
    return true;
}

// Range of the vertices a draw reads (from its indices).
template <class T> void index_range(const T *idx, GLsizei n, uint32_t &lo, uint32_t &hi) {
    lo = ~0u, hi = 0;
    for (GLsizei i = 0; i < n; i++) lo = std::min<uint32_t>(lo, idx[i]), hi = std::max<uint32_t>(hi, idx[i]);
}
// 16-bit indices: two at a time (most draws)
void index_range(const uint16_t *idx, GLsizei n, uint32_t &lo, uint32_t &hi) {
    uint32_t a = 0xffff, b = 0, a2 = 0xffff, b2 = 0;
    GLsizei i = 0;
    if ((uintptr_t(idx) & 2) && n > 0) a = b = idx[i++];
    const uint32_t *w = reinterpret_cast<const uint32_t *>(idx + i);
    for (GLsizei k = 0; k < (n - i) / 2; k++) {
        const uint32_t v = w[k], l = v & 0xffff, h = v >> 16;
        a = std::min(a, l), b = std::max(b, l), a2 = std::min(a2, h), b2 = std::max(b2, h);
    }
    if ((n - i) & 1) a = std::min<uint32_t>(a, idx[n - 1]), b = std::max<uint32_t>(b, idx[n - 1]);
    lo = std::min(a, a2), hi = std::max(b, b2);
    if (n <= 0) lo = ~0u, hi = 0;
}

// citro3d re-sends any state it is handed, so the fragment state, vertex layout and texture
// bindings are compared with what was last given and only changes go through.
struct FragState {
    bool alpha_test;
    uint8_t alpha_ref;
    GPU_TESTFUNC depth_func;
    int writes;
    GPU_BLENDEQUATION eq_rgb, eq_a;
    GPU_BLENDFACTOR src_rgb, dst_rgb, src_a, dst_a;
    GPU_CULLMODE cull;
    bool stencil;
    GPU_TESTFUNC stencil_func;
    int stencil_ref, stencil_mask, stencil_write;
    GPU_STENCILOP s_fail, s_zfail, s_zpass;
    bool wbuffer;
    float depth_scale, depth_offset;
    int vp[4], sc[5];
    bool operator==(const FragState &o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
};
FragState g_frag;
bool g_frag_known = false;
const C3D_Tex *g_bound_tex[3];
uint32_t g_bound_tex_version;
void forget_bindings() {  // after a target switch (it resets the viewport) or a deleted texture
    g_frag_known = false;
    g_bound_tex[0] = g_bound_tex[1] = g_bound_tex[2] = reinterpret_cast<const C3D_Tex *>(1);
}

void set_frag(const FragState &f) {
    const FragState &o = g_frag;
    const bool all = !g_frag_known;
    if (all || f.alpha_test != o.alpha_test || f.alpha_ref != o.alpha_ref) C3D_AlphaTest(f.alpha_test, GPU_GREATER, f.alpha_ref);
    if (all || f.depth_func != o.depth_func || f.writes != o.writes) C3D_DepthTest(true, f.depth_func, GPU_WRITEMASK(f.writes));
    if (all || f.eq_rgb != o.eq_rgb || f.eq_a != o.eq_a || f.src_rgb != o.src_rgb || f.dst_rgb != o.dst_rgb || f.src_a != o.src_a || f.dst_a != o.dst_a)
        C3D_AlphaBlend(f.eq_rgb, f.eq_a, f.src_rgb, f.dst_rgb, f.src_a, f.dst_a);
    if (all || f.cull != o.cull) C3D_CullFace(f.cull);
    if (all || f.stencil != o.stencil || f.stencil_func != o.stencil_func || f.stencil_ref != o.stencil_ref || f.stencil_mask != o.stencil_mask ||
        f.stencil_write != o.stencil_write)
        C3D_StencilTest(f.stencil, f.stencil_func, f.stencil_ref, f.stencil_mask, f.stencil_write);
    if (all || f.s_fail != o.s_fail || f.s_zfail != o.s_zfail || f.s_zpass != o.s_zpass) C3D_StencilOp(f.s_fail, f.s_zfail, f.s_zpass);
    if (all || f.wbuffer != o.wbuffer || f.depth_scale != o.depth_scale || f.depth_offset != o.depth_offset)
        C3D_DepthMap(!f.wbuffer, f.depth_scale, f.depth_offset);
    if (all || std::memcmp(f.vp, o.vp, sizeof f.vp) != 0) C3D_SetViewport(u32(f.vp[0]), u32(f.vp[1]), u32(f.vp[2]), u32(f.vp[3]));
    if (all || std::memcmp(f.sc, o.sc, sizeof f.sc) != 0)
        C3D_SetScissor(GPU_SCISSORMODE(f.sc[0]), u32(f.sc[1]), u32(f.sc[2]), u32(f.sc[3]), u32(f.sc[4]));
    g_frag = f;
    g_frag_known = true;
}

// The vertex layout of the last draw (attribute formats and buffer addresses).
C3D_AttrInfo g_attr_info;
C3D_BufInfo g_buf_info;
float g_fixed[8][4];

// Indices of a draw, for the vertex range when vertices come from client memory.
struct Indices {
    const uint8_t *cpu = nullptr;  // null: glDrawArrays
    GLenum type = 0;
    GLsizei count = 0;
};


// ---- the state citro3d has pending, sent by the backend ---------------------------------------------
// citro3d's setters (C3D_TexBind, C3D_SetTexEnv, C3D_AlphaBlend, C3D_SetBufInfo ...) only note the
// state in its context and set a flag; at each draw C3Di_UpdateContext sends what is flagged, one
// function call per register write, and every time scans all 96 float uniforms of the vertex and
// of the geometry shader for changes: in chapter 3's busy frames about 10 us of a draw's 35 (Azahar).
// The backend keeps calling the setters (citro3d's context stays right: a return from the HOME menu
// sends it all again), but sends the state that changes from draw to draw itself, the same register
// writes as citro3d's, straight into the command buffer, and clears those flags: vertex layout and
// buffers, fragment operations, texture units, combiners, the fog's mode, colour and table, and the
// uniforms set_fvec changed. Anything else pending (render target, viewport, scissor, shader, fixed
// attributes) goes through C3Di_UpdateContext as before.
//
// The context's layout is that of devkitPro's citro3d (offsets read from libcitro3d.a's code) and
// is checked at start with its own setters (c3d_check): if it differs, every draw goes through
// C3Di_UpdateContext. In chapter 3's busy frames (replay, Azahar): 7.8 -> 6.0 ms a frame.
extern "C" {
void C3Di_UpdateContext(void);
extern u8 __C3D_Context[];
}
enum : u32 {
    CF_ACTIVE = 1u << 0, CF_DRAW_USED = 1u << 1, CF_ATTR_INFO = 1u << 2, CF_BUF_INFO = 1u << 3, CF_EFFECT = 1u << 4,
    CF_TEXENV_BUF = 1u << 9, CF_TEX_STATUS = 1u << 14, CF_FOG_LUT = 1u << 17, CF_TEX_ALL = 7u << 23, CF_TEXENV_ALL = 0x3Fu << 26,
    CF_DIRECT = CF_ATTR_INFO | CF_BUF_INFO | CF_EFFECT | CF_TEXENV_BUF | CF_FOG_LUT | CF_TEX_ALL | CF_TEXENV_ALL,
};
template <class T> T &c3d_at(uint32_t offset) { return *reinterpret_cast<T *>(__C3D_Context + offset); }
u32 &c3d_flags() { return c3d_at<u32>(32); }
constexpr uint32_t kCtxAttrInfo = 40, kCtxBufInfo = 64, kCtxEffect = 216, kCtxTexConfig = 272, kCtxTexShadow = 276, kCtxTex = 280,
                   kCtxTexEnv = 292, kCtxTexEnvBuf = 412, kCtxTexEnvBufColor = 416, kCtxFogColor = 420, kCtxFogLut = 424,
                   kCtxFixedDirty = 520;
bool g_direct = false;  // the context is as expected (c3d_check)

// Command words written into citro3d's command buffer as GPUCMD_Add does: the first parameter, the
// header (register, byte mask, parameter count, consecutive registers), the other parameters,
// padding to 8 bytes.
struct CmdWriter {
    u32 *o;
    void put(u32 header, u32 v) { o[0] = v, o[1] = header, o += 2; }
    void put(u32 header, const u32 *v, u32 n) {
        o[0] = v[0], o[1] = header | (n - 1) << 20;
        for (u32 i = 1; i < n; i++) o[i + 1] = v[i];
        o += n + 1;
        if (!(n & 1)) *o++ = 0;
    }
};
// Room for any one draw's state and commands (the uniforms all 96 at worst, the fog table ...).
constexpr u32 kDrawWords = 1024;

// The uniforms set_fvec changed: FLOATUNIFORM_CONFIG (32-bit floats, first register), then the
// data, w z y x per register as citro3d keeps them, at most 64 registers per write.
void send_uniforms(CmdWriter &w) {
    for (int k = 0; k < 3; k++)
        while (g_unif_dirty[k]) {
            const int first = k * 32 + __builtin_ctz(g_unif_dirty[k]);
            int n = 0;
            while (first + n < 96 && n < 64 && (g_unif_dirty[(first + n) >> 5] >> ((first + n) & 31) & 1)) {
                g_unif_dirty[(first + n) >> 5] &= ~(1u << ((first + n) & 31));
                n++;
            }
            w.put(0x000f02c0, 0x80000000u | u32(first));
            w.put(0x000f02c1, reinterpret_cast<const u32 *>(&C3D_FVUnif[GPU_VERTEX_SHADER][first]), u32(n) * 4);
        }
}

// What C3Di_UpdateContext would send for the flags in CF_DIRECT, in its order: C3Di_AttrInfoBind,
// C3Di_BufInfoBind, C3Di_EffectBind, C3Di_SetTex + the texture unit configuration, the combiner
// buffer and fog colours, the combiner stages, the fog table.
void send_direct(CmdWriter &w, u32 &f) {
    if (f & CF_ATTR_INFO) {
        const C3D_AttrInfo &ai = c3d_at<C3D_AttrInfo>(kCtxAttrInfo);
        w.put(0x800f0201, ai.flags, 2);
        w.put(0x000b02b9, u32(ai.attrCount - 1) | 0xa0000000u);
        w.put(0x000f0242, u32(ai.attrCount - 1));
        w.put(0x800f02bb, reinterpret_cast<const u32 *>(&ai.permutation), 2);
    }
    if (f & CF_BUF_INFO) {
        const C3D_BufInfo &bi = c3d_at<C3D_BufInfo>(kCtxBufInfo);
        w.put(0x000f0200, bi.base_paddr >> 3);
        w.put(0x800f0203, reinterpret_cast<const u32 *>(bi.buffers), 36);
    }
    if (f & CF_EFFECT) {
        const u8 *e = &c3d_at<u8>(kCtxEffect);
        auto word = [&](int at) { u32 v; std::memcpy(&v, e + at, 4); return v; };
        w.put(0x000f006d, e[17]);                     // DEPTHMAP_ENABLE
        w.put(0x000f0040, e[16] & 3u);                // FACECULLING_CONFIG
        const u32 depth_map[2] = {word(8), word(12)};
        w.put(0x800f004d, depth_map, 2);              // DEPTHMAP_SCALE, _OFFSET
        const u32 tests[4] = {word(24), word(28), word(32), word(36)};
        w.put(0x800f0104, tests, 4);                  // alpha test, stencil test, stencil op, depth and colour mask
        w.put(0x00080126, (u32(0xaf02 >> ((word(36) >> 3) & 14)) << 24) & 0x3000000u);
        w.put(0x000f0103, word(40));                  // BLEND_COLOR
        w.put(0x000f0101, word(44));                  // BLEND_FUNC
        w.put(0x000f0102, e[48]);                     // LOGIC_OP
        w.put(0x00070100, word(0));                   // COLOR_OPERATION
        w.put(0x000f0130, word(4));                   // FRAGOP_SHADOW
        w.put(0x00010062, e[18]);                     // early depth
        w.put(0x000f0118, e[18]);
        w.put(0x00010061, e[19]);
        w.put(0x0007006a, word(20));
    }
    if (f & CF_TEX_ALL) {
        static const u32 unit_reg[3][2] = {{0x800f0081, 0x000f008e}, {0x800f0091, 0x000f0096}, {0x800f0099, 0x000f009e}};
        u32 units = 0;
        for (int i = 0; i < 3; i++) {
            const C3D_Tex *t = c3d_at<const C3D_Tex *>(kCtxTex + 4 * u32(i));
            if (!t) continue;
            units |= 1u << i;
            if (!(f & (1u << (23 + i)))) continue;
            const u32 regs[5] = {t->border, t->dim, t->param, t->lodParam, osConvertVirtToPhys(t->data) >> 3};
            w.put(unit_reg[i][0], regs, 5);
            w.put(unit_reg[i][1], u32(t->fmt) & 15);
        }
        u32 &config = c3d_at<u32>(kCtxTexConfig);
        config = (config & ~7u) | units | 1u << 16;
        f |= CF_TEX_STATUS;
    }
    if (f & CF_TEX_STATUS) {  // the units, then clearing the texture cache
        u32 &config = c3d_at<u32>(kCtxTexConfig);
        w.put(0x000b0080, config);
        if (config & 1u << 16) {
            config &= ~(1u << 16);
            w.put(0x00040080, 1u << 16);
        }
        w.put(0x000f008b, c3d_at<u32>(kCtxTexShadow));
    }
    const u32 texenv_buf = c3d_at<u32>(kCtxTexEnvBuf);
    if (f & CF_TEXENV_BUF) {
        w.put(0x000700e0, texenv_buf);
        w.put(0x000f00fd, c3d_at<u32>(kCtxTexEnvBufColor));
        w.put(0x000f00e1, c3d_at<u32>(kCtxFogColor));
    }
    for (int i = 0; i < 6; i++)
        if (f & (1u << (26 + i))) {
            const C3D_TexEnv &e = c3d_at<C3D_TexEnv>(kCtxTexEnv + 20 * u32(i));
            w.put(0x800f0000 | u32(i < 4 ? 0xc0 + 8 * i : 0xf0 + 8 * (i - 4)), reinterpret_cast<const u32 *>(&e), 5);
        }
    f &= ~(CF_DIRECT | CF_TEX_STATUS);
    if ((texenv_buf & 7) == 0) f |= CF_FOG_LUT & c3d_flags();  // (citro3d sends the table once fog is on)
    else if (c3d_flags() & CF_FOG_LUT) {
        if (const C3D_FogLut *lut = c3d_at<const C3D_FogLut *>(kCtxFogLut)) {
            w.put(0x000f00e6, 0);
            w.put(0x000f00e8, lut->data, 128);
        }
    }
}

// The pending state, before a draw's own commands: false if the command buffer is full.
bool send_state() {
    u32 &f = c3d_flags();
    const bool others = (f & ~(CF_DIRECT | CF_ACTIVE | CF_DRAW_USED)) || (c3d_at<u32>(kCtxFixedDirty) & 0xffff) ||
                        C3D_BoolUnifsDirty[GPU_VERTEX_SHADER] || !g_direct || (g_debug & 1);
    if (others) C3Di_UpdateContext();  // (all that is pending, the flags above too)
    if (gpuCmdBufOffset + kDrawWords > gpuCmdBufSize) return false;
    CmdWriter w{gpuCmdBuf + gpuCmdBufOffset};
    if (!others && (f & CF_DIRECT)) {
        u32 flags = f;
        send_direct(w, flags);
        f = flags;
    }
    send_uniforms(w);
    gpuCmdBufOffset = u32(w.o - gpuCmdBuf);
    return true;
}

// Whether citro3d's context is laid out as send_state() expects: its own setters' effects read back.
bool c3d_check() {
    const u32 f0 = c3d_flags();
    bool ok = (f0 & CF_ACTIVE) != 0;
    C3D_CullFace(GPU_CULL_FRONT_CCW);
    ok = ok && c3d_at<u8>(kCtxEffect + 16) == GPU_CULL_FRONT_CCW && (c3d_flags() & CF_EFFECT);
    C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_ALL);
    ok = ok && c3d_at<u32>(kCtxEffect + 36) == (1u | u32(GPU_GEQUAL) << 4 | u32(GPU_WRITE_ALL) << 8);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_AttrInfo ai;
    AttrInfo_Init(&ai);
    AttrInfo_AddLoader(&ai, 0, GPU_FLOAT, 3);
    C3D_SetAttrInfo(&ai);
    ok = ok && std::memcmp(&c3d_at<C3D_AttrInfo>(kCtxAttrInfo), &ai, sizeof ai) == 0 && (c3d_flags() & CF_ATTR_INFO);
    C3D_BufInfo bi;
    BufInfo_Init(&bi);
    C3D_SetBufInfo(&bi);
    ok = ok && std::memcmp(&c3d_at<C3D_BufInfo>(kCtxBufInfo), &bi, sizeof bi) == 0 && (c3d_flags() & CF_BUF_INFO);
    C3D_TexEnv env;
    C3D_TexEnvInit(&env);
    C3D_TexEnvColor(&env, 0x12345678);
    C3D_SetTexEnv(5, &env);
    ok = ok && std::memcmp(&c3d_at<C3D_TexEnv>(kCtxTexEnv + 100), &env, sizeof env) == 0 && (c3d_flags() & 1u << 31);
    C3D_TexEnvInit(&env);
    C3D_SetTexEnv(5, &env);
    C3D_FogColor(0x123456);
    ok = ok && c3d_at<u32>(kCtxFogColor) == 0x123456 && (c3d_flags() & CF_TEXENV_BUF);
    C3D_FogColor(0);
    C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false);
    ok = ok && (c3d_at<u32>(kCtxTexEnvBuf) & 0x1000f) == GPU_FOG;
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    C3D_FixedAttribSet(3, 0, 0, 0, 1);
    ok = ok && (c3d_at<u32>(kCtxFixedDirty) & 8);
    C3D_TexBind(1, &g_blank);
    ok = ok && c3d_at<const C3D_Tex *>(kCtxTex + 4) == &g_blank && (c3d_flags() & 1u << 24);
    return ok;
}
// Back from the HOME menu or sleep: citro3d sends its whole context again, the backend its
// uniforms (all but the bound shader's constants, which citro3d writes).
aptHookCookie g_apt_hook;
void on_apt(APT_HookType hook, void *) {
    if (hook != APTHOOK_ONRESTORE && hook != APTHOOK_ONWAKEUP) return;
    g_unif_dirty[0] = g_unif_dirty[1] = ~0u, g_unif_dirty[2] = ~0u;
    if (g_bound_vs)
        for (int i = 0; i < g_bound_vs->prog.vertexShader->numFloat24Uniforms; i++) {
            const int id = g_bound_vs->prog.vertexShader->float24Uniforms[i].id & 0x7f;
            g_unif_dirty[id >> 5] &= ~(1u << (id & 31));
        }
}

// C3D_DrawElements without its GPUCMD_Add calls (one function call per register write): the
// pending state (send_state), then the same command words written straight into the command
// buffer, and the command list marked as used as citro3d does. The indices are in the ring
// (fresh_indices).
void draw_elements(GPU_Primitive_t prim, int count, int type, const void *indices) {
    const u32 pa = osConvertVirtToPhys(indices), base = g_buf_info.base_paddr;
    if (pa < base) return;
    if (!send_state() || gpuCmdBufOffset + 32 > gpuCmdBufSize) {
        log_once("cmdbuf", "pica: command buffer full: draws skipped");
        return;
    }
    const bool tri = prim == GPU_TRIANGLES;
    u32 *o = gpuCmdBuf + gpuCmdBufOffset;
    auto w = [&](u32 header, u32 v) { o[0] = v, o[1] = header, o += 2; };
    w(0x0002025e, tri ? 0x300 : u32(prim));                  // PRIMITIVE_CONFIG (triangle lists: as geometry)
    w(0x000f025f, 1);                                         // RESTART_PRIMITIVE
    w(0x000f0227, (pa - base) | u32(type) << 31);            // INDEXBUFFER_CONFIG
    w(0x000f0228, u32(count));                                // NUMVERTICES
    w(0x000f022a, 0);                                         // VERTEX_OFFSET
    if (tri) w(0x00020229, 0x100), w(0x00020253, 0x100);     // GEOSTAGE_CONFIG(2): element mode
    w(0x00010245, 0);                                         // START_DRAW_FUNC0
    w(0x000f022f, 1);                                         // DRAWELEMENTS
    w(0x00010245, 1);
    if (tri) w(0x00020229, 0), w(0x00020253, 0);
    w(0x000f0231, 1);                                         // VTX_FUNC: clears the post-vertex cache
    w(0x0008025e, 0), w(0x0008025e, 0);                       // PRIMITIVE_CONFIG
    gpuCmdBufOffset = u32(o - gpuCmdBuf);
    c3d_flags() |= CF_DRAW_USED;  // (C3D_DrawElements: the list is submitted at the next split)
}


// The terrain's blue map: four textures with the lightmap, three units. A second pass over the
// same vertices adds blue map x vertex blue x lightmap x 2 to what the first drew (depth EQUAL,
// blending ONE + ONE); the distance fog is linear, so its colour came with the first pass and
// this one fogs toward black. Drawn when the first pass does not blend (the terrain never does).
void terrain_blue(const Program &p, const DrawCtx &c, FragState f, const std::function<void()> &again) {
    const Recipe &r = *p.recipe;
    UnitSpec spec;
    spec.sampler = r.u_blue;
    Texture *t = unit_texture(p, spec);
    if (!t || g.blend) return;
    t->used_frame = g_frame_id;
    const int uv = r.a_uv[0] >= 0 ? p.attribs[size_t(r.a_uv[0])].loc : -1;
    const float x[6] = {1, 0, 0, 0, 1, 0};
    float rows[2][4];
    uv_rows(rows, t, attr_scale(uv), x);
    set_fvecs(c.vs->r[R_TC0A], rows, 2);  // (unit 0: the red map's coordinates, uv0)
    C3D_TexBind(0, &t->tex);
    g_bound_tex[0] = &t->tex;
    C3D_TexEnv s0 = tev_stage(), s1 = tev_stage();
    C3D_TexEnvSrc(&s0, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpRgb(&s0, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_B);
    C3D_TexEnvOpAlpha(&s0, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_B);
    C3D_TexEnvFunc(&s0, C3D_Both, GPU_MODULATE);
    C3D_TexEnvSrc(&s1, C3D_RGB, GPU_PREVIOUS, c.tex[1] ? GPU_TEXTURE1 : GPU_CONSTANT);
    C3D_TexEnvFunc(&s1, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvScale(&s1, C3D_RGB, GPU_TEVSCALE_2);
    C3D_TexEnvColor(&s1, pack_color(0.5f, 0.5f, 0.5f, 1));
    set_tev(0, s0);
    set_tev(1, s1);
    set_tev(2, tev_stage());
    g_tev_recipe = nullptr;
    forget_material();
    if (g_fog_on && g_fog_color != 0) C3D_FogColor(0), g_fog_color = 0;
    f.depth_func = g.depth_test ? GPU_EQUAL : GPU_ALWAYS;
    f.writes &= GPU_WRITE_COLOR;
    f.eq_rgb = f.eq_a = GPU_BLEND_ADD;
    f.src_rgb = f.dst_rgb = f.src_a = f.dst_a = GPU_ONE;
    f.s_fail = f.s_zfail = f.s_zpass = GPU_STENCIL_KEEP;
    set_frag(f);
    again();
    g_stats.draws++;
}

// Startup GPU check. On a console the GPU never finished the first frame that drew (the loading
// screen: three small draws from client memory) and the game thread waited for it in
// C3D_FrameBegin; Azahar's GPU goes on. For the first kCheckFrames frames that draw, each draw is
// submitted with its data flushed (submit_split), the queue started and waited for (citro3d only
// runs it at the frame's end; its completion callback swaps the screens only after C3D_FrameEnd),
// then stopped again as C3D_FrameBegin leaves it. A draw the GPU does not finish within 3 s is
// described in the log with the GPU's own state before the game stops: P3D still busy (the GPU
// stuck on the commands) or idle (it finished and the completion was not seen).
// (The check of 2026-10-12 10:13 ran the queue without flushing the command list or the ring:
// on a console the GPU read stale memory, so its "did not finish draw 0" was its own doing.)
constexpr int kCheckFrames = 3;
int g_checked_frames = 0;      // frames with draws checked so far
bool g_frame_checked = false;  // this frame had a checked draw
int g_check_index = 0;         // the draw's number in its frame

void check_draw(const Program &p, const DrawCtx &c, GPU_Primitive_t prim, int count, bool indexed, uint32_t index_type,
                const C3D_AttrInfo &ai, const C3D_BufInfo &bi, const FragState &f) {
    if (g_checked_frames >= kCheckFrames) return;
    g_frame_checked = true;
    const int n = g_check_index++;
    const u32 *list = gpuCmdBuf;  // (GPUCMD_Split moves gpuCmdBuf past what it submits)
    submit_split();
    const u32 words = u32(gpuCmdBuf - list);
    // on the SD card before the GPU runs it: a console that freezes keeps this line
    rt::log("pica: startup check: frame %u draw %d to the GPU: program %s, prim %d, %d vertices, %lu words at %08lx", g_frame_id, n,
            p.signature.c_str(), int(prim), count, (unsigned long)words, (unsigned long)osConvertVirtToPhys(list));
    rt::log_flush();
    auto *queue = reinterpret_cast<gxCmdQueue_s *>(__C3D_Context);  // C3D_Context starts with its gxCmdQueue_s
    gxCmdQueueRun(queue);
    const uint64_t t0 = svcGetSystemTick();
    const bool done = gxCmdQueueWait(queue, 3'000'000'000LL);
    const double ms = double(svcGetSystemTick() - t0) / CPU_TICKS_PER_MSEC;
    if (done) {
        gxCmdQueueStop(queue);
        gxCmdQueueClear(queue);
        rt::log("pica: startup check: frame %u draw %d done (%.2f ms): prim %d, %d vertices", g_frame_id, n, ms, int(prim), count);
        return;
    }
    rt::log("pica: startup check: the GPU did not finish frame %u draw %d in 3 s", g_frame_id, n);
    // GPU registers (GSP: offsets from 0x1EB00000): the busy bits (PSC0 26, PSC1 27, PPF 30, P3D
    // 31) and the PICA's command list registers 0x238-0x23C (size, address, run)
    u32 busy = 0, cmdlist[5] = {};
    const Result rb = GSPGPU_ReadHWRegs(0x400034, &busy, 4), rc = GSPGPU_ReadHWRegs(0x4018E0, cmdlist, sizeof cmdlist);
    rt::log("  GPU: busy %08lx (P3D %s, PPF %s; read %08lx), command list registers %08lx %08lx %08lx %08lx %08lx (read %08lx)",
            (unsigned long)busy, busy >> 31 ? "busy" : "idle", busy >> 30 & 1 ? "busy" : "idle", (unsigned long)rb,
            (unsigned long)cmdlist[0], (unsigned long)cmdlist[1], (unsigned long)cmdlist[2], (unsigned long)cmdlist[3],
            (unsigned long)cmdlist[4], (unsigned long)rc);
    rt::log("  submitted: %lu words at physical %08lx (address >> 3: %08lx), ending %08lx %08lx", (unsigned long)words,
            (unsigned long)osConvertVirtToPhys(list), (unsigned long)(osConvertVirtToPhys(list) >> 3),
            (unsigned long)(words >= 2 ? list[words - 2] : 0), (unsigned long)(words >= 1 ? list[words - 1] : 0));
    rt::log("  program %s, prim %d, %d vertices, %s", p.signature.c_str(), int(prim), count,
            indexed ? (index_type == 0x1401 ? "8-bit indices" : "16-bit indices") : "arrays");
    rt::log("  attributes: %d, permutation %08lx%08lx, formats %08lx %08lx", int(ai.attrCount), (unsigned long)(ai.permutation >> 32),
            (unsigned long)ai.permutation, (unsigned long)ai.flags[0], (unsigned long)ai.flags[1]);
    rt::log("  buffers: %d, base %08lx", bi.bufCount, (unsigned long)bi.base_paddr);
    for (int i = 0; i < bi.bufCount && i < 12; i++)
        rt::log("    buffer %d: offset %08lx, config %08lx %08lx", i, (unsigned long)bi.buffers[i].offset, (unsigned long)bi.buffers[i].flags[0],
                (unsigned long)bi.buffers[i].flags[1]);
    for (int u = 0; u < 3; u++) {
        const Texture *t = c.tex[u];
        if (t)
            rt::log("  unit %d: %dx%d, PICA format %d, %d levels, data %p (physical %08lx)", u, int(t->tex.width), int(t->tex.height),
                    int(t->tex.fmt), t->levels, t->tex.data, (unsigned long)osConvertVirtToPhys(t->tex.data));
        else
            rt::log("  unit %d: %s", u, u == 1 && c.ramp ? "the fog ramp" : "blank");
    }
    rt::log("  blend %d %d %d / %d %d %d, writes %x, depth %d, cull %d, viewport %d %d %d %d, scissor %d", int(f.eq_rgb), int(f.src_rgb),
            int(f.dst_rgb), int(f.eq_a), int(f.src_a), int(f.dst_a), unsigned(f.writes), int(f.depth_func), int(f.cull), f.vp[0], f.vp[1],
            f.vp[2], f.vp[3], int(f.sc[0]));
    rt::log("  target: %s", g_target_is_screen ? "the screen" : "a texture");
    rt::log_flush();
    rt::fatal("the GPU stopped on a draw at the start (details in the log)");
}

// The GPU's indices for an indexed draw: the draw's own, minus `lo` (its first vertex copied or
// addressed), plus `k`, a block of index numbers no draw used lately; the caller moves the vertex
// buffers' addresses back by k vertices. The console's PICA keeps transformed vertices in its
// post-vertex cache by index number, and the write to GPUREG_VTX_FUNC after each draw (citro3d's
// "clear the post-vertex cache") did not clear it: a draw's index 1 came out as the vertex 1 of the
// draw before (the loading screen's quad took a corner's colour from the spinner, menus and play
// were slivers). Azahar has no such cache. Numbers repeat after 65536: long gone from the cache.
uint32_t g_index_next = 0;
const void *fresh_indices(const Indices &ix, GLsizei count, uint32_t lo, uint32_t hi, uint32_t &k) {
    const uint32_t span = hi - lo + 1;
    if (g_index_next + span > 0x10000) g_index_next = 0;
    k = g_index_next;
    g_index_next += span;
    auto *dst = reinterpret_cast<uint16_t *>(ring_alloc(uint32_t(count) * 2, 2));
    const uint32_t shift = k - lo;  // (mod 2^32)
    if (ix.type == 0x1401)
        for (GLsizei i = 0; i < count; i++) dst[i] = uint16_t(ix.cpu[i] + shift);
    else if (ix.type == 0x1403)
        for (GLsizei i = 0; i < count; i++) dst[i] = uint16_t(reinterpret_cast<const uint16_t *>(ix.cpu)[i] + shift);
    else
        for (GLsizei i = 0; i < count; i++) dst[i] = uint16_t(reinterpret_cast<const uint32_t *>(ix.cpu)[i] + shift);
    return dst;
}

bool draw(GLenum mode, GLsizei count, uint32_t first, const Indices &ix) {
    g_stats.draws_requested++;
    Program *p = glprog::current();
    if (!p || !p->linked || count <= 0) return false;
    if (!p->recipe) {
        if (!p->reported && !p->warmup) {
            p->reported = true;
            rt::log("pica: no material for program %u (%s): not drawn", current_name(), p->signature.c_str());
        }
        g_stats.draws_skipped++;
        return false;
    }
    const GPU_Primitive_t prim = primitive(mode);
    if (prim == GPU_GEOMETRY_PRIM) {
        log_once("prim" + std::to_string(mode), "pica: primitive %u not drawn", mode);
        return false;
    }
    if (g.cull && g.cull_face == 0x0408) return false;  // FRONT_AND_BACK: nothing
    begin_frame();
    PROFILE_MARK(0);
    if (!bind_target()) return false;
    DrawCtx c;
    for (int &l : c.input_loc) l = -1;
    // the vertices it uses: for setup's position origin and the copies below
    c.vlo = first, c.vhi = first + uint32_t(count) - 1;
    if (ix.cpu) {
        if (ix.type == 0x1401) index_range(ix.cpu, ix.count, c.vlo, c.vhi);
        else if (ix.type == 0x1403) index_range(reinterpret_cast<const uint16_t *>(ix.cpu), ix.count, c.vlo, c.vhi);
        else index_range(reinterpret_cast<const uint32_t *>(ix.cpu), ix.count, c.vlo, c.vhi);
    }
    PROFILE_MARK(1);
    if (!setup(*p, c)) {
        g_stats.draws_skipped++;
        return false;
    }
    PROFILE_MARK(9);
    // vertex data: buffer objects as they are; client memory (or formats the PICA lacks) copied
    // for the vertices drawn, lo..hi, and the indices then rebased to lo
    bool client = false;
    for (int r = 0; r < c.inputs; r++) {
        const AttribState *a = attr_of(c.input_loc[r]);
        client = client || (a && a->enabled && (!a->buffer || !native_attrib(*a)));
    }
    uint32_t lo = c.vlo, hi = c.vhi;
    if (ix.cpu) {  // (issue() writes the GPU's indices)
        if (!client || lo < (hi - lo + 1) / 8) lo = 0;  // buffer objects: as they are; client: a few unused vertices copied
        if (hi - lo > 0xffff) {
            log_once("idxrange", "pica: a draw spans more than 65536 vertices: not drawn");
            return false;
        }
    }
    const uint32_t base = client ? lo : 0;  // the first vertex the attribute addresses point at
    PROFILE_MARK(11);
    C3D_AttrInfo ai;
    C3D_BufInfo bi;
    AttrInfo_Init(&ai);
    BufInfo_Init(&bi);
    Copies copies;
    Stream streams[8];
    int nstreams = 0;
    for (int r = 0; r < c.inputs; r++) {
        const AttribState *a = attr_of(c.input_loc[r]);
        if (a && a->enabled) {
            Stream &st = streams[nstreams++];
            GPU_FORMATS fmt;
            int n;
            if (!attrib_source(*a, base, hi, st.data, st.stride, fmt, n, copies, r == 0 && !a->buffer ? c.origin : nullptr)) {
                log_once("attr", "pica: vertex attribute %d unreadable (buffer %u, offset %u): draws skipped", c.input_loc[r], a->buffer,
                         uint32_t(a->pointer));
                g_stats.draws_skipped++;
                return false;
            }
            st.loader = AttrInfo_AddLoader(&ai, r, fmt, n);
            st.bytes = uint32_t(n) * (fmt == GPU_FLOAT ? 4u : fmt == GPU_SHORT ? 2u : 1u);
        } else {
            const int idx = AttrInfo_AddFixed(&ai, r);
            const float *v = a ? a->value : nullptr;
            const float val[4] = {v ? v[0] : 0, v ? v[1] : 0, v ? v[2] : 0, v ? v[3] : 1};
            if (!g_layout_known || std::memcmp(g_fixed[idx & 7], val, 16) != 0) {
                C3D_FixedAttribSet(idx, val[0], val[1], val[2], val[3]);
                std::memcpy(g_fixed[idx & 7], val, 16);
            }
        }
    }
    PROFILE_MARK(12);
    if (!buffers(bi, streams, nstreams)) {
        log_once("bufinfo", "pica: vertex data outside the GPU's reach");
        return false;
    }
    PROFILE_MARK(13);
    if (!g_layout_known || std::memcmp(&ai, &g_attr_info, sizeof ai) != 0) C3D_SetAttrInfo(&ai), g_attr_info = ai;
    g_layout_known = true;  // (the buffers: issue() below)
    PROFILE_MARK(2);
    // textures
    for (int u = 0; u < 3; u++) {
        Texture *t = c.tex[u];
        const C3D_Tex *ct = t ? &t->tex : u == 1 && c.ramp ? &g_ramp : &g_blank;  // (citro3d reads the texture it is given)
        if (t) t->used_frame = g_frame_id;
        // a unit the combiners do not read keeps what it has (a binding is ~8 register writes)
        if (!t && ct == &g_blank && g_bound_tex[u] != reinterpret_cast<const C3D_Tex *>(1) && g_bound_tex_version == g_tex_version) continue;
        if (ct != g_bound_tex[u] || g_bound_tex_version != g_tex_version) {
            C3D_TexBind(u, const_cast<C3D_Tex *>(ct));
            g_bound_tex[u] = ct;
        }
    }
    g_bound_tex_version = g_tex_version;
    // fragment operations
    FragState f;
    std::memset(&f, 0, sizeof f);
    f.alpha_test = c.alpha_test;
    f.alpha_ref = uint8_t(std::clamp(int(c.alpha_ref * 255.0f + 0.5f), 0, 255));
    f.depth_func = g.depth_test ? test_func(g.depth_func) : GPU_ALWAYS;
    f.writes = (g.color_mask & 0xF) | (g.depth_mask && g.depth_test ? GPU_WRITE_DEPTH : 0);
    if (g.blend) {
        f.eq_rgb = blend_eq(g.blend_eq_rgb), f.eq_a = blend_eq(g.blend_eq_a);
        f.src_rgb = blend_factor(g.blend_src_rgb), f.dst_rgb = blend_factor(g.blend_dst_rgb);
        f.src_a = blend_factor(g.blend_src_a), f.dst_a = blend_factor(g.blend_dst_a);
        if (p->recipe->unpremultiply && f.src_rgb == GPU_SRC_ALPHA) f.src_rgb = GPU_ONE;  // (rgb / a) * a
    } else {
        f.eq_rgb = f.eq_a = GPU_BLEND_ADD, f.src_rgb = f.src_a = GPU_ONE, f.dst_rgb = f.dst_a = GPU_ZERO;
    }
    // GL culls back faces (counter-clockwise front); the screen's rotation keeps the winding
    f.cull = !g.cull ? GPU_CULL_NONE : (g.front_face == 0x0901) == (g.cull_face == 0x0405) ? GPU_CULL_BACK_CCW : GPU_CULL_FRONT_CCW;
    f.stencil = g.stencil_test, f.stencil_func = test_func(g.stencil_func), f.stencil_ref = g.stencil_ref;
    f.stencil_mask = int(g.stencil_mask), f.stencil_write = int(g.stencil_write);
    f.s_fail = stencil_op(g.stencil_fail), f.s_zfail = stencil_op(g.stencil_zfail), f.s_zpass = stencil_op(g.stencil_zpass);
    // perspective: W-buffer, depth = view distance / g_depth_far (see set_fog); otherwise GL's z
    f.wbuffer = c.perspective;
    f.depth_scale = c.perspective ? 0.0f : -1.0f;
    // glPolygonOffset (decals): GL offsets by z-buffer steps and the polygon's depth slope, which
    // the PICA lacks; here a constant kPolygonStep of the depth range per unit of either
    const float bias = g.polygon_offset ? (g.polygon_factor + g.polygon_units) * kPolygonStep : 0.0f;
    f.depth_offset = c.perspective ? 1.0f / g_depth_far : bias;
    if (g.depth_near == g.depth_far) {  // the sky: one depth (exact without W-buffering)
        f.wbuffer = false, f.depth_scale = 0, f.depth_offset = g.depth_near;
    } else if (c.perspective && (g.depth_near != 0 || g.depth_far != 1 || bias != 0) && c.zb != 0) {
        // glDepthRangef (the sky at 0.99999, the weapon in 0..0.001) and the offset: depth =
        // s z' + o w with z' = alpha w + beta (the PICA z of this projection) = near + bias +
        // (far - near) w / F
        const float alpha = -0.5f * (1 - c.za), beta = -0.5f * c.zb;
        f.depth_scale = (g.depth_near + bias) / beta;
        f.depth_offset = (g.depth_far - g.depth_near) / g_depth_far - f.depth_scale * alpha;
    } else if (!c.perspective && (g.depth_near != 0 || g.depth_far != 1)) {
        f.depth_scale = -(g.depth_far - g.depth_near), f.depth_offset += g.depth_near;  // depth = n + (f - n) * -z'/w
    }
    int x, y, w, h;
    target_rect(g.viewport, x, y, w, h);
    f.vp[0] = std::max(x, 0), f.vp[1] = std::max(y, 0), f.vp[2] = std::max(w, 0), f.vp[3] = std::max(h, 0);
    if (g.scissor_test) {
        target_rect(g.scissor, x, y, w, h);
        f.sc[0] = GPU_SCISSOR_NORMAL, f.sc[1] = std::max(x, 0), f.sc[2] = std::max(y, 0), f.sc[3] = std::max(x + w, 0), f.sc[4] = std::max(y + h, 0);
    } else {
        f.sc[0] = GPU_SCISSOR_DISABLE;
    }
    set_frag(f);
    PROFILE_MARK(3);
#ifdef PICA_PROFILE
    send_state();  // (separately: the state sent, then the draw's own commands)
    PROFILE_MARK(10);
#endif
    // The draw. Indexed: index numbers the GPU has not seen lately (fresh_indices), the vertex
    // buffers' addresses moved back to match; each pass of a draw takes its own.
    auto issue = [&] {
        if (ix.cpu) {
            uint32_t k;
            const void *idx = fresh_indices(ix, count, lo, hi, k);
            C3D_BufInfo shifted = bi;
            for (int j = 0; j < shifted.bufCount; j++) shifted.buffers[j].offset -= k * (shifted.buffers[j].flags[1] >> 16 & 0xFFF);
            if (std::memcmp(&shifted, &g_buf_info, sizeof shifted) != 0) C3D_SetBufInfo(&shifted), g_buf_info = shifted;
            draw_elements(prim, count, C3D_UNSIGNED_SHORT, idx);
        } else {
            if (std::memcmp(&bi, &g_buf_info, sizeof bi) != 0) C3D_SetBufInfo(&bi), g_buf_info = bi;
            send_state();
            C3D_DrawArrays(prim, int(first - base), count);
        }
        g_draws_in_list++;
    };
    issue();
    if (p->recipe->tev == TEV_TERRAIN && p->recipe->u_blue >= 0) terrain_blue(*p, c, f, issue);
    check_draw(*p, c, prim, count, ix.cpu != nullptr, 0x1403, ai, g_buf_info, f);
    PROFILE_MARK(4);
    g_stats.draws++;
    g_stats.vertices += uint32_t(count);
    return true;
}

// ---- the GL functions -----------------------------------------------------------------------------
GLuint new_name() { return g_next_name++; }

void glActiveTexture(GLenum t) { g.unit = int(t - 0x84C0) & 7; }
Buffer *g_bound_buf[2];  // the objects of g.array_buffer, g.element_buffer (no lookups per call)
void glBindBuffer(GLenum t, GLuint b) {
    const int k = t == 0x8893;
    (k ? g.element_buffer : g.array_buffer) = b;
    g_bound_buf[k] = b ? &g_buffers[b] : nullptr;
}
void glBindFramebuffer(GLenum, GLuint f) {
    g.framebuffer = f;
    if (f) g_framebuffers[f];
}
void glBindRenderbuffer(GLenum, GLuint r) {
    g.renderbuffer = r;
    if (r) g_renderbuffers[r];
}
void glBindTexture(GLenum target, GLuint t) {
    if (target != 0x0DE1) log_once("textarget" + std::to_string(target), "pica: texture target %04x not supported", target);
    g.tex[g.unit] = t;
    if (!t) {
        g_texp[g.unit] = nullptr;
        return;
    }
    std::unique_ptr<Texture> &tp = g_textures[t];
    if (!tp) tp = std::make_unique<Texture>();
    g_texp[g.unit] = tp.get();
}
void glBlendColor(GLfloat r, GLfloat gr, GLfloat b, GLfloat a) {
    g.blend_color[0] = r, g.blend_color[1] = gr, g.blend_color[2] = b, g.blend_color[3] = a;
    auto c = [](float v) { return u32(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    C3D_BlendingColor(c(r) | c(gr) << 8 | c(b) << 16 | c(a) << 24);
}
void glBlendEquation(GLenum e) { g.blend_eq_rgb = g.blend_eq_a = e; }
void glBlendEquationSeparate(GLenum c, GLenum a) { g.blend_eq_rgb = c, g.blend_eq_a = a; }
void glBlendFunc(GLenum s, GLenum d) { g.blend_src_rgb = g.blend_src_a = s, g.blend_dst_rgb = g.blend_dst_a = d; }
void glBlendFuncSeparate(GLenum sc, GLenum dc, GLenum sa, GLenum da) {
    g.blend_src_rgb = sc, g.blend_dst_rgb = dc, g.blend_src_a = sa, g.blend_dst_a = da;
}
Buffer *bound_buffer(GLenum t) { return g_bound_buf[t == 0x8893]; }
void glBufferData(GLenum t, GLsizeiptr size, const void *data, GLenum) {
    Buffer *b = bound_buffer(t);
    if (!b) return set_error(0x0502);
    if (!b->data || b->size != uint32_t(size) || b->used_frame == g_frame_id) {
        buf_free(*b);
        if (size > 0) {
            b->data = static_cast<uint8_t *>(linearAlloc(size_t(size)));
            if (!b->data) {
                log_once("bufalloc", "pica: out of linear memory for a %d-byte buffer (%.1f MB of buffers)", int(size), g_buf_bytes / 1048576.0);
                return set_error(0x0505);
            }
            b->size = uint32_t(size);
            g_buf_bytes += b->size;
            g_stats.buf_peak = std::max<uint64_t>(g_stats.buf_peak, g_buf_bytes);
        }
        b->used_frame = 0;
    }
    b->origins.clear();  // (the game's positions again)
    if (data && size > 0) {
        std::memcpy(b->data, data, size_t(size));
        gpu_flush(b->data, u32(size), "a buffer");
    }
}
void glBufferSubData(GLenum t, GLintptr off, GLsizeiptr size, const void *data) {
    Buffer *b = bound_buffer(t);
    if (!b || !b->data || off < 0 || size < 0 || uint32_t(off + size) > b->size) return set_error(0x0501);
    if (b->used_frame == g_frame_id) {  // the GPU may still read the old contents
        auto *fresh = static_cast<uint8_t *>(linearAlloc(b->size));
        if (!fresh) return set_error(0x0505);
        std::memcpy(fresh, b->data, b->size);
        free_later(b->data);
        b->data = fresh;
        b->used_frame = 0;
        g_stats.buf_cow++;
        gpu_flush(b->data, b->size, "a buffer");
    }
    std::memcpy(b->data + off, data, size_t(size));
    // positions written into a recentred range: relative to its origin too
    for (const Origin &o : b->origins) {
        const int64_t first = std::max<int64_t>(o.lo, (int64_t(off) - o.field + o.stride - 1) / int64_t(o.stride));
        for (int64_t i = first; i <= int64_t(o.hi); i++) {
            const int64_t at = o.field + i * int64_t(o.stride);
            if (at + 12 > int64_t(off + size)) break;
            if (at >= int64_t(off)) shift_position(b->data + at, o.c, -1);
        }
    }
    gpu_flush(b->data + off, u32(size), "a buffer");
}
GLenum glCheckFramebufferStatus(GLenum) {
    if (!g.framebuffer) return 0x8CD5;
    Framebuffer &f = g_framebuffers[g.framebuffer];
    if (!f.color_tex) return f.color_rb ? 0x8CDD /*UNSUPPORTED*/ : 0x8CD7 /*MISSING_ATTACHMENT*/;
    return 0x8CD5;
}
void glClear(GLbitfield mask) {
    begin_frame();
    if (!bind_target()) return;
    if (g_draws_in_list) {  // earlier draws go to the GPU before the fill
        submit_split();
        g_draws_in_list = 0;
    }
    auto c = [](float v) { return u32(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    const u32 color = c(g.clear_color[0]) << 24 | c(g.clear_color[1]) << 16 | c(g.clear_color[2]) << 8 | c(g.clear_color[3]);
    const u32 depth = u32(std::clamp(g.clear_depth, 0.0f, 1.0f) * 16777215.0f) | u32(g.clear_stencil & 0xff) << 24;
    int bits = 0;
    if (mask & 0x4000) bits |= C3D_CLEAR_COLOR;
    if (mask & 0x0500) bits |= C3D_CLEAR_DEPTH;  // depth and/or stencil (one buffer)
    if (bits) C3D_RenderTargetClear(g_drawing_on, C3D_ClearBits(bits), color, depth);
    g_stats.clears++;
}
void glClearColor(GLfloat r, GLfloat gr, GLfloat b, GLfloat a) {
    g.clear_color[0] = r, g.clear_color[1] = gr, g.clear_color[2] = b, g.clear_color[3] = a;
}
void glClearDepthf(GLfloat d) { g.clear_depth = d; }
void glClearStencil(GLint s) { g.clear_stencil = s; }
void glColorMask(GLboolean r, GLboolean gr, GLboolean b, GLboolean a) {
    g.color_mask = uint8_t((r ? 1 : 0) | (gr ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0));
}
Texture *bound_texture() { return g_texp[g.unit]; }
// ETC1 4x4 blocks (big-endian 64-bit words) to RGBA, rows bottom-up as GL has them.
void etc1_decode(const uint8_t *src, int w, int h, uint8_t *rgba) {
    static const int tables[8][4] = {{2, 8, -2, -8}, {5, 17, -5, -17}, {9, 29, -9, -29}, {13, 42, -13, -42},
                                     {18, 60, -18, -60}, {24, 80, -24, -80}, {33, 106, -33, -106}, {47, 183, -47, -183}};
    for (int by = 0; by < (h + 3) / 4; by++)
        for (int bx = 0; bx < (w + 3) / 4; bx++, src += 8) {
            const uint32_t hi = uint32_t(src[0]) << 24 | src[1] << 16 | src[2] << 8 | src[3];
            const uint32_t lo = uint32_t(src[4]) << 24 | src[5] << 16 | src[6] << 8 | src[7];
            const bool diff = hi & 2, flip = hi & 1;
            int base[2][3];
            for (int c = 0; c < 3; c++) {
                const int shift = 24 - 8 * c;
                if (diff) {
                    const int b5 = int(hi >> (shift + 3)) & 31;
                    int d = int(hi >> shift) & 7;
                    if (d >= 4) d -= 8;
                    const int b2 = (b5 + d) & 31;
                    base[0][c] = b5 << 3 | b5 >> 2, base[1][c] = b2 << 3 | b2 >> 2;
                } else {
                    base[0][c] = int(hi >> (shift + 4) & 15) * 17, base[1][c] = int(hi >> shift & 15) * 17;
                }
            }
            const int table[2] = {int(hi >> 5) & 7, int(hi >> 2) & 7};
            for (int i = 0; i < 16; i++) {  // pixel i: column i / 4, row i % 4
                const int x = i / 4, y = i % 4, px = bx * 4 + x, py = by * 4 + y;
                if (px >= w || py >= h) continue;
                const int sub = flip ? y >= 2 : x >= 2;
                const int delta = tables[table[sub]][int(lo >> i & 1) | int(lo >> (i + 16) & 1) << 1];
                uint8_t *o = rgba + (size_t(py) * size_t(w) + size_t(px)) * 4;
                for (int c = 0; c < 3; c++) o[c] = uint8_t(std::clamp(base[sub][c] + delta, 0, 255));
                o[3] = 255;
            }
        }
}
// Any upload to RGBA (for images the PICA cannot take as they are).
std::vector<uint8_t> to_rgba(GLenum format, GLenum type, const uint8_t *src, int w, int h, int unpack) {
    std::vector<uint8_t> out(size_t(w) * size_t(h) * 4);
    if (format == 0x8D64) {
        etc1_decode(src, w, h, out.data());
        return out;
    }
    const int in = type == 0x8363 || type == 0x8033 || type == 0x8034 || format == 0x190A ? 2
                   : format == 0x1907 ? 3 : format == 0x1906 || format == 0x1909 ? 1 : 4;
    const size_t row = (size_t(w) * size_t(in) + size_t(unpack) - 1) / size_t(unpack) * size_t(unpack);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *s = src + size_t(y) * row + size_t(x) * size_t(in);
            uint8_t *o = &out[(size_t(y) * size_t(w) + size_t(x)) * 4];
            uint16_t v = 0;
            if (in == 2) std::memcpy(&v, s, 2);
            if (type == 0x8363) o[0] = uint8_t((v >> 11) * 255 / 31), o[1] = uint8_t((v >> 5 & 63) * 255 / 63), o[2] = uint8_t((v & 31) * 255 / 31), o[3] = 255;
            else if (type == 0x8033) o[0] = uint8_t((v >> 12) * 17), o[1] = uint8_t((v >> 8 & 15) * 17), o[2] = uint8_t((v >> 4 & 15) * 17), o[3] = uint8_t((v & 15) * 17);
            else if (type == 0x8034) o[0] = uint8_t((v >> 11) * 255 / 31), o[1] = uint8_t((v >> 6 & 31) * 255 / 31), o[2] = uint8_t((v >> 1 & 31) * 255 / 31), o[3] = (v & 1) ? 255 : 0;
            else if (format == 0x190A) o[0] = o[1] = o[2] = s[0], o[3] = s[1];
            else if (format == 0x1909) o[0] = o[1] = o[2] = s[0], o[3] = 255;
            else if (format == 0x1906) o[0] = o[1] = o[2] = 0, o[3] = s[0];
            else if (format == 0x80E1) o[0] = s[2], o[1] = s[1], o[2] = s[0], o[3] = s[3];
            else o[0] = s[0], o[1] = s[1], o[2] = s[2], o[3] = in == 4 ? s[3] : 255;
        }
    return out;
}

// A level arrives: the texture's PICA storage for it (allocated or grown). -1: dropped.
int tex_level(Texture &t, GLint level, GLsizei w, GLsizei h, GPU_TEXCOLOR fmt, GLenum gl_format) {
    if (level == 0) {
        t.w = w, t.h = h, t.gl_format = gl_format;
        t.skip = 0;
        while ((w >> t.skip) > kMaxTex || (h >> t.skip) > kMaxTex) t.skip++;
        const int lw = std::max(1, w >> t.skip), lh = std::max(1, h >> t.skip);
        const int pw = next_pow2(lw), ph = next_pow2(lh);
        t.su = float(lw) / float(pw), t.sv = float(lh) / float(ph);
        if (!t.allocated || t.fmt != fmt || t.pw != pw || t.ph != ph || t.levels != 1) {
            tex_free(t);
            if (!tex_alloc(t, fmt, pw, ph, 1)) return -1;
        }
        t.have = 0;
        t.render_target = false;
        if (t.target) {
            C3D_RenderTargetDelete(t.target);
            t.target = nullptr;
        }
        return 0;
    }
    if (!t.allocated || fmt != t.fmt || t.skip) return -1;  // (a downscaled image keeps one level)
    const int pl = level;
    if ((t.pw >> pl) < 8 || (t.ph >> pl) < 8) return -1;  // the PICA's smallest level is 8x8
    if (t.pw != t.w || t.ph != t.h) return -1;            // padded images have no mips
    if (pl >= t.levels) {  // grow to the full chain down to 8x8
        int levels = 1;
        while ((t.pw >> levels) >= 8 && (t.ph >> levels) >= 8) levels++;
        if (!tex_alloc(t, t.fmt, t.pw, t.ph, levels)) return -1;
    }
    return pl;
}
void tex_upload(Texture &t, int pl, int x, int y, int w, int h, GLenum format, GLenum type, const void *data, int unpack) {
    tex_cow(t);
    if (data) tex_store(t, pl, x, y, w, h, format, type, static_cast<const uint8_t *>(data), unpack);
    t.have |= 1u << pl;
    tex_params(t);
    tex_written(t.tex);
    g_stats.tex_uploads++;
}
// ETC1 encoding of a 4x4 RGBA block (pixel i at rgba + (i % 4) * row + (i / 4) * 4: column i / 4,
// row i % 4), as 8 big-endian bytes, for the few images that must be scaled down. Fast rather
// than best: the partition with the smaller colour variance, differential base colours when they
// fit, and per subblock the table whose modifiers best match each pixel's brightness offset from
// the base (a modifier moves all three channels alike, so that alone decides it).
void etc1_encode(const uint8_t *rgba, size_t row, uint8_t out[8]) {
    static const int tables[8][4] = {{2, 8, -2, -8}, {5, 17, -5, -17}, {9, 29, -9, -29}, {13, 42, -13, -42},
                                     {18, 60, -18, -60}, {24, 80, -24, -80}, {33, 106, -33, -106}, {47, 183, -47, -183}};
    int px[16][3];
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 3; c++) px[i][c] = rgba[size_t(i % 4) * row + size_t(i / 4) * 4 + size_t(c)];
    auto in_sub = [](int flip, int i) { return flip ? i % 4 >= 2 : i / 4 >= 2; };
    // the partition: smaller summed variance of its two halves
    int flip = 0;
    int64_t best_var = INT64_MAX;
    int avg[2][2][3] = {};
    for (int f = 0; f < 2; f++) {
        int64_t sum[2][3] = {}, sq[2] = {};
        for (int i = 0; i < 16; i++)
            for (int c = 0; c < 3; c++) sum[in_sub(f, i)][c] += px[i][c], sq[in_sub(f, i)] += px[i][c] * px[i][c];
        int64_t var = 0;
        for (int s = 0; s < 2; s++)
            for (int c = 0; c < 3; c++) {
                var += sq[s] / 3 - sum[s][c] * sum[s][c] / 8 / 3;
                avg[f][s][c] = int((sum[s][c] + 4) / 8);
            }
        if (var < best_var) best_var = var, flip = f;
    }
    int q5[2][3], q4[2][3], base[2][3];
    bool diff = true;
    for (int s = 0; s < 2; s++)
        for (int c = 0; c < 3; c++) q5[s][c] = (avg[flip][s][c] * 31 + 127) / 255, q4[s][c] = (avg[flip][s][c] * 15 + 127) / 255;
    for (int c = 0; c < 3; c++) diff = diff && q5[1][c] - q5[0][c] >= -4 && q5[1][c] - q5[0][c] <= 3;
    for (int s = 0; s < 2; s++)
        for (int c = 0; c < 3; c++) base[s][c] = diff ? q5[s][c] << 3 | q5[s][c] >> 2 : q4[s][c] * 17;
    int table[2] = {0, 0};
    uint32_t idx = 0;
    for (int s = 0; s < 2; s++) {
        int d[8], pix[8], n = 0;  // three times each pixel's brightness offset from the base
        for (int i = 0; i < 16; i++)
            if (in_sub(flip, i) == (s == 1)) d[n] = px[i][0] + px[i][1] + px[i][2] - base[s][0] - base[s][1] - base[s][2], pix[n++] = i;
        int64_t best = INT64_MAX;
        for (int t = 0; t < 8; t++) {
            int64_t e = 0;
            for (int j = 0; j < 8; j++) {
                int m = INT32_MAX;
                for (int k = 0; k < 4; k++) m = std::min(m, (3 * tables[t][k] - d[j]) * (3 * tables[t][k] - d[j]));
                e += m;
            }
            if (e < best) best = e, table[s] = t;
        }
        for (int j = 0; j < 8; j++) {
            int bk = 0, m = INT32_MAX;
            for (int k = 0; k < 4; k++) {
                const int v = (3 * tables[table[s]][k] - d[j]) * (3 * tables[table[s]][k] - d[j]);
                if (v < m) m = v, bk = k;
            }
            idx |= uint32_t(bk & 1) << pix[j] | uint32_t(bk >> 1) << (pix[j] + 16);
        }
    }
    uint32_t hi = uint32_t(table[0]) << 5 | uint32_t(table[1]) << 2 | (diff ? 2u : 0u) | uint32_t(flip);
    for (int c = 0; c < 3; c++) {
        const int shift = 24 - 8 * c;
        if (diff) hi |= uint32_t(q5[0][c]) << (shift + 3) | uint32_t((q5[1][c] - q5[0][c]) & 7) << shift;
        else hi |= uint32_t(q4[0][c]) << (shift + 4) | uint32_t(q4[1][c]) << shift;
    }
    const uint64_t bits = uint64_t(hi) << 32 | idx;
    for (int i = 0; i < 8; i++) out[i] = uint8_t(bits >> (56 - 8 * i));
}
// The 3DS texture pack (tools/3ds_glreplay/picapack.py): the large ETC1 images already halved and
// encoded again, in the PICA's layout, keyed by the CRC-32 of the uploaded ETC1 bytes and the size.
struct PackEntry {
    uint32_t crc, w, h, ow, oh, offset, size;
};
std::vector<PackEntry> g_pack;
FILE *g_pack_file;  // open while the game runs (unbuffered: an image is one request)
// CRC-32 eight bytes at a time ("slicing by 8": the menus' 2 MB atlases took ~75 ms each in Azahar
// a byte at a time)
uint32_t crc32(const uint8_t *p, size_t n) {
    static uint32_t table[8][256];
    if (!table[0][1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; i++)
            for (int k = 1; k < 8; k++) table[k][i] = (table[k - 1][i] >> 8) ^ table[0][table[k - 1][i] & 0xff];
    }
    uint32_t c = 0xffffffffu;
    for (; n >= 8; n -= 8, p += 8) {
        uint32_t a, b;
        std::memcpy(&a, p, 4);
        std::memcpy(&b, p + 4, 4);
        a ^= c;
        c = table[7][a & 0xff] ^ table[6][(a >> 8) & 0xff] ^ table[5][(a >> 16) & 0xff] ^ table[4][a >> 24] ^
            table[3][b & 0xff] ^ table[2][(b >> 8) & 0xff] ^ table[1][(b >> 16) & 0xff] ^ table[0][b >> 24];
    }
    for (; n; n--) c = table[0][(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}
// A large ETC1 level 0 from the pack: false if it is not there.
bool tex_from_pack(Texture &t, GLsizei w, GLsizei h, const void *data, GLsizei size) {
    if (!g_pack_file || std::none_of(g_pack.begin(), g_pack.end(), [&](const PackEntry &e) { return e.w == uint32_t(w) && e.h == uint32_t(h); }))
        return false;
    const uint32_t crc = crc32(static_cast<const uint8_t *>(data), size_t(size));
    for (const PackEntry &e : g_pack) {
        if (e.crc != crc || e.w != uint32_t(w) || e.h != uint32_t(h)) continue;
        const int pw = int(e.ow), ph = int(e.oh);
        if (next_pow2(pw) != pw || next_pow2(ph) != ph) return false;
        if (!t.allocated || t.fmt != GPU_ETC1 || t.pw != pw || t.ph != ph || t.levels != 1) {
            tex_free(t);
            if (!tex_alloc(t, GPU_ETC1, pw, ph, 1)) return false;
        }
        tex_cow(t);
        u32 room;
        void *dst = C3D_Tex2DGetImagePtr(&t.tex, 0, &room);
        if (e.size != room || std::fseek(g_pack_file, long(e.offset), SEEK_SET) != 0 || std::fread(dst, 1, e.size, g_pack_file) != e.size)
            return false;
        t.w = w, t.h = h, t.gl_format = 0x8D64, t.skip = 1, t.su = t.sv = 1;
        t.have = 1;
        tex_params(t);
        tex_written(t.tex);
        g_stats.tex_uploads++;
        log_once("pack" + std::to_string(w) + "x" + std::to_string(h), "pica: %dx%d texture from the pack at %dx%d", w, h, pw, ph);
        return true;
    }
    return false;
}

// Level 0 larger than the PICA takes (the menus' 2048x2048 sprite atlases, without mip levels):
// decoded a block row at a time and box-filtered down, without mip levels. ETC1 images are halved
// to 1024 and encoded again (as small as before, and the interface keeps its detail); others go
// to at most 512 as 16 or 32-bit texels (a 400x240 screen gains nothing from more).
void tex_upload_large(Texture &t, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *data) {
    const bool etc1 = format == 0x8D64;
    const bool alpha = format == 0x1908 || format == 0x80E1 || format == 0x190A || format == 0x1906 || type == 0x8033 || type == 0x8034;
    const int limit = etc1 ? kMaxTex : 512;
    int f = 1;
    while (w / f > limit || h / f > limit) f *= 2;
    const int ow = std::max(1, w / f), oh = std::max(1, h / f);
    const int pw = next_pow2(ow), ph = next_pow2(oh);
    const bool reencode = etc1 && ow == pw && oh == ph;  // (ETC1 storage needs whole 8x8 tiles)
    t.w = w, t.h = h, t.gl_format = format;
    t.skip = 1;  // (no further levels)
    t.su = float(ow) / float(pw), t.sv = float(oh) / float(ph);
    const GPU_TEXCOLOR fmt = reencode ? GPU_ETC1 : alpha ? GPU_RGBA8 : GPU_RGB565;
    if (!t.allocated || t.fmt != fmt || t.pw != pw || t.ph != ph || t.levels != 1) {
        tex_free(t);
        if (!tex_alloc(t, fmt, pw, ph, 1)) return;
    }
    t.have = 0;
    tex_cow(t);
    log_once("big" + std::to_string(w) + "x" + std::to_string(h), "pica: %dx%d texture stored at %dx%d%s", w, h, ow, oh,
             reencode ? " (ETC1 again)" : "");
    const auto *src = static_cast<const uint8_t *>(data);
    std::vector<uint8_t> rows;                  // decoded source rows: one ETC1 block row, or f rows
    std::vector<uint32_t> sum(size_t(ow) * 4);
    std::vector<uint8_t> out(size_t(ow) * 4 * (reencode ? 4 : 1));  // reencoding: four output rows
    std::vector<uint8_t> blocks(reencode ? size_t(ow / 4) * 8 : 0);
    int have_y = -1;                            // first source row in `rows`
    const int band = etc1 ? 4 : f;
    for (int oy = 0; oy < oh; oy++) {
        std::fill(sum.begin(), sum.end(), 0u);
        for (int sy = oy * f; sy < oy * f + f && sy < h; sy++) {
            if (have_y < 0 || sy < have_y || sy >= have_y + band) {
                have_y = sy - sy % band;
                const int n = std::min(band, h - have_y);
                if (etc1) {
                    rows.assign(size_t(w) * 4 * 4, 0);
                    etc1_decode(src + size_t(have_y / 4) * size_t((w + 3) / 4) * 8, w, n, rows.data());
                } else {
                    const int in = type == 0x8363 || type == 0x8033 || type == 0x8034 || format == 0x190A ? 2
                                   : format == 0x1907 ? 3 : format == 0x1906 || format == 0x1909 ? 1 : 4;
                    const size_t row = (size_t(w) * size_t(in) + size_t(g.unpack) - 1) / size_t(g.unpack) * size_t(g.unpack);
                    rows = to_rgba(format, type, src + size_t(have_y) * row, w, n, g.unpack);
                }
            }
            const uint8_t *r = &rows[size_t(sy - have_y) * size_t(w) * 4];
            for (int x = 0; x < ow * f && x < w; x++)
                for (int c = 0; c < 4; c++) sum[size_t(x / f) * 4 + size_t(c)] += r[size_t(x) * 4 + size_t(c)];
        }
        uint8_t *o = &out[reencode ? size_t(oy % 4) * size_t(ow) * 4 : 0];
        for (size_t i = 0; i < sum.size(); i++) o[i] = uint8_t(sum[i] / uint32_t(f * f));
        if (!reencode) {
            tex_store(t, 0, 0, oy, ow, 1, 0x1908, 0x1401, out.data(), 1);
        } else if (oy % 4 == 3) {  // a block row: encoded, stored
            for (int bx = 0; bx < ow / 4; bx++) etc1_encode(&out[size_t(bx) * 16], size_t(ow) * 4, &blocks[size_t(bx) * 8]);
            tex_store(t, 0, 0, oy - 3, ow, 4, 0x8D64, 0, blocks.data(), 1);
        }
    }
    // the padding (non power of two): the last column and row repeated
    if (!reencode && (ow < pw || oh < ph)) {
        uint8_t *dst = static_cast<uint8_t *>(C3D_Tex2DGetImagePtr(&t.tex, 0, nullptr));
        const int b = bpp(t.fmt) / 8;
        for (int y = 0; y < ph; y++)
            for (int x = (y < oh ? ow : 0); x < pw; x++)
                std::memcpy(dst + tiled(uint32_t(x), uint32_t(y), uint32_t(pw)) * uint32_t(b),
                            dst + tiled(uint32_t(std::min(x, ow - 1)), uint32_t(std::min(y, oh - 1)), uint32_t(pw)) * uint32_t(b), size_t(b));
    }
    t.have = 1;
    tex_params(t);
    tex_written(t.tex);
    g_stats.tex_uploads++;
}
// Level 0 without pixels: a render target. In VRAM, at most 512 wide and high (the menus draw
// their 960x640 interface into 1024x1024 textures: 4 MB each, and as much again for depth, at
// full size); draws into it are scaled to match (target_rect).
void tex_target(Texture &t, GLsizei w, GLsizei h, GPU_TEXCOLOR fmt) {
    int shift = 0;
    while ((next_pow2(w) >> shift) > 512 || (next_pow2(h) >> shift) > 512) shift++;
    const int pw = next_pow2(w) >> shift, ph = next_pow2(h) >> shift;
    if (t.target) {
        C3D_RenderTargetDelete(t.target);
        t.target = nullptr;
    }
    if (!t.allocated || t.fmt != fmt || t.pw != pw || t.ph != ph || t.levels != 1 || !t.vram) {
        tex_free(t);
        if (!tex_alloc(t, fmt, pw, ph, 1, true)) return;
        // VRAM full, linear memory: no dirty cache line of its earlier use may land on what the GPU draws
        tex_written(t.tex);
    }
    t.w = w, t.h = h, t.gl_format = 0, t.skip = 0, t.rt_shift = shift;
    t.su = float(w) / float(pw << shift), t.sv = float(h) / float(ph << shift);
    t.have = 1;
    t.render_target = true;
    if (shift) log_once("rtsize" + std::to_string(w) + "x" + std::to_string(h), "pica: %dx%d render target at %dx%d", w, h, pw, ph);
    tex_params(t);
}
void glCompressedTexImage2D(GLenum, GLint level, GLenum format, GLsizei w, GLsizei h, GLint, GLsizei size, const void *data) {
    Texture *t = bound_texture();
    if (!t) return set_error(0x0502);
    if (format != 0x8D64) {
        log_once("cformat" + std::to_string(format), "pica: compressed format %04x not supported", format);
        return set_error(0x0500);
    }
    if (level == 0 && (w > kMaxTex || h > kMaxTex)) {
        if (!tex_from_pack(*t, w, h, data, size)) tex_upload_large(*t, w, h, format, 0, data);
        return;
    }
    const int pl = tex_level(*t, level, w, h, GPU_ETC1, format);
    if (pl >= 0) tex_upload(*t, pl, 0, 0, w, h, format, 0, data, 1);
}
void glCompressedTexSubImage2D(GLenum, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLsizei, const void *data) {
    Texture *t = bound_texture();
    if (!t || !t->allocated || t->fmt != GPU_ETC1 || format != 0x8D64) return set_error(0x0502);
    if (level < 0 || level >= t->levels || (x | y) & 3) return;
    tex_upload(*t, level, x, y, w, h, format, 0, data, 1);
}
void glCopyTexSubImage2D(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei) {
    log_once("copytex", "pica: glCopyTexSubImage2D not supported");
}
void glCullFace(GLenum m) { g.cull_face = m; }
void glDeleteBuffers(GLsizei n, const GLuint *names) {
    for (GLsizei i = 0; i < n; i++) {
        auto it = g_buffers.find(names[i]);
        if (it == g_buffers.end()) continue;
        buf_free(it->second);
        for (AttribState &a : g.attr)
            if (a.buf == &it->second) a.buf = nullptr;
        g_buffers.erase(it);
        if (g.array_buffer == names[i]) g.array_buffer = 0, g_bound_buf[0] = nullptr;
        if (g.element_buffer == names[i]) g.element_buffer = 0, g_bound_buf[1] = nullptr;
    }
}
void glDeleteFramebuffers(GLsizei n, const GLuint *names) {
    for (GLsizei i = 0; i < n; i++) {
        g_framebuffers.erase(names[i]);
        if (g.framebuffer == names[i]) g.framebuffer = 0;
        g_fb_target = nullptr;
    }
}
void glDeleteRenderbuffers(GLsizei n, const GLuint *names) {
    for (GLsizei i = 0; i < n; i++) g_renderbuffers.erase(names[i]);
}
void glDeleteTextures(GLsizei n, const GLuint *names) {
    for (GLsizei i = 0; i < n; i++) {
        auto it = g_textures.find(names[i]);
        if (it == g_textures.end()) continue;
        for (int u = 0; u < 8; u++)
            if (g.tex[u] == names[i]) g.tex[u] = 0, g_texp[u] = nullptr;
        g_fb_target = nullptr;
        for (int u = 0; u < 3; u++) C3D_TexBind(u, &g_blank);  // (rebound at the next draw)
        forget_bindings();
        tex_free(*it->second);
        g_textures.erase(it);
    }
}
void glDepthFunc(GLenum f) { g.depth_func = f; }
void glDepthMask(GLboolean m) { g.depth_mask = m != 0; }
void glDepthRangef(GLfloat n, GLfloat f) { g.depth_near = n, g.depth_far = f; }
bool *cap(GLenum c) {
    switch (c) {
    case 0x0BE2: return &g.blend;
    case 0x0B71: return &g.depth_test;
    case 0x0B44: return &g.cull;
    case 0x0C11: return &g.scissor_test;
    case 0x0B90: return &g.stencil_test;
    case 0x8037: return &g.polygon_offset;
    case 0x0BD0: return &g.dither;
    default: return nullptr;
    }
}
void glDisable(GLenum c) {
    if (bool *b = cap(c)) *b = false;
}
void glEnable(GLenum c) {
    if (bool *b = cap(c)) *b = true;
    else log_once("cap" + std::to_string(c), "pica: glEnable(%04x) ignored", c);
}
void glDisableVertexAttribArray(GLuint i) {
    if (i < 16) g.attr[i].enabled = false;
}
void glEnableVertexAttribArray(GLuint i) {
    if (i < 16) g.attr[i].enabled = true;
}
void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (count <= 0 || first < 0) return;
    draw(mode, count, uint32_t(first), Indices{});
}
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    if (count <= 0) return;
    Indices ix;  // (the GPU reads a rewritten copy: fresh_indices)
    ix.type = type, ix.count = count;
    if (g.element_buffer) {
        const Buffer &ib = *g_bound_buf[1];
        const size_t off = reinterpret_cast<uintptr_t>(indices);
        if (!ib.data || off + size_t(count) * size_t(type == 0x1405 ? 4 : type == 0x1403 ? 2 : 1) > ib.size) return set_error(0x0501);
        ix.cpu = ib.data + off;
    } else {
        ix.cpu = static_cast<const uint8_t *>(indices);
        if (!ix.cpu) return;
    }
    draw(mode, count, 0, ix);
}
void glFlush() {}
Framebuffer *bound_fb() { return g.framebuffer ? &g_framebuffers[g.framebuffer] : nullptr; }
void glFramebufferRenderbuffer(GLenum, GLenum attachment, GLenum, GLuint rb) {
    Framebuffer *f = bound_fb();
    if (!f) return set_error(0x0502);
    g_fb_target = nullptr;
    if (attachment == 0x8CE0) f->color_rb = rb;
    else if (attachment == 0x8D00) f->depth_rb = rb;
    else if (attachment == 0x8D20) f->stencil_rb = rb;
}
void glFramebufferTexture2D(GLenum, GLenum attachment, GLenum, GLuint tex, GLint level) {
    Framebuffer *f = bound_fb();
    if (!f) return set_error(0x0502);
    g_fb_target = nullptr;
    if (attachment == 0x8CE0) f->color_tex = tex;
    else log_once("fbtex" + std::to_string(attachment), "pica: texture attachment %04x not supported", attachment);
    if (level) log_once("fblevel", "pica: rendering to mip levels not supported");
}
void glFrontFace(GLenum m) { g.front_face = m; }
void gen(GLsizei n, GLuint *out) {
    for (GLsizei i = 0; i < n; i++) out[i] = new_name();
}
void glGenBuffers(GLsizei n, GLuint *o) { gen(n, o); }
void glGenFramebuffers(GLsizei n, GLuint *o) { gen(n, o); }
void glGenRenderbuffers(GLsizei n, GLuint *o) { gen(n, o); }
void glGenTextures(GLsizei n, GLuint *o) {
    gen(n, o);
    for (GLsizei i = 0; i < n; i++) g_textures[o[i]] = std::make_unique<Texture>();
}
void glGenerateMipmap(GLenum) {
    Texture *t = bound_texture();
    if (!t || !t->allocated || t->fmt == GPU_ETC1 || t->pw != t->w || t->ph != t->h) return;
    int levels = 1;
    while ((t->pw >> levels) >= 8 && (t->ph >> levels) >= 8) levels++;
    if (levels < 2) return;
    if (t->levels < levels && !tex_alloc(*t, t->fmt, t->pw, t->ph, levels)) return;
    tex_cow(*t);
    // box filter on the CPU (load time; the GPU's display transfer cannot read tiled textures)
    const int b = bpp(t->fmt) / 8;
    for (int l = 1; l < levels; l++) {
        const int w = t->pw >> l, h = t->ph >> l;
        const uint8_t *src = static_cast<const uint8_t *>(C3D_Tex2DGetImagePtr(&t->tex, l - 1, nullptr));
        uint8_t *dst = static_cast<uint8_t *>(C3D_Tex2DGetImagePtr(&t->tex, l, nullptr));
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint8_t *o = dst + tiled(uint32_t(x), uint32_t(y), uint32_t(w)) * uint32_t(b);
                if (b == 2) {  // packed 16-bit: nearest
                    std::memcpy(o, src + tiled(uint32_t(2 * x), uint32_t(2 * y), uint32_t(2 * w)) * 2, 2);
                    continue;
                }
                for (int k = 0; k < b; k++) {
                    int sum = 0;
                    for (int dy = 0; dy < 2; dy++)
                        for (int dx = 0; dx < 2; dx++) sum += src[tiled(uint32_t(2 * x + dx), uint32_t(2 * y + dy), uint32_t(2 * w)) * uint32_t(b) + uint32_t(k)];
                    o[k] = uint8_t((sum + 2) / 4);
                }
            }
        t->have |= 1u << l;
    }
    tex_params(*t);
    tex_written(t->tex);
}
GLenum glGetError() {
    GLenum e = g.error;
    if (!e) e = take_error();
    g.error = 0;
    return e;
}
int int_param(GLenum pname, GLint *out) {
    switch (pname) {
    case 0x0D33: case 0x851C: case 0x84E8: return kMaxTex;   // max texture / cube / renderbuffer size
    case 0x8869: return 12;                                    // MAX_VERTEX_ATTRIBS
    case 0x8872: return 8;                                     // MAX_TEXTURE_IMAGE_UNITS
    case 0x8B4C: return 0;                                     // MAX_VERTEX_TEXTURE_IMAGE_UNITS
    case 0x8B4D: return 8;                                     // MAX_COMBINED_TEXTURE_IMAGE_UNITS
    case 0x8DFB: return 251;                                   // MAX_VERTEX_UNIFORM_VECTORS (an Adreno 200's)
    case 0x8DFD: return 221;                                   // MAX_FRAGMENT_UNIFORM_VECTORS
    case 0x8DFC: return 8;                                     // MAX_VARYING_VECTORS
    case 0x86A2: return 1;                                     // NUM_COMPRESSED_TEXTURE_FORMATS
    case 0x86A3: return 0x8D64;                                // COMPRESSED_TEXTURE_FORMATS: ETC1
    case 0x0D52: case 0x0D53: case 0x0D54: case 0x0D55: return 8;  // red/green/blue/alpha bits
    case 0x0D56: return 24;                                    // DEPTH_BITS
    case 0x0D57: return 8;                                     // STENCIL_BITS
    case 0x0D50: return 4;                                     // SUBPIXEL_BITS
    case 0x0BA2: for (int i = 0; i < 4; i++) out[i] = g.viewport[i]; return g.viewport[0];
    case 0x0C10: for (int i = 0; i < 4; i++) out[i] = g.scissor[i]; return g.scissor[0];
    case 0x0D3A: out[0] = kMaxTex, out[1] = kMaxTex; return kMaxTex;  // MAX_VIEWPORT_DIMS
    case 0x8CA6: return int(g.framebuffer);
    case 0x8CA7: return int(g.renderbuffer);
    case 0x8069: return int(g.tex[g.unit]);
    case 0x8894: return int(g.array_buffer);
    case 0x8895: return int(g.element_buffer);
    case 0x8B8D: return int(current_name());
    case 0x84E0: return 0x84C0 + g.unit;
    case 0x0CF5: return g.unpack;
    case 0x0D05: return g.pack;
    case 0x8B9A: return 0x1908;                                // IMPLEMENTATION_COLOR_READ_FORMAT
    case 0x8B9B: return 0x1401;                                //   ... TYPE
    case 0x80A8: case 0x80A9: return 0;                        // SAMPLE_BUFFERS, SAMPLES
    case 0x8DF9: return 0;                                     // NUM_SHADER_BINARY_FORMATS
    case 0x87FE: return 0;                                     // NUM_PROGRAM_BINARY_FORMATS
    }
    log_once("getint" + std::to_string(pname), "pica: glGetIntegerv(%04x): 0", pname);
    return 0;
}
void glGetIntegerv(GLenum pname, GLint *out) {
    if (!out) return;
    const int v = int_param(pname, out);
    if (pname != 0x0BA2 && pname != 0x0C10 && pname != 0x0D3A) *out = v;
}
void glGetFloatv(GLenum pname, GLfloat *out) {
    if (!out) return;
    if (pname == 0x846E || pname == 0x846D) {  // aliased line width / point size range
        out[0] = 1, out[1] = 1;
        return;
    }
    if (pname == 0x84FF) {  // MAX_TEXTURE_MAX_ANISOTROPY_EXT
        out[0] = 1;
        return;
    }
    if (pname == 0x0C22) {  // COLOR_CLEAR_VALUE
        std::memcpy(out, g.clear_color, 16);
        return;
    }
    GLint v[4] = {};
    v[0] = int_param(pname, v);
    for (int i = 0; i < ((pname == 0x0BA2 || pname == 0x0C10) ? 4 : 1); i++) out[i] = float(v[i]);
}
const char *glGetString(GLenum name) {
    switch (name) {
    case 0x1F00: return "Nintendo";
    case 0x1F01: return "PICA200";
    case 0x1F02: return "OpenGL ES 2.0 citro3d";
    case 0x8B8C: return "OpenGL ES GLSL ES 1.00";
    case 0x1F03:
        // as ANGLE (the PC traces) offers them, with ETC1 (decoded by the PICA); the game only
        // takes a name followed by a space
        return "GL_OES_compressed_ETC1_RGB8_texture GL_OES_depth24 GL_OES_packed_depth_stencil GL_OES_rgb8_rgba8 "
               "GL_OES_texture_npot GL_OES_element_index_uint GL_EXT_texture_format_BGRA8888 GL_OES_mapbuffer ";
    }
    return nullptr;
}
void glLineWidth(GLfloat) {}
void glPixelStorei(GLenum pname, GLint v) {
    if (pname == 0x0CF5) g.unpack = v;
    else if (pname == 0x0D05) g.pack = v;
}
void glPolygonOffset(GLfloat f, GLfloat u) { g.polygon_factor = f, g.polygon_units = u; }
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum, GLenum, void *out) {
    // the screen's last finished frame, RGBA (a screenshot); only of the default framebuffer
    if (!out || g.framebuffer) return;
    read_screen(x, y, w, h, static_cast<uint8_t *>(out));
}
void glRenderbufferStorage(GLenum, GLenum format, GLsizei w, GLsizei h) {
    if (!g.renderbuffer) return set_error(0x0502);
    Renderbuffer &r = g_renderbuffers[g.renderbuffer];
    r.format = format, r.w = w, r.h = h;
}
void glSampleCoverage(GLfloat, GLboolean) {}
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { g.scissor[0] = x, g.scissor[1] = y, g.scissor[2] = w, g.scissor[3] = h; }
void glStencilFunc(GLenum f, GLint ref, GLuint mask) { g.stencil_func = f, g.stencil_ref = ref, g.stencil_mask = mask; }
void glStencilMask(GLuint m) { g.stencil_write = m; }
void glStencilOp(GLenum f, GLenum zf, GLenum zp) { g.stencil_fail = f, g.stencil_zfail = zf, g.stencil_zpass = zp; }
void glTexImage2D(GLenum, GLint level, GLint, GLsizei w, GLsizei h, GLint, GLenum format, GLenum type, const void *data) {
    Texture *t = bound_texture();
    if (!t) return set_error(0x0502);
    GPU_TEXCOLOR fmt = pica_format(format, type);
    if (!data && level == 0) fmt = format == 0x1907 && type == 0x8363 ? GPU_RGB565 : GPU_RGBA8;  // a render target
    if (data && level == 0 && (w > kMaxTex || h > kMaxTex)) return tex_upload_large(*t, w, h, format, type, data);
    if (!data && level == 0) return tex_target(*t, w, h, fmt);
    const int pl = tex_level(*t, level, w, h, fmt, format);
    if (pl < 0) return;
    if (!data) {  // storage only: cleared (level 0 of a render target: see tex_target)
        tex_cow(*t);
        if (!t->vram) {
            std::memset(C3D_Tex2DGetImagePtr(&t->tex, pl, nullptr), 0, C3D_TexCalcLevelSize(t->tex.size, pl));
            tex_written(t->tex);
        }
        t->have |= 1u << pl;
        tex_params(*t);
        return;
    }
    tex_upload(*t, pl, 0, 0, w, h, format, type, data, g.unpack);
}
void tex_param(GLenum pname, GLint v) {
    Texture *t = bound_texture();
    if (!t) return;
    switch (pname) {
    case 0x2801: t->min_filter = GLenum(v); break;
    case 0x2800: t->mag_filter = GLenum(v); break;
    case 0x2802: t->wrap_s = GLenum(v); break;
    case 0x2803: t->wrap_t = GLenum(v); break;
    default: return;
    }
    tex_params(*t);
}
void glTexParameterf(GLenum, GLenum pname, GLfloat v) { tex_param(pname, GLint(v)); }
void glTexParameteri(GLenum, GLenum pname, GLint v) { tex_param(pname, v); }
void glTexSubImage2D(GLenum, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *data) {
    Texture *t = bound_texture();
    if (!t || !t->allocated || t->fmt == GPU_ETC1) return set_error(0x0502);
    if (t->skip || level < 0 || level >= t->levels || !data) return;
    tex_upload(*t, level, x, y, w, h, format, type, data, g.unpack);
}
void glVertexAttrib4f(GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    if (i >= 16) return;
    float *v = g.attr[i].value;
    v[0] = x, v[1] = y, v[2] = z, v[3] = w;
}
void glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *p) {
    if (i >= 16) return;
    AttribState &a = g.attr[i];
    a.size = size, a.type = type, a.norm = norm != 0, a.stride = stride;
    a.pointer = reinterpret_cast<uintptr_t>(p);
    a.buffer = g.array_buffer;
    a.buf = g_bound_buf[0];
}
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { g.viewport[0] = x, g.viewport[1] = y, g.viewport[2] = w, g.viewport[3] = h; }

struct Proc {
    const char *name;
    void *fn;
};
#define P(name) {#name, reinterpret_cast<void *>(&name)}
const Proc kProcs[] = {
    P(glActiveTexture), P(glAttachShader), P(glBindBuffer), P(glBindFramebuffer), P(glBindRenderbuffer), P(glBindTexture),
    P(glBlendColor), P(glBlendEquation), P(glBlendEquationSeparate), P(glBlendFunc), P(glBlendFuncSeparate), P(glBufferData),
    P(glBufferSubData), P(glCheckFramebufferStatus), P(glClear), P(glClearColor), P(glClearDepthf), P(glClearStencil),
    P(glColorMask), P(glCompileShader), P(glCompressedTexImage2D), P(glCompressedTexSubImage2D), P(glCopyTexSubImage2D),
    P(glCreateProgram), P(glCreateShader), P(glCullFace), P(glDeleteBuffers), P(glDeleteFramebuffers), P(glDeleteProgram),
    P(glDeleteRenderbuffers), P(glDeleteShader), P(glDeleteTextures), P(glDepthFunc), P(glDepthMask), P(glDepthRangef),
    P(glDisable), P(glDisableVertexAttribArray), P(glDrawArrays), P(glDrawElements), P(glEnable), P(glEnableVertexAttribArray),
    P(glFlush), P(glFramebufferRenderbuffer), P(glFramebufferTexture2D), P(glFrontFace), P(glGenBuffers), P(glGenFramebuffers),
    P(glGenRenderbuffers), P(glGenTextures), P(glGenerateMipmap), P(glGetActiveAttrib), P(glGetActiveUniform),
    P(glGetAttribLocation), P(glGetError), P(glGetFloatv), P(glGetIntegerv), P(glGetProgramInfoLog), P(glGetProgramiv),
    P(glGetShaderInfoLog), P(glGetShaderSource), P(glGetShaderiv), P(glGetString), P(glGetUniformLocation), P(glLineWidth),
    P(glLinkProgram), P(glPixelStorei), P(glPolygonOffset), P(glReadPixels), P(glRenderbufferStorage), P(glSampleCoverage),
    P(glScissor), P(glShaderSource), P(glStencilFunc), P(glStencilMask), P(glStencilOp), P(glTexImage2D), P(glTexParameterf),
    P(glTexParameteri), P(glTexSubImage2D), P(glUniform1f), P(glUniform1fv), P(glUniform1i), P(glUniform1iv), P(glUniform2fv),
    P(glUniform2iv), P(glUniform3fv), P(glUniform3iv), P(glUniform4fv), P(glUniform4iv), P(glUniformMatrix4fv), P(glUseProgram),
    P(glVertexAttrib4f), P(glVertexAttribPointer), P(glViewport),
};
#undef P

}  // namespace

// ---- the screen ---------------------------------------------------------------------------------
// Waits for the submitted frame's GPU work: citro3d's queue of command lists and transfers.
// (gspWaitForP3D returned at once on an interrupt nobody had waited for, as the replay's frame
// between two traces left: the first frame of the next trace was read while being drawn.)
extern "C" void C3Di_RenderQueueWaitDone(void);
void gpu_idle() {
    if (!g_gpu_pending) return;
    C3Di_RenderQueueWaitDone();
    g_gpu_pending = false;
}
void finish() {
    gpu_idle();
    C3D_Fini();
}
void read_screen(int x, int y, int w, int h, uint8_t *rgba) {
    // the frame's GPU work must be done: the color buffer is 240xW RGBA8, tiled and rotated
    gpu_idle();
    const int sw = g_screen_w;
    const u32 bytes = 240u * u32(sw) * 4;
    const auto *buf = static_cast<const uint8_t *>(g_screen->frameBuf.colorBuf);
    // The GPU copies it from VRAM (the .cia maps VRAM read-only, and the CPU's view of it may be
    // stale on a console) into linear memory, then read with the CPU's caches invalidated. Between
    // frames: citro3d's queue is idle and stopped again as C3D_FrameBegin leaves it.
    static uint8_t *copy;
    static u32 copy_bytes;
    if (copy_bytes < bytes) {
        if (copy) linearFree(copy);
        copy = static_cast<uint8_t *>(linearMemAlign(bytes, 0x80));
        copy_bytes = copy ? bytes : 0;
    }
    if (copy && !g_in_frame) {
        GSPGPU_FlushDataCache(copy, bytes);  // (no dirty line of its own may land on the copy later)
        // lines of 240 pixels (960 bytes, in 16-byte units), no gaps
        GX_TextureCopy(reinterpret_cast<u32 *>(const_cast<uint8_t *>(buf)), GX_BUFFER_DIM(60, 0), reinterpret_cast<u32 *>(copy),
                       GX_BUFFER_DIM(60, 0), bytes, GX_TRANSFER_RAW_COPY(1));
        auto *queue = reinterpret_cast<gxCmdQueue_s *>(__C3D_Context);
        gxCmdQueueRun(queue);
        const bool done = gxCmdQueueWait(queue, 1'000'000'000LL);
        gxCmdQueueStop(queue);
        gxCmdQueueClear(queue);
        GSPGPU_InvalidateDataCache(copy, bytes);
        if (done) buf = copy;
        else log_once("readscreen", "pica: the screen's copy did not finish: read from VRAM");
    }
    if (buf != copy) svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, u32(buf), bytes);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            const int gx = x + i, gy = y + j;  // GL: from the bottom left
            uint8_t *o = rgba + (size_t(j) * size_t(w) + size_t(i)) * 4;
            if (gx < 0 || gy < 0 || gx >= sw || gy >= kScreenH) {
                o[0] = o[1] = o[2] = 0, o[3] = 255;
                continue;
            }
            // buffer x = GL y, buffer y = W - 1 - GL x; buffer rows count from its bottom
            const uint32_t bx = uint32_t(gy), by = uint32_t(sw - 1 - gx);
            const uint8_t *s = buf + tiled(bx, uint32_t(sw - 1) - by, 240) * 4;
            o[0] = s[3], o[1] = s[2], o[2] = s[1], o[3] = s[0];
        }
}

// The screen the game is drawn on (docs/3ds.md, Platform: the menus on the bottom screen): the top
// one (400x240) or the bottom one (320x240). One render target serves both: its buffers are the
// top screen's 240x400 (colour and depth in VRAM), of which a 240x320 frame uses the first 320
// rows (the PICA's buffers are rows of 8x8 tiles 240 wide: a prefix), and its output is linked to
// one screen at a time, so citro3d transfers to that one only and the other keeps its last image.
// Between frames. The frame drawn next is not shown: the game takes the new size at its next
// step, and a menu made for the other size may still be drawn in it.
void set_screen(bool bottom) {
    const int w = bottom ? 320 : kScreenW;
    if (w == g_screen_w) return;
    gpu_idle();  // (and its transfer and buffer swap)
    C3D_RenderTargetSetOutput(nullptr, bottom ? GFX_TOP : GFX_BOTTOM, GFX_LEFT, 0);
    C3D_FrameBufAttrib(&g_screen->frameBuf, 240, u16(w), false);
    C3D_RenderTargetSetOutput(g_screen, bottom ? GFX_BOTTOM : GFX_TOP, GFX_LEFT, kScreenTransfer);
    g_screen_w = w;
    g_drawing_on = nullptr;
    g_hide_frame = true;
    rt::log("pica: drawing on the %s screen (%dx240)", bottom ? "bottom" : "top", w);
}

// Startup cache check (console bring-up): does what the CPU writes reach the memory the GPU reads,
// after each way of flushing it? The CPU writes a 64 KB buffer (twice the ARM11's L1 data cache:
// without the New 3DS's 2 MB L2 some of it is evicted to memory on its own, with it none), then
// the GPU's DMA (a TextureCopy, which flushes nothing itself) copies it and the copy is compared:
// "fresh" is the share of the CPU's new words the GPU saw. One line per method: none, this core's
// L1 cleaned with CP15 (the port's code loader; not the L2), GSP's flush of the buffer, GSP's
// flush of the whole linear heap (what C3D_FrameEnd(0) did every frame until 2026-10-12).
// Azahar keeps no caches: everything is fresh there.
void cache_check() {
    constexpr u32 kBytes = 64u << 10, kWords = kBytes / 4;
    auto *src = static_cast<u32 *>(linearMemAlign(kBytes, 0x1000)), *dst = static_cast<u32 *>(linearMemAlign(kBytes, 0x1000));
    auto *queue = reinterpret_cast<gxCmdQueue_s *>(__C3D_Context);
    struct Method {
        const char *name;
        Result (*apply)(u32 *src);
    };
    const Method methods[] = {
        {"none", [](u32 *) { return Result(0); }},
        {"L1 clean (CP15)", [](u32 *) { return rt::native_emulator() ? Result(-1) : Result(svcBackdoor(rt_native_flush_caches_kernel)); }},
        {"GSP flush of the buffer", [](u32 *s) { return GSPGPU_FlushDataCache(s, kBytes); }},
        {"GSP flush of the linear heap",
         [](u32 *) { return GSPGPU_FlushDataCache(reinterpret_cast<const void *>(__ctru_linear_heap), __ctru_linear_heap_size); }},
    };
    if (!src || !dst) {
        rt::log("pica: cache check: no linear memory");
    } else {
        rt::log("pica: cache check: linear heap %08lx, %lu KB; %lu KB written by the CPU, copied by the GPU", (unsigned long)__ctru_linear_heap,
                (unsigned long)(__ctru_linear_heap_size >> 10), (unsigned long)(kBytes >> 10));
        for (u32 m = 0; m < sizeof methods / sizeof methods[0]; m++) {
            const u32 before = 0x5a000000u | m << 16, after = 0xa5000000u | m << 16;
            for (u32 i = 0; i < kWords; i++) src[i] = before | i, dst[i] = 0;  // memory: the old words, out of the caches
            GSPGPU_FlushDataCache(src, kBytes);
            GSPGPU_FlushDataCache(dst, kBytes);
            for (u32 i = 0; i < kWords; i++) src[i] = after | i;  // the new words: in the caches
            const uint64_t t0 = svcGetSystemTick();
            const Result rc = methods[m].apply(src);
            const double ms = double(svcGetSystemTick() - t0) / CPU_TICKS_PER_MSEC;
            // line width 4 KB, no gaps (16-byte units)
            GX_TextureCopy(src, GX_BUFFER_DIM(256, 0), dst, GX_BUFFER_DIM(256, 0), kBytes, GX_TRANSFER_RAW_COPY(1));
            gxCmdQueueRun(queue);
            const bool done = gxCmdQueueWait(queue, 1'000'000'000LL);
            gxCmdQueueStop(queue);
            gxCmdQueueClear(queue);
            if (!done) {
                rt::log("pica: cache check: %-28s the GPU's copy did not finish", methods[m].name);
                break;
            }
            GSPGPU_InvalidateDataCache(dst, kBytes);
            u32 fresh = 0, stale = 0;
            for (u32 i = 0; i < kWords; i++) fresh += dst[i] == (after | i), stale += dst[i] == (before | i);
            rt::log("pica: cache check: %-28s fresh %3lu%%, stale %3lu%% (call %08lx, %.2f ms)", methods[m].name,
                    (unsigned long)(fresh * 100 / kWords), (unsigned long)(stale * 100 / kWords), (unsigned long)rc, ms);
        }
    }
    if (src) linearFree(src);
    if (dst) linearFree(dst);
}

// Startup draw test (console bring-up): what the console's GPU makes of draws like the game's, which
// Azahar draws right and the console did not (2026-10-12: garbled, stretched, missing). The game's
// own generic.v.pica draws a red quad over the left half of a blue 64x64 target, read back through
// the GPU's DMA, its red and blue pixels counted (2048 each: right). Each variant adds one thing
// the game's draws have, so the first one wrong names it:
//   1 citro3d alone (C3D_FVUnifSet, C3D_DrawArrays), every shader input from a vertex buffer
//   2 + fixed attributes (the inputs a draw has no array for: as the game's)
//   3 + C3D_DrawElements (16-bit indices)
//   4 + the backend's uniforms (set_fvec, send_state) and draw_elements
//   5 + as the loading screen's first draw: positions in pixels of a 400x240 screen (the quad
//       0..200 x 0..240) through a scaling matrix, and alpha blending (source alpha 255)
// Then the backend's caches of the GPU state are cleared: the game's first draw sends it all.
void draw_test() {
    constexpr int kSize = 64;
    C3D_Tex tex;
    if (!C3D_TexInitVRAM(&tex, kSize, kSize, GPU_RGBA8)) return;
    C3D_RenderTarget *rt = C3D_RenderTargetCreateFromTex(&tex, GPU_TEXFACE_2D, 0, C3D_DEPTHTYPE(-1));
    auto *verts = static_cast<uint8_t *>(linearMemAlign(4096, 0x80));
    auto *copy = static_cast<uint8_t *>(linearMemAlign(kSize * kSize * 4, 0x80));
    if (!rt || !verts || !copy) {
        rt::log("pica: draw test: no memory");
        if (rt) C3D_RenderTargetDelete(rt);
        C3D_TexDelete(&tex);
        if (verts) linearFree(verts);
        if (copy) linearFree(copy);
        return;
    }
    // the quad x -1..0, y -1..1 (clip space, z -0.5): 4 vertices (16 bytes: position, RGBA
    // bytes), the same 6 as a list, 6 indices, and zeros for the texture coordinates
    struct V { float x, y, z; uint8_t r, g, b, a; };
    const V quad[4] = {{-1, -1, -0.5f, 255, 0, 0, 255}, {0, -1, -0.5f, 255, 0, 0, 255}, {-1, 1, -0.5f, 255, 0, 0, 255}, {0, 1, -0.5f, 255, 0, 0, 255}};
    auto *v4 = reinterpret_cast<V *>(verts);
    auto *v6 = reinterpret_cast<V *>(verts + 256);
    auto *idx = reinterpret_cast<uint16_t *>(verts + 512);
    auto *zeros = reinterpret_cast<float *>(verts + 1024);
    static const uint16_t kIdx[6] = {0, 1, 2, 1, 3, 2};
    for (int i = 0; i < 4; i++) v4[i] = quad[i];
    for (int i = 0; i < 6; i++) v6[i] = quad[kIdx[i]], idx[i] = kIdx[i];
    std::memset(zeros, 0, 6 * 24);
    GSPGPU_FlushDataCache(verts, 4096);
    VShader &vs = g_vshaders[VS_GENERIC];
    auto *queue = reinterpret_cast<gxCmdQueue_s *>(__C3D_Context);
    for (int variant = 1; variant <= 5; variant++) {
        const bool fixed = variant >= 2, elements = variant >= 3, backend = variant >= 4, pixels = variant >= 5;
        if (pixels) {
            const V px[4] = {{0, 0, 0, 255, 0, 0, 255}, {200, 0, 0, 255, 0, 0, 255}, {0, 240, 0, 255, 0, 0, 255}, {200, 240, 0, 255, 0, 0, 255}};
            for (int i = 0; i < 4; i++) v4[i] = px[i];
            GSPGPU_FlushDataCache(v4, 64);
        }
        C3D_FrameBegin(0);
        C3D_RenderTargetClear(rt, C3D_CLEAR_COLOR, 0x0000FFFF, 0);  // blue (0xRRGGBBAA)
        C3D_FrameDrawOn(rt);
        C3D_BindProgram(&vs.prog);
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, vs.fogzon, false);
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        C3D_CullFace(GPU_CULL_NONE);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
        if (pixels) C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
        else C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        C3D_DepthMap(true, -1.0f, 0.0f);
        C3D_TexEnv env;
        C3D_TexEnvInit(&env);
        C3D_TexEnvSrc(&env, C3D_Both, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(&env, C3D_Both, GPU_REPLACE);
        C3D_SetTexEnv(0, &env);
        for (int i = 1; i < 6; i++) {
            C3D_TexEnvInit(&env);
            C3D_SetTexEnv(i, &env);
        }
        // uniforms: mvp identity, colour = bytes / 255, texture coordinates 0
        const float ident[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
        const float scale[4][4] = {{1 / 200.0f, 0, 0, -1}, {0, 1 / 120.0f, 0, -1}, {0, 0, 1, -0.5f}, {0, 0, 0, 1}};
        const float (&rows)[4][4] = pixels ? scale : ident;
        auto unif = [&](int reg, float x, float y, float z, float w) {
            if (reg < 0) return;
            if (backend) {
                C3D_FVUnif[GPU_VERTEX_SHADER][reg].w = NAN;  // (sent again whatever the earlier variant left)
                set_fvec(reg, x, y, z, w);
            } else {
                C3D_FVUnifSet(GPU_VERTEX_SHADER, reg, x, y, z, w);
            }
        };
        for (int i = 0; i < 4; i++) unif(vs.r[R_MVP] + i, rows[i][0], rows[i][1], rows[i][2], rows[i][3]);
        for (int k = R_TC0A; k <= R_TC2B; k++)
            for (int i = 0; i < 2; i++) unif(vs.r[k] + i, 0, 0, 0, 0);
        unif(vs.r[R_CMUL], 1 / 255.0f, 1 / 255.0f, 1 / 255.0f, 1 / 255.0f);
        unif(vs.r[R_CADD], 0, 0, 0, 0);
        // inputs: v0 position, v1 uv0, v2 uv1, v3 colour, v4 uv2
        C3D_AttrInfo ai;
        AttrInfo_Init(&ai);
        C3D_BufInfo bi;
        BufInfo_Init(&bi);
        const void *pos = elements ? static_cast<const void *>(v4) : static_cast<const void *>(v6);
        if (fixed) {
            AttrInfo_AddLoader(&ai, 0, GPU_FLOAT, 3);
            AttrInfo_AddFixed(&ai, 1);
            AttrInfo_AddFixed(&ai, 2);
            AttrInfo_AddLoader(&ai, 3, GPU_UNSIGNED_BYTE, 4);
            AttrInfo_AddFixed(&ai, 4);
            for (int id : {1, 2, 4}) C3D_FixedAttribSet(id, 0, 0, 0, 1);
            BufInfo_Add(&bi, pos, 16, 2, 0x30);
        } else {
            AttrInfo_AddLoader(&ai, 0, GPU_FLOAT, 3);
            AttrInfo_AddLoader(&ai, 1, GPU_FLOAT, 2);
            AttrInfo_AddLoader(&ai, 2, GPU_FLOAT, 2);
            AttrInfo_AddLoader(&ai, 3, GPU_UNSIGNED_BYTE, 4);
            AttrInfo_AddLoader(&ai, 4, GPU_FLOAT, 2);
            BufInfo_Add(&bi, pos, 16, 2, 0x30);
            BufInfo_Add(&bi, zeros, 24, 3, 0x421);
        }
        C3D_SetAttrInfo(&ai);
        C3D_SetBufInfo(&bi);
        g_buf_info = bi;  // (draw_elements takes the attribute base from it)
        if (backend) draw_elements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, idx);
        else if (elements) C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, idx);
        else C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
        C3D_FrameEnd(GX_CMDLIST_FLUSH);
        if (!gxCmdQueueWait(queue, 1'000'000'000LL)) {
            rt::log("pica: draw test %d: the GPU did not finish in 1 s", variant);
            rt::log_flush();
            break;
        }
        // read back: GPU copy to linear memory (as read_screen), then count
        GSPGPU_FlushDataCache(copy, kSize * kSize * 4);
        GX_TextureCopy(static_cast<u32 *>(tex.data), GX_BUFFER_DIM(kSize * 4 / 16, 0), reinterpret_cast<u32 *>(copy),
                       GX_BUFFER_DIM(kSize * 4 / 16, 0), kSize * kSize * 4, GX_TRANSFER_RAW_COPY(1));
        gxCmdQueueRun(queue);
        const bool copied = gxCmdQueueWait(queue, 1'000'000'000LL);
        gxCmdQueueStop(queue);
        gxCmdQueueClear(queue);
        GSPGPU_InvalidateDataCache(copy, kSize * kSize * 4);
        int red = 0, blue = 0, other = 0, red_left = 0;
        u32 sample = 0;
        for (int y = 0; y < kSize; y++)
            for (int x = 0; x < kSize; x++) {
                const uint8_t *s = copy + tiled(uint32_t(x), uint32_t(y), kSize) * 4;  // A B G R
                const u32 rgba = u32(s[3]) << 24 | u32(s[2]) << 16 | u32(s[1]) << 8 | s[0];
                if (rgba == 0xFF0000FFu) red++, red_left += x < kSize / 2;
                else if (rgba == 0x0000FFFFu) blue++;
                else if (other++ == 0) sample = rgba;
            }
        const bool ok = copied && red == kSize * kSize / 2 && blue == kSize * kSize / 2;
        rt::log("pica: draw test %d (%s%s%s%s%s): %s: red %d (%d in the left half), blue %d, other %d (first other RGBA %08lx)", variant,
                "citro3d", fixed ? " + fixed attributes" : "", elements ? " + elements" : "", backend ? " + the backend's uniforms and draw" : "",
                pixels ? " + pixels, blending" : "", ok ? "right" : "WRONG", red, red_left, blue, other, (unsigned long)sample);
        rt::log_flush();
    }
    // the backend's view of the GPU state: unknown again
    gpu_idle();
    C3D_RenderTargetDelete(rt);
    C3D_TexDelete(&tex);
    linearFree(verts);
    linearFree(copy);
    forget_bindings();
    forget_material();
    g_layout_known = false;
    g_tev_recipe = nullptr;
    for (bool &k : g_tev_known) k = false;
    g_bound_vs = nullptr;
    vs.fogzon_sent = -1;
    std::memset(&g_buf_info, 0, sizeof g_buf_info);
    std::memset(&g_attr_info, 0, sizeof g_attr_info);
}

bool init() {
    glprog::set_hooks({[] { forget_material(); },
                       [](const Program &p, bool matched) {
                           g_stats.programs_linked++;
                           if (matched) return;
                           g_stats.programs_unmatched++;
                           log_once("link:" + p.signature, "pica: no material for a program (%s)", p.signature.c_str());
                       }});
    C3D_Init(0x100000);
    g_screen = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!g_screen) return false;
    C3D_RenderTargetSetOutput(g_screen, GFX_TOP, GFX_LEFT, kScreenTransfer);
    g_ring.base = static_cast<uint8_t *>(linearAlloc(kRingSize));
    g_dvlb = DVLB_ParseFile(const_cast<u32 *>(reinterpret_cast<const u32 *>(pica_shbin)), u32(pica_shbin_end - pica_shbin));
    if (!g_dvlb || !g_ring.base) return false;
    g_vshaders.resize(size_t(g_dvlb->numDVLE));
    for (int i = 0; i < int(g_dvlb->numDVLE); i++) {
        VShader &v = g_vshaders[size_t(i)];
        shaderProgramInit(&v.prog);
        shaderProgramSetVsh(&v.prog, &g_dvlb->DVLE[i]);
        for (int k = 0; k < R_COUNT; k++) v.r[k] = shaderInstanceGetUniformLocation(v.prog.vertexShader, kRegNames[k]);
        v.fogzon = shaderInstanceGetUniformLocation(v.prog.vertexShader, "fogzon");  // (a bool: 0x68 + n)
    }
    if (!C3D_TexInit(&g_blank, 8, 8, GPU_RGBA8)) return false;
    std::memset(g_blank.data, 0xff, 8 * 8 * 4);
    C3D_TexFlush(&g_blank);
    if (!C3D_TexInit(&g_ramp, 8, 8, GPU_RGBA8)) return false;
    for (uint32_t y = 0; y < 8; y++)  // (memory row y is t = 1 - (y + 0.5) / 8)
        for (uint32_t x = 0; x < 8; x++) {
            uint8_t *o = static_cast<uint8_t *>(g_ramp.data) + tiled(x, y, 8) * 4;  // A B G R
            o[0] = 255, o[1] = 0, o[2] = uint8_t((7 - y) * 255 / 7), o[3] = uint8_t(x * 255 / 7);
        }
    C3D_TexSetFilter(&g_ramp, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&g_ramp, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexFlush(&g_ramp);
    // the combiners' default: the first stage's result (materials set what they use)
    for (int i = 0; i < 6; i++) {
        C3D_TexEnv *env = C3D_GetTexEnv(i);
        C3D_TexEnvInit(env);
    }
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    g_direct = c3d_check();
    if (!g_direct) rt::log("pica: citro3d's context is not as expected: its state goes through C3Di_UpdateContext");
    cache_check();
    draw_test();
    aptHook(&g_apt_hook, on_apt, nullptr);
    rt::log("pica: citro3d ready, %d vertex shaders, linear memory free %.1f MB", g_dvlb->numDVLE, linearSpaceFree() / 1048576.0);
    return true;
}

void *proc(const char *name) {
    for (const Proc &p : kProcs)
        if (!std::strcmp(p.name, name)) return p.fn;
    return nullptr;
}

void swap() {
    if (g_frame_checked) g_checked_frames++, g_frame_checked = false;
    g_check_index = 0;
    if (g_in_frame) {
        ring_flush();
        if (g_hide_frame) g_screen->used = false;  // (citro3d transfers only the targets drawn on)
        C3D_FrameEnd(GX_CMDLIST_FLUSH);  // (not 0: the whole linear heap flushed, see Caches above)
        g_in_frame = false;
        g_gpu_pending = true;
    }
    g_hide_frame = false;
    g_stats.frames++;
    g_stats.gpu_ms += C3D_GetDrawingTime();
    g_stats.ring_peak = std::max(g_stats.ring_peak, g_ring.used);
    g_stats.cmdbuf_peak = std::max(g_stats.cmdbuf_peak, C3D_GetCmdBufUsage());
    g_stats.tex_bytes = g_tex_bytes;
    g_stats.buf_bytes = g_buf_bytes;
}

bool read_texture(GLuint name, int &w, int &h, std::vector<uint8_t> &rgba) {
    auto it = g_textures.find(name);
    if (it == g_textures.end() || !it->second->allocated || it->second->fmt != GPU_RGBA8) return false;
    gpu_idle();
    const Texture &t = *it->second;
    w = t.pw, h = t.ph;
    rgba.resize(size_t(w) * size_t(h) * 4);
    const auto *src = static_cast<const uint8_t *>(t.tex.data);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *s = src + tiled(uint32_t(x), uint32_t(y), uint32_t(w)) * 4;
            uint8_t *o = &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
            o[0] = s[3], o[1] = s[2], o[2] = s[1], o[3] = s[0];
        }
    return true;
}
void set_debug(int flags) { g_debug = flags; }
void reset_depth_scale() {
    g_depth_far = 4096, g_depth_far_next = 0;
    g_fog_key[0] = -1;
}
std::vector<GLuint> render_targets() {
    std::vector<GLuint> out;
    for (auto &[name, t] : g_textures)
        if (t->render_target && t->allocated) out.push_back(name);
    return out;
}

void set_texture_pack(const char *path) {
    g_pack.clear();
    if (g_pack_file) std::fclose(g_pack_file);
    g_pack_file = std::fopen(path, "rb");
    if (!g_pack_file) {
        rt::log("pica: no texture pack %s (large images converted at load)", path);
        return;
    }
    std::setvbuf(g_pack_file, nullptr, _IONBF, 0);
    uint32_t head[3] = {};
    if (std::fread(head, 4, 3, g_pack_file) == 3 && head[0] == 0x5450334e /*'N3PT'*/ && head[1] == 1) {
        g_pack.resize(head[2]);
        if (std::fread(g_pack.data(), sizeof(PackEntry), g_pack.size(), g_pack_file) != g_pack.size()) g_pack.clear();
    }
    if (g_pack.empty()) {
        std::fclose(g_pack_file);
        g_pack_file = nullptr;
    }
    rt::log("pica: texture pack %s: %zu images", path, g_pack.size());
}

Stats take_stats() {
    Stats s = g_stats;
    g_stats = Stats{};
    g_stats.tex_peak = g_tex_bytes, g_stats.buf_peak = g_buf_bytes;
    return s;
}

}  // namespace pica

namespace rt {
bool install_pica_gl() {
    if (!pica::init()) {
        log("pica: citro3d initialisation failed");
        return false;
    }
    return gl_load(&pica::proc);
}
}  // namespace rt
