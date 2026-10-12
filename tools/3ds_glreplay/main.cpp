// GL trace player for the New 3DS (docs/3ds.md, Graphics). Plays a trace recorded on the PC
// by runtime/gles.cpp (NOVA3_GLTRACE) through runtime/gles_pica.cpp, the backend the game uses
// on the 3DS, shows each recorded frame on the top screen, saves it as a PNG and measures it.
//
// Run: put the trace on the SD card and a settings file sdmc:/3ds/glreplay/replay.txt:
//   trace=sdmc:/3ds/glreplay/level.bin
//   repeat=4        (each recorded frame drawn this many times: the timings are of the last ones)
// The log is sdmc:/3ds/glreplay/replay.log (ends with "replay: done"), the screenshots
// sdmc:/3ds/glreplay/frame_<n>.png. tools/3ds_glreplay/run.py does all of it in Azahar.
//
// The trace holds the PC driver's names and locations; they are mapped to the backend's: object
// names as they are created, uniform and attribute locations by name from the game's own queries.
// Vertex attributes are set in the trace's (ANGLE's) locations, so they are kept here and given to
// the backend in its locations at each draw, for the program drawn with.
#include <3ds.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "gles_pica.h"

// The game's textures need more linear memory than half of the 64 MB a .3dsx gets in Azahar.
extern "C" {
u32 __ctru_linear_heap_size = 40 * 1024 * 1024;
}

namespace rt {
FILE *g_log;
void log(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_log) {
        std::fprintf(g_log, "%s\n", buf);
        std::fflush(g_log);
    }
    std::printf("%s\n", buf);
}
using GlLoader = void *(*)(const char *name);
bool gl_load(GlLoader) { return true; }
}  // namespace rt

