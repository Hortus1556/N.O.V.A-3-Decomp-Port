// GLES2 on the GameCube's Flipper (GX, libogc2): the "driver" under runtime/gles.cpp
// (docs/gamecube.md, Graphics). Like the New 3DS's backend (gles_pica.cpp), every GLSL program
// is recognised at link time and drawn as a material (runtime/gl_program.cpp: classify); the
// differences:
//
//  * Transform: GX takes the projection only in a perspective or an orthographic form, and a
//    3x4 position matrix. The game's MVP (one matrix) is split: position rows R0, R1, -R3 (GX's
//    eye z is GL's -w) and a projection whose z row turns GL's clip depth [-w, w] into GX's
//    [-w, 0] (gx_matrices). Lighting (the irradiance and the two-point-light materials) is done
//    on the CPU per vertex, as the PICA's vertex shaders do it.
//  * Vertices go through the write-gather pipe in direct mode, converted on the way from the
//    game's little-endian arrays (buffer objects' contents are kept in the guest heap, so they
//    page like the rest of the game's memory).
//  * Textures: GX's tiled formats, each with a copy in ARAM (shared with the pager); MEM1's
//    texture pool holds the ones recent frames drew with (least recently used out, never one
//    the current frame uses), filled back from ARAM by DMA when a draw needs them; ETC1 becomes CMPR (DXT1, 4 bits a pixel
//    too; the split alpha textures as well), RGBA RGBA8, RGB RGB565, one channel I8; the
//    interface's images over 1024 halved, the others over 512 (tex_alloc); sides that are not powers of two padded to whole tiles (texture
//    coordinates scaled by the texture matrix). Converted 4x4 blocks at a time: no copies.
//  * Render targets: drawn in the embedded frame buffer (at most 640x528: larger targets at a
//    scale) and copied into their texture when the game binds another framebuffer.
//
// Byte order: data the GL layer hands over (buffers, images, client arrays, indices) is the
// game's (little-endian); small arrays (names, uniforms, queries) arrive in the host's order
// (runtime/gles.cpp: In / Out).
#include "gl_program.h"
#include "gles_gx.h"

#include <gccore.h>
#include <ogc/lwp_heap.h>
extern "C" {
#include "gc_pager.h"
}
#include <malloc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace rt {
void log(const char *fmt, ...);
[[noreturn]] void fatal(const char *fmt, ...);
using GlLoader = void *(*)(const char *name);
bool gl_load(GlLoader loader);
uint32_t gmalloc(uint32_t size);
void gfree(uint32_t p);
}  // namespace rt