namespace {

const char *kDir = "sdmc:/3ds/glreplay";

// The backend's GL functions, by name.
#define GLF(name, ret, ...) ret (*name)(__VA_ARGS__);
#define GL_LIST(X)                                                                                         \
    X(glActiveTexture, void, uint32_t)                                                                     \
    X(glAttachShader, void, uint32_t, uint32_t)                                                            \
    X(glBindBuffer, void, uint32_t, uint32_t)                                                              \
    X(glBindFramebuffer, void, uint32_t, uint32_t)                                                         \
    X(glBindRenderbuffer, void, uint32_t, uint32_t)                                                        \
    X(glBindTexture, void, uint32_t, uint32_t)                                                             \
    X(glBlendColor, void, float, float, float, float)                                                      \
    X(glBlendEquation, void, uint32_t)                                                                     \
    X(glBlendEquationSeparate, void, uint32_t, uint32_t)                                                   \
    X(glBlendFunc, void, uint32_t, uint32_t)                                                               \
    X(glBlendFuncSeparate, void, uint32_t, uint32_t, uint32_t, uint32_t)                                   \
    X(glBufferData, void, uint32_t, intptr_t, const void *, uint32_t)                                      \
    X(glBufferSubData, void, uint32_t, intptr_t, intptr_t, const void *)                                   \
    X(glClear, void, uint32_t)                                                                             \
    X(glClearColor, void, float, float, float, float)                                                      \
    X(glClearDepthf, void, float)                                                                          \
    X(glClearStencil, void, int32_t)                                                                       \
    X(glColorMask, void, uint8_t, uint8_t, uint8_t, uint8_t)                                               \
    X(glCompileShader, void, uint32_t)                                                                     \
    X(glCompressedTexImage2D, void, uint32_t, int32_t, uint32_t, int32_t, int32_t, int32_t, int32_t, const void *) \
    X(glCompressedTexSubImage2D, void, uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, int32_t, const void *) \
    X(glCreateProgram, uint32_t)                                                                           \
    X(glCreateShader, uint32_t, uint32_t)                                                                  \
    X(glCullFace, void, uint32_t)                                                                          \
    X(glDeleteBuffers, void, int32_t, const uint32_t *)                                                    \
    X(glDeleteFramebuffers, void, int32_t, const uint32_t *)                                               \
    X(glDeleteProgram, void, uint32_t)                                                                     \
    X(glDeleteRenderbuffers, void, int32_t, const uint32_t *)                                              \
    X(glDeleteShader, void, uint32_t)                                                                      \
    X(glDeleteTextures, void, int32_t, const uint32_t *)                                                   \
    X(glDepthFunc, void, uint32_t)                                                                         \
    X(glDepthMask, void, uint8_t)                                                                          \
    X(glDepthRangef, void, float, float)                                                                   \
    X(glDisable, void, uint32_t)                                                                           \
    X(glDisableVertexAttribArray, void, uint32_t)                                                          \
    X(glDrawArrays, void, uint32_t, int32_t, int32_t)                                                      \
    X(glDrawElements, void, uint32_t, int32_t, uint32_t, const void *)                                     \
    X(glEnable, void, uint32_t)                                                                            \
    X(glEnableVertexAttribArray, void, uint32_t)                                                           \
    X(glFramebufferRenderbuffer, void, uint32_t, uint32_t, uint32_t, uint32_t)                             \
    X(glFramebufferTexture2D, void, uint32_t, uint32_t, uint32_t, uint32_t, int32_t)                       \
    X(glFrontFace, void, uint32_t)                                                                         \
    X(glGenBuffers, void, int32_t, uint32_t *)                                                             \
    X(glGenFramebuffers, void, int32_t, uint32_t *)                                                        \
    X(glGenRenderbuffers, void, int32_t, uint32_t *)                                                       \
    X(glGenTextures, void, int32_t, uint32_t *)                                                            \
    X(glGenerateMipmap, void, uint32_t)                                                                    \
    X(glGetAttribLocation, int32_t, uint32_t, const char *)                                                \
    X(glGetUniformLocation, int32_t, uint32_t, const char *)                                               \
    X(glLinkProgram, void, uint32_t)                                                                       \
    X(glPixelStorei, void, uint32_t, int32_t)                                                              \
    X(glPolygonOffset, void, float, float)                                                                 \
    X(glRenderbufferStorage, void, uint32_t, uint32_t, int32_t, int32_t)                                   \
    X(glScissor, void, int32_t, int32_t, int32_t, int32_t)                                                 \
    X(glShaderSource, void, uint32_t, int32_t, const char *const *, const int32_t *)                       \
    X(glStencilFunc, void, uint32_t, int32_t, uint32_t)                                                    \
    X(glStencilMask, void, uint32_t)                                                                       \
    X(glStencilOp, void, uint32_t, uint32_t, uint32_t)                                                     \
    X(glTexImage2D, void, uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *) \
    X(glTexParameterf, void, uint32_t, uint32_t, float)                                                    \
    X(glTexParameteri, void, uint32_t, uint32_t, int32_t)                                                  \
    X(glTexSubImage2D, void, uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *) \
    X(glUniform1f, void, int32_t, float)                                                                   \
    X(glUniform1fv, void, int32_t, int32_t, const float *)                                                 \
    X(glUniform1i, void, int32_t, int32_t)                                                                 \
    X(glUniform1iv, void, int32_t, int32_t, const int32_t *)                                               \
    X(glUniform2fv, void, int32_t, int32_t, const float *)                                                 \
    X(glUniform2iv, void, int32_t, int32_t, const int32_t *)                                               \
    X(glUniform3fv, void, int32_t, int32_t, const float *)                                                 \
    X(glUniform3iv, void, int32_t, int32_t, const int32_t *)                                               \
    X(glUniform4fv, void, int32_t, int32_t, const float *)                                                 \
    X(glUniform4iv, void, int32_t, int32_t, const int32_t *)                                               \
    X(glUniformMatrix4fv, void, int32_t, int32_t, uint8_t, const float *)                                  \
    X(glUseProgram, void, uint32_t)                                                                        \
    X(glVertexAttrib4f, void, uint32_t, float, float, float, float)                                        \
    X(glVertexAttribPointer, void, uint32_t, int32_t, uint32_t, uint8_t, int32_t, const void *)            \
    X(glViewport, void, int32_t, int32_t, int32_t, int32_t)
GL_LIST(GLF)
#undef GLF

// Every backend call is timed, per function (svcGetSystemTick: Azahar's CPU ticks): the backend's
// CPU time is all of its calls, less the timer's own cost and the wait for the GPU at the frame's
// start (inside its first draw or clear).
#define FN_ID(name, ...) F_##name,
enum Fn { GL_LIST(FN_ID) kFns };
#undef FN_ID
#define FN_NAME(name, ...) #name,
const char *const kFnNames[kFns] = {GL_LIST(FN_NAME)};
#undef FN_NAME
uint64_t g_fn_ticks[kFns];
uint32_t g_fn_calls[kFns];
template <int ID, class F> struct Timed;
template <int ID, class R, class... A> struct Timed<ID, R (*)(A...)> {
    static inline R (*real)(A...) = nullptr;
    static R call(A... a) {
        const uint64_t t = svcGetSystemTick();
        if constexpr (std::is_void_v<R>) {
            real(a...);
            g_fn_ticks[ID] += svcGetSystemTick() - t;
            g_fn_calls[ID]++;
        } else {
            R r = real(a...);
            g_fn_ticks[ID] += svcGetSystemTick() - t;
            g_fn_calls[ID]++;
            return r;
        }
    }
};

float F(uint32_t w) {
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}

// ---- the trace -----------------------------------------------------------------------------------
struct Reader {
    FILE *f = nullptr;
    std::vector<std::string> names;  // function id -> name
    uint32_t id = 0;
    std::string name;
    std::vector<uint32_t> w;
    std::vector<uint8_t> blob[2];
    uint32_t blobs = 0;
    bool open(const char *path) {
        f = std::fopen(path, "rb");
        if (!f) return false;
        static std::vector<char> buf(256 * 1024);
        std::setvbuf(f, buf.data(), _IOFBF, buf.size());
        uint32_t head[3];
        if (std::fread(head, 4, 3, f) != 3 || head[0] != 0x5447334e) return false;
        for (uint32_t i = 0; i < head[2]; i++) {
            uint32_t n;
            std::fread(&n, 4, 1, f);
            std::string s(n, '\0');
            std::fread(s.data(), 1, n, f);
            std::fseek(f, long((4 - n % 4) % 4), SEEK_CUR);
            names.push_back(s);
        }
        return true;
    }
    bool next() {
        uint32_t h[2];
        if (std::fread(h, 4, 2, f) != 2) return false;
        id = h[0] & 0xffff;
        blobs = h[0] >> 16;
        w.resize(h[1]);
        if (h[1] && std::fread(w.data(), 4, h[1], f) != h[1]) return false;
        for (uint32_t b = 0; b < blobs; b++) {
            uint32_t n;
            if (std::fread(&n, 4, 1, f) != 1) return false;
            std::vector<uint8_t> &v = blob[b < 2 ? b : 1];
            v.resize((n + 3) & ~3u);
            if (n && std::fread(v.data(), 1, (n + 3) & ~3u, f) != ((n + 3) & ~3u)) return false;
            v.resize(n);
        }
        name = id < names.size() ? names[id] : id == 0x1000 ? "FRAME" : id == 0x1001 ? "CLIENT" : "?";
        return true;
    }
};

// ---- names and locations ---------------------------------------------------------------------------
std::unordered_map<uint32_t, uint32_t> g_tex, g_buf, g_fb, g_rb, g_prog, g_shader;
uint32_t map(std::unordered_map<uint32_t, uint32_t> &m, uint32_t name) {
    if (!name) return 0;
    auto it = m.find(name);
    return it == m.end() ? 0 : it->second;
}
struct ProgInfo {
    std::map<int32_t, int32_t> uniforms;                    // trace location -> backend location
    std::vector<std::pair<uint32_t, int32_t>> attribs;      // trace location -> backend location
};
std::unordered_map<uint32_t, ProgInfo> g_info;  // by trace program
uint32_t g_cur_prog;                            // trace name
int32_t uloc(int32_t loc) {
    auto it = g_info.find(g_cur_prog);
    if (it == g_info.end()) return -1;
    auto u = it->second.uniforms.find(loc);
    return u == it->second.uniforms.end() ? -1 : u->second;
}

// Vertex attributes in the trace's locations.
struct Attr {
    bool enabled = false, set = false;
    uint32_t size = 4, type = 0x1406, norm = 0, stride = 0, ptr = 0, buffer = 0;  // trace buffer
    float value[4] = {0, 0, 0, 1};
    bool value_set = false;
    std::vector<uint8_t> client;  // this draw's client-memory data
    uint32_t client_off = 0;
    bool has_client = false;
};
Attr g_attr[16];
uint32_t g_array_buffer;  // trace name
void apply_attribs() {
    auto it = g_info.find(g_cur_prog);
    if (it == g_info.end()) return;
    for (auto [tl, bl] : it->second.attribs) {
        if (tl >= 16 || bl < 0) continue;
        Attr &a = g_attr[tl];
        if (a.set) {
            glBindBuffer(0x8892, map(g_buf, a.buffer));
            const void *p = a.buffer ? reinterpret_cast<const void *>(uintptr_t(a.ptr))
                                     : a.has_client ? static_cast<const void *>(a.client.data() - a.client_off) : nullptr;
            glVertexAttribPointer(uint32_t(bl), int32_t(a.size), a.type, uint8_t(a.norm), int32_t(a.stride), p);
        }
        if (a.enabled) glEnableVertexAttribArray(uint32_t(bl));
        else glDisableVertexAttribArray(uint32_t(bl));
        if (a.value_set) glVertexAttrib4f(uint32_t(bl), a.value[0], a.value[1], a.value[2], a.value[3]);
    }
    glBindBuffer(0x8892, map(g_buf, g_array_buffer));
    for (Attr &a : g_attr) a.has_client = false;
}

// ---- PNG (stored deflate blocks) -------------------------------------------------------------------
uint32_t crc(const uint8_t *p, size_t n, uint32_t c = 0xffffffffu) {
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
    }
    return c;
}
void write_png(const char *path, int w, int h, const uint8_t *rgba_bottom_up, bool alpha = false) {
    std::vector<uint8_t> raw;
    const size_t n = alpha ? 4 : 3;
    raw.reserve(size_t(h) * (size_t(w) * n + 1));
    for (int y = h - 1; y >= 0; y--) {
        raw.push_back(0);
        for (int x = 0; x < w; x++) raw.insert(raw.end(), rgba_bottom_up + (size_t(y) * w + x) * 4, rgba_bottom_up + (size_t(y) * w + x) * 4 + n);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (size_t pos = 0; pos < raw.size();) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n == raw.size());
        z.push_back(uint8_t(n)), z.push_back(uint8_t(n >> 8)), z.push_back(uint8_t(~n)), z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + long(pos), raw.begin() + long(pos + n));
        for (size_t i = pos; i < pos + n; i++) a = (a + raw[i]) % 65521, b = (b + a) % 65521;
        pos += n;
    }
    const uint32_t adler = b << 16 | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back(uint8_t(adler >> s));
    FILE *f = std::fopen(path, "wb");
    if (!f) return;
    auto be32 = [&](uint32_t v) { const uint8_t t[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; std::fwrite(t, 1, 4, f); };
    auto chunk = [&](const char *type, const std::vector<uint8_t> &d) {
        be32(uint32_t(d.size()));
        std::vector<uint8_t> td(type, type + 4);
        td.insert(td.end(), d.begin(), d.end());
        std::fwrite(td.data(), 1, td.size(), f);
        be32(~crc(td.data(), td.size()));
    };
    const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    std::fwrite(sig, 1, 8, f);
    chunk("IHDR", {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w), uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, uint8_t(alpha ? 6 : 2), 0, 0, 0});
    chunk("IDAT", z);
    chunk("IEND", {});
    std::fclose(f);
}

// ---- playing one record ------------------------------------------------------------------------------
void play(Reader &r) {
    const std::vector<uint32_t> &w = r.w;
    const std::string &n = r.name;
    const uint8_t *b0 = r.blobs ? r.blob[0].data() : nullptr;
    auto gen = [&](void (*fn)(int32_t, uint32_t *), std::unordered_map<uint32_t, uint32_t> &m) {
        const uint32_t count = w[0];
        std::vector<uint32_t> mine(count);
        fn(int32_t(count), mine.data());
        for (uint32_t i = 0; i < count && (i + 1) * 4 <= r.blob[0].size(); i++) m[reinterpret_cast<const uint32_t *>(b0)[i]] = mine[i];
    };
    auto del = [&](void (*fn)(int32_t, const uint32_t *), std::unordered_map<uint32_t, uint32_t> &m) {
        std::vector<uint32_t> mine;
        for (uint32_t i = 0; i < w[0] && (i + 1) * 4 <= r.blob[0].size(); i++) {
            const uint32_t t = reinterpret_cast<const uint32_t *>(b0)[i];
            if (uint32_t v = map(m, t)) mine.push_back(v);
            m.erase(t);
        }
        fn(int32_t(mine.size()), mine.data());
    };
    if (n == "glActiveTexture") glActiveTexture(w[0]);
    else if (n == "glAttachShader") glAttachShader(map(g_prog, w[0]), map(g_shader, w[1]));
    else if (n == "glBindBuffer") {
        if (w[0] == 0x8892) g_array_buffer = w[1];
        glBindBuffer(w[0], map(g_buf, w[1]));
    } else if (n == "glBindFramebuffer") glBindFramebuffer(w[0], map(g_fb, w[1]));
    else if (n == "glBindRenderbuffer") glBindRenderbuffer(w[0], map(g_rb, w[1]));
    else if (n == "glBindTexture") glBindTexture(w[0], map(g_tex, w[1]));
    else if (n == "glBlendColor") glBlendColor(F(w[0]), F(w[1]), F(w[2]), F(w[3]));
    else if (n == "glBlendEquation") glBlendEquation(w[0]);
    else if (n == "glBlendEquationSeparate") glBlendEquationSeparate(w[0], w[1]);
    else if (n == "glBlendFunc") glBlendFunc(w[0], w[1]);
    else if (n == "glBlendFuncSeparate") glBlendFuncSeparate(w[0], w[1], w[2], w[3]);
    else if (n == "glBufferData") glBufferData(w[0], intptr_t(w[1]), w[2] ? b0 : nullptr, w[3]);
    else if (n == "glBufferSubData") glBufferSubData(w[0], intptr_t(w[1]), intptr_t(w[2]), b0);
    else if (n == "glClear") glClear(w[0]);
    else if (n == "glClearColor") glClearColor(F(w[0]), F(w[1]), F(w[2]), F(w[3]));
    else if (n == "glClearDepthf") glClearDepthf(F(w[0]));
    else if (n == "glClearStencil") glClearStencil(int32_t(w[0]));
    else if (n == "glColorMask") glColorMask(uint8_t(w[0]), uint8_t(w[1]), uint8_t(w[2]), uint8_t(w[3]));
    else if (n == "glCompileShader") glCompileShader(map(g_shader, w[0]));
    else if (n == "glCompressedTexImage2D") glCompressedTexImage2D(w[0], int32_t(w[1]), w[2], int32_t(w[3]), int32_t(w[4]), int32_t(w[5]), int32_t(w[6]), b0);
    else if (n == "glCompressedTexSubImage2D") glCompressedTexSubImage2D(w[0], int32_t(w[1]), int32_t(w[2]), int32_t(w[3]), int32_t(w[4]), int32_t(w[5]), w[6], int32_t(w[7]), b0);
    else if (n == "glCreateProgram") g_prog[w[0]] = glCreateProgram();
    else if (n == "glCreateShader") g_shader[w[1]] = glCreateShader(w[0]);
    else if (n == "glCullFace") glCullFace(w[0]);
    else if (n == "glDeleteBuffers") del(glDeleteBuffers, g_buf);
    else if (n == "glDeleteFramebuffers") del(glDeleteFramebuffers, g_fb);
    else if (n == "glDeleteRenderbuffers") del(glDeleteRenderbuffers, g_rb);
    else if (n == "glDeleteTextures") del(glDeleteTextures, g_tex);
    else if (n == "glDeleteProgram") {
        glDeleteProgram(map(g_prog, w[0]));
        g_info.erase(w[0]);
    } else if (n == "glDeleteShader") glDeleteShader(map(g_shader, w[0]));
    else if (n == "glDepthFunc") glDepthFunc(w[0]);
    else if (n == "glDepthMask") glDepthMask(uint8_t(w[0]));
    else if (n == "glDepthRangef") glDepthRangef(F(w[0]), F(w[1]));
    else if (n == "glDisable") glDisable(w[0]);
    else if (n == "glEnable") glEnable(w[0]);
    else if (n == "glDisableVertexAttribArray") { if (w[0] < 16) g_attr[w[0]].enabled = false; }
    else if (n == "glEnableVertexAttribArray") { if (w[0] < 16) g_attr[w[0]].enabled = true; }
    else if (n == "glVertexAttrib4f") {
        if (w[0] < 16) {
            Attr &a = g_attr[w[0]];
            for (int i = 0; i < 4; i++) a.value[i] = F(w[1 + i]);
            a.value_set = true;
        }
    } else if (n == "glVertexAttribPointer") {
        if (w[0] < 16) {
            Attr &a = g_attr[w[0]];
            a.set = true, a.size = w[1], a.type = w[2], a.norm = w[3], a.stride = w[4], a.ptr = w[5], a.buffer = g_array_buffer;
        }
    } else if (n == "CLIENT") {
        if (w[0] < 16) {
            Attr &a = g_attr[w[0]];
            a.client = r.blob[0];
            a.client_off = w[1];
            a.has_client = true;
        }
    } else if (n == "glDrawArrays") {
        apply_attribs();
        glDrawArrays(w[0], int32_t(w[1]), int32_t(w[2]));
    } else if (n == "glDrawElements") {
        apply_attribs();
        glDrawElements(w[0], int32_t(w[1]), w[2], r.blob[0].empty() ? reinterpret_cast<const void *>(uintptr_t(w[3])) : b0);
    } else if (n == "glFramebufferRenderbuffer") glFramebufferRenderbuffer(w[0], w[1], w[2], map(g_rb, w[3]));
    else if (n == "glFramebufferTexture2D") glFramebufferTexture2D(w[0], w[1], w[2], map(g_tex, w[3]), int32_t(w[4]));
    else if (n == "glFrontFace") glFrontFace(w[0]);
    else if (n == "glGenBuffers") gen(glGenBuffers, g_buf);
    else if (n == "glGenFramebuffers") gen(glGenFramebuffers, g_fb);
    else if (n == "glGenRenderbuffers") gen(glGenRenderbuffers, g_rb);
    else if (n == "glGenTextures") gen(glGenTextures, g_tex);
    else if (n == "glGenerateMipmap") glGenerateMipmap(w[0]);
    else if (n == "glGetAttribLocation") {
        const int32_t mine = glGetAttribLocation(map(g_prog, w[0]), reinterpret_cast<const char *>(b0));
        if (int32_t(w[1]) >= 0) g_info[w[0]].attribs.emplace_back(w[1], mine);
    } else if (n == "glGetUniformLocation") {
        const int32_t mine = glGetUniformLocation(map(g_prog, w[0]), reinterpret_cast<const char *>(b0));
        if (int32_t(w[1]) >= 0) g_info[w[0]].uniforms[int32_t(w[1])] = mine;
    } else if (n == "glLinkProgram") {
        glLinkProgram(map(g_prog, w[0]));
        g_info[w[0]] = ProgInfo{};
    } else if (n == "glPixelStorei") glPixelStorei(w[0], int32_t(w[1]));
    else if (n == "glPolygonOffset") glPolygonOffset(F(w[0]), F(w[1]));
    else if (n == "glRenderbufferStorage") glRenderbufferStorage(w[0], w[1], int32_t(w[2]), int32_t(w[3]));
    else if (n == "glScissor") glScissor(int32_t(w[0]), int32_t(w[1]), int32_t(w[2]), int32_t(w[3]));
    else if (n == "glShaderSource") {
        const char *src = reinterpret_cast<const char *>(b0);
        const int32_t len = int32_t(r.blob[0].size());
        glShaderSource(map(g_shader, w[0]), 1, &src, &len);
    } else if (n == "glStencilFunc") glStencilFunc(w[0], int32_t(w[1]), w[2]);
    else if (n == "glStencilMask") glStencilMask(w[0]);
    else if (n == "glStencilOp") glStencilOp(w[0], w[1], w[2]);
    else if (n == "glTexImage2D") glTexImage2D(w[0], int32_t(w[1]), int32_t(w[2]), int32_t(w[3]), int32_t(w[4]), int32_t(w[5]), w[6], w[7], w[8] ? b0 : nullptr);
    else if (n == "glTexParameterf") glTexParameterf(w[0], w[1], F(w[2]));
    else if (n == "glTexParameteri") glTexParameteri(w[0], w[1], int32_t(w[2]));
    else if (n == "glTexSubImage2D") glTexSubImage2D(w[0], int32_t(w[1]), int32_t(w[2]), int32_t(w[3]), int32_t(w[4]), int32_t(w[5]), w[6], w[7], b0);
    else if (n == "glUniform1f") glUniform1f(uloc(int32_t(w[0])), F(w[1]));
    else if (n == "glUniform1i") glUniform1i(uloc(int32_t(w[0])), int32_t(w[1]));
    else if (n == "glUniform1fv") glUniform1fv(uloc(int32_t(w[0])), int32_t(w[1]), reinterpret_cast<const float *>(b0));
    else if (n == "glUniform2fv") glUniform2fv(uloc(int32_t(w[0])), int32_t(w[1]), reinterpret_cast<const float *>(b0));
    else if (n == "glUniform3fv") glUniform3fv(uloc(int32_t(w[0])), int32_t(w[1]), reinterpret_cast<const float *>(b0));
    else if (n == "glUniform4fv") glUniform4fv(uloc(int32_t(w[0])), int32_t(w[1]), reinterpret_cast<const float *>(b0));
    else if (n == "glUniform1iv") glUniform1iv(uloc(int32_t(w[0])), int32_t(w[1]), reinterpret_cast<const int32_t *>(b0));
    else if (n == "glUniform2iv") glUniform2iv(uloc(int32_t(w[0])), int32_t(w[1]), reinterpret_cast<const int32_t *>(b0));
    else if (n == "glUniform3iv") glUniform3iv(uloc(int32_t(w[0])), int32_t(w[1]), reinterpret_cast<const int32_t *>(b0));
    else if (n == "glUniform4iv") glUniform4iv(uloc(int32_t(w[0])), int32_t(w[1]), reinterpret_cast<const int32_t *>(b0));
    else if (n == "glUniformMatrix4fv") glUniformMatrix4fv(uloc(int32_t(w[0])), int32_t(w[1]), uint8_t(r.w.size() > 2 ? w[2] : 0), reinterpret_cast<const float *>(b0));
    else if (n == "glUseProgram") {
        g_cur_prog = w[0];
        glUseProgram(map(g_prog, w[0]));
    } else if (n == "glViewport") glViewport(int32_t(w[0]), int32_t(w[1]), int32_t(w[2]), int32_t(w[3]));
}