namespace gx {
namespace {

using namespace glprog;
using GLbitfield = uint32_t;
using GLintptr = intptr_t;
using GLsizeiptr = intptr_t;

constexpr int kMaxTex = 1024;  // GX's largest texture side
constexpr int kMaxTexWorld = 512;  // the largest side of the world's and models' textures (tex_alloc)

template <class... A> void log_once(const std::string &key, const char *fmt, A... a) {
    static std::set<std::string> seen;
    if (seen.insert(key).second) rt::log(fmt, a...);
}

inline uint32_t le32(const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return __builtin_bswap32(v); }
inline uint16_t le16(const uint8_t *p) { uint16_t v; std::memcpy(&v, p, 2); return __builtin_bswap16(v); }
inline float lef(const uint8_t *p) { const uint32_t v = le32(p); float f; std::memcpy(&f, &v, 4); return f; }

Stats g_stats;

// ---- the GPU's memory: textures (a heap in MEM1) ------------------------------------------------
heap_cntrl g_pool;
uint32_t g_pool_size, g_pool_used;
void *pool_alloc(uint32_t size) {
    void *p = __lwp_heap_allocate(&g_pool, (size + 31) & ~31u);
    if (p) g_pool_used += size;
    return p;
}
void pool_free(void *p, uint32_t size) {
    if (!p) return;
    __lwp_heap_free(&g_pool, p);
    g_pool_used -= size;
}

// ---- objects ------------------------------------------------------------------------------------
struct Texture {
    GXTexObj obj{};
    void *data = nullptr;
    uint32_t bytes = 0;
    int w = 0, h = 0;        // as GL knows it
    int pw = 0, ph = 0;      // as stored (powers of two, at most 1024)
    int shift = 0;           // halvings of a large image
    float us = 1, vs = 1;    // texture coordinates' scale: the image's part of the storage
    float rt_scale = 1;      // render targets: EFB pixels per texel (the EFB is 640x528 at most)
    uint32_t aram = 0;       // its copy in ARAM (0: none: kept in MEM1)
    uint32_t last_used = 0;  // the frame that last drew with it
    uint8_t fmt = GX_TF_RGBA8;
    bool allocated = false, render_target = false;
    GLenum min_filter = 0x2702, mag_filter = 0x2601, wrap_s = 0x2901, wrap_t = 0x2901;  // (GL's defaults)
    uint32_t version = 0;
};
struct Buffer {
    uint32_t guest = 0, size = 0;  // contents in the guest heap
};
struct Renderbuffer {
    GLenum format = 0;
    int w = 0, h = 0;
};
struct Framebuffer {
    GLuint color_tex = 0, depth_rb = 0;
};
std::unordered_map<GLuint, Texture> g_textures;
std::unordered_map<GLuint, Buffer> g_buffers;
uint32_t g_frame_no = 1;  // frames presented (+1)

// MEM1 for a texture: textures with an ARAM copy that the current frame has not drawn with
// leave the pool, least recently used first, until it fits
void *pool_take(uint32_t size, const Texture *self) {
    for (;;) {
        if (void *p = pool_alloc(size)) return p;
        Texture *lru = nullptr;
        for (auto &kv : g_textures) {
            Texture &t = kv.second;
            if (t.data && t.aram && !t.render_target && &t != self && t.last_used < g_frame_no &&
                (!lru || t.last_used < lru->last_used))
                lru = &t;
        }
        if (!lru) return nullptr;
        pool_free(lru->data, lru->bytes);
        lru->data = nullptr;
        g_stats.tex_evictions++;
    }
}
// The texture's MEM1 contents (and its ARAM copy) after an upload
void tex_backup(Texture &t) {
    if (!t.data || t.render_target) return;
    if (!t.aram && !(t.aram = pager_aram_alloc(t.bytes))) {
        log_once("aramfull", "gx: no ARAM for a texture's copy (%ux%u): kept in MEM1", t.w, t.h);
        return;
    }
    pager_aram_copy(1, t.data, t.aram, t.bytes);
}
// In MEM1 for a draw of this frame: false when the pool has no room
bool tex_resident(Texture &t) {
    t.last_used = g_frame_no;
    if (t.data) return true;
    if (!t.aram) return false;
    if (!(t.data = pool_take(t.bytes, &t))) {
        log_once("poolframe", "gx: a frame's textures do not fit the pool (%u KB)", g_pool_size >> 10);
        return false;
    }
    pager_aram_copy(0, t.data, t.aram, t.bytes);
    GX_InitTexObj(&t.obj, t.data, uint16_t(t.pw), uint16_t(t.ph), t.fmt, GX_CLAMP, GX_CLAMP, GX_FALSE);  // (tex_params below)
    g_stats.tex_fills++;
    return true;
}
std::unordered_map<GLuint, Renderbuffer> g_renderbuffers;
std::unordered_map<GLuint, Framebuffer> g_framebuffers;
GLuint g_next_name = 1;

// ---- GL state -----------------------------------------------------------------------------------
struct AttribState {
    bool enabled = false, norm = false;
    int size = 4;
    GLenum type = 0x1406;
    int stride = 0;
    const uint8_t *ptr = nullptr;  // client memory, or an offset into buf
    Buffer *buf = nullptr;
    float value[4] = {0, 0, 0, 1};
};
struct State {
    GLuint array_buffer = 0, element_buffer = 0, framebuffer = 0, renderbuffer = 0;
    int unit = 0;
    GLuint tex[8] = {};
    AttribState attr[16];
    bool blend = false, depth_test = false, cull = false, scissor_test = false;
    GLenum blend_src = 1, blend_dst = 0, depth_func = 0x0201, cull_face = 0x0405, front_face = 0x0901;
    bool depth_mask = true;
    uint8_t color_mask = 0xF;
    float depth_near = 0, depth_far = 1;
    int viewport[4] = {0, 0, 640, 480}, scissor[4] = {0, 0, 640, 480};
    float clear_color[4] = {0, 0, 0, 0}, clear_depth = 1;
    int unpack = 4;
    GLenum error = 0;
} g;

void set_error(GLenum e) {
    if (!g.error) g.error = e;
}
Texture *bound_texture() {
    auto it = g_textures.find(g.tex[g.unit]);
    return it == g_textures.end() ? nullptr : &it->second;
}

// ---- the screen ---------------------------------------------------------------------------------
GXRModeObj *g_mode;
void *g_xfb[2];
int g_xfb_cur;
int g_screen_w = 640, g_screen_h = 480;
void *g_fifo;
bool g_in_frame;
bool g_trace;  // this frame's GL calls go to the log (trace_draw)
bool g_screen_drawn;  // this frame has drawn to the screen

// The current target: the screen, or a texture drawn in the EFB at a scale (copied at unbind).
struct Target {
    Texture *tex = nullptr;  // null: the screen
    int w = 640, h = 480;    // GL's size of the target
    float scale = 1;         // EFB pixels per GL pixel
    bool alpha = false;      // the EFB's format has alpha (RGBA6)
} g_target;

void set_efb_format(bool alpha) {
    if (g_target.alpha == alpha && g_in_frame) return;
    GX_SetPixelFmt(alpha ? GX_PF_RGBA6_Z24 : GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    g_target.alpha = alpha;
}

// The target's image (w x h at the scale, top-left of the EFB) into its texture, in whole tiles.
void copy_target_to_texture() {
    Texture *t = g_target.tex;
    if (!t || !t->data) return;
    const float iw = float(t->w) * g_target.scale, ih = float(t->h) * g_target.scale;
    const int ew = (int(std::ceil(iw)) + 3) & ~3, eh = (int(std::ceil(ih)) + 3) & ~3;
    GX_SetTexCopySrc(0, 0, uint16_t(ew), uint16_t(eh));
    GX_SetTexCopyDst(uint16_t(ew), uint16_t(eh), GX_TF_RGBA8, GX_FALSE);
    GX_CopyTex(t->data, GX_FALSE);
    GX_PixModeSync();
    GX_InvalidateTexAll();
    GX_InitTexObj(&t->obj, t->data, uint16_t(ew), uint16_t(eh), GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjLOD(&t->obj, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
    t->us = iw / float(ew), t->vs = ih / float(eh);
    t->version++;
    if (g_trace) {  // a few texels of the copy (RGBA8 tiles: AR pairs, then GB pairs)
        GX_DrawDone();
        DCInvalidateRange(t->data, t->bytes);
        const uint8_t *d = static_cast<const uint8_t *>(t->data);
        uint32_t alpha = 0, colour = 0;
        for (uint32_t tile = 0; tile < uint32_t(ew / 4) * uint32_t(eh / 4); tile++)
            for (int i = 0; i < 16; i++) {
                const uint8_t *q = d + size_t(tile) * 64;
                alpha += q[2 * i] != 0;
                colour += (q[2 * i + 1] | q[32 + 2 * i] | q[32 + 2 * i + 1]) != 0;
            }
        rt::log("gx copy %dx%d: %u texels with alpha, %u with colour", ew, eh, alpha, colour);
        for (int k = 0; k < 5; k++) {
            const int x = ew * (k + 1) / 6, y = eh * (k + 1) / 6;
            const uint8_t *tile = d + size_t((y / 4) * (ew / 4) + x / 4) * 64;
            const int i = (y % 4) * 4 + x % 4;
            rt::log("gx copy %dx%d texel (%d,%d): r %u g %u b %u a %u", ew, eh, x, y, tile[2 * i + 1], tile[32 + 2 * i],
                    tile[32 + 2 * i + 1], tile[2 * i]);
        }
    }
}

void bind_target(GLuint fb) {
    Texture *tex = nullptr;
    if (fb) {
        auto it = g_framebuffers.find(fb);
        if (it != g_framebuffers.end())
            if (auto t = g_textures.find(it->second.color_tex); t != g_textures.end()) tex = &t->second;
    }
    if (tex == g_target.tex) return;
    copy_target_to_texture();
    g_target.tex = tex;
    if (tex) {
        g_target.w = tex->w, g_target.h = tex->h;
        g_target.scale = tex->rt_scale;
        if (g_trace) {
            uint32_t bits;
            std::memcpy(&bits, &tex->rt_scale, 4);
            rt::log("gx: target %p (fb %u, texture %u) %dx%d scale %.4f (%08x)", static_cast<void *>(tex), fb,
                    g_framebuffers[fb].color_tex, g_target.w, g_target.h, double(g_target.scale), bits);
        }
        set_efb_format(true);
        // the EFB is the screen's too: what was drawn of the frame so far is lost
        if (g_screen_drawn) log_once("rt-mid", "gx: a render target drawn after the screen in a frame (the screen's drawing is lost)");
    } else {
        g_target.w = g_screen_w, g_target.h = g_screen_h, g_target.scale = 1;
        set_efb_format(false);
    }
}

// ---- textures: GX's tiled formats ---------------------------------------------------------------
// The pool's textures, largest first (once: when it is first full)
void log_pool() {
    static bool done;
    if (done) return;
    done = true;
    std::vector<std::pair<uint32_t, GLuint>> v;
    for (auto &[name, t] : g_textures)
        if (t.data) v.push_back({t.bytes, name});
    std::sort(v.rbegin(), v.rend());
    for (auto &[bytes, name] : v) {
        const Texture &t = g_textures[name];
        rt::log("gx pool: texture %u %dx%d stored %dx%d fmt %d%s: %u KB", name, t.w, t.h, t.pw, t.ph, int(t.fmt),
                t.render_target ? " (render target)" : "", bytes >> 10);
    }
}

bool is_pow2(int v) { return v > 0 && (v & (v - 1)) == 0; }
int align_up(int v, int a) { return (v + a - 1) / a * a; }
uint32_t tex_bytes(uint8_t fmt, int w, int h) { return GX_GetTexBufferSize(uint16_t(w), uint16_t(h), fmt, GX_FALSE, 0); }

void tex_release(Texture &t) {
    pool_free(t.data, t.bytes);
    if (t.aram) pager_aram_free(t.aram, t.bytes);
    t.data = nullptr, t.aram = 0, t.bytes = 0, t.allocated = false;
}

// Storage for a w x h image: halved while a side is over 1024; a power-of-two side kept (GX's
// repeat and mirror need them), any other side rounded up to whole tiles (8 texels: every
// format's tile width; GL ES 2 clamps those textures). The image's part of the storage scales
// the texture coordinates (us, vs).
bool tex_alloc(Texture &t, uint8_t fmt, int w, int h) {
    // the largest side kept: the interface's images (screens at their own sizes, the 2048 sprite
    // atlases) 1024, the others (powers of two: the world's, the models') 512, which a 640x480
    // screen shows whole; MEM1 and ARAM hold the rest
    const bool interface = !is_pow2(w) || !is_pow2(h) || std::max(w, h) >= 2048;
    const int max_side = interface ? kMaxTex : kMaxTexWorld;
    int shift = 0;
    while ((std::max(w, 1) >> shift) > max_side || (std::max(h, 1) >> shift) > max_side) shift++;
    const float sw = std::max(1.0f, float(w) / float(1 << shift)), sh = std::max(1.0f, float(h) / float(1 << shift));
    const int iw = int(std::ceil(sw)), ih = int(std::ceil(sh));
    const int pw = is_pow2(iw) && iw >= 8 ? iw : align_up(iw, 8), ph = is_pow2(ih) && ih >= 8 ? ih : align_up(ih, 8);
    if (!(t.allocated && t.fmt == fmt && t.pw == pw && t.ph == ph)) {
        const uint32_t bytes = tex_bytes(fmt, pw, ph);
        tex_release(t);
        t.data = pool_take(bytes, &t);
        if (!t.data) {
            log_once("pool", "gx: the texture pool is full (%u of %u KB; %ux%u)", g_pool_used >> 10, g_pool_size >> 10, w, h);
            log_pool();
            return false;
        }
        t.bytes = bytes, t.fmt = fmt, t.pw = pw, t.ph = ph, t.allocated = true;
        std::memset(t.data, 0, bytes);
    } else if (!t.data) {
        if (!(t.data = pool_take(t.bytes, &t))) return false;
    }
    t.w = w, t.h = h, t.shift = shift, t.render_target = false;
    t.us = sw / float(pw), t.vs = sh / float(ph);
    g_stats.tex_bytes = g_pool_used;
    return true;
}

// GL's wrap mode; GX repeats and mirrors power-of-two sides only (GL ES 2 has the others clamped)
uint8_t gx_wrap(GLenum w, int side) {
    if (!is_pow2(side)) return GX_CLAMP;
    return w == 0x2901 ? GX_REPEAT : w == 0x8370 ? GX_MIRROR : GX_CLAMP;
}
// A render target's storage: the size it is drawn at in the EFB (at most 640x528), in whole tiles.
bool rt_alloc(Texture &t, int w, int h) {
    const float scale = std::min({1.0f, 640.0f / float(std::max(w, 1)), 528.0f / float(std::max(h, 1))});
    const int ew = (int(std::ceil(float(w) * scale)) + 3) & ~3, eh = (int(std::ceil(float(h) * scale)) + 3) & ~3;
    if (!(t.allocated && t.fmt == GX_TF_RGBA8 && t.pw == ew && t.ph == eh)) {
        const uint32_t bytes = tex_bytes(GX_TF_RGBA8, ew, eh);
        tex_release(t);
        t.data = pool_take(bytes, &t);
        if (!t.data) {
            log_once("pool", "gx: the texture pool is full (%u of %u KB; render target %ux%u)", g_pool_used >> 10, g_pool_size >> 10, w, h);
            log_pool();
            return false;
        }
        t.bytes = bytes, t.fmt = GX_TF_RGBA8, t.pw = ew, t.ph = eh, t.allocated = true;
        std::memset(t.data, 0, bytes);
        DCFlushRange(t.data, bytes);
    }
    t.w = w, t.h = h, t.shift = 0, t.render_target = true, t.rt_scale = scale;
    uint32_t bits;
    std::memcpy(&bits, &t.rt_scale, 4);
    rt::log("gx: render target %p %dx%d: stored %dx%d, scale %.4f (%08x)", static_cast<void *>(&t), w, h, ew, eh, double(t.rt_scale), bits);
    t.us = float(w) * scale / float(ew), t.vs = float(h) * scale / float(eh);
    g_stats.tex_bytes = g_pool_used;
    return true;
}

void tex_params(Texture &t) {
    if (!t.data) return;
    GX_InitTexObj(&t.obj, t.data, uint16_t(t.pw), uint16_t(t.ph), t.fmt, gx_wrap(t.wrap_s, t.pw), gx_wrap(t.wrap_t, t.ph), GX_FALSE);
    const bool lin = t.mag_filter != 0x2600;
    GX_InitTexObjLOD(&t.obj, lin ? GX_LINEAR : GX_NEAR, lin ? GX_LINEAR : GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
}

// ---- ETC1 -> RGBA8 (then DXT1) -------------------------------------------------------
const int kEtcMod[8][4] = {{2, 8, -2, -8}, {5, 17, -5, -17}, {9, 29, -9, -29}, {13, 42, -13, -42},
                           {18, 60, -18, -60}, {24, 80, -24, -80}, {33, 106, -33, -106}, {47, 183, -47, -183}};
inline uint8_t clamp8(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }
void etc1_block(const uint8_t *b, uint8_t out[4][4][4]) {  // out[y][x] RGBA
    const uint32_t hi = uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
    const uint32_t lo = uint32_t(b[4]) << 24 | uint32_t(b[5]) << 16 | uint32_t(b[6]) << 8 | b[7];
    int c[2][3];
    if (hi & 2) {  // differential
        for (int k = 0; k < 3; k++) {
            const int base = int(hi >> (27 - 8 * k)) & 31;
            int d = int(hi >> (24 - 8 * k)) & 7;
            if (d >= 4) d -= 8;
            c[0][k] = (base << 3) | (base >> 2);
            const int b2 = base + d;
            c[1][k] = (b2 << 3) | (b2 >> 2);
        }
    } else {
        for (int k = 0; k < 3; k++) {
            const int a = int(hi >> (28 - 8 * k)) & 15, bb = int(hi >> (24 - 8 * k)) & 15;
            c[0][k] = a * 17, c[1][k] = bb * 17;
        }
    }
    const int t0 = int(hi >> 5) & 7, t1 = int(hi >> 2) & 7;
    const bool flip = hi & 1;
    for (int x = 0; x < 4; x++)
        for (int y = 0; y < 4; y++) {
            const int i = x * 4 + y;
            const int sub = flip ? (y >= 2) : (x >= 2);
            const int idx = int((lo >> i) & 1) | int((lo >> (i + 16)) & 1) << 1;
            const int m = kEtcMod[sub ? t1 : t0][idx == 0 ? 0 : idx == 1 ? 1 : idx == 2 ? 2 : 3];
            uint8_t *o = out[y][x];
            o[0] = clamp8(c[sub][0] + m), o[1] = clamp8(c[sub][1] + m), o[2] = clamp8(c[sub][2] + m), o[3] = 255;
        }
}

// ---- RGBA8 -> CMPR (DXT1 in GX's layout) --------------------------------------------------------
// A quick encoder: the block's colour range along its brightest/darkest axis, inset by 1/16, and
// each pixel's nearest of the four colours. GX's CMPR: 8x8 tiles of four 4x4 blocks (TL, TR, BL,
// BR), colours big-endian, two bits a pixel with the first pixel in the high bits.
inline uint16_t to565(int r, int g, int b) { return uint16_t((clamp8(r) >> 3) << 11 | (clamp8(g) >> 2) << 5 | clamp8(b) >> 3); }
void dxt1_block(const uint8_t px[16][4], uint8_t out[8]) {
    int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 3; k++) mn[k] = std::min(mn[k], int(px[i][k])), mx[k] = std::max(mx[k], int(px[i][k]));
    for (int k = 0; k < 3; k++) {
        const int inset = (mx[k] - mn[k]) >> 4;
        mn[k] += inset, mx[k] -= inset;
    }
    uint16_t c0 = to565(mx[0], mx[1], mx[2]), c1 = to565(mn[0], mn[1], mn[2]);
    if (c0 < c1) std::swap(c0, c1);
    uint32_t idx = 0;
    if (c0 != c1) {
        int pal[4][3];
        auto expand = [](uint16_t c, int *o) {
            o[0] = (c >> 11 & 31) * 255 / 31, o[1] = (c >> 5 & 63) * 255 / 63, o[2] = (c & 31) * 255 / 31;
        };
        expand(c0, pal[0]), expand(c1, pal[1]);
        for (int k = 0; k < 3; k++) pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3, pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
        for (int i = 0; i < 16; i++) {
            int best = 0, bd = 1 << 30;
            for (int j = 0; j < 4; j++) {
                const int dr = px[i][0] - pal[j][0], dg = px[i][1] - pal[j][1], db = px[i][2] - pal[j][2];
                const int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
                if (d < bd) bd = d, best = j;
            }
            idx |= uint32_t(best) << (30 - 2 * i);
        }
    }
    out[0] = uint8_t(c0 >> 8), out[1] = uint8_t(c0), out[2] = uint8_t(c1 >> 8), out[3] = uint8_t(c1);
    out[4] = uint8_t(idx >> 24), out[5] = uint8_t(idx >> 16), out[6] = uint8_t(idx >> 8), out[7] = uint8_t(idx);
}
// ---- uploads ----------------------------------------------------------------------------------
// The source of an upload: the image's texel (x, y) as RGBA8 (x < w, y < h).
using Fetch = std::function<void(int x, int y, uint8_t *rgba)>;

// The image into the texture's storage, 4x4 blocks at a time in the format's tiling: every
// stored texel samples the source at its position (halved images: every other texel), the
// texels past the image repeat its last row and column.
void store(Texture &t, const Fetch &src) {
    const int iw = std::max(1, int(std::ceil(float(t.w) / float(1 << t.shift))));
    const int ih = std::max(1, int(std::ceil(float(t.h) / float(1 << t.shift))));
    uint8_t px[16][4];
    auto block = [&](int bx, int by) {
        for (int i = 0; i < 16; i++) {
            const int x = std::min(bx + (i & 3), iw - 1), y = std::min(by + (i >> 2), ih - 1);
            src(std::min(x << t.shift, t.w - 1), std::min(y << t.shift, t.h - 1), px[i]);
        }
    };
    auto *out = static_cast<uint8_t *>(t.data);
    switch (t.fmt) {
    case GX_TF_RGBA8:  // 4x4 tiles: 16 AR pairs, then 16 GB pairs
        for (int ty = 0; ty < t.ph; ty += 4)
            for (int tx = 0; tx < t.pw; tx += 4, out += 64) {
                block(tx, ty);
                for (int k = 0; k < 16; k++) {
                    out[2 * k] = px[k][3], out[2 * k + 1] = px[k][0];
                    out[32 + 2 * k] = px[k][1], out[32 + 2 * k + 1] = px[k][2];
                }
            }
        break;
    case GX_TF_RGB565:  // 4x4 tiles of big-endian 16-bit texels
        for (int ty = 0; ty < t.ph; ty += 4)
            for (int tx = 0; tx < t.pw; tx += 4) {
                block(tx, ty);
                for (int k = 0; k < 16; k++) {
                    const uint16_t v = uint16_t((px[k][0] >> 3) << 11 | (px[k][1] >> 2) << 5 | px[k][2] >> 3);
                    *out++ = uint8_t(v >> 8), *out++ = uint8_t(v);
                }
            }
        break;
    case GX_TF_I8:  // 8x4 tiles of the red channel (luminance, alpha)
        for (int ty = 0; ty < t.ph; ty += 4)
            for (int tx = 0; tx < t.pw; tx += 8, out += 32)
                for (int half = 0; half < 2; half++) {
                    block(tx + 4 * half, ty);
                    for (int k = 0; k < 16; k++) out[(k >> 2) * 8 + half * 4 + (k & 3)] = px[k][0];
                }
        break;
    case GX_TF_IA8:  // 4x4 tiles of (alpha, intensity)
        for (int ty = 0; ty < t.ph; ty += 4)
            for (int tx = 0; tx < t.pw; tx += 4) {
                block(tx, ty);
                for (int k = 0; k < 16; k++) *out++ = px[k][3], *out++ = px[k][0];
            }
        break;
    case GX_TF_CMPR:  // 8x8 tiles of four DXT1 blocks (top left, top right, bottom left, bottom right)
        for (int ty = 0; ty < t.ph; ty += 8)
            for (int tx = 0; tx < t.pw; tx += 8)
                for (int b = 0; b < 4; b++, out += 8) {
                    block(tx + (b & 1) * 4, ty + (b >> 1) * 4);
                    dxt1_block(px, out);
                }
        break;
    }
    DCFlushRange(t.data, t.bytes);
    GX_InvalidateTexAll();
    t.version++;
    tex_backup(t);
}

// GL pixel data (format, type; 16-bit types little-endian, rows padded to `unpack`) as a
// Fetch; false for a format not handled.
bool gl_fetch(GLenum format, GLenum type, const uint8_t *src, int w, int unpack, Fetch &out) {
    int bpp = 2;
    if (type == 0x1401) {
        switch (format) {
        case 0x1908: case 0x80E1: bpp = 4; break;
        case 0x1907: bpp = 3; break;
        case 0x190A: bpp = 2; break;
        case 0x1909: case 0x1906: bpp = 1; break;
        default: return false;
        }
    } else if (type != 0x8363 && type != 0x8033 && type != 0x8034) {
        return false;
    }
    const size_t row = size_t((w * bpp + unpack - 1) / unpack * unpack);
    out = [=](int x, int y, uint8_t *o) {
        const uint8_t *p = src + size_t(y) * row + size_t(x * bpp);
        if (type == 0x1401) {
            switch (format) {
            case 0x1908: o[0] = p[0], o[1] = p[1], o[2] = p[2], o[3] = p[3]; break;  // RGBA
            case 0x80E1: o[0] = p[2], o[1] = p[1], o[2] = p[0], o[3] = p[3]; break;  // BGRA
            case 0x1907: o[0] = p[0], o[1] = p[1], o[2] = p[2], o[3] = 255; break;   // RGB
            case 0x1909: o[0] = o[1] = o[2] = p[0], o[3] = 255; break;               // LUMINANCE
            case 0x1906: o[0] = o[1] = o[2] = 255, o[3] = p[0]; break;               // ALPHA
            default: o[0] = o[1] = o[2] = p[0], o[3] = p[1]; break;                  // LUMINANCE_ALPHA
            }
            return;
        }
        const uint16_t v = le16(p);
        switch (type) {
        case 0x8363:  // 565
            o[0] = uint8_t((v >> 11) * 255 / 31), o[1] = uint8_t((v >> 5 & 63) * 255 / 63), o[2] = uint8_t((v & 31) * 255 / 31);
            o[3] = 255;
            break;
        case 0x8033:  // 4444
            o[0] = uint8_t((v >> 12) * 17), o[1] = uint8_t((v >> 8 & 15) * 17), o[2] = uint8_t((v >> 4 & 15) * 17);
            o[3] = uint8_t((v & 15) * 17);
            break;
        default:  // 5551
            o[0] = uint8_t((v >> 11) * 255 / 31), o[1] = uint8_t((v >> 6 & 31) * 255 / 31), o[2] = uint8_t((v >> 1 & 31) * 255 / 31);
            o[3] = (v & 1) ? 255 : 0;
            break;
        }
    };
    return true;
}

// ETC1 data as a Fetch: the blocks decoded as they are asked for (a few kept: a halved image's
// 4x4 block takes texels of four)
Fetch etc1_fetch(const uint8_t *data, int w) {
    struct Cache {
        int key[4] = {-1, -1, -1, -1};
        uint8_t px[4][4][4][4];
        int next = 0;
    };
    auto cache = std::make_shared<Cache>();
    const int bw = (w + 3) / 4;
    return [=](int x, int y, uint8_t *o) {
        const int key = (y >> 2) * bw + (x >> 2);
        int slot = -1;
        for (int i = 0; i < 4; i++)
            if (cache->key[i] == key) slot = i;
        if (slot < 0) {
            slot = cache->next, cache->next = (cache->next + 1) & 3;
            cache->key[slot] = key;
            etc1_block(data + size_t(key) * 8, cache->px[slot]);
        }
        std::memcpy(o, cache->px[slot][y & 3][x & 3], 4);
    };
}

uint8_t gx_format(GLenum format, GLenum type) {
    if (format == 0x1909 || format == 0x1906) return GX_TF_I8;
    if (format == 0x190A) return GX_TF_IA8;
    if (format == 0x1907 || type == 0x8363) return GX_TF_RGB565;
    return GX_TF_RGBA8;
}

// ---- matrices -----------------------------------------------------------------------------------
// GL's column-major MVP (positions scaled by `scale` first) as GX's projection and position
// matrices. False for a projection neither form can hold (the draw is then skipped).
bool gx_matrices(const float *m, float scale, int size, bool flip_y, Mtx44 proj, Mtx pos) {
    float r[4][4];
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++) r[row][col] = m[col * 4 + row] * (col < 3 || size == 4 ? scale : 1.0f);
    if (flip_y)  // render targets: GL's first row is the image's bottom, the EFB's its top
        for (int col = 0; col < 4; col++) r[1][col] = -r[1][col];
    std::memset(proj, 0, sizeof(Mtx44));
    const bool persp = std::fabs(r[3][0]) + std::fabs(r[3][1]) + std::fabs(r[3][2]) > 1e-6f || std::fabs(r[3][3] - 1) > 1e-6f;
    for (int col = 0; col < 4; col++) pos[0][col] = r[0][col], pos[1][col] = r[1][col];
    proj[0][0] = 1, proj[1][1] = 1;
    if (!persp) {
        // z' = (z_gl - 1) / 2: GX's [-1, 0] from GL's [-1, 1]
        for (int col = 0; col < 4; col++) pos[2][col] = r[2][col];
        proj[2][2] = 0.5f, proj[2][3] = -0.5f, proj[3][3] = 1;
        GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
        return true;
    }
    // R2 = alpha R3 + beta e4 for a projection of the usual form; GX: z = p4 * ez + p5, ez = -R3.v
    int k = 0;
    for (int c = 1; c < 3; c++)
        if (std::fabs(r[3][c]) > std::fabs(r[3][k])) k = c;
    if (std::fabs(r[3][k]) < 1e-12f) return false;
    const float alpha = r[2][k] / r[3][k], beta = r[2][3] - alpha * r[3][3];
    for (int c = 0; c < 3; c++)
        if (std::fabs(r[2][c] - alpha * r[3][c]) > 1e-3f * (std::fabs(r[2][c]) + 1)) return false;
    for (int col = 0; col < 4; col++) pos[2][col] = -r[3][col];
    // GX's clip z = ((alpha R3 + beta) - R3) / 2 = p4 (-R3) + p5
    proj[2][2] = (1 - alpha) / 2, proj[2][3] = beta / 2, proj[3][2] = -1;
    GX_LoadProjectionMtx(proj, GX_PERSPECTIVE);
    return true;
}

// ---- materials ----------------------------------------------------------------------------------
uint8_t g_white_tex[32] __attribute__((aligned(32)));
GXTexObj g_white, g_grey;
uint8_t g_grey_tex[32] __attribute__((aligned(32)));

// Texture coordinates of unit n: the GL attribute's set through the unit's matrix (offset,
// scale and offset, or the game's transform; then the padding's scale).
void unit_matrix(const Program &p, const UnitSpec &s, const Texture *t, int n) {
    float a = 1, b = 0, c = 0, d = 0, e = 1, f = 0;  // s' = a s + b t + c, t' = d s + e t + f
    if (s.offset >= 0) {
        const float *o = p.value(s.offset);
        c = o[0], f = o[1];
    }
    if (s.scaleoffset >= 0) {
        const float *o = p.value(s.scaleoffset);
        a = o[0], e = o[1], c = o[2], f = o[3];
    }
    if (s.transform >= 0) {
        const float *m = p.value(s.transform);  // column-major, on (u, v, 1, 0)
        a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9];
    }
    if (t) a *= t->us, b *= t->us, c *= t->us, d *= t->vs, e *= t->vs, f *= t->vs;  // (the image's part of the storage)
    Mtx mt = {{a, b, 0, c}, {d, e, 0, f}, {0, 0, 1, 0}};
    GX_LoadTexMtxImm(mt, GX_TEXMTX0 + 3 * n, GX_MTX2x4);
}

// The texture a sampler reads, made resident for this frame's draw (null: none, or no room)
const Texture *sampler_texture(const Program &p, int sampler) {
    if (sampler < 0) return nullptr;
    const int unit = int(p.value(sampler)[0]);
    if (unit < 0 || unit >= 8) return nullptr;
    auto it = g_textures.find(g.tex[unit]);
    if (it == g_textures.end() || !it->second.allocated) return nullptr;
    Texture &t = it->second;
    if (!t.data) {
        if (!tex_resident(t)) return nullptr;
        if (!t.render_target) tex_params(t);
    }
    t.last_used = g_frame_no;
    return &t;
}

// The split alpha textures (ETC1: CMPR, whose alpha is 1) give their red as the alpha
constexpr uint8_t kSwapRedAlpha = GX_TEV_SWAP3;

// One stage of the TEV: out = a op mixed (GX's d + ((1 - c) a + c b)).
void tev(int stage, uint8_t coord, uint32_t map, uint8_t chan, uint8_t ca, uint8_t cb, uint8_t cc, uint8_t cd, uint8_t aa,
         uint8_t ab, uint8_t ac, uint8_t ad, uint8_t cscale = GX_CS_SCALE_1, uint8_t tex_swap = GX_TEV_SWAP0) {
    GX_SetTevOrder(uint8_t(GX_TEVSTAGE0 + stage), coord, map, chan);
    GX_SetTevSwapMode(uint8_t(GX_TEVSTAGE0 + stage), GX_TEV_SWAP0, tex_swap);
    GX_SetTevColorIn(uint8_t(GX_TEVSTAGE0 + stage), ca, cb, cc, cd);
    GX_SetTevAlphaIn(uint8_t(GX_TEVSTAGE0 + stage), aa, ab, ac, ad);
    GX_SetTevColorOp(uint8_t(GX_TEVSTAGE0 + stage), GX_TEV_ADD, GX_TB_ZERO, cscale, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaOp(uint8_t(GX_TEVSTAGE0 + stage), GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

struct DrawSetup {
    int uv_attr[3] = {-1, -1, -1};  // the GL attribute feeding each texture coordinate set sent
    int units = 0;
    bool lit = false;               // the colour is lit on the CPU (VS_LIT / VS_POINT2)
    float cmul[4] = {1, 1, 1, 1};   // the colour's factor (material colour, fade)
};

// The material: texture units, coordinates, TEV stages. Returns false for a recipe not done yet.
bool setup_material(const Program &p, const Recipe &r, DrawSetup &d) {
    const Texture *tex[3] = {};
    int units = 0;
    for (int n = 0; n < 3; n++) {
        const UnitSpec &s = r.unit[n];
        if (s.sampler < 0) continue;
        tex[n] = sampler_texture(p, s.sampler);
        GX_LoadTexObj(tex[n] ? &tex[n]->obj : (s.rgb < 1 ? &g_grey : &g_white), uint8_t(GX_TEXMAP0 + n));
        d.uv_attr[n] = r.a_uv[s.uv] >= 0 ? r.a_uv[s.uv] : r.a_uv[0];
        GX_SetTexCoordGen(uint16_t(GX_TEXCOORD0 + n), GX_TG_MTX2x4, uint32_t(GX_TG_TEX0 + n), uint32_t(GX_TEXMTX0 + 3 * n));
        unit_matrix(p, s, tex[n], n);
        units = n + 1;
    }
    d.units = units;
    GX_SetNumTexGens(uint32_t(units));
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    if (r.u_color >= 0) {
        const float *c = p.value(r.u_color);
        if (r.color_mask & 1) d.cmul[0] = c[0], d.cmul[1] = c[1], d.cmul[2] = c[2];
        if (r.color_mask & 2) d.cmul[3] = c[3];
    }
    if (r.u_fade >= 0) {
        const float f = p.value(r.u_fade)[0];
        d.cmul[0] *= f, d.cmul[1] *= f, d.cmul[2] *= f;
    }
    d.lit = r.vs == VS_LIT || r.vs == VS_POINT2;
    // (the lit colour arrives halved: doubled in the first stage that uses it)
    const uint8_t ras_scale = d.lit ? GX_CS_SCALE_2 : GX_CS_SCALE_1;
    int stages = 0;
    switch (r.tev) {
    case TEV_VC:
        tev(0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC, GX_CA_ZERO,
            GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA, ras_scale);
        stages = 1;
        break;
    case TEV_TEXVC:
    case TEV_TINTED:
    case TEV_GLOW:
    case TEV_TEX2_MUL:
    case TEV_TEX2_MIX:
    case TEV_MASK:
    case TEV_TERRAIN:
        // texture x vertex colour (the other recipes come with their own stages: for now the first texture)
        tev(0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO, GX_CA_ZERO, GX_CA_TEXA,
            GX_CA_RASA, GX_CA_ZERO, ras_scale);
        stages = 1;
        if (r.split_alpha && units == 3) {  // alpha = unit 2's red (I8: its intensity) x vertex alpha
            tev(1, GX_TEXCOORD2, GX_TEXMAP2, GX_COLOR0A0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV, GX_CA_ZERO,
                GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO, GX_CS_SCALE_1, kSwapRedAlpha);
            stages = 2;
        }
        if (r.tev != TEV_TEXVC && r.tev != TEV_TINTED && r.tev != TEV_GLOW)
            log_once("tev" + std::to_string(r.tev), "gx: material %d drawn as texture x colour for now", int(r.tev));
        break;
    case TEV_LIGHTMAP:
    case TEV_LIGHTMAP_TEX2:
        // diffuse x vertex colour, then x lightmap x 2
        tev(0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO, GX_CA_ZERO, GX_CA_TEXA,
            GX_CA_RASA, GX_CA_ZERO);
        tev(1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLORNULL, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO, GX_CA_ZERO, GX_CA_ZERO,
            GX_CA_ZERO, GX_CA_APREV, GX_CS_SCALE_2);
        stages = 2;
        if (r.split_alpha && units == 3) {
            tev(2, GX_TEXCOORD2, GX_TEXMAP2, GX_COLORNULL, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV, GX_CA_ZERO,
                GX_CA_TEXA, GX_CA_APREV, GX_CA_ZERO, GX_CS_SCALE_1, kSwapRedAlpha);
            stages = 3;
        }
        break;
    }
    GX_SetNumTevStages(uint8_t(stages));
    return true;
}

// ---- fixed-function state ---------------------------------------------------------------------
uint8_t gx_blend(GLenum f) {
    switch (f) {
    case 0: return GX_BL_ZERO;
    case 1: return GX_BL_ONE;
    case 0x0300: return GX_BL_SRCCLR;
    case 0x0301: return GX_BL_INVSRCCLR;
    case 0x0302: return GX_BL_SRCALPHA;
    case 0x0303: return GX_BL_INVSRCALPHA;
    case 0x0304: return GX_BL_DSTALPHA;
    case 0x0305: return GX_BL_INVDSTALPHA;
    case 0x0306: return GX_BL_DSTCLR;
    case 0x0307: return GX_BL_INVDSTCLR;
    default: return GX_BL_ONE;
    }
}
uint8_t gx_compare(GLenum f) {
    switch (f) {
    case 0x0200: return GX_NEVER;
    case 0x0201: return GX_LESS;
    case 0x0202: return GX_EQUAL;
    case 0x0203: return GX_LEQUAL;
    case 0x0204: return GX_GREATER;
    case 0x0205: return GX_NEQUAL;
    case 0x0206: return GX_GEQUAL;
    default: return GX_ALWAYS;
    }
}

// The viewport and scissor in the EFB (GL's are bottom-up, in the target's size).
void set_viewport() {
    // GL's rectangles count from the bottom: the screen's is the EFB's bottom, a render target's
    // its top (drawn mirrored, so that the texture's first row is GL's)
    const float s = g_target.scale;
    auto top = [](int y, int h) { return g_target.tex ? y : g_target.h - y - h; };
    GX_SetViewport(float(g.viewport[0]) * s, float(top(g.viewport[1], g.viewport[3])) * s, float(g.viewport[2]) * s,
                   float(g.viewport[3]) * s, g.depth_near, g.depth_far);
    int x = 0, y = 0, w = g_target.w, h = g_target.h;
    if (g.scissor_test) x = g.scissor[0], y = top(g.scissor[1], g.scissor[3]), w = g.scissor[2], h = g.scissor[3];
    GX_SetScissor(uint32_t(std::max(0, int(float(x) * s))), uint32_t(std::max(0, int(float(y) * s))),
                  uint32_t(std::max(0, int(float(w) * s))), uint32_t(std::max(0, int(float(h) * s))));
}

void set_fixed(const Recipe *r) {
    set_viewport();
    if (g.blend) {
        GLenum src = g.blend_src;
        if (r && r->unpremultiply && src == 0x0302) src = 1;  // (the composite divides by alpha: blending with ONE is the same)
        GX_SetBlendMode(GX_BM_BLEND, gx_blend(src), gx_blend(g.blend_dst), GX_LO_CLEAR);
    } else {
        GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    }
    GX_SetZMode(g.depth_test ? GX_TRUE : GX_FALSE, gx_compare(g.depth_func), g.depth_test && g.depth_mask ? GX_TRUE : GX_FALSE);
    GX_SetColorUpdate((g.color_mask & 7) ? GX_TRUE : GX_FALSE);
    GX_SetAlphaUpdate((g.color_mask & 8) && g_target.alpha ? GX_TRUE : GX_FALSE);
    // GX culls by its own winding (clockwise is front); render targets mirror y, which turns it
    uint8_t cull = GX_CULL_NONE;
    if (g.cull) {
        bool back = g.cull_face == 0x0405, front = g.cull_face == 0x0404;
        if (g.cull_face == 0x0408) back = front = true;
        const bool ccw_front = (g.front_face == 0x0901) != (g_target.tex != nullptr);
        if (back && front) cull = GX_CULL_ALL;
        else if (back) cull = ccw_front ? GX_CULL_FRONT : GX_CULL_BACK;
        else if (front) cull = ccw_front ? GX_CULL_BACK : GX_CULL_FRONT;
    }
    GX_SetCullMode(cull);
    if (r && r->u_alpharef >= 0) {
        const Program *p = current();
        const float ref = p ? p->value(r->u_alpharef)[0] : 0.5f;
        GX_SetAlphaCompare(GX_GREATER, uint8_t(std::clamp(ref, 0.0f, 1.0f) * 255), GX_AOP_AND, GX_ALWAYS, 0);
        GX_SetZCompLoc(GX_FALSE);
    } else {
        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        GX_SetZCompLoc(GX_TRUE);
    }
}

// ---- vertices -----------------------------------------------------------------------------------
int type_size(GLenum t) { return t == 0x1406 || t == 0x140C ? 4 : t == 0x1402 || t == 0x1403 || t == 0x8D61 ? 2 : 1; }

const uint8_t *attr_base(const AttribState &a) {
    if (!a.enabled) return nullptr;
    if (a.buf) return a.buf->guest ? reinterpret_cast<const uint8_t *>(uintptr_t(a.buf->guest)) + uintptr_t(a.ptr) : nullptr;
    return a.ptr;
}
// One component of attribute a's vertex i, as GL hands it to the shader.
inline float comp(const AttribState &a, const uint8_t *base, uint32_t i, int c) {
    if (!base || c >= a.size) return a.enabled ? (c == 3 ? 1.0f : 0.0f) : a.value[c];
    const int ts = type_size(a.type);
    const uint8_t *p = base + size_t(i) * size_t(a.stride ? a.stride : a.size * ts) + size_t(c * ts);
    switch (a.type) {
    case 0x1406: return lef(p);
    case 0x1401: return a.norm ? float(*p) / 255.0f : float(*p);
    case 0x1400: return a.norm ? std::max(-1.0f, float(int8_t(*p)) / 127.0f) : float(int8_t(*p));
    case 0x1403: return a.norm ? float(le16(p)) / 65535.0f : float(le16(p));
    case 0x1402: return a.norm ? std::max(-1.0f, float(int16_t(le16(p))) / 32767.0f) : float(int16_t(le16(p)));
    case 0x140C: return float(int32_t(le32(p))) / 65536.0f;
    default: return 0;
    }
}

struct Light {  // the CPU's lighting of a lit material: runtime/pica/lit.v.pica and point2.v.pica
    const Recipe *r = nullptr;
    const Program *p = nullptr;
    float nrm[3][3] = {};    // object normal -> world (WorldInverseTransposed, upper 3x3)
    float world[3][4] = {};
    float v4(int u, int i, float d) const { return u >= 0 ? p->value(u)[i] : d; }
    void init(const Program &prog, const Recipe &rec) {
        p = &prog, r = &rec;
        const float *wit = prog.value(rec.u_wit);
        for (int row = 0; row < 3; row++)
            for (int col = 0; col < 3; col++) nrm[row][col] = wit ? wit[col * 4 + row] : float(row == col);
        if (const float *w = prog.value(rec.u_world))
            for (int row = 0; row < 3; row++)
                for (int col = 0; col < 4; col++) world[row][col] = w[col * 4 + row];
    }
    // rgb: the lighting, halved (the TEV doubles it); alpha: the vertex alpha (in rgba[3])
    void color(const float pos[3], const float no[3], float rgba[4]) const {
        float n[3], out[4] = {0, 0, 0, rgba[3]};
        for (int k = 0; k < 3; k++) n[k] = nrm[k][0] * no[0] + nrm[k][1] * no[1] + nrm[k][2] * no[2];
        if (r->vs == VS_LIT) {
            const float n1[4] = {n[0], n[1], n[2], 1}, n2[4] = {n[0] * n[1], n[1] * n[2], n[2] * n[2], n[2] * n[0]};
            const float x2y2 = n[0] * n[0] - n[1] * n[1];
            for (int c = 0; c < 3; c++) {
                float s = 0;
                for (int i = 0; i < 4; i++) s += v4(r->u_ird[c], i, 0) * n1[i] + v4(r->u_ird[3 + c], i, 0) * n2[i];
                out[c] = 2 * (s + v4(r->u_ird[6], c, 0) * x2y2);
            }
            // point light in object space
            float l[3] = {v4(r->u_lpos, 0, 0) - pos[0], v4(r->u_lpos, 1, 0) - pos[1], v4(r->u_lpos, 2, 0) - pos[2]};
            const float d = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
            if (d > 1e-12f) {
                const float ndl = std::clamp((l[0] * no[0] + l[1] * no[1] + l[2] * no[2]) / d, 0.0f, 1.0f);
                const float k = ndl * std::max(0.0f, 1 - v4(r->u_latt, 1, 1) * d);
                for (int c = 0; c < 4; c++) out[c] += v4(r->u_lcol, c, 0) * k;
            }
        } else {  // VS_POINT2: two point lights in world space and an ambient term
            float wp[3];
            for (int k = 0; k < 3; k++) wp[k] = world[k][0] * pos[0] + world[k][1] * pos[1] + world[k][2] * pos[2] + world[k][3];
            auto light = [&](int up, int uc, int ui) {
                float l[3] = {v4(up, 0, 0) - wp[0], v4(up, 1, 0) - wp[1], v4(up, 2, 0) - wp[2]};
                const float d = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
                if (d < 1e-12f) return;
                const float k = std::clamp((l[0] * n[0] + l[1] * n[1] + l[2] * n[2]) / d, 0.0f, 1.0f) * v4(ui, 0, 1);
                for (int c = 0; c < 3; c++) out[c] += v4(uc, c, 0) * k;
            };
            light(r->u_l1p, r->u_l1c, r->u_l1i);
            light(r->u_l2p, r->u_l2c, r->u_l2i);
            for (int c = 0; c < 3; c++) out[c] += v4(r->u_amb, c, 0);
        }
        rgba[0] = out[0] * 0.5f, rgba[1] = out[1] * 0.5f, rgba[2] = out[2] * 0.5f, rgba[3] = out[3];
    }
};

uint8_t gx_prim(GLenum mode) {
    switch (mode) {
    case 0: return GX_POINTS;
    case 1: return GX_LINES;
    case 3: return GX_LINESTRIP;
    case 5: return GX_TRIANGLESTRIP;
    case 6: return GX_TRIANGLEFAN;
    default: return GX_TRIANGLES;
    }
}

struct Indices {
    const uint8_t *data = nullptr;  // null: first .. first + count
    int size = 0;
    uint32_t first = 0;
    uint32_t at(uint32_t i) const {
        if (!data) return first + i;
        return size == 1 ? data[i] : size == 2 ? le16(data + 2 * i) : le32(data + 4 * i);
    }
};

void flush_pending_clear();

// The draws of one frame in 1000, in the log (bring-up): material, textures, the first vertex
// as GL would place it (clip coordinates from the MVP) and its texture coordinates.
void trace_draw(const Program &p, const Recipe &r, const DrawSetup &d, GLenum mode, GLsizei count, const Indices &ix,
                const Mtx44 &, const Mtx &) {
    const AttribState &a = g.attr[r.a_vertex];
    const uint8_t *b = attr_base(a);
    const uint32_t i = ix.at(0);
    const float v[4] = {comp(a, b, i, 0), comp(a, b, i, 1), a.size > 2 ? comp(a, b, i, 2) : 0.0f, 1};
    const float *m = p.value(r.u_mvp);
    float c[4];
    for (int row = 0; row < 4; row++) c[row] = m[row] * v[0] + m[4 + row] * v[1] + m[8 + row] * v[2] + m[12 + row] * v[3];
    char tex[96] = "";
    for (int n = 0; n < d.units; n++) {
        const Texture *t = sampler_texture(p, r.unit[n].sampler);
        const AttribState &u = g.attr[std::max(0, d.uv_attr[n])];
        const float uv[2] = {d.uv_attr[n] >= 0 ? comp(u, attr_base(u), i, 0) : -1, d.uv_attr[n] >= 0 ? comp(u, attr_base(u), i, 1) : -1};
        std::snprintf(tex + std::strlen(tex), sizeof tex - std::strlen(tex), " t%d=%dx%d(%s) uv(%.3f,%.3f)", n, t ? t->w : 0,
                      t ? t->h : 0, t ? (t->data ? "ok" : "none") : "-", double(uv[0]), double(uv[1]));
    }
    rt::log("gx draw: prog %u vs %d tev %d %s mode %u n %d v(%.1f,%.1f,%.1f) clip(%.2f,%.2f,%.2f,%.2f) blend %d%s%s",
            current_name(), int(r.vs), int(r.tev), g_target.tex ? "rt" : "screen", mode, count, double(v[0]), double(v[1]),
            double(v[2]), double(c[0]), double(c[1]), double(c[2]), double(c[3]), g.blend ? 1 : 0, tex,
            a.buf ? " vbo" : "");
    rt::log("gx   state: depth %d func %x mask %d, cull %d face %x front %x, colour mask %x, blend %x %x, scissor %d (%d %d %d %d), "
            "viewport %d %d %d %d, efb alpha %d",
            g.depth_test, g.depth_func, g.depth_mask, g.cull, g.cull_face, g.front_face, g.color_mask, g.blend_src, g.blend_dst,
            g.scissor_test, g.scissor[0], g.scissor[1], g.scissor[2], g.scissor[3], g.viewport[0], g.viewport[1],
            g.viewport[2], g.viewport[3], g_target.alpha);
    if (count <= 6 && d.units > 0) {  // small draws (quads): every vertex, in NDC, with unit 0's coordinates
        const AttribState &u = g.attr[std::max(0, d.uv_attr[0])];
        for (int k = 0; k < count; k++) {
            const uint32_t j = ix.at(uint32_t(k));
            const float q[4] = {comp(a, b, j, 0), comp(a, b, j, 1), a.size > 2 ? comp(a, b, j, 2) : 0.0f, 1};
            float e[4];
            for (int row = 0; row < 4; row++) e[row] = m[row] * q[0] + m[4 + row] * q[1] + m[8 + row] * q[2] + m[12 + row] * q[3];
            rt::log("gx   v%d ndc(%.3f,%.3f) uv(%.3f,%.3f)", k, double(e[0] / e[3]), double(e[1] / e[3]),
                    double(comp(u, attr_base(u), j, 0)), double(comp(u, attr_base(u), j, 1)));
        }
        if (const Texture *t = sampler_texture(p, r.unit[0].sampler))
            rt::log("gx   t0: %dx%d stored %dx%d us %.3f vs %.3f%s scale %.3f", t->w, t->h, t->pw, t->ph, double(t->us),
                    double(t->vs), t->render_target ? " render target" : "", double(t->rt_scale));
    }
}

void draw(GLenum mode, GLsizei count, const Indices &ix) {
    g_stats.draws_requested++;
    Program *p = current();
    if (!p || !p->linked || count <= 0) return;
    if (!p->recipe) {
        if (!p->reported && !p->warmup) {
            p->reported = true;
            rt::log("gx: no material for program %u (%s): not drawn", current_name(), p->signature.c_str());
        }
        return;
    }
    const Recipe &r = *p->recipe;
    if (!g_target.tex) {
        flush_pending_clear();
        g_screen_drawn = true;
    }
    const AttribState &apos = g.attr[r.a_vertex];
    Mtx44 proj;
    Mtx pos;
    if (!gx_matrices(p->value(r.u_mvp), 1.0f, apos.size, g_target.tex != nullptr, proj, pos)) {
        log_once("proj", "gx: a projection GX cannot hold: draws skipped");
        return;
    }
    GX_LoadPosMtxImm(pos, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
    DrawSetup d;
    if (!setup_material(*p, r, d)) return;
    if (g_trace) trace_draw(*p, r, d, mode, count, ix, proj, pos);
    set_fixed(&r);
    g_in_frame = true;

    // vertex format: position, colour, up to three texture coordinates, all direct
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    for (int n = 0; n < d.units; n++) {
        GX_SetVtxDesc(uint8_t(GX_VA_TEX0 + n), GX_DIRECT);
        GX_SetVtxAttrFmt(GX_VTXFMT0, uint32_t(GX_VA_TEX0 + n), GX_TEX_ST, GX_F32, 0);
    }
    const uint8_t *bpos = attr_base(apos);
    const AttribState *acol = r.a_color >= 0 ? &g.attr[r.a_color] : nullptr;
    const uint8_t *bcol = acol ? attr_base(*acol) : nullptr;
    const AttribState *anrm = r.a_normal >= 0 ? &g.attr[r.a_normal] : nullptr;
    const uint8_t *bnrm = anrm ? attr_base(*anrm) : nullptr;
    const uint8_t *buv[3] = {};
    for (int n = 0; n < d.units; n++) buv[n] = d.uv_attr[n] >= 0 ? attr_base(g.attr[d.uv_attr[n]]) : nullptr;
    Light light;
    if (d.lit) light.init(*p, r);

    const uint8_t prim = gx_prim(mode);
    // GX takes at most 65535 vertices per GX_Begin: triangle lists are split on a triangle's end
    const uint32_t per = prim == GX_TRIANGLES ? 65535 / 3 * 3 : 65535;
    for (uint32_t start = 0; start < uint32_t(count); start += per) {
        const uint32_t n = std::min(per, uint32_t(count) - start);
        GX_Begin(prim, GX_VTXFMT0, uint16_t(n));
        for (uint32_t k = 0; k < n; k++) {
            const uint32_t i = ix.at(start + k);
            float xyz[3] = {comp(apos, bpos, i, 0), comp(apos, bpos, i, 1), apos.size > 2 ? comp(apos, bpos, i, 2) : 0.0f};
            float rgba[4] = {1, 1, 1, 1};
            if (acol)
                for (int c = 0; c < 4; c++) rgba[c] = comp(*acol, bcol, i, c);
            if (!d.lit)
                for (int c = 0; c < 4; c++) rgba[c] *= d.cmul[c];
            else {
                float nn[3] = {0, 0, 1};
                if (anrm)
                    for (int c = 0; c < 3; c++) nn[c] = comp(*anrm, bnrm, i, c);
                light.color(xyz, nn, rgba);
            }
            GX_Position3f32(xyz[0], xyz[1], xyz[2]);
            GX_Color4u8(clamp8(int(rgba[0] * 255 + 0.5f)), clamp8(int(rgba[1] * 255 + 0.5f)), clamp8(int(rgba[2] * 255 + 0.5f)),
                        clamp8(int(rgba[3] * 255 + 0.5f)));
            for (int u = 0; u < d.units; u++) {
                const AttribState &a = g.attr[std::max(0, d.uv_attr[u])];
                GX_TexCoord2f32(d.uv_attr[u] >= 0 ? comp(a, buv[u], i, 0) : 0.0f, d.uv_attr[u] >= 0 ? comp(a, buv[u], i, 1) : 0.0f);
            }
        }
        GX_End();
    }
    g_stats.draws++;
    g_stats.vertices += uint32_t(count);
    if (!g_target.tex) g_screen_drawn = true;
}

// A clear: a quad over the viewport's scissored area with the clear colour and the far depth.
void clear(GLbitfield mask) {
    g_in_frame = true;
    const bool color = mask & 0x4000, depth = mask & 0x100;
    if (!color && !depth) return;
    set_viewport();
    GX_SetViewport(0, 0, float(g_target.w) * g_target.scale, float(g_target.h) * g_target.scale, 0, 1);
    Mtx44 proj = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    Mtx id = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    GX_LoadPosMtxImm(id, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
    GX_SetNumTexGens(0);
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    tev(0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC, GX_CA_ZERO, GX_CA_ZERO,
        GX_CA_ZERO, GX_CA_RASA);
    GX_SetNumTevStages(1);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZMode(depth ? GX_TRUE : GX_FALSE, GX_ALWAYS, depth ? GX_TRUE : GX_FALSE);
    GX_SetColorUpdate(color && (g.color_mask & 7) ? GX_TRUE : GX_FALSE);
    GX_SetAlphaUpdate(color && (g.color_mask & 8) && g_target.alpha ? GX_TRUE : GX_FALSE);
    GX_SetCullMode(GX_CULL_NONE);
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    const uint8_t c[4] = {clamp8(int(g.clear_color[0] * 255)), clamp8(int(g.clear_color[1] * 255)), clamp8(int(g.clear_color[2] * 255)),
                          clamp8(int(g.clear_color[3] * 255))};
    // GX's orthographic clip depth runs from -1 (near) to 0 (far) and clips at both: the clear
    // depth (GL's 0..1) just inside
    const float z = std::min(std::clamp(g.clear_depth, 0.0f, 1.0f) - 1.0f, -1.0f / float(1 << 20));
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(-1, 1, z), GX_Color4u8(c[0], c[1], c[2], c[3]);
    GX_Position3f32(1, 1, z), GX_Color4u8(c[0], c[1], c[2], c[3]);
    GX_Position3f32(1, -1, z), GX_Color4u8(c[0], c[1], c[2], c[3]);
    GX_Position3f32(-1, -1, z), GX_Color4u8(c[0], c[1], c[2], c[3]);
    GX_End();
    g_stats.clears++;
}

// ---- the GL entry points ------------------------------------------------------------------------
GLuint new_name() { return g_next_name++; }

void glActiveTexture(GLenum t) { g.unit = int(t - 0x84C0) & 7; }
void glBindBuffer(GLenum t, GLuint b) {
    if (b) g_buffers[b];  // (created at first bind)
    (t == 0x8893 ? g.element_buffer : g.array_buffer) = b;
}
void glBindFramebuffer(GLenum, GLuint f) {
    if (g_trace) rt::log("gx bind framebuffer %u", f);
    g.framebuffer = f;
    bind_target(f);
}
void glBindRenderbuffer(GLenum, GLuint r) { g.renderbuffer = r; }
void glBindTexture(GLenum, GLuint t) {
    if (t) g_textures[t];
    g.tex[g.unit] = t;
}
void glBlendColor(GLfloat, GLfloat, GLfloat, GLfloat) {}
void glBlendEquation(GLenum) {}
void glBlendEquationSeparate(GLenum, GLenum) {}
void glBlendFunc(GLenum s, GLenum dd) { g.blend_src = s, g.blend_dst = dd; }
void glBlendFuncSeparate(GLenum sc, GLenum dc, GLenum, GLenum) { g.blend_src = sc, g.blend_dst = dc; }
void glBufferData(GLenum t, GLsizeiptr size, const void *data, GLenum) {
    const GLuint name = t == 0x8893 ? g.element_buffer : g.array_buffer;
    if (!name) return set_error(0x0502);
    Buffer &b = g_buffers[name];
    if (b.size != uint32_t(size)) {
        if (b.guest) rt::gfree(b.guest);
        b.guest = size > 0 ? rt::gmalloc(uint32_t(size)) : 0;
        b.size = uint32_t(size);
    }
    if (data && b.guest) std::memcpy(reinterpret_cast<void *>(uintptr_t(b.guest)), data, size_t(size));
}
void glBufferSubData(GLenum t, GLintptr off, GLsizeiptr size, const void *data) {
    const GLuint name = t == 0x8893 ? g.element_buffer : g.array_buffer;
    auto it = g_buffers.find(name);
    if (it == g_buffers.end() || !it->second.guest || uint32_t(off + size) > it->second.size) return set_error(0x0501);
    std::memcpy(reinterpret_cast<void *>(uintptr_t(it->second.guest + uint32_t(off))), data, size_t(size));
}
GLenum glCheckFramebufferStatus(GLenum) { return 0x8CD5; }  // FRAMEBUFFER_COMPLETE
// A clear of the screen waits for the frame's first draw to it (or the swap): render targets
// share the EFB, and the game clears the screen before it draws its render targets.
struct PendingClear {
    GLbitfield mask = 0;
    float color[4], depth;
    bool scissor_test;
    int scissor[4];
    uint8_t color_mask;
} g_pending;
void flush_pending_clear() {
    if (!g_pending.mask || g_target.tex) return;
    const State saved = g;
    std::memcpy(g.clear_color, g_pending.color, sizeof g.clear_color);
    g.clear_depth = g_pending.depth, g.scissor_test = g_pending.scissor_test, g.color_mask = g_pending.color_mask;
    std::memcpy(g.scissor, g_pending.scissor, sizeof g.scissor);
    const GLbitfield mask = g_pending.mask;
    g_pending.mask = 0;
    clear(mask);
    g = saved;
}

void glClear(GLbitfield mask) {
    if (g_trace)
        rt::log("gx clear %x color (%.2f %.2f %.2f %.2f) viewport %d %d %d %d scissor %d", mask, double(g.clear_color[0]),
                double(g.clear_color[1]), double(g.clear_color[2]), double(g.clear_color[3]), g.viewport[0], g.viewport[1],
                g.viewport[2], g.viewport[3], g.scissor_test ? 1 : 0);
    if (!g_target.tex && !g_screen_drawn) {
        if (g_pending.mask && g_pending.scissor_test != g.scissor_test) flush_pending_clear();
        g_pending.mask |= mask & (0x4000 | 0x100);
        std::memcpy(g_pending.color, g.clear_color, sizeof g_pending.color);
        g_pending.depth = g.clear_depth, g_pending.scissor_test = g.scissor_test, g_pending.color_mask = g.color_mask;
        std::memcpy(g_pending.scissor, g.scissor, sizeof g_pending.scissor);
        return;
    }
    clear(mask);
}
void glClearColor(GLfloat r, GLfloat gr, GLfloat b, GLfloat a) {
    g.clear_color[0] = r, g.clear_color[1] = gr, g.clear_color[2] = b, g.clear_color[3] = a;
}
void glClearDepthf(GLfloat d) { g.clear_depth = d; }
void glClearStencil(GLint) {}
void glColorMask(GLboolean r, GLboolean gr, GLboolean b, GLboolean a) {
    g.color_mask = uint8_t((r ? 1 : 0) | (gr ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0));
}
void glCompressedTexImage2D(GLenum, GLint level, GLenum format, GLsizei w, GLsizei h, GLint, GLsizei, const void *data) {
    Texture *t = bound_texture();
    if (!t) return set_error(0x0502);
    if (level != 0) return;  // (mipmaps: not yet)
    if (format != 0x8D64) {
        log_once("cfmt" + std::to_string(format), "gx: compressed format %04x not supported", format);
        return;
    }
    // ETC1 (4 bits a texel) to CMPR (the same): the split alpha textures too, whose red the
    // combiners take as the alpha (kSwapRedAlpha)
    if (!tex_alloc(*t, GX_TF_CMPR, w, h)) return;
    tex_params(*t);
    store(*t, etc1_fetch(static_cast<const uint8_t *>(data), w));
    g_stats.tex_uploads++;
}
void glCompressedTexSubImage2D(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *) {
    log_once("ctexsub", "gx: glCompressedTexSubImage2D not supported");
}
void glCopyTexSubImage2D(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei) {
    log_once("copytex", "gx: glCopyTexSubImage2D not supported");
}
void glCullFace(GLenum m) { g.cull_face = m; }
void glDeleteBuffers(GLsizei n, const GLuint *names) {
    for (GLsizei i = 0; i < n; i++) {
        auto it = g_buffers.find(names[i]);
        if (it == g_buffers.end()) continue;
        if (it->second.guest) rt::gfree(it->second.guest);
        for (AttribState &a : g.attr)
            if (a.buf == &it->second) a.buf = nullptr;
        if (g.array_buffer == names[i]) g.array_buffer = 0;
        if (g.element_buffer == names[i]) g.element_buffer = 0;
        g_buffers.erase(it);
    }
}
void glDeleteFramebuffers(GLsizei n, const GLuint *names) {
    for (GLsizei i = 0; i < n; i++) {
        if (g.framebuffer == names[i]) glBindFramebuffer(0, 0);
        g_framebuffers.erase(names[i]);
    }
}
void glDeleteRenderbuffers(GLsizei n, const GLuint *names) {
    for (GLsizei i = 0; i < n; i++) g_renderbuffers.erase(names[i]);
}
void glDeleteTextures(GLsizei n, const GLuint *names) {
    for (GLsizei i = 0; i < n; i++) {
        auto it = g_textures.find(names[i]);
        if (it == g_textures.end()) continue;
        if (g_target.tex == &it->second) bind_target(0);
        tex_release(it->second);
        g_textures.erase(it);
        for (GLuint &u : g.tex)
            if (u == names[i]) u = 0;
    }
    g_stats.tex_bytes = g_pool_used;
}
void glDepthFunc(GLenum f) { g.depth_func = f; }
void glDepthMask(GLboolean m) { g.depth_mask = m != 0; }
void glDepthRangef(GLfloat n, GLfloat f) { g.depth_near = n, g.depth_far = f; }
void cap(GLenum c, bool on) {
    switch (c) {
    case 0x0BE2: g.blend = on; break;
    case 0x0B71: g.depth_test = on; break;
    case 0x0B44: g.cull = on; break;
    case 0x0C11: g.scissor_test = on; break;
    default: break;
    }
}
void glDisable(GLenum c) { cap(c, false); }
void glEnable(GLenum c) { cap(c, true); }
void glDisableVertexAttribArray(GLuint i) {
    if (i < 16) g.attr[i].enabled = false;
}
void glEnableVertexAttribArray(GLuint i) {
    if (i < 16) g.attr[i].enabled = true;
}
void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    Indices ix;
    ix.first = uint32_t(first);
    draw(mode, count, ix);
}
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    Indices ix;
    ix.size = type == 0x1405 ? 4 : type == 0x1403 ? 2 : 1;
    if (g.element_buffer) {
        auto it = g_buffers.find(g.element_buffer);
        if (it == g_buffers.end() || !it->second.guest) return;
        ix.data = reinterpret_cast<const uint8_t *>(uintptr_t(it->second.guest)) + uintptr_t(indices);
    } else {
        ix.data = static_cast<const uint8_t *>(indices);
    }
    draw(mode, count, ix);
}
void glFlush() {}
void glFramebufferRenderbuffer(GLenum, GLenum attachment, GLenum, GLuint rb) {
    if (attachment != 0x8CE0) g_framebuffers[g.framebuffer].depth_rb = rb;  // (the EFB has its own depth)
}
void glFramebufferTexture2D(GLenum, GLenum attachment, GLenum, GLuint tex, GLint) {
    if (attachment == 0x8CE0) g_framebuffers[g.framebuffer].color_tex = tex;
    if (g.framebuffer) bind_target(g.framebuffer);
}
void glFrontFace(GLenum m) { g.front_face = m; }
void gen(GLsizei n, GLuint *o) {
    for (GLsizei i = 0; i < n; i++) o[i] = new_name();
}
void glGenBuffers(GLsizei n, GLuint *o) { gen(n, o); }
void glGenFramebuffers(GLsizei n, GLuint *o) { gen(n, o); }
void glGenRenderbuffers(GLsizei n, GLuint *o) { gen(n, o); }
void glGenTextures(GLsizei n, GLuint *o) { gen(n, o); }
void glGenerateMipmap(GLenum) {}  // (mipmaps: not yet)
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
    case 0x8DFB: return 251;                                   // MAX_VERTEX_UNIFORM_VECTORS
    case 0x8DFD: return 221;                                   // MAX_FRAGMENT_UNIFORM_VECTORS
    case 0x8DFC: return 8;                                     // MAX_VARYING_VECTORS
    case 0x86A2: return 1;                                     // NUM_COMPRESSED_TEXTURE_FORMATS
    case 0x86A3: return 0x8D64;                                // COMPRESSED_TEXTURE_FORMATS: ETC1
    case 0x0D52: case 0x0D53: case 0x0D54: case 0x0D55: return 8;
    case 0x0D56: return 24;
    case 0x0D57: return 8;
    case 0x0D50: return 4;
    case 0x0BA2: for (int i = 0; i < 4; i++) out[i] = g.viewport[i]; return g.viewport[0];
    case 0x0C10: for (int i = 0; i < 4; i++) out[i] = g.scissor[i]; return g.scissor[0];
    case 0x0D3A: out[0] = kMaxTex, out[1] = kMaxTex; return kMaxTex;
    case 0x8CA6: return int(g.framebuffer);
    case 0x8CA7: return int(g.renderbuffer);
    case 0x8069: return int(g.tex[g.unit]);
    case 0x8894: return int(g.array_buffer);
    case 0x8895: return int(g.element_buffer);
    case 0x8B8D: return int(current_name());
    case 0x84E0: return 0x84C0 + g.unit;
    case 0x0CF5: return g.unpack;
    case 0x8B9A: return 0x1908;
    case 0x8B9B: return 0x1401;
    case 0x80A8: case 0x80A9: case 0x8DF9: case 0x87FE: return 0;
    }
    log_once("getint" + std::to_string(pname), "gx: glGetIntegerv(%04x): 0", pname);
    return 0;
}
void glGetIntegerv(GLenum pname, GLint *out) {
    if (!out) return;
    const int v = int_param(pname, out);
    if (pname != 0x0BA2 && pname != 0x0C10 && pname != 0x0D3A) *out = v;
}
void glGetFloatv(GLenum pname, GLfloat *out) {
    if (!out) return;
    if (pname == 0x846E || pname == 0x846D) {
        out[0] = 1, out[1] = 1;
        return;
    }
    if (pname == 0x84FF) {
        out[0] = 1;
        return;
    }
    if (pname == 0x0C22) {
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
    case 0x1F01: return "Flipper";
    case 0x1F02: return "OpenGL ES 2.0 GX";
    case 0x8B8C: return "OpenGL ES GLSL ES 1.00";
    case 0x1F03:  // ETC1 as the 3DS offers it (converted to CMPR); a name must be followed by a space
        return "GL_OES_compressed_ETC1_RGB8_texture GL_OES_depth24 GL_OES_packed_depth_stencil GL_OES_rgb8_rgba8 "
               "GL_OES_element_index_uint GL_EXT_texture_format_BGRA8888 GL_OES_mapbuffer ";
    }
    return nullptr;
}
void glLineWidth(GLfloat) {}
void glPixelStorei(GLenum pname, GLint v) {
    if (pname == 0x0CF5) g.unpack = v;
}
void glPolygonOffset(GLfloat, GLfloat) {}
void glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *) {}
void glRenderbufferStorage(GLenum, GLenum format, GLsizei w, GLsizei h) {
    if (!g.renderbuffer) return set_error(0x0502);
    Renderbuffer &r = g_renderbuffers[g.renderbuffer];
    r.format = format, r.w = w, r.h = h;
}
void glSampleCoverage(GLfloat, GLboolean) {}
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { g.scissor[0] = x, g.scissor[1] = y, g.scissor[2] = w, g.scissor[3] = h; }
void glStencilFunc(GLenum, GLint, GLuint) {}
void glStencilMask(GLuint) {}
void glStencilOp(GLenum, GLenum, GLenum) {}
void glTexImage2D(GLenum, GLint level, GLint, GLsizei w, GLsizei h, GLint, GLenum format, GLenum type, const void *data) {
    Texture *t = bound_texture();
    if (!t) return set_error(0x0502);
    if (level != 0) return;  // (mipmaps: not yet)
    if (!data) {  // a render target: drawn in the EFB, copied in as RGBA8 at the size drawn there
        if (!rt_alloc(*t, w, h)) return;
        tex_params(*t);
        return;
    }
    const uint8_t fmt = gx_format(format, type);
    if (!tex_alloc(*t, fmt, w, h)) return;
    tex_params(*t);
    Fetch src;
    if (!gl_fetch(format, type, static_cast<const uint8_t *>(data), w, g.unpack, src)) {
        log_once("fmt" + std::to_string(format) + "_" + std::to_string(type), "gx: texture format %04x/%04x not supported",
                 format, type);
        return;
    }
    store(*t, src);
    g_stats.tex_uploads++;
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
    if (!t->render_target) tex_params(*t);
}
void glTexParameterf(GLenum, GLenum pname, GLfloat v) { tex_param(pname, GLint(v)); }
void glTexParameteri(GLenum, GLenum pname, GLint v) { tex_param(pname, v); }
void glTexSubImage2D(GLenum, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *data) {
    Texture *t = bound_texture();
    if (!t || !t->allocated || level != 0 || !data || t->fmt == GX_TF_CMPR) return;
    if (x != 0 || y != 0 || w != t->w || h != t->h) {
        log_once("texsub", "gx: partial glTexSubImage2D not supported yet (whole images only)");
        return;
    }
    Fetch src;
    if (gl_fetch(format, type, static_cast<const uint8_t *>(data), w, g.unpack, src)) store(*t, src);
}
void glVertexAttrib4f(GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    if (i >= 16) return;
    float *v = g.attr[i].value;
    v[0] = x, v[1] = y, v[2] = z, v[3] = w;
}
void glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *ptr) {
    if (i >= 16) return;
    AttribState &a = g.attr[i];
    a.size = size, a.type = type, a.norm = norm != 0, a.stride = stride;
    a.ptr = static_cast<const uint8_t *>(ptr);
    auto it = g.array_buffer ? g_buffers.find(g.array_buffer) : g_buffers.end();
    a.buf = it == g_buffers.end() ? nullptr : &it->second;
}
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (g_trace) rt::log("gx viewport %d %d %d %d", x, y, w, h); g.viewport[0] = x, g.viewport[1] = y, g.viewport[2] = w, g.viewport[3] = h; }

struct Proc {
    const char *name;
    void *fn;
};
#define P(n) {#n, reinterpret_cast<void *>(&n)}
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

bool init(void *pool, uint32_t pool_size) {
    g_pool_size = pool_size;
    __lwp_heap_init(&g_pool, pool, pool_size, 32);
    g_mode = VIDEO_GetPreferredMode(nullptr);
    for (void *&x : g_xfb) x = MEM_K0_TO_K1(SYS_AllocateFramebuffer(g_mode));
    VIDEO_Configure(g_mode);
    VIDEO_SetNextFramebuffer(g_xfb[0]);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (g_mode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
    constexpr uint32_t kFifo = 256u << 10;
    g_fifo = memalign(32, kFifo);
    std::memset(g_fifo, 0, kFifo);
    GX_Init(g_fifo, kFifo);
    GX_SetCopyClear({0, 0, 0, 0xff}, GX_MAX_Z24);
    g_screen_w = g_mode->fbWidth, g_screen_h = g_mode->efbHeight;
    g_target.w = g_screen_w, g_target.h = g_screen_h;
    const float yscale = GX_GetYScaleFactor(g_mode->efbHeight, g_mode->xfbHeight);
    const uint32_t xfb_h = GX_SetDispCopyYScale(yscale);
    GX_SetDispCopySrc(0, 0, g_mode->fbWidth, g_mode->efbHeight);
    GX_SetDispCopyDst(g_mode->fbWidth, uint16_t(xfb_h));
    GX_SetCopyFilter(g_mode->aa, g_mode->sample_pattern, GX_TRUE, g_mode->vfilter);
    GX_SetFieldMode(g_mode->field_rendering, g_mode->viHeight == 2 * g_mode->xfbHeight ? GX_ENABLE : GX_DISABLE);
    GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetDispCopyGamma(GX_GM_1_0);
    GX_SetClipMode(GX_CLIP_ENABLE);
    GX_SetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
    GX_SetTevSwapModeTable(kSwapRedAlpha, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_RED);
    GX_InvVtxCache();
    GX_InvalidateTexAll();
    std::memset(g_white_tex, 0xff, sizeof g_white_tex);
    std::memset(g_grey_tex, 0x80, sizeof g_grey_tex);
    DCFlushRange(g_white_tex, sizeof g_white_tex);
    DCFlushRange(g_grey_tex, sizeof g_grey_tex);
    GX_InitTexObj(&g_white, g_white_tex, 8, 4, GX_TF_I8, GX_REPEAT, GX_REPEAT, GX_FALSE);
    GX_InitTexObj(&g_grey, g_grey_tex, 8, 4, GX_TF_I8, GX_REPEAT, GX_REPEAT, GX_FALSE);
    glprog::set_hooks({nullptr, [](const Program &p, bool matched) {
                           g_stats.programs_linked++;
                           if (!matched) log_once("link:" + p.signature, "gx: no material for a program (%s)", p.signature.c_str());
                       }});
    rt::log("gx: %ux%u (%s), XFB %ux%u, FIFO %u KB, texture pool %u KB", g_mode->fbWidth, g_mode->efbHeight,
            (g_mode->viTVMode & VI_NON_INTERLACE) ? "progressive" : "interlaced", g_mode->fbWidth, xfb_h, kFifo >> 10,
            pool_size >> 10);
    return true;
}

void *proc(const char *name) {
    for (const Proc &p : kProcs)
        if (!std::strcmp(p.name, name)) return p.fn;
    return nullptr;
}

int screen_width() { return g_screen_w; }
int screen_height() { return g_screen_h; }

void swap() {
    bind_target(0);
    flush_pending_clear();
    g_xfb_cur ^= 1;
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_SetColorUpdate(GX_TRUE);
    GX_CopyDisp(g_xfb[g_xfb_cur], GX_TRUE);
    GX_DrawDone();
    VIDEO_SetNextFramebuffer(g_xfb[g_xfb_cur]);
    VIDEO_Flush();
    g_in_frame = false, g_screen_drawn = false;
    g_stats.frames++;
    g_frame_no++;
    static uint32_t frames, traced;
    if (g_trace) rt::log("gx: frame traced (viewport %d %d %d %d)", g.viewport[0], g.viewport[1], g.viewport[2], g.viewport[3]);
    g_trace = ++frames % 1000 == 0 && traced++ < 3;
}

Stats peek_stats() {
    Stats s = g_stats;
    s.tex_bytes = g_pool_used;
    return s;
}

Stats take_stats() {
    Stats s = g_stats;
    g_stats = Stats{};
    g_stats.tex_bytes = g_pool_used;
    return s;
}

}  // namespace gx

namespace rt {
bool install_gx_gl(void *pool, uint32_t pool_size) {
    if (!gx::init(pool, pool_size)) return false;
    return gl_load(&gx::proc);
}
}  // namespace rt