// Between traces: every object the last one made is deleted (the backend frees their memory
// once the GPU is past them) and the trace-side state forgotten.
void reset() {
    auto names = [](std::unordered_map<uint32_t, uint32_t> &m) {
        std::vector<uint32_t> v;
        for (auto &[t, mine] : m) v.push_back(mine);
        m.clear();
        return v;
    };
    std::vector<uint32_t> v = names(g_tex);
    glDeleteTextures(int32_t(v.size()), v.data());
    v = names(g_buf);
    glDeleteBuffers(int32_t(v.size()), v.data());
    v = names(g_fb);
    glDeleteFramebuffers(int32_t(v.size()), v.data());
    v = names(g_rb);
    glDeleteRenderbuffers(int32_t(v.size()), v.data());
    glUseProgram(0);
    for (uint32_t p : names(g_prog)) glDeleteProgram(p);
    for (uint32_t sh : names(g_shader)) glDeleteShader(sh);
    g_info.clear();
    g_cur_prog = 0, g_array_buffer = 0;
    for (Attr &a : g_attr) a = Attr{};
    glBindFramebuffer(0x8D40, 0);
    glClear(0x4500);  // (a frame start: memory the GPU no longer reads is freed)
    pica::swap();
    pica::reset_depth_scale();
}

std::map<std::string, std::string> settings() {
    std::map<std::string, std::string> s;
    char path[128];
    std::snprintf(path, sizeof path, "%s/replay.txt", kDir);
    FILE *f = std::fopen(path, "r");
    if (!f) return s;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
        const size_t eq = l.find('=');
        if (eq != std::string::npos && l[0] != '#') s[l.substr(0, eq)] = l.substr(eq + 1);
    }
    std::fclose(f);
    return s;
}

}  // namespace

int main() {
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, nullptr);
    // Calibration of the timings below: a fixed loop (10 M dependent adds) at 268 and 804 MHz
    double calib[2];
    for (int k = 0; k < 2; k++) {
        osSetSpeedupEnable(k == 1);
        svcSleepThread(10 * 1000 * 1000);
        volatile uint32_t acc = 0;
        const uint64_t t0 = svcGetSystemTick();
        for (uint32_t i = 0; i < 10000000; i++) acc = acc + i;
        calib[k] = double(svcGetSystemTick() - t0) / CPU_TICKS_PER_MSEC;
    }
    char path[128];
    std::snprintf(path, sizeof path, "%s/replay.log", kDir);
    rt::g_log = std::fopen(path, "w");
    auto cfg = settings();
    // the traces of this launch: trace=, trace1=, trace2= ... (each one's files prefixed t<k>_ from the
    // second on, so that one emulator start plays several)
    std::vector<std::string> traces = {cfg.count("trace") ? cfg["trace"] : std::string(kDir) + "/trace.bin"};
    for (int k = 1; cfg.count("trace" + std::to_string(k)); k++) traces.push_back(cfg["trace" + std::to_string(k)]);
    const std::string &trace = traces[0];
    const int repeat = cfg.count("repeat") ? std::max(1, std::atoi(cfg["repeat"].c_str())) : 1;
    const bool shots = !cfg.count("shots") || cfg["shots"] != "0";
    rt::log("replay: %s, each frame %d times; application memory %.1f MB, linear heap %.1f MB", trace.c_str(), repeat,
            osGetMemRegionSize(MEMREGION_APPLICATION) / 1048576.0, __ctru_linear_heap_size / 1048576.0);
    rt::log("replay: calibration loop %.1f ms at 268 MHz, %.1f ms with the New 3DS speedup", calib[0], calib[1]);
    if (!rt::install_pica_gl()) {
        rt::log("replay: backend failed");
    } else {
        if (cfg.count("pack")) pica::set_texture_pack(cfg["pack"].c_str());
#define RESOLVE(name, ...)                                                                    \
    Timed<F_##name, decltype(name)>::real = reinterpret_cast<decltype(name)>(pica::proc(#name)); \
    name = &Timed<F_##name, decltype(name)>::call;
        GL_LIST(RESOLVE)
#undef RESOLVE
        // the timer's own cost: a pair of syscalls per call
        uint64_t t0 = svcGetSystemTick();
        for (int i = 0; i < 1000; i++) svcGetSystemTick();
        const double pair_ticks = 2.0 * double(svcGetSystemTick() - t0) / 1000;
        for (size_t k = 0; k < traces.size(); k++) {
        const std::string prefix = k ? "t" + std::to_string(k) + "_" : "";
        if (k) {
            reset();
            rt::log("replay: next trace %s (files %s*)", traces[k].c_str(), prefix.c_str());
        }
        const std::string dbg = "debug" + (k ? std::to_string(k) : std::string());
        pica::set_debug(cfg.count(dbg) ? std::atoi(cfg[dbg].c_str()) : 0);
        Reader r;
        if (!r.open(traces[k].c_str())) {
            rt::log("replay: cannot read %s", traces[k].c_str());
        } else {
            uint64_t records = 0, load_start = svcGetSystemTick();
            long frame_pos = -1;
            int frame = -1, pass = 0;
            uint64_t frame_start = 0;
            pica::take_stats();
            while (aptMainLoop() && r.next()) {
                records++;
                if (r.name == "FRAME") {
                    if (r.w[1]) {  // begins
                        if (pass == 0) {
                            rt::log("replay: frame %u after %.1f s, %llu records", r.w[0], double(svcGetSystemTick() - load_start) / CPU_TICKS_PER_MSEC / 1000,
                                    (unsigned long long)records);
                            pica::take_stats();
                            std::memset(g_fn_ticks, 0, sizeof g_fn_ticks);
                            std::memset(g_fn_calls, 0, sizeof g_fn_calls);
                        }
                        frame = int(r.w[0]);
                        frame_pos = std::ftell(r.f);
                        frame_start = svcGetSystemTick();
                        continue;
                    }
                    pica::swap();
                    const double wall = double(svcGetSystemTick() - frame_start) / CPU_TICKS_PER_MSEC;
                    if (pass == 0 && shots) {
                        std::vector<uint8_t> rgba(400 * 240 * 4);
                        pica::read_screen(0, 0, 400, 240, rgba.data());
                        char out[128];
                        std::snprintf(out, sizeof out, "%s/%sframe_%d.png", kDir, prefix.c_str(), frame);
                        write_png(out, 400, 240, rgba.data());
                        if (cfg.count("targets"))  // the render-target textures too (PICA rows: top first)
                            for (uint32_t name : pica::render_targets()) {
                                int tw, th;
                                std::vector<uint8_t> img;
                                if (!pica::read_texture(name, tw, th, img)) continue;
                                std::vector<uint8_t> flipped(img.size());
                                for (int y = 0; y < th; y++)
                                    std::memcpy(&flipped[size_t(th - 1 - y) * size_t(tw) * 4], &img[size_t(y) * size_t(tw) * 4], size_t(tw) * 4);
                                std::snprintf(out, sizeof out, "%s/%sframe_%d_target_%lu.png", kDir, prefix.c_str(), frame, (unsigned long)name);
                                write_png(out, tw, th, flipped.data(), true);
                            }
                    }
                    if (++pass < repeat) {  // again, for the timings
                        std::fseek(r.f, frame_pos, SEEK_SET);
                        frame_start = svcGetSystemTick();
                        continue;
                    }
                    const pica::Stats s = pica::take_stats();
                    const double n = s.frames ? s.frames : 1;
                    double ticks = 0, calls = 0, draw_ticks = 0;
                    int order[kFns];
                    for (int i = 0; i < kFns; i++) {
                        order[i] = i;
                        g_fn_ticks[i] = uint64_t(std::max(0.0, double(g_fn_ticks[i]) - pair_ticks * g_fn_calls[i]));
                        ticks += double(g_fn_ticks[i]), calls += g_fn_calls[i];
                    }
                    draw_ticks = double(g_fn_ticks[F_glDrawElements] + g_fn_ticks[F_glDrawArrays]);
                    const double wait_ticks = s.wait_ms * CPU_TICKS_PER_MSEC;
                    std::sort(order, order + kFns, [](int a, int b) { return g_fn_ticks[a] > g_fn_ticks[b]; });
                    rt::log("replay: frame %d: %u draws (%u skipped) %.0f vertices, backend CPU %.2f ms in %.0f calls (draws %.2f ms, "
                            "%.0f KB copied, %u materials reused), GPU %.2f ms, wait %.2f ms, wall %.2f ms (last pass); ring %u KB, "
                            "cmdbuf %.0f%%, textures %.1f MB, buffers %.1f MB",
                            frame, uint32_t(s.draws / n), uint32_t(s.draws_skipped / n), s.vertices / n,
                            std::max(0.0, ticks - wait_ticks) / CPU_TICKS_PER_MSEC / n, calls / n,
                            std::max(0.0, draw_ticks - wait_ticks) / CPU_TICKS_PER_MSEC / n, s.copied_bytes / n / 1024, uint32_t(s.materials_reused / n),
                            s.gpu_ms / n, s.wait_ms / n, wall, s.ring_peak / 1024, s.cmdbuf_peak * 100, s.tex_bytes / 1048576.0,
                            s.buf_bytes / 1048576.0);
                    char top[512];
                    int len = 0;
                    for (int k = 0; k < 8 && g_fn_calls[order[k]] && len < int(sizeof top); k++)
                        len += std::snprintf(top + len, sizeof top - size_t(len), " %s %.0f x %.3f ms", kFnNames[order[k]],
                                             g_fn_calls[order[k]] / n, double(g_fn_ticks[order[k]]) / CPU_TICKS_PER_MSEC / n);
                    rt::log("replay: frame %d: by function (the first draw or clear holds the wait):%s", frame, top);
                    if (s.phase_ticks[0]) {  // (make PROFILE=1)
                        // draws: target, material (shader, inputs, position | textures | colour, lighting | fog | combiners),
                        // vertex data, state and issue
                        auto ms = [&](int k) { return (double(s.phase_ticks[k]) - pair_ticks / 2 * s.draws) / CPU_TICKS_PER_MSEC / n; };
                        rt::log("replay: frame %d: draws: target %.2f ms, material %.2f (shader %.2f, textures %.2f, colour %.2f, "
                                "fog %.2f, combiners %.2f), vertex data %.2f, textures and fragment state %.2f, citro3d state %.2f, issue %.2f",
                                frame, ms(0), ms(4) + ms(5) + ms(6) + ms(7) + ms(8), ms(4), ms(5), ms(6), ms(7), ms(8), ms(1), ms(2),
                                ms(9), ms(3));
                        rt::log("replay: frame %d: vertex data: indices %.2f ms, attributes %.2f, buffers %.2f, layout %.2f", frame,
                                ms(10), ms(11), ms(12), ms(1));
                    }
                    pass = 0;
                    continue;
                }
                play(r);
                if (records % 50000 == 0)  // (where a slow or stuck replay is)
                    rt::log("replay: %llu records, at %s", (unsigned long long)records, r.name.c_str());
            }
            rt::log("replay: %llu records", (unsigned long long)records);
            std::fclose(r.f);
        }
        }
    }
    rt::log("replay: done");
    if (rt::g_log) std::fclose(rt::g_log);
    rt::g_log = nullptr;
    // stay up until START (or the emulator is closed by the runner)
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
    gfxExit();
    return 0;
}
