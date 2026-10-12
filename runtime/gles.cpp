// GLES2 imports. The host creates the context (SDL: ANGLE on Windows, native EGL/GLES
// elsewhere) and hands gl_load() a function loader; the guest's GL imports forward here.
// Guest pointers become host pointers except where GLES treats them as offsets into a
// bound buffer object (vertex attributes, element indices).
#include "os.h"
#include "runtime.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <map>
#include <type_traits>
#include <vector>

namespace rt {
namespace {

using u32 = uint32_t;
using GLenum = uint32_t;
using GLuint = uint32_t;
using GLint = int32_t;
using GLsizei = int32_t;
using GLfloat = float;
using GLboolean = uint8_t;
using GLbitfield = uint32_t;
using GLintptr = intptr_t;
using GLsizeiptr = intptr_t;

// X(name, return type, host parameter types...)
#define GL_FUNCS(X)                                                                                              \
    X(glActiveTexture, void, GLenum)                                                                             \
    X(glAttachShader, void, GLuint, GLuint)                                                                      \
    X(glBindBuffer, void, GLenum, GLuint)                                                                        \
    X(glBindFramebuffer, void, GLenum, GLuint)                                                                   \
    X(glBindRenderbuffer, void, GLenum, GLuint)                                                                  \
    X(glBindTexture, void, GLenum, GLuint)                                                                       \
    X(glBlendColor, void, GLfloat, GLfloat, GLfloat, GLfloat)                                                   \
    X(glBlendEquation, void, GLenum)                                                                             \
    X(glBlendEquationSeparate, void, GLenum, GLenum)                                                             \
    X(glBlendFunc, void, GLenum, GLenum)                                                                         \
    X(glBlendFuncSeparate, void, GLenum, GLenum, GLenum, GLenum)                                                 \
    X(glBufferData, void, GLenum, GLsizeiptr, const void *, GLenum)                                              \
    X(glBufferSubData, void, GLenum, GLintptr, GLsizeiptr, const void *)                                         \
    X(glCheckFramebufferStatus, GLenum, GLenum)                                                                  \
    X(glClear, void, GLbitfield)                                                                                 \
    X(glClearColor, void, GLfloat, GLfloat, GLfloat, GLfloat)                                                   \
    X(glClearDepthf, void, GLfloat)                                                                              \
    X(glClearStencil, void, GLint)                                                                               \
    X(glColorMask, void, GLboolean, GLboolean, GLboolean, GLboolean)                                             \
    X(glCompileShader, void, GLuint)                                                                             \
    X(glCompressedTexImage2D, void, GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *)       \
    X(glCompressedTexSubImage2D, void, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *) \
    X(glCopyTexSubImage2D, void, GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei)                    \
    X(glCreateProgram, GLuint)                                                                                   \
    X(glCreateShader, GLuint, GLenum)                                                                            \
    X(glCullFace, void, GLenum)                                                                                  \
    X(glDeleteBuffers, void, GLsizei, const GLuint *)                                                            \
    X(glDeleteFramebuffers, void, GLsizei, const GLuint *)                                                       \
    X(glDeleteProgram, void, GLuint)                                                                             \
    X(glDeleteRenderbuffers, void, GLsizei, const GLuint *)                                                      \
    X(glDeleteShader, void, GLuint)                                                                              \
    X(glDeleteTextures, void, GLsizei, const GLuint *)                                                           \
    X(glDepthFunc, void, GLenum)                                                                                 \
    X(glDepthMask, void, GLboolean)                                                                              \
    X(glDepthRangef, void, GLfloat, GLfloat)                                                                     \
    X(glDisable, void, GLenum)                                                                                   \
    X(glDisableVertexAttribArray, void, GLuint)                                                                  \
    X(glDrawArrays, void, GLenum, GLint, GLsizei)                                                                \
    X(glDrawElements, void, GLenum, GLsizei, GLenum, const void *)                                               \
    X(glEnable, void, GLenum)                                                                                    \
    X(glEnableVertexAttribArray, void, GLuint)                                                                   \
    X(glFlush, void)                                                                                             \
    X(glFramebufferRenderbuffer, void, GLenum, GLenum, GLenum, GLuint)                                           \
    X(glFramebufferTexture2D, void, GLenum, GLenum, GLenum, GLuint, GLint)                                       \
    X(glFrontFace, void, GLenum)                                                                                 \
    X(glGenBuffers, void, GLsizei, GLuint *)                                                                     \
    X(glGenFramebuffers, void, GLsizei, GLuint *)                                                                \
    X(glGenRenderbuffers, void, GLsizei, GLuint *)                                                               \
    X(glGenTextures, void, GLsizei, GLuint *)                                                                    \
    X(glGenerateMipmap, void, GLenum)                                                                            \
    X(glGetActiveAttrib, void, GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, char *)                    \
    X(glGetActiveUniform, void, GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, char *)                   \
    X(glGetAttribLocation, GLint, GLuint, const char *)                                                          \
    X(glGetError, GLenum)                                                                                        \
    X(glGetFloatv, void, GLenum, GLfloat *)                                                                      \
    X(glGetIntegerv, void, GLenum, GLint *)                                                                      \
    X(glGetProgramInfoLog, void, GLuint, GLsizei, GLsizei *, char *)                                             \
    X(glGetProgramiv, void, GLuint, GLenum, GLint *)                                                             \
    X(glGetShaderInfoLog, void, GLuint, GLsizei, GLsizei *, char *)                                              \
    X(glGetShaderSource, void, GLuint, GLsizei, GLsizei *, char *)                                               \
    X(glGetShaderiv, void, GLuint, GLenum, GLint *)                                                              \
    X(glGetString, const char *, GLenum)                                                                         \
    X(glGetUniformLocation, GLint, GLuint, const char *)                                                         \
    X(glLineWidth, void, GLfloat)                                                                                \
    X(glLinkProgram, void, GLuint)                                                                               \
    X(glPixelStorei, void, GLenum, GLint)                                                                        \
    X(glPolygonOffset, void, GLfloat, GLfloat)                                                                   \
    X(glReadPixels, void, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)                                \
    X(glRenderbufferStorage, void, GLenum, GLenum, GLsizei, GLsizei)                                             \
    X(glSampleCoverage, void, GLfloat, GLboolean)                                                                \
    X(glScissor, void, GLint, GLint, GLsizei, GLsizei)                                                           \
    X(glShaderSource, void, GLuint, GLsizei, const char *const *, const GLint *)                                 \
    X(glStencilFunc, void, GLenum, GLint, GLuint)                                                                \
    X(glStencilMask, void, GLuint)                                                                               \
    X(glStencilOp, void, GLenum, GLenum, GLenum)                                                                 \
    X(glTexImage2D, void, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)           \
    X(glTexParameterf, void, GLenum, GLenum, GLfloat)                                                            \
    X(glTexParameteri, void, GLenum, GLenum, GLint)                                                              \
    X(glTexSubImage2D, void, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)        \
    X(glUniform1f, void, GLint, GLfloat)                                                                         \
    X(glUniform1fv, void, GLint, GLsizei, const GLfloat *)                                                       \
    X(glUniform1i, void, GLint, GLint)                                                                           \
    X(glUniform1iv, void, GLint, GLsizei, const GLint *)                                                         \
    X(glUniform2fv, void, GLint, GLsizei, const GLfloat *)                                                       \
    X(glUniform2iv, void, GLint, GLsizei, const GLint *)                                                         \
    X(glUniform3fv, void, GLint, GLsizei, const GLfloat *)                                                       \
    X(glUniform3iv, void, GLint, GLsizei, const GLint *)                                                         \
    X(glUniform4fv, void, GLint, GLsizei, const GLfloat *)                                                       \
    X(glUniform4iv, void, GLint, GLsizei, const GLint *)                                                         \
    X(glUniformMatrix4fv, void, GLint, GLsizei, GLboolean, const GLfloat *)                                      \
    X(glUseProgram, void, GLuint)                                                                                \
    X(glVertexAttrib4f, void, GLuint, GLfloat, GLfloat, GLfloat, GLfloat)                                       \
    X(glVertexAttribPointer, void, GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)                      \
    X(glViewport, void, GLint, GLint, GLsizei, GLsizei)

#define DECLARE(name, ret, ...) ret (*p_##name)(__VA_ARGS__);
GL_FUNCS(DECLARE)
#undef DECLARE

// OES_texture_3D entry points are optional in ES2 contexts.
void (*p_glTexImage3DOES)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
void (*p_glTexSubImage3DOES)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void *);
void (*p_glCompressedTexImage3DOES)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLsizei, GLint, GLsizei, const void *);
void (*p_glCompressedTexSubImage3DOES)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLsizei, const void *);

GlLoader g_loader;
template <class F>
void load(F &fn, const char *name) {
    fn = reinterpret_cast<F>(g_loader(name));
}

// ---- GL trace (NOVA3_GLTRACE=dir) -----------------------------------------------------------------
// For building other GL backends (the 3DS's PICA200: docs/3ds.md, Graphics) against what the game
// really does. Every call that reaches the driver is recorded with the data it references into
// <dir>/gltrace.bin, which tools/3ds_glreplay plays back. Object, upload, shader and query calls
// are recorded from the start; state and draw calls only in the frames listed in
// NOVA3_GLTRACE_FRAMES ("1200,5000-5010", the --screenshot frame numbers). The first frame of each
// run of listed frames starts with the state the game had set (the last value of every piece of
// state), so a replay draws those frames exactly. With the game's low-end profile (NOVA3_DEVICE=3ds)
// ETC1 is advertised as on the 3DS: the game then uploads its ETC1 textures as they are, the
// trace keeps them, and they are decoded here for ANGLE, which lacks ETC1. Off when unset.
//
// Format (little-endian u32 words): 'N3GT', version, function count, then each GL function's name
// (u32 length + bytes, padded to 4), in the order of the ids below. Records: id | blob count << 16,
// word count, the words (arguments; floats as their bits, pointers as their low 32 bits; a return
// value or query result last), then each blob (u32 size, bytes, padded to 4). Ids past the GL
// functions: kTraceFrame (frame, 1 = begins / 0 = ends), kTraceClient (client-memory vertex data
// for the next draw: attribute, byte offset of the blob from the attribute's pointer).
namespace gltrace {
#define TRACE_ID(name, ...) T_##name,
enum Id : uint32_t { GL_FUNCS(TRACE_ID) kFuncCount };
#undef TRACE_ID
enum : uint32_t { kTraceFrame = 0x1000, kTraceClient = 0x1001 };

FILE *g_file;
bool g_window;  // the current frame is recorded in full
int g_frame;
std::vector<std::pair<int, int>> g_frames;  // listed frame ranges
bool g_etc1;    // ETC1 advertised (and decoded for the real driver)

struct Blob {
    const void *p;
    size_t n;
};
std::vector<uint8_t> encode(uint32_t id, const uint32_t *w, size_t nw, std::initializer_list<Blob> blobs = {}) {
    std::vector<uint8_t> r;
    auto put = [&](uint32_t v) { r.insert(r.end(), reinterpret_cast<uint8_t *>(&v), reinterpret_cast<uint8_t *>(&v) + 4); };
    put(id | uint32_t(blobs.size()) << 16);
    put(uint32_t(nw));
    for (size_t i = 0; i < nw; i++) put(w[i]);
    for (const Blob &b : blobs) {
        put(uint32_t(b.n));
        if (b.n) r.insert(r.end(), static_cast<const uint8_t *>(b.p), static_cast<const uint8_t *>(b.p) + b.n);
        r.resize((r.size() + 3) & ~size_t(3));
    }
    return r;
}
void emit(const std::vector<uint8_t> &r) { std::fwrite(r.data(), 1, r.size(), g_file); }
template <class T> uint32_t word(T v) {
    if constexpr (std::is_floating_point_v<T>) {
        const float f = float(v);
        uint32_t w;
        std::memcpy(&w, &f, 4);
        return w;
    } else if constexpr (std::is_pointer_v<T>) {
        return uint32_t(reinterpret_cast<uintptr_t>(v));
    } else {
        return uint32_t(v);
    }
}

// State: the last record of each piece of state as the game set it (real) and as the trace has it
// (traced), with the object it names (binding: the object; vertex attribute: its buffer).
enum Kind : uint64_t {  // in the order a snapshot writes them
    K_UNIF = 1, K_VAP, K_BT, K_BB, K_FUNC, K_CAP, K_PIXELSTORE, K_VAE, K_VA4, K_BF, K_BR, K_AT, K_UP
};
uint64_t key(Kind k, uint32_t a = 0, uint32_t b = 0) { return uint64_t(k) << 56 | uint64_t(a & 0xffffff) << 32 | b; }
struct Value {
    std::vector<uint8_t> rec;
    uint32_t obj = 0;
};
struct Slot {
    Value real, traced;
};
std::map<uint64_t, Slot> g_state;
// What the game has bound (needed to key and size the calls).
uint32_t g_unit, g_program, g_buffer[2], g_unpack = 4, g_attr_on;
struct Attr {
    int32_t size = 4;
    uint32_t type = 0x1406, norm = 0;
    int32_t stride = 0;
    const void *ptr = nullptr;
    uint32_t buffer = 0;
} g_attr[16];
std::unordered_map<uint32_t, std::vector<uint8_t>> g_buffers;  // buffer contents (index ranges of draws)

int target_slot(uint32_t target) { return target == 0x8893 /*ELEMENT_ARRAY_BUFFER*/; }
void write_state(uint64_t k, const std::vector<uint8_t> &rec, uint32_t obj) {
    emit(rec);
    Slot &s = g_state[k];
    s.traced = {rec, obj};
}
void set_state(uint64_t k, std::vector<uint8_t> rec, uint32_t obj) {
    Slot &s = g_state[k];
    if (g_window) {
        emit(rec);
        s.traced = {rec, obj};
    }
    s.real = {std::move(rec), obj};
}
// The trace gets the game's value of this state (if it has another).
void sync(uint64_t k) {
    auto it = g_state.find(k);
    if (it == g_state.end() || (it->second.real.rec == it->second.traced.rec && it->second.real.obj == it->second.traced.obj)) return;
    Slot &s = it->second;
    const Kind kind = Kind(k >> 56);
    if (kind == K_UNIF) {  // the program it belongs to must be current
        const uint32_t prog = uint32_t(k >> 32) & 0xffffff;
        const uint64_t up = key(K_UP);
        if (g_state[up].traced.obj != prog || g_state[up].traced.rec.empty()) {
            const uint32_t w[1] = {prog};
            write_state(up, encode(T_glUseProgram, w, 1), prog);
        }
    } else if (kind == K_VAP) {  // the buffer it reads
        const uint64_t bb = key(K_BB, 0, 0x8892);
        if (g_state[bb].traced.obj != s.real.obj || g_state[bb].traced.rec.empty()) {
            const uint32_t w[2] = {0x8892, s.real.obj};
            write_state(bb, encode(T_glBindBuffer, w, 2), s.real.obj);
        }
    } else if (kind == K_BT) {  // its texture unit
        const uint32_t unit = uint32_t(k >> 32) & 0xffffff;
        const uint64_t at = key(K_AT);
        if (g_state[at].traced.obj != unit || g_state[at].traced.rec.empty()) {
            const uint32_t w[1] = {0x84C0 + unit};
            write_state(at, encode(T_glActiveTexture, w, 1), unit);
        }
    }
    write_state(k, s.real.rec, s.real.obj);
}
void sync_all() {  // a recorded frame begins: all state as the game set it
    std::vector<uint64_t> keys;
    for (auto &[k, s] : g_state) keys.push_back(k);
    for (uint64_t k : keys) sync(k);
    sync(key(K_BB, 0, 0x8892)), sync(key(K_AT)), sync(key(K_UP));  // put back after the dependencies above
}
void sync_texture(uint32_t target) {
    sync(key(K_BT, g_unit, target == 0x8513 /*CUBE_MAP*/ || (target >= 0x8515 && target <= 0x851A) ? 0x8513 : target));
    sync(key(K_AT));
    sync(key(K_PIXELSTORE, 0x0CF5 /*UNPACK_ALIGNMENT*/));
}
// An object is deleted: GL unbinds it.
void unbind(Kind kind, uint32_t name) {
    for (auto &[k, s] : g_state) {
        if (Kind(k >> 56) != kind) continue;
        for (Value *v : {&s.real, &s.traced}) {
            if (v->obj != name || v->rec.empty()) continue;
            uint32_t w[2] = {uint32_t(k), 0};
            if (kind == K_BT) v->rec = encode(T_glBindTexture, w, 2);
            else if (kind == K_BB) v->rec = encode(T_glBindBuffer, w, 2);
            else if (kind == K_BF) w[0] = 0x8D40, v->rec = encode(T_glBindFramebuffer, w, 2);
            else if (kind == K_BR) w[0] = 0x8D41, v->rec = encode(T_glBindRenderbuffer, w, 2);
            v->obj = 0;
        }
    }
}
void forget_program(uint32_t program) {  // linked again or deleted: its uniform values are gone
    for (auto it = g_state.lower_bound(key(K_UNIF, program)); it != g_state.end() && it->first < key(K_UNIF, program + 1);)
        it = g_state.erase(it);
}

bool listed(int frame) {
    for (auto &[a, b] : g_frames)
        if (frame >= a && frame <= b) return true;
    return false;
}
void frame_done() {
    if (!g_file) return;
    if (g_window) {
        const uint32_t w[2] = {uint32_t(g_frame), 0};
        emit(encode(kTraceFrame, w, 2));
        std::fflush(g_file);
    }
    g_frame++;
    const bool on = listed(g_frame);
    if (on && !g_window) sync_all();
    g_window = on;
    if (on) {
        const uint32_t w[2] = {uint32_t(g_frame), 1};
        emit(encode(kTraceFrame, w, 2));
    }
}

// Generic recording: the arguments as words. Which calls are state, which act on objects.
template <Id ID, class F> struct Rec;
template <Id ID, class R, class... A> struct Rec<ID, R (*)(A...)> {
    static inline R (*real)(A...) = nullptr;
    static void record(const uint32_t *w, size_t n);
    static R call(A... a) {
        const uint32_t w[sizeof...(A) + 1] = {word(a)..., 0};
        if constexpr (std::is_void_v<R>) {
            real(a...);
            record(w, sizeof...(A));
        } else {
            R r = real(a...);
            uint32_t wr[sizeof...(A) + 1];
            std::memcpy(wr, w, sizeof wr);
            wr[sizeof...(A)] = word(r);
            record(wr, sizeof...(A) + 1);
            return r;
        }
    }
};
template <Id ID, class R, class... A> void Rec<ID, R (*)(A...)>::record(const uint32_t *w, size_t n) {
    std::vector<uint8_t> r = encode(ID, w, n);
    switch (ID) {
    // queries and reads: not needed to replay
    case T_glGetError: case T_glGetString: case T_glGetIntegerv: case T_glGetFloatv: case T_glReadPixels:
    case T_glGetProgramInfoLog: case T_glGetShaderInfoLog: case T_glGetShaderSource: case T_glGetShaderiv:
    case T_glCheckFramebufferStatus:
        return;
    // drawing: only in recorded frames
    case T_glClear: case T_glFlush: case T_glCopyTexSubImage2D:
        if (g_window) emit(r);
        return;
    // objects, shaders, textures
    case T_glCreateProgram: case T_glCreateShader: case T_glCompileShader: case T_glAttachShader:
    case T_glDeleteShader:
        emit(r);
        return;
    case T_glLinkProgram: case T_glDeleteProgram:
        forget_program(w[0]);
        emit(r);
        return;
    case T_glTexParameterf: case T_glTexParameteri: case T_glGenerateMipmap:
        sync_texture(w[0]);
        emit(r);
        return;
    case T_glFramebufferTexture2D: case T_glFramebufferRenderbuffer:
        sync(key(K_BF));
        emit(r);
        return;
    case T_glRenderbufferStorage:
        sync(key(K_BR));
        emit(r);
        return;
    // state
    case T_glActiveTexture: g_unit = (w[0] - 0x84C0) & 31; set_state(key(K_AT), r, g_unit); return;
    case T_glBindBuffer: g_buffer[target_slot(w[0])] = w[1]; set_state(key(K_BB, 0, w[0]), r, w[1]); return;
    case T_glBindTexture: set_state(key(K_BT, g_unit, w[0]), r, w[1]); return;
    case T_glBindFramebuffer: set_state(key(K_BF), r, w[1]); return;
    case T_glBindRenderbuffer: set_state(key(K_BR), r, w[1]); return;
    case T_glUseProgram: g_program = w[0]; set_state(key(K_UP), r, w[0]); return;
    case T_glEnable: case T_glDisable: set_state(key(K_CAP, 0, w[0]), r, 0); return;
    case T_glEnableVertexAttribArray: case T_glDisableVertexAttribArray:
        if (w[0] < 16) g_attr_on = ID == T_glEnableVertexAttribArray ? g_attr_on | 1u << w[0] : g_attr_on & ~(1u << w[0]);
        set_state(key(K_VAE, 0, w[0]), r, 0);
        return;
    case T_glVertexAttrib4f: set_state(key(K_VA4, 0, w[0]), r, 0); return;
    case T_glPixelStorei:
        if (w[0] == 0x0CF5) g_unpack = w[1];
        set_state(key(K_PIXELSTORE, 0, w[0]), r, 0);
        return;
    case T_glUniform1f: case T_glUniform1i: set_state(key(K_UNIF, g_program, w[0]), r, 0); return;
    case T_glBlendFuncSeparate: set_state(key(K_FUNC, 0, T_glBlendFunc), r, 0); return;
    case T_glBlendEquationSeparate: set_state(key(K_FUNC, 0, T_glBlendEquation), r, 0); return;
    default: set_state(key(K_FUNC, 0, ID), r, 0); return;
    }
}
#define REAL(name) Rec<T_##name, decltype(p_##name)>::real

// Bytes of a glTexImage2D / glTexSubImage2D upload (rows padded to the unpack alignment).
size_t upload_size(GLsizei w, GLsizei h, GLenum fmt, GLenum type) {
    int bpp = 4;
    if (type == 0x8363 /*565*/ || type == 0x8033 /*4444*/ || type == 0x8034 /*5551*/) bpp = 2;
    else if (fmt == 0x1907 /*RGB*/) bpp = 3;
    else if (fmt == 0x190A /*LUMINANCE_ALPHA*/) bpp = 2;
    else if (fmt == 0x1906 /*ALPHA*/ || fmt == 0x1909 /*LUMINANCE*/) bpp = 1;
    if (type == 0x1406 /*FLOAT*/) bpp *= 4;
    else if (type == 0x8D61 /*HALF_FLOAT_OES*/) bpp *= 2;
    const size_t a = g_unpack ? g_unpack : 1;
    const size_t row = (size_t(w) * size_t(bpp) + a - 1) / a * a;
    return h > 0 ? row * size_t(h - 1) + size_t(w) * size_t(bpp) : 0;
}

// ETC1 (for the real driver when it is advertised but not there): 4x4 blocks to RGB.
void etc1_decode(const uint8_t *src, int w, int h, uint8_t *rgb) {
    static const int tables[8][4] = {{2, 8, -2, -8}, {5, 17, -5, -17}, {9, 29, -9, -29}, {13, 42, -13, -42},
                                     {18, 60, -18, -60}, {24, 80, -24, -80}, {33, 106, -33, -106}, {47, 183, -47, -183}};
    auto clamp = [](int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); };
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
                    base[0][c] = b5 << 3 | b5 >> 2;
                    const int b2 = (b5 + d) & 31;
                    base[1][c] = b2 << 3 | b2 >> 2;
                } else {
                    base[0][c] = int(hi >> (shift + 4) & 15) * 17;
                    base[1][c] = int(hi >> shift & 15) * 17;
                }
            }
            const int table[2] = {int(hi >> 5) & 7, int(hi >> 2) & 7};
            for (int i = 0; i < 16; i++) {  // pixel i: column i / 4, row i % 4
                const int x = i / 4, y = i % 4;
                const int sub = flip ? y >= 2 : x >= 2;
                const int delta = tables[table[sub]][int(lo >> i & 1) | int(lo >> (i + 16) & 1) << 1];
                const int px = bx * 4 + x, py = by * 4 + y;
                if (px >= w || py >= h) continue;
                uint8_t *o = rgb + (size_t(py) * size_t(w) + size_t(px)) * 3;
                for (int c = 0; c < 3; c++) o[c] = clamp(base[sub][c] + delta);
            }
        }
}

void tr_glBufferData(GLenum t, GLsizeiptr size, const void *data, GLenum usage) {
    REAL(glBufferData)(t, size, data, usage);
    sync(key(K_BB, 0, t));
    const uint32_t w[4] = {t, uint32_t(size), data ? 1u : 0u, usage};
    emit(encode(T_glBufferData, w, 4, {{data, data ? size_t(size) : 0}}));
    std::vector<uint8_t> &shadow = g_buffers[g_buffer[target_slot(t)]];
    shadow.assign(size_t(size), 0);
    if (data) std::memcpy(shadow.data(), data, size_t(size));
}
void tr_glBufferSubData(GLenum t, GLintptr off, GLsizeiptr size, const void *data) {
    REAL(glBufferSubData)(t, off, size, data);
    sync(key(K_BB, 0, t));
    const uint32_t w[3] = {t, uint32_t(off), uint32_t(size)};
    emit(encode(T_glBufferSubData, w, 3, {{data, size_t(size)}}));
    std::vector<uint8_t> &shadow = g_buffers[g_buffer[target_slot(t)]];
    if (shadow.size() >= size_t(off + size)) std::memcpy(shadow.data() + off, data, size_t(size));
}
void tr_glCompressedTexImage2D(GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b, GLsizei size, const void *data) {
    if (f == 0x8D64 /*ETC1_RGB8_OES*/ && g_etc1) {
        std::vector<uint8_t> rgb(size_t(w) * size_t(h) * 3);
        etc1_decode(static_cast<const uint8_t *>(data), w, h, rgb.data());
        REAL(glPixelStorei)(0x0CF5, 1);
        REAL(glTexImage2D)(t, l, 0x1907, w, h, b, 0x1907, 0x1401, rgb.data());
        REAL(glPixelStorei)(0x0CF5, GLint(g_unpack));
    } else {
        REAL(glCompressedTexImage2D)(t, l, f, w, h, b, size, data);
    }
    sync_texture(t);
    const uint32_t wd[7] = {t, uint32_t(l), f, uint32_t(w), uint32_t(h), uint32_t(b), uint32_t(size)};
    emit(encode(T_glCompressedTexImage2D, wd, 7, {{data, size_t(size)}}));
}
void tr_glCompressedTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLsizei size, const void *data) {
    if (f == 0x8D64 && g_etc1) {
        std::vector<uint8_t> rgb(size_t(w) * size_t(h) * 3);
        etc1_decode(static_cast<const uint8_t *>(data), w, h, rgb.data());
        REAL(glPixelStorei)(0x0CF5, 1);
        REAL(glTexSubImage2D)(t, l, x, y, w, h, 0x1907, 0x1401, rgb.data());
        REAL(glPixelStorei)(0x0CF5, GLint(g_unpack));
    } else {
        REAL(glCompressedTexSubImage2D)(t, l, x, y, w, h, f, size, data);
    }
    sync_texture(t);
    const uint32_t wd[8] = {t, uint32_t(l), uint32_t(x), uint32_t(y), uint32_t(w), uint32_t(h), f, uint32_t(size)};
    emit(encode(T_glCompressedTexSubImage2D, wd, 8, {{data, size_t(size)}}));
}
void tr_glTexImage2D(GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum fmt, GLenum type, const void *data) {
    REAL(glTexImage2D)(t, l, ifmt, w, h, b, fmt, type, data);
    sync_texture(t);
    const uint32_t wd[9] = {t, uint32_t(l), uint32_t(ifmt), uint32_t(w), uint32_t(h), uint32_t(b), fmt, type, data ? 1u : 0u};
    emit(encode(T_glTexImage2D, wd, 9, {{data, data ? upload_size(w, h, fmt, type) : 0}}));
}
void tr_glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void *data) {
    REAL(glTexSubImage2D)(t, l, x, y, w, h, fmt, type, data);
    sync_texture(t);
    const uint32_t wd[8] = {t, uint32_t(l), uint32_t(x), uint32_t(y), uint32_t(w), uint32_t(h), fmt, type};
    emit(encode(T_glTexSubImage2D, wd, 8, {{data, upload_size(w, h, fmt, type)}}));
}
template <Id ID, Kind K> void deleted(GLsizei n, const GLuint *names) {
    const uint32_t w[1] = {uint32_t(n)};
    emit(encode(ID, w, 1, {{names, size_t(n) * 4}}));
    for (GLsizei i = 0; i < n; i++) {
        unbind(K, names[i]);
        if (K == K_BB) g_buffers.erase(names[i]);
    }
}
void tr_glDeleteBuffers(GLsizei n, const GLuint *p) { REAL(glDeleteBuffers)(n, p); deleted<T_glDeleteBuffers, K_BB>(n, p); }
void tr_glDeleteTextures(GLsizei n, const GLuint *p) { REAL(glDeleteTextures)(n, p); deleted<T_glDeleteTextures, K_BT>(n, p); }
void tr_glDeleteFramebuffers(GLsizei n, const GLuint *p) { REAL(glDeleteFramebuffers)(n, p); deleted<T_glDeleteFramebuffers, K_BF>(n, p); }
void tr_glDeleteRenderbuffers(GLsizei n, const GLuint *p) { REAL(glDeleteRenderbuffers)(n, p); deleted<T_glDeleteRenderbuffers, K_BR>(n, p); }
template <Id ID> void generated(GLsizei n, const GLuint *names) {
    const uint32_t w[1] = {uint32_t(n)};
    emit(encode(ID, w, 1, {{names, size_t(n) * 4}}));
}
void tr_glGenBuffers(GLsizei n, GLuint *p) { REAL(glGenBuffers)(n, p); generated<T_glGenBuffers>(n, p); }
void tr_glGenTextures(GLsizei n, GLuint *p) { REAL(glGenTextures)(n, p); generated<T_glGenTextures>(n, p); }
void tr_glGenFramebuffers(GLsizei n, GLuint *p) { REAL(glGenFramebuffers)(n, p); generated<T_glGenFramebuffers>(n, p); }
void tr_glGenRenderbuffers(GLsizei n, GLuint *p) { REAL(glGenRenderbuffers)(n, p); generated<T_glGenRenderbuffers>(n, p); }
void tr_glShaderSource(GLuint s, GLsizei count, const char *const *strings, const GLint *lengths) {
    REAL(glShaderSource)(s, count, strings, lengths);
    std::string all;
    for (GLsizei i = 0; i < count; i++) all += lengths && lengths[i] >= 0 ? std::string(strings[i], size_t(lengths[i])) : std::string(strings[i]);
    const uint32_t w[1] = {s};
    emit(encode(T_glShaderSource, w, 1, {{all.data(), all.size()}}));
}
GLint tr_glGetAttribLocation(GLuint p, const char *name) {
    const GLint r = REAL(glGetAttribLocation)(p, name);
    const uint32_t w[2] = {p, uint32_t(r)};
    emit(encode(T_glGetAttribLocation, w, 2, {{name, std::strlen(name) + 1}}));
    return r;
}
GLint tr_glGetUniformLocation(GLuint p, const char *name) {
    const GLint r = REAL(glGetUniformLocation)(p, name);
    const uint32_t w[2] = {p, uint32_t(r)};
    emit(encode(T_glGetUniformLocation, w, 2, {{name, std::strlen(name) + 1}}));
    return r;
}
template <Id ID> void active(GLuint p, GLuint i, const GLint *size, const GLenum *type, const char *name) {
    const uint32_t w[4] = {p, i, size ? uint32_t(*size) : 0, type ? *type : 0};
    emit(encode(ID, w, 4, {{name, name ? std::strlen(name) + 1 : 0}}));
}
void tr_glGetActiveAttrib(GLuint p, GLuint i, GLsizei n, GLsizei *len, GLint *size, GLenum *type, char *name) {
    REAL(glGetActiveAttrib)(p, i, n, len, size, type, name);
    active<T_glGetActiveAttrib>(p, i, size, type, name);
}
void tr_glGetActiveUniform(GLuint p, GLuint i, GLsizei n, GLsizei *len, GLint *size, GLenum *type, char *name) {
    REAL(glGetActiveUniform)(p, i, n, len, size, type, name);
    active<T_glGetActiveUniform>(p, i, size, type, name);
}
void tr_glGetProgramiv(GLuint p, GLenum pname, GLint *out) {
    REAL(glGetProgramiv)(p, pname, out);
    const uint32_t w[3] = {p, pname, out ? uint32_t(*out) : 0};
    emit(encode(T_glGetProgramiv, w, 3));
}
template <Id ID> void uniform(GLint loc, GLsizei n, const void *data, size_t bytes, uint32_t extra = ~0u) {
    const uint32_t w[3] = {uint32_t(loc), uint32_t(n), extra};
    set_state(key(K_UNIF, g_program, uint32_t(loc)), encode(ID, w, extra == ~0u ? 2 : 3, {{data, bytes * size_t(n)}}), 0);
}
void tr_glUniform1fv(GLint l, GLsizei n, const GLfloat *v) { REAL(glUniform1fv)(l, n, v); uniform<T_glUniform1fv>(l, n, v, 4); }
void tr_glUniform2fv(GLint l, GLsizei n, const GLfloat *v) { REAL(glUniform2fv)(l, n, v); uniform<T_glUniform2fv>(l, n, v, 8); }
void tr_glUniform3fv(GLint l, GLsizei n, const GLfloat *v) { REAL(glUniform3fv)(l, n, v); uniform<T_glUniform3fv>(l, n, v, 12); }
void tr_glUniform4fv(GLint l, GLsizei n, const GLfloat *v) { REAL(glUniform4fv)(l, n, v); uniform<T_glUniform4fv>(l, n, v, 16); }
void tr_glUniform1iv(GLint l, GLsizei n, const GLint *v) { REAL(glUniform1iv)(l, n, v); uniform<T_glUniform1iv>(l, n, v, 4); }
void tr_glUniform2iv(GLint l, GLsizei n, const GLint *v) { REAL(glUniform2iv)(l, n, v); uniform<T_glUniform2iv>(l, n, v, 8); }
void tr_glUniform3iv(GLint l, GLsizei n, const GLint *v) { REAL(glUniform3iv)(l, n, v); uniform<T_glUniform3iv>(l, n, v, 12); }
void tr_glUniform4iv(GLint l, GLsizei n, const GLint *v) { REAL(glUniform4iv)(l, n, v); uniform<T_glUniform4iv>(l, n, v, 16); }
void tr_glUniformMatrix4fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v) {
    REAL(glUniformMatrix4fv)(l, n, t, v);
    uniform<T_glUniformMatrix4fv>(l, n, v, 64, t);
}
void tr_glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *p) {
    REAL(glVertexAttribPointer)(i, size, type, norm, stride, p);
    const uint32_t buffer = g_buffer[0];
    if (i < 16) g_attr[i] = {size, type, norm, stride, p, buffer};
    const uint32_t w[6] = {i, uint32_t(size), type, norm, uint32_t(stride), word(p)};
    set_state(key(K_VAP, 0, i), encode(T_glVertexAttribPointer, w, 6), buffer);
}
// Before a recorded draw: the vertices it reads from client memory (attributes without a buffer).
void client_arrays(uint32_t first, uint32_t last) {
    for (uint32_t i = 0; i < 16; i++) {
        const Attr &a = g_attr[i];
        if (!(g_attr_on >> i & 1) || a.buffer || !a.ptr) continue;
        const int comp = a.type == 0x1406 || a.type == 0x140C /*FIXED*/ ? 4 : a.type == 0x1402 || a.type == 0x1403 ? 2 : 1;
        const size_t elem = size_t(a.size) * size_t(comp), stride = a.stride ? size_t(a.stride) : elem;
        const size_t off = size_t(first) * stride, bytes = size_t(last - first) * stride + elem;
        const uint32_t w[2] = {i, uint32_t(off)};
        emit(encode(kTraceClient, w, 2, {{static_cast<const uint8_t *>(a.ptr) + off, bytes}}));
    }
}
void tr_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    REAL(glDrawArrays)(mode, first, count);
    if (!g_window || count <= 0) return;
    client_arrays(uint32_t(first), uint32_t(first + count - 1));
    const uint32_t w[3] = {mode, uint32_t(first), uint32_t(count)};
    emit(encode(T_glDrawArrays, w, 3));
}
void tr_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    REAL(glDrawElements)(mode, count, type, indices);
    if (!g_window || count <= 0) return;
    const size_t isize = type == 0x1405 /*UINT*/ ? 4 : type == 0x1403 ? 2 : 1;
    const uint8_t *idx = static_cast<const uint8_t *>(indices);
    if (g_buffer[1]) {  // indices in a buffer object: its shadow copy
        const std::vector<uint8_t> &b = g_buffers[g_buffer[1]];
        const size_t off = size_t(reinterpret_cast<uintptr_t>(indices));
        idx = off + size_t(count) * isize <= b.size() ? b.data() + off : nullptr;
    }
    uint32_t lo = ~0u, hi = 0;
    for (GLsizei i = 0; idx && i < count; i++) {
        uint32_t v = isize == 4 ? reinterpret_cast<const uint32_t *>(idx)[i] : isize == 2 ? reinterpret_cast<const uint16_t *>(idx)[i] : idx[i];
        lo = std::min(lo, v), hi = std::max(hi, v);
    }
    if (idx) client_arrays(lo, hi);
    const uint32_t w[4] = {mode, uint32_t(count), type, word(indices)};
    emit(encode(T_glDrawElements, w, 4, {{g_buffer[1] ? nullptr : indices, g_buffer[1] ? 0 : size_t(count) * isize}}));
}

void start(const char *dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string path = std::string(dir) + "/gltrace.bin";
    g_file = std::fopen(path.c_str(), "wb");
    if (!g_file) {
        log("gl trace: cannot write %s", path.c_str());
        return;
    }
    static std::vector<char> buffer(1 << 20);
    std::setvbuf(g_file, buffer.data(), _IOFBF, buffer.size());
    if (const char *f = std::getenv("NOVA3_GLTRACE_FRAMES"))
        for (const char *p = f; *p;) {
            char *end;
            const long a = std::strtol(p, &end, 10);
            long b = a;
            if (*end == '-') b = std::strtol(end + 1, &end, 10);
            if (end == p) break;
            g_frames.emplace_back(int(a), int(b));
            p = *end ? end + 1 : end;
        }
    g_etc1 = low_end_device();
    const uint32_t head[3] = {0x5447334e /*'N3GT'*/, 1, kFuncCount};
    std::fwrite(head, 4, 3, g_file);
#define NAME(name, ...)                                                       \
    {                                                                         \
        const uint32_t n = uint32_t(std::strlen(#name));                      \
        const uint32_t zero = 0;                                              \
        std::fwrite(&n, 4, 1, g_file);                                        \
        std::fwrite(#name, 1, n, g_file);                                     \
        std::fwrite(&zero, 1, (4 - n % 4) % 4, g_file);                        \
    }
    GL_FUNCS(NAME)
#undef NAME
#define WRAP(name, ...)                                                           \
    Rec<T_##name, decltype(p_##name)>::real = p_##name;                           \
    p_##name = &Rec<T_##name, decltype(p_##name)>::call;
    GL_FUNCS(WRAP)
#undef WRAP
    p_glBufferData = tr_glBufferData, p_glBufferSubData = tr_glBufferSubData;
    p_glCompressedTexImage2D = tr_glCompressedTexImage2D, p_glCompressedTexSubImage2D = tr_glCompressedTexSubImage2D;
    p_glTexImage2D = tr_glTexImage2D, p_glTexSubImage2D = tr_glTexSubImage2D;
    p_glDeleteBuffers = tr_glDeleteBuffers, p_glDeleteTextures = tr_glDeleteTextures;
    p_glDeleteFramebuffers = tr_glDeleteFramebuffers, p_glDeleteRenderbuffers = tr_glDeleteRenderbuffers;
    p_glGenBuffers = tr_glGenBuffers, p_glGenTextures = tr_glGenTextures;
    p_glGenFramebuffers = tr_glGenFramebuffers, p_glGenRenderbuffers = tr_glGenRenderbuffers;
    p_glShaderSource = tr_glShaderSource;
    p_glGetAttribLocation = tr_glGetAttribLocation, p_glGetUniformLocation = tr_glGetUniformLocation;
    p_glGetActiveAttrib = tr_glGetActiveAttrib, p_glGetActiveUniform = tr_glGetActiveUniform;
    p_glGetProgramiv = tr_glGetProgramiv;
    p_glUniform1fv = tr_glUniform1fv, p_glUniform2fv = tr_glUniform2fv, p_glUniform3fv = tr_glUniform3fv;
    p_glUniform4fv = tr_glUniform4fv, p_glUniform1iv = tr_glUniform1iv, p_glUniform2iv = tr_glUniform2iv;
    p_glUniform3iv = tr_glUniform3iv, p_glUniform4iv = tr_glUniform4iv, p_glUniformMatrix4fv = tr_glUniformMatrix4fv;
    p_glVertexAttribPointer = tr_glVertexAttribPointer;
    p_glDrawArrays = tr_glDrawArrays, p_glDrawElements = tr_glDrawElements;
    g_window = listed(0);
    if (g_window) {
        const uint32_t w[2] = {0, 1};
        emit(encode(kTraceFrame, w, 2));
    }
    log("gl trace: %s (%zu frame ranges%s)", path.c_str(), g_frames.size(), g_etc1 ? ", ETC1 advertised" : "");
}
#undef REAL
}  // namespace gltrace

// ---- GL profile (NOVA3_GLPROFILE=1) ----------------------------------------------------------------
// Time inside the driver per GL function (after the wrappers below: what the backend itself
// costs), logged with the frame lines: for comparing the game's GL time (which also holds the
// imports' gates, these wrappers and the timer) with the 3DS trace replays. Off when unset.
namespace glprofile {
using gltrace::Id;
using gltrace::kFuncCount;
uint64_t g_ticks[kFuncCount];
uint32_t g_calls[kFuncCount];
uint64_t g_ticks0;
int64_t g_ns0;
#define NAME_STR(name, ...) #name,
const char *const kNames[kFuncCount] = {GL_FUNCS(NAME_STR)};
#undef NAME_STR

template <Id ID, class F> struct Prof;
template <Id ID, class R, class... A> struct Prof<ID, R (*)(A...)> {
    static inline R (*real)(A...) = nullptr;
    static R call(A... a) {
        const uint64_t t = os::ticks();
        if constexpr (std::is_void_v<R>) {
            real(a...);
            g_ticks[ID] += os::ticks() - t;
            g_calls[ID]++;
        } else {
            R r = real(a...);
            g_ticks[ID] += os::ticks() - t;
            g_calls[ID]++;
            return r;
        }
    }
};
bool g_on;
double g_read_ticks;  // one timer read (on the 3DS a kernel call): a timed span holds one, less from each call
void start() {
#define WRAP(name, ...)                                                    \
    Prof<gltrace::T_##name, decltype(p_##name)>::real = p_##name;          \
    p_##name = &Prof<gltrace::T_##name, decltype(p_##name)>::call;
    GL_FUNCS(WRAP)
#undef WRAP
    g_on = true;
    const uint64_t t0 = os::ticks();
    for (int i = 0; i < 1000; i++) (void)os::ticks();
    g_read_ticks = double(os::ticks() - t0) / 1001;
    g_ticks0 = os::ticks(), g_ns0 = os::monotonic_ns();
    log("gl profile: on (time inside the driver per GL function, less the timer's own %.1f ticks a call)", g_read_ticks);
}
// The functions that took the most time since the last report, per frame.
void report(int frames) {
    if (!g_on || frames <= 0) return;
    const uint64_t t = os::ticks();
    const int64_t ns = os::monotonic_ns();
    const double ns_per_tick = t > g_ticks0 ? double(ns - g_ns0) / double(t - g_ticks0) : 1.0;
    g_ticks0 = t, g_ns0 = ns;
    uint32_t order[kFuncCount];
    uint64_t all = 0, raw = 0, calls = 0;
    for (uint32_t i = 0; i < kFuncCount; i++) {
        raw += g_ticks[i];
        g_ticks[i] = uint64_t(std::max(0.0, double(g_ticks[i]) - g_read_ticks * g_calls[i]));
        order[i] = i, all += g_ticks[i], calls += g_calls[i];
    }
    std::sort(order, order + kFuncCount, [](uint32_t a, uint32_t b) { return g_ticks[a] > g_ticks[b]; });
    char line[512];
    int len = std::snprintf(line, sizeof line, "gl profile: %.0f calls, %.2f ms a frame in the driver (%.2f with the timer):",
                            double(calls) / frames, double(all) * ns_per_tick / 1e6 / frames, double(raw) * ns_per_tick / 1e6 / frames);
    for (int k = 0; k < 8 && g_calls[order[k]] && len > 0 && len < int(sizeof line); k++)
        len += std::snprintf(line + len, sizeof line - size_t(len), " %s %.0f x %.2f ms", kNames[order[k]],
                             double(g_calls[order[k]]) / frames, double(g_ticks[order[k]]) * ns_per_tick / 1e6 / frames);
    log("%s", line);
    std::memset(g_ticks, 0, sizeof g_ticks);
    std::memset(g_calls, 0, sizeof g_calls);
}
}  // namespace glprofile

// ---- guest pointer translation and per-context state --------------------------------------------
const void *host(u32 p) { return p ? HOST(p) : nullptr; }
GlTextureHooks g_texture_hooks;  // gl_set_texture_hooks
template <class T> T *hostp(u32 p) { return p ? reinterpret_cast<T *>(HOST(p)) : nullptr; }

// Small arrays of 32-bit values between the game and the driver (object names, query results,
// uniform values) reach the driver in the host's byte order: on a big-endian host (the GameCube)
// In copies and swaps n values from the guest's little-endian memory, Out collects n values and
// stores them there when it goes; elsewhere both are the guest memory itself. Bulk data (buffer
// contents, texture images, client vertex arrays and indices) stays in the guest's order: the
// driver converts it anyway.
#ifdef RECOMP_BIG_ENDIAN
template <class T> struct In {
    static_assert(sizeof(T) == 4);
    std::vector<T> v;
    T *p = nullptr;
    In(u32 a, size_t n) {
        if (!a) return;
        v.resize(n);
        for (size_t i = 0; i < n; i++) {
            const uint32_t w = LD32(a + 4 * u32(i));
            std::memcpy(&v[i], &w, 4);
        }
        p = v.data();
    }
    operator T *() { return p; }
};
template <class T> struct Out {
    static_assert(sizeof(T) == 4);
    u32 a;
    std::vector<T> v;
    Out(u32 a_, size_t n) : a(a_), v(a_ ? n : 0) {}
    ~Out() {
        for (size_t i = 0; i < v.size(); i++) {
            uint32_t w;
            std::memcpy(&w, &v[i], 4);
            ST32(a + 4 * u32(i), w);
        }
    }
    operator T *() { return a ? v.data() : nullptr; }
};
#else
template <class T> struct In {
    T *p;
    In(u32 a, size_t) : p(hostp<T>(a)) {}
    operator T *() { return p; }
};
template <class T> struct Out {
    T *p;
    Out(u32 a, size_t) : p(hostp<T>(a)) {}
    operator T *() { return p; }
};
#endif
// how many values glGetIntegerv / glGetFloatv write for a name
size_t get_count(GLenum pname) {
    switch (pname) {
    case 0x0BA2: case 0x0C10: case 0x0C22: case 0x0C23: return 4;  // viewport, scissor box, clear colour, colour mask
    case 0x0D3A: case 0x846D: case 0x846E: return 2;               // max viewport dims, point size / line width ranges
    default: return 1;
    }
}

std::mutex g_lock;
u32 g_array_buffer, g_element_buffer;
std::unordered_map<u32, u32> g_buffer_size;   // buffer name -> size (for map emulation)
std::unordered_map<u32, u32> g_mapped;        // buffer name -> guest shadow memory
std::unordered_map<u32, u32> g_strings;       // glGetString results copied into guest memory

// Live texture memory per texture and level: bytes as stored here (RGB is RGBA8 on ANGLE), and
// as a 3DS would store them (ETC1 at 4 bits a pixel for RGB, ETC1A4 at 8 for RGBA, 16-bit
// render targets), for sizing ports to small machines. Game thread only.
struct TexLevel { uint64_t bytes = 0, bytes_3ds = 0; bool target = false; };
std::unordered_map<u32, std::unordered_map<int, TexLevel>> g_tex_mem;
uint64_t g_tex_bytes, g_tex_3ds, g_tex_targets;

u32 &bound(GLenum target) { return target == 0x8893 ? g_element_buffer : g_array_buffer; }

// ---- wrappers with guest signatures -------------------------------------------------------------
// Per-frame call statistics (the log's frame lines): what the game asks of the driver, and how
// much of it repeats the current state. Game thread only, so plain counters.
struct GlStats {
    uint64_t draws = 0, client_draws = 0, programs = 0, programs_same = 0, textures = 0, textures_same = 0,
             caps = 0, caps_same = 0, uniforms = 0, uniforms_same = 0;
    // resource churn: uploads (and their bytes) and deletions
    uint64_t state = 0, state_same = 0;  // the other state calls below (skipped when unchanged)
    uint64_t tex_uploads = 0, tex_bytes = 0, buf_uploads = 0, buf_bytes = 0, tex_deleted = 0, buf_deleted = 0;
} g_stats;
GLuint g_cur_program = ~0u, g_cur_unit = 0, g_cur_tex[32];
void tex_account(GLint level, TexLevel t) {
    TexLevel &old = g_tex_mem[g_cur_tex[g_cur_unit]][level];
    g_tex_bytes += t.bytes - old.bytes, g_tex_3ds += t.bytes_3ds - old.bytes_3ds;
    g_tex_targets += (t.target ? t.bytes : 0) - (old.target ? old.bytes : 0);
    old = t;
}
// Uniform values: a program keeps its uniforms' values (until it is linked again), and the game
// sets every uniform before each draw, most of them to the value already there (the same
// matrices and colours for every draw of a material). Calls that would not change the
// program's value are skipped: each is a driver call (Mesa on the Switch: about 2 us).
std::unordered_map<GLuint, std::unordered_map<GLint, std::vector<uint8_t>>> g_uniforms;  // program -> location -> type + bytes
std::unordered_map<GLint, std::vector<uint8_t>> *g_cur_uniforms;                        // the current program's

// False when `loc` of the current program already holds these bytes; otherwise records them.
bool uniform_changes(GLint loc, char type, const void *data, size_t size) {
    g_stats.uniforms++;
    if (loc < 0 || !g_cur_uniforms) return true;
    std::vector<uint8_t> &v = (*g_cur_uniforms)[loc];
    if (v.size() == size + 1 && v[0] == uint8_t(type) && std::memcmp(v.data() + 1, data, size) == 0) {
        g_stats.uniforms_same++;
        return false;
    }
    v.resize(size + 1);
    v[0] = uint8_t(type);
    std::memcpy(v.data() + 1, data, size);
    return true;
}
// A pooled program handed out again: every uniform set before goes back to 0 (a link's state).
void reset_uniforms(GLuint program) {
    auto it = g_uniforms.find(program);
    if (it == g_uniforms.end()) return;
    p_glUseProgram(program);
    static const std::vector<uint8_t> zeros(4096);
    for (const auto &[loc, v] : it->second) {
        const GLsizei bytes = GLsizei(v.size() - 1);
        const auto *f = reinterpret_cast<const GLfloat *>(zeros.data());
        const auto *i = reinterpret_cast<const GLint *>(zeros.data());
        if (bytes > GLsizei(zeros.size())) continue;
        switch (v[0]) {
        case 'f': p_glUniform1f(loc, 0); break;
        case 'i': p_glUniform1i(loc, 0); break;
        case 'F': p_glUniform1fv(loc, bytes / 4, f); break;
        case 'I': p_glUniform1iv(loc, bytes / 4, i); break;
        case '2': p_glUniform2fv(loc, bytes / 8, f); break;
        case 'j': p_glUniform2iv(loc, bytes / 8, i); break;
        case '3': p_glUniform3fv(loc, bytes / 12, f); break;
        case 'k': p_glUniform3iv(loc, bytes / 12, i); break;
        case '4': p_glUniform4fv(loc, bytes / 16, f); break;
        case 'l': p_glUniform4iv(loc, bytes / 16, i); break;
        case 'M': case 'T': p_glUniformMatrix4fv(loc, bytes / 64, GLboolean(0), f); break;
        default: break;
        }
    }
    g_uniforms.erase(it);
    if (g_cur_program != program) p_glUseProgram(g_cur_program == ~0u ? 0 : g_cur_program);
    else g_cur_uniforms = &g_uniforms[program];
}
void forget_uniforms(GLuint program) {
    g_uniforms.erase(program);
    if (g_cur_program == program) g_cur_uniforms = nullptr;
}

uint32_t g_attr_client = 0;   // vertex attributes set from client memory (bit per index)
uint32_t g_attr_enabled = 0;
uint8_t g_cap_on[0x10000 / 8];
bool g_cap_known[0x10000];
// State calls that set what is already set are skipped (the game sets the whole vertex layout,
// buffer and blending for every draw): each would be a driver call, and for the vertex layout
// a revalidation at the next draw. Only the game changes this state (the runtime's own GL calls
// leave it alone), and the defaults below are GL's initial state.
struct AttribPointer {
    bool set = false;
    GLint size;
    GLenum type;
    uint32_t norm;
    GLsizei stride;
    u32 pointer;
    GLuint buffer;
};
AttribPointer g_attr_pointer[32];
GLfloat g_attr_value[32][4];
bool g_attr_value_set[32];
GLenum g_blend[4];
bool g_blend_set = false;
int g_depth_mask = -1, g_front_face = -1;
bool state_same(bool same) {
    g_stats.state++;
    g_stats.state_same += same;
    return same;
}

void w_glBindBuffer(GLenum t, GLuint b) {
    if (state_same(bound(t) == b)) return;
    bound(t) = b;
    p_glBindBuffer(t, b);
}
void w_glBufferData(GLenum t, int32_t size, u32 data, GLenum usage) {
    { std::lock_guard<std::mutex> l(g_lock); g_buffer_size[bound(t)] = u32(size); }
    g_stats.buf_uploads++;
    g_stats.buf_bytes += u32(size);
    p_glBufferData(t, size, host(data), usage);
}
void w_glBufferSubData(GLenum t, int32_t off, int32_t size, u32 data) {
    g_stats.buf_uploads++;
    g_stats.buf_bytes += u32(size);
    p_glBufferSubData(t, off, size, host(data)); }
// glMapBufferOES: ANGLE's mapping is host memory the guest cannot address, so hand out a
// guest shadow buffer and upload it on unmap (ES2 only allows write-only mappings).
u32 w_glMapBufferOES(GLenum t, GLenum) {
    std::lock_guard<std::mutex> l(g_lock);
    u32 b = bound(t);
    u32 &shadow = g_mapped[b];
    if (!shadow) shadow = gmalloc(std::max<u32>(g_buffer_size[b], 4u));
    return shadow;
}
uint32_t w_glUnmapBufferOES(GLenum t) {
    u32 b, shadow, size;
    {
        std::lock_guard<std::mutex> l(g_lock);
        b = bound(t);
        auto it = g_mapped.find(b);
        if (it == g_mapped.end()) return 0;
        shadow = it->second;
        size = g_buffer_size[b];
        g_mapped.erase(it);
    }
    p_glBufferSubData(t, 0, size, HOST(shadow));
    gfree(shadow);
    return 1;
}
void w_glDeleteBuffers(GLsizei n, u32 names) {
    {
        std::lock_guard<std::mutex> l(g_lock);
        for (GLsizei i = 0; i < n; i++) {
            u32 b = LD32(names + 4 * u32(i));
            g_buffer_size.erase(b);
            if (g_array_buffer == b) g_array_buffer = 0;
            if (g_element_buffer == b) g_element_buffer = 0;
            for (AttribPointer &a : g_attr_pointer)  // attributes reading it go back to no buffer
                if (a.set && a.buffer == b) a.set = false;
        }
    }
    g_stats.buf_deleted += u32(n);
    p_glDeleteBuffers(n, In<GLuint>(names, size_t(n)));
}
void w_glVertexAttribPointer(GLuint i, GLint size, GLenum type, uint32_t norm, GLsizei stride, u32 p) {
    if (i < 32) {
        AttribPointer &a = g_attr_pointer[i];
        if (state_same(a.set && a.size == size && a.type == type && a.norm == norm && a.stride == stride && a.pointer == p &&
                       a.buffer == g_array_buffer))
            return;
        a = {true, size, type, norm, stride, p, g_array_buffer};
        g_attr_client = g_array_buffer ? g_attr_client & ~(1u << i) : g_attr_client | (1u << i);
    }
    // with an array buffer bound the "pointer" is a byte offset
    p_glVertexAttribPointer(i, size, type, GLboolean(norm), stride,
                            g_array_buffer ? reinterpret_cast<const void *>(uintptr_t(p)) : host(p));
}
void w_glDrawElements(GLenum mode, GLsizei count, GLenum type, u32 p) {
    g_stats.draws++;
    g_stats.client_draws += (g_attr_client & g_attr_enabled) != 0 || !g_element_buffer;
    p_glDrawElements(mode, count, type, g_element_buffer ? reinterpret_cast<const void *>(uintptr_t(p)) : host(p));
}
// Uniforms are declared highp: the game declares some with different precisions in the
// vertex and fragment shaders (NV_Lightmap: WaveTime highp / lowp), which phone drivers
// accepted and ANGLE refuses to link ("Precisions of uniform 'WaveTime' differ"); the game
// then drew those surfaces as pink wireframe. Direct3D computes in 32-bit float anyway.
std::string raise_uniform_precision(std::string text) {
    static const char *const lower[] = {"lowp", "mediump"};
    for (size_t at = text.find("uniform"); at != std::string::npos; at = text.find("uniform", at + 7)) {
        if (at > 0 && (std::isalnum(uint8_t(text[at - 1])) || text[at - 1] == '_')) continue;
        size_t q = at + 7;
        while (q < text.size() && (text[q] == ' ' || text[q] == '	')) q++;
        for (const char *p : lower) {
            const size_t n = std::strlen(p);
            if (text.compare(q, n, p) == 0 && q + n < text.size() && std::isspace(uint8_t(text[q + n]))) {
                text.replace(q, n, "highp");
                break;
            }
        }
    }
    return text;
}

// Program binary cache (gl_set_shader_cache): Mesa (the Switch) and ANGLE compile every GLSL
// shader on the CPU while a level loads. With a cache folder and a driver that offers program
// binaries, glCompileShader is deferred (reported as compiled); glLinkProgram hashes the
// program's sources with the driver's identity and loads the saved binary, or compiles, links
// and saves it. A binary the driver rejects falls back to compiling.
using GetProgramBinaryFn = void (*)(GLuint, GLsizei, GLsizei *, GLenum *, void *);
using ProgramBinaryFn = void (*)(GLuint, GLenum, const void *, GLsizei);
GetProgramBinaryFn p_glGetProgramBinary;
ProgramBinaryFn p_glProgramBinary;
std::string g_cache_dir;            // empty: no cache
bool g_cache_on = false;            // decided at the first link
std::unordered_map<GLuint, std::string> g_shader_src;
std::unordered_map<GLuint, bool> g_deferred;              // shader -> compile deferred
std::unordered_map<GLuint, std::vector<GLuint>> g_attached;  // program -> shaders
uint64_t g_driver_hash = 0;
int g_cache_hits = 0, g_cache_misses = 0;

// Program pool: the game deletes its materials' programs at every level and section load and
// links the same sources again (a chapter linked 519 programs, about 220 different ones); on
// the Switch, without program binaries, each was compiled and linked anew (Mesa: 2 ms a
// shader, 7 ms a link). A deleted program stays linked here, and a later program with the
// same sources uses it instead (its name is an alias for the pooled one): no compile, no
// link. The uniforms the previous user set go back to 0 first, as after a link.
std::unordered_map<GLuint, GLuint> g_alias;                 // the game's program -> the pooled one it uses
std::unordered_map<GLuint, uint64_t> g_program_key;         // linked program -> its sources' key
std::unordered_map<uint64_t, std::vector<GLuint>> g_pool;  // key -> deleted programs kept linked
int g_pool_hits = 0;

// Programs from earlier runs (without program binaries: the Switch): every program linked is
// recorded (shader types and sources, in attach order) in programs.list in the cache folder,
// and the next run compiles them into the pool while the game sits in its menus
// (gl_prewarm_step, one a frame), so a level's first load finds them linked.
std::unordered_map<GLuint, GLenum> g_shader_type;
std::unordered_set<uint64_t> g_recorded;  // keys in programs.list
struct Recorded {
    uint64_t key;
    std::vector<std::pair<GLenum, std::string>> shaders;
};
std::deque<Recorded> g_prewarm;           // read from programs.list, not compiled yet
bool g_list_loaded = false;
int g_prewarmed = 0;

uint64_t fnv1a(const void *data, size_t n, uint64_t h = 1469598103934665603ull) {
    for (size_t i = 0; i < n; i++) h = (h ^ static_cast<const uint8_t *>(data)[i]) * 1099511628211ull;
    return h;
}
uint64_t sources_key(GLuint program) {
    uint64_t key = 1469598103934665603ull;
    for (GLuint s : g_attached[program]) {
        const std::string &src = g_shader_src[s];
        key = fnv1a(src.data(), src.size(), key) * 31 + src.size();
    }
    return key;
}
// The GL program behind a game program name.
GLuint prog(GLuint program) {
    auto it = g_alias.find(program);
    return it == g_alias.end() ? program : it->second;
}
std::string program_list() { return g_cache_dir + "/programs.list"; }
void load_program_list() {
    if (g_list_loaded || g_cache_dir.empty()) return;
    g_list_loaded = true;
    FILE *f = std::fopen(program_list().c_str(), "rb");
    if (!f) return;
    uint32_t n;
    while (std::fread(&n, 4, 1, f) == 1 && n > 0 && n < 8) {
        Recorded r;
        uint64_t key = 1469598103934665603ull;
        bool ok = true;
        for (uint32_t i = 0; i < n && ok; i++) {
            uint32_t type = 0, len = 0;
            ok = std::fread(&type, 4, 1, f) == 1 && std::fread(&len, 4, 1, f) == 1 && len < (1u << 20);
            std::string src(ok ? len : 0, '\0');
            ok = ok && std::fread(src.data(), 1, len, f) == len;
            key = fnv1a(src.data(), src.size(), key) * 31 + src.size();
            r.shaders.emplace_back(GLenum(type), std::move(src));
        }
        if (!ok) break;  // a cut-off last record
        r.key = key;
        if (g_recorded.insert(key).second) g_prewarm.push_back(std::move(r));
    }
    std::fclose(f);
    if (!g_prewarm.empty()) log("gl: %zu programs from earlier runs to compile ahead", g_prewarm.size());
}
void record_program(GLuint program, uint64_t key) {
    if (g_cache_dir.empty()) return;
    load_program_list();
    if (!g_recorded.insert(key).second) return;
    FILE *f = std::fopen(program_list().c_str(), "ab");
    if (!f) return;
    const std::vector<GLuint> &shaders = g_attached[program];
    const uint32_t n = uint32_t(shaders.size());
    std::fwrite(&n, 4, 1, f);
    for (GLuint sh : shaders) {
        const std::string &src = g_shader_src[sh];
        const uint32_t type = g_shader_type[sh], len = uint32_t(src.size());
        std::fwrite(&type, 4, 1, f);
        std::fwrite(&len, 4, 1, f);
        std::fwrite(src.data(), 1, src.size(), f);
    }
    std::fclose(f);
}
bool cache_active() {
    static bool decided = false;
    if (!decided) {
        decided = true;
        GLint formats = 0;
        if (!g_cache_dir.empty() && p_glProgramBinary && p_glGetProgramBinary && !gltrace::g_file) {  // (a trace needs the sources)
            p_glGetIntegerv(0x87FE /*GL_NUM_PROGRAM_BINARY_FORMATS*/, &formats);
            for (GLenum n : {GLenum(0x1F00), GLenum(0x1F01), GLenum(0x1F02)})
                if (const char *s = reinterpret_cast<const char *>(p_glGetString(n))) g_driver_hash = fnv1a(s, std::strlen(s), g_driver_hash ? g_driver_hash : 1469598103934665603ull);
        }
        g_cache_on = formats > 0;
        log("gl: shader cache %s%s", g_cache_on ? "in " : "off", g_cache_on ? g_cache_dir.c_str() : "");
    }
    return g_cache_on;
}
void compile_now(GLuint shader) {
    p_glCompileShader(shader);
    GLint ok = 1;
    p_glGetShaderiv(shader, 0x8B81 /* GL_COMPILE_STATUS */, &ok);
    if (ok) return;
    char info[1024] = {};
    p_glGetShaderInfoLog(shader, sizeof info, nullptr, info);
    log("gl: shader %u failed to compile: %s", shader, info);
}

void w_glShaderSource(GLuint s, GLsizei count, u32 strings, u32 lengths) {
    std::vector<std::string> text(size_t(std::max<GLsizei>(count, 0)));
    std::vector<const char *> src(text.size());
    std::vector<GLint> len(text.size());
    for (GLsizei i = 0; i < count; i++) {
        const char *part = str(LD32(strings + 4 * u32(i)));
        const GLint n = lengths ? int32_t(LD32(lengths + 4 * u32(i))) : -1;
        text[size_t(i)] = raise_uniform_precision(n < 0 ? std::string(part) : std::string(part, size_t(n)));
        src[size_t(i)] = text[size_t(i)].c_str();
        len[size_t(i)] = GLint(text[size_t(i)].size());
    }
    p_glShaderSource(s, count, src.data(), len.data());
    std::string all;
    for (const std::string &t : text) all += t;
    g_shader_src[s] = std::move(all);
}
u32 w_glGetString(GLenum name) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_strings.find(name);
    if (it != g_strings.end()) return it->second;
    const char *s = p_glGetString(name);
    if (low_end_device() && (name == 0x1F00 || name == 0x1F01))  // vendor, renderer: the game's GPU_0 profile
        s = name == 0x1F00 ? "Qualcomm" : "Adreno (TM) 200";
    std::string ext;
    if (name == 0x1F03 && gltrace::g_etc1 && s) {  // (the game's parser wants each name followed by one space)
        ext = s;
        if (!ext.empty() && ext.back() != ' ') ext += ' ';
        s = (ext += "GL_OES_compressed_ETC1_RGB8_texture ").c_str();
    }
    return g_strings[name] = s ? gstrdup(s) : 0;
}

void w_glTexImage3DOES(GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLsizei d, GLint b, GLenum fmt, GLenum type, u32 p) {
    if (p_glTexImage3DOES) p_glTexImage3DOES(t, l, f, w, h, d, b, fmt, type, host(p));
}
void w_glTexSubImage3DOES(GLenum t, GLint l, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum fmt, GLenum type, u32 p) {
    if (p_glTexSubImage3DOES) p_glTexSubImage3DOES(t, l, x, y, z, w, h, d, fmt, type, host(p));
}
void w_glCompressedTexImage3DOES(GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLsizei d, GLint b, GLsizei size, u32 p) {
    if (p_glCompressedTexImage3DOES) p_glCompressedTexImage3DOES(t, l, f, w, h, d, b, size, host(p));
}
void w_glCompressedTexSubImage3DOES(GLenum t, GLint l, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum fmt, GLsizei size, u32 p) {
    if (p_glCompressedTexSubImage3DOES) p_glCompressedTexSubImage3DOES(t, l, x, y, z, w, h, d, fmt, size, host(p));
}

// Straight forwards: scalar arguments, or pointers that are plain client memory.
#define B(x) GLboolean(x)
void w_glActiveTexture(GLenum a) {
    if (state_same(g_cur_unit == ((a - 0x84C0 /*TEXTURE0*/) & 31))) return;
    g_cur_unit = (a - 0x84C0) & 31;
    p_glActiveTexture(a);
}
void w_glAttachShader(GLuint a, GLuint b) {
    g_attached[a].push_back(b);
    p_glAttachShader(a, b);
}
void w_glBindFramebuffer(GLenum a, GLuint b) { p_glBindFramebuffer(a, b); }
void w_glBindRenderbuffer(GLenum a, GLuint b) { p_glBindRenderbuffer(a, b); }
void w_glBindTexture(GLenum a, GLuint b) {
    g_stats.textures++;
    g_stats.textures_same += g_cur_tex[g_cur_unit] == b;
    g_cur_tex[g_cur_unit] = b;
    p_glBindTexture(a, b);
}
void w_glBlendColor(GLfloat a, GLfloat b, GLfloat c, GLfloat d) { p_glBlendColor(a, b, c, d); }
void w_glBlendEquation(GLenum a) { p_glBlendEquation(a); }
void w_glBlendEquationSeparate(GLenum a, GLenum b) { p_glBlendEquationSeparate(a, b); }
void w_glBlendFunc(GLenum a, GLenum b) {
    if (state_same(g_blend_set && g_blend[0] == a && g_blend[1] == b && g_blend[2] == a && g_blend[3] == b)) return;
    g_blend_set = true;
    g_blend[0] = g_blend[2] = a, g_blend[1] = g_blend[3] = b;
    p_glBlendFunc(a, b);
}
void w_glBlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d) {
    if (state_same(g_blend_set && g_blend[0] == a && g_blend[1] == b && g_blend[2] == c && g_blend[3] == d)) return;
    g_blend_set = true;
    g_blend[0] = a, g_blend[1] = b, g_blend[2] = c, g_blend[3] = d;
    p_glBlendFuncSeparate(a, b, c, d);
}
GLenum w_glCheckFramebufferStatus(GLenum a) { return p_glCheckFramebufferStatus(a); }
void w_glClear(GLbitfield a) { p_glClear(a); }
void w_glClearColor(GLfloat a, GLfloat b, GLfloat c, GLfloat d) { p_glClearColor(a, b, c, d); }
void w_glClearDepthf(GLfloat a) { p_glClearDepthf(a); }
void w_glClearStencil(GLint a) { p_glClearStencil(a); }
void w_glColorMask(u32 a, u32 b, u32 c, u32 d) { p_glColorMask(B(a), B(b), B(c), B(d)); }
void w_glCompileShader(GLuint a) {
    g_deferred[a] = true;  // compiled at link time, if the program is neither pooled nor cached
}
// Texture uploads the driver refused (the first few): they leave the texture incomplete.
void check_upload(const char *what, GLenum target, GLint level, GLenum format, GLsizei w, GLsizei h) {
    static int reported;
    if (reported >= 20) return;
    if (GLenum e = p_glGetError()) {
        reported++;
        log("gl: %s(target %04x, level %d, format %04x, %dx%d) failed: error %04x", what, target, level, format, w, h, e);
    }
}
void w_glCompressedTexImage2D(GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b, GLsizei size, u32 p) {
    tex_account(l, {u32(size), u32(size), false});
    g_stats.tex_uploads++;
    g_stats.tex_bytes += u32(size);
    p_glCompressedTexImage2D(t, l, f, w, h, b, size, host(p));
    check_upload("glCompressedTexImage2D", t, l, f, w, h);
}
void w_glCompressedTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLsizei size, u32 p) {
    p_glCompressedTexSubImage2D(t, l, x, y, w, h, f, size, host(p));
}
void w_glCopyTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h) {
    p_glCopyTexSubImage2D(t, l, xo, yo, x, y, w, h);
}
GLuint w_glCreateProgram() { return p_glCreateProgram(); }
GLuint w_glCreateShader(GLenum a) {
    const GLuint sh = p_glCreateShader(a);
    g_shader_type[sh] = a;
    return sh;
}
void w_glCullFace(GLenum a) { p_glCullFace(a); }
void w_glDeleteFramebuffers(GLsizei n, u32 p) { p_glDeleteFramebuffers(n, In<GLuint>(p, size_t(n))); }
void pool_put(GLuint real) {
    auto key = g_program_key.find(real);
    if (key != g_program_key.end()) g_pool[key->second].push_back(real);
}
void w_glDeleteProgram(GLuint a) {
    g_attached.erase(a);
    if (auto it = g_alias.find(a); it != g_alias.end()) {  // a borrowed program goes back
        pool_put(it->second);
        g_alias.erase(it);
        p_glDeleteProgram(a);
        return;
    }
    if (g_program_key.count(a)) {  // linked: kept for the next program with these sources
        pool_put(a);
        return;
    }
    forget_uniforms(a);
    p_glDeleteProgram(a);
}
void w_glDeleteRenderbuffers(GLsizei n, u32 p) { p_glDeleteRenderbuffers(n, In<GLuint>(p, size_t(n))); }
void w_glDeleteShader(GLuint a) {
    g_shader_type.erase(a);
    g_shader_src.erase(a);
    g_deferred.erase(a);
    p_glDeleteShader(a);
}
void w_glDeleteTextures(GLsizei n, u32 p) {
    g_stats.tex_deleted += u32(n);
    for (GLsizei i = 0; i < n; i++) {
        auto it = g_tex_mem.find(LD32(p + 4 * u32(i)));
        if (it == g_tex_mem.end()) continue;
        for (auto &[level, t] : it->second) g_tex_bytes -= t.bytes, g_tex_3ds -= t.bytes_3ds, g_tex_targets -= t.target ? t.bytes : 0;
        g_tex_mem.erase(it);
    }
    p_glDeleteTextures(n, In<GLuint>(p, size_t(n)));
}
void w_glDepthFunc(GLenum a) { p_glDepthFunc(a); }
void w_glDepthMask(u32 a) {
    if (state_same(g_depth_mask == int(B(a)))) return;
    g_depth_mask = B(a);
    p_glDepthMask(B(a));
}
void w_glDepthRangef(GLfloat a, GLfloat b) { p_glDepthRangef(a, b); }
void w_glDisable(GLenum a) {
    g_stats.caps++;
    if (a < 0x10000) {
        g_stats.caps_same += g_cap_known[a] && !(g_cap_on[a / 8] >> (a % 8) & 1);
        g_cap_known[a] = true;
        g_cap_on[a / 8] &= uint8_t(~(1u << (a % 8)));
    }
    p_glDisable(a);
}
void w_glDisableVertexAttribArray(GLuint a) {
    if (a < 32 && state_same(!(g_attr_enabled >> a & 1))) return;
    if (a < 32) g_attr_enabled &= ~(1u << a);
    p_glDisableVertexAttribArray(a);
}
void w_glDrawArrays(GLenum m, GLint f, GLsizei c) {
    g_stats.draws++;
    g_stats.client_draws += (g_attr_client & g_attr_enabled) != 0;
    p_glDrawArrays(m, f, c);
}
void w_glEnable(GLenum a) {
    g_stats.caps++;
    if (a < 0x10000) {
        g_stats.caps_same += g_cap_known[a] && (g_cap_on[a / 8] >> (a % 8) & 1);
        g_cap_known[a] = true;
        g_cap_on[a / 8] |= uint8_t(1u << (a % 8));
    }
    p_glEnable(a);
}
void w_glEnableVertexAttribArray(GLuint a) {
    if (a < 32 && state_same(g_attr_enabled >> a & 1)) return;
    if (a < 32) g_attr_enabled |= 1u << a;
    p_glEnableVertexAttribArray(a);
}
void w_glFlush() { p_glFlush(); }
void w_glFramebufferRenderbuffer(GLenum a, GLenum b, GLenum c, GLuint d) { p_glFramebufferRenderbuffer(a, b, c, d); }
void w_glFramebufferTexture2D(GLenum a, GLenum b, GLenum c, GLuint d, GLint e) { p_glFramebufferTexture2D(a, b, c, d, e); }
void w_glFrontFace(GLenum a) {
    if (state_same(g_front_face == int(a))) return;
    g_front_face = int(a);
    p_glFrontFace(a);
}
void w_glGenBuffers(GLsizei n, u32 p) { p_glGenBuffers(n, Out<GLuint>(p, size_t(n))); }
void w_glGenFramebuffers(GLsizei n, u32 p) { p_glGenFramebuffers(n, Out<GLuint>(p, size_t(n))); }
void w_glGenRenderbuffers(GLsizei n, u32 p) { p_glGenRenderbuffers(n, Out<GLuint>(p, size_t(n))); }
void w_glGenTextures(GLsizei n, u32 p) { p_glGenTextures(n, Out<GLuint>(p, size_t(n))); }
void w_glGenerateMipmap(GLenum a) {
    if (g_texture_hooks.generate_mipmap && g_texture_hooks.generate_mipmap(a)) return;
    if (auto it = g_tex_mem.find(g_cur_tex[g_cur_unit]); it != g_tex_mem.end() && it->second.count(0)) {
        const TexLevel l0 = it->second[0];
        tex_account(1, {l0.bytes / 3, l0.bytes_3ds / 3, l0.target});  // the whole chain, as one level
    }
    p_glGenerateMipmap(a);
}
void w_glGetActiveAttrib(GLuint p, GLuint i, GLsizei n, u32 len, u32 size, u32 type, u32 name) {
    p_glGetActiveAttrib(prog(p), i, n, Out<GLsizei>(len, 1), Out<GLint>(size, 1), Out<GLenum>(type, 1), hostp<char>(name));
}
void w_glGetActiveUniform(GLuint p, GLuint i, GLsizei n, u32 len, u32 size, u32 type, u32 name) {
    p_glGetActiveUniform(prog(p), i, n, Out<GLsizei>(len, 1), Out<GLint>(size, 1), Out<GLenum>(type, 1), hostp<char>(name));
}
GLint w_glGetAttribLocation(GLuint p, u32 name) { return p_glGetAttribLocation(prog(p), str(name)); }
GLenum w_glGetError() { return p_glGetError(); }
void w_glGetFloatv(GLenum a, u32 p) { p_glGetFloatv(a, Out<GLfloat>(p, get_count(a))); }
void w_glGetIntegerv(GLenum a, u32 p) { p_glGetIntegerv(a, Out<GLint>(p, get_count(a))); }
void w_glGetProgramInfoLog(GLuint a, GLsizei n, u32 len, u32 log) { p_glGetProgramInfoLog(prog(a), n, Out<GLsizei>(len, 1), hostp<char>(log)); }
void w_glGetProgramiv(GLuint a, GLenum b, u32 p) { p_glGetProgramiv(prog(a), b, Out<GLint>(p, 1)); }
void w_glGetShaderInfoLog(GLuint a, GLsizei n, u32 len, u32 log) {
    if (g_deferred.count(a)) {  // not compiled (yet): no messages
        if (len) ST32(len, 0);
        if (log && n > 0) ST8(log, 0);
        return;
    }
    p_glGetShaderInfoLog(a, n, Out<GLsizei>(len, 1), hostp<char>(log));
}
void w_glGetShaderSource(GLuint a, GLsizei n, u32 len, u32 src) { p_glGetShaderSource(a, n, Out<GLsizei>(len, 1), hostp<char>(src)); }
void w_glGetShaderiv(GLuint a, GLenum b, u32 p) {
    if (g_deferred.count(a) && (b == 0x8B81 /*COMPILE_STATUS*/ || b == 0x8B84 /*INFO_LOG_LENGTH*/)) {
        ST32(p, b == 0x8B81 ? 1 : 0);
        return;
    }
    p_glGetShaderiv(a, b, Out<GLint>(p, 1));
}
GLint w_glGetUniformLocation(GLuint p, u32 name) { return p_glGetUniformLocation(prog(p), str(name)); }
void w_glLineWidth(GLfloat a) { p_glLineWidth(a); }
void w_glLinkProgram(GLuint a) {
    if (auto it = g_alias.find(a); it != g_alias.end()) {  // linked again: give the borrowed one back
        pool_put(it->second);
        g_alias.erase(it);
    }
    forget_uniforms(a);  // linking resets the values
    g_program_key.erase(a);
    if (g_cur_program == a) g_cur_uniforms = &g_uniforms[a];
    const uint64_t sources = sources_key(a);
    if (auto pooled = g_pool.find(sources); pooled != g_pool.end() && !pooled->second.empty()) {
        const GLuint real = pooled->second.back();
        pooled->second.pop_back();
        reset_uniforms(real);
        g_alias[a] = real;
        g_pool_hits++;
        return;
    }
    if (!cache_active()) {
        for (GLuint s : g_attached[a])
            if (g_deferred.erase(s)) compile_now(s);
        p_glLinkProgram(a);
        GLint ok = 0;
        p_glGetProgramiv(a, 0x8B82 /*LINK_STATUS*/, &ok);
        if (ok) {
            g_program_key[a] = sources;
            record_program(a, sources);
        }
        return;
    }
    g_program_key[a] = sources;  // (dropped again below if it does not link)
    uint64_t key = g_driver_hash;
    for (GLuint s : g_attached[a]) {
        const std::string &src = g_shader_src[s];
        key = fnv1a(src.data(), src.size(), key) * 31 + src.size();
    }
    char name[32];
    std::snprintf(name, sizeof name, "/%016llx.bin", (unsigned long long)key);
    const std::string path = g_cache_dir + name;
    GLint ok = 0;
    if (FILE *f = std::fopen(path.c_str(), "rb")) {  // a hit: no compiling
        std::vector<uint8_t> data;
        uint8_t chunk[65536];
        size_t n;
        while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) data.insert(data.end(), chunk, chunk + n);
        std::fclose(f);
        if (data.size() > 4) {
            GLenum format;
            std::memcpy(&format, data.data(), 4);
            p_glProgramBinary(a, format, data.data() + 4, GLsizei(data.size() - 4));
            p_glGetProgramiv(a, 0x8B82 /*LINK_STATUS*/, &ok);
        }
        if (ok) {
            g_cache_hits++;
            return;
        }
    }
    for (GLuint s : g_attached[a])  // a miss (or a rejected binary): compile and link as usual
        if (g_deferred.erase(s)) compile_now(s);
    p_glLinkProgram(a);
    p_glGetProgramiv(a, 0x8B82 /*LINK_STATUS*/, &ok);
    g_cache_misses++;
    if (!ok) {
        g_program_key.erase(a);
        return;
    }
    GLint size = 0;
    p_glGetProgramiv(a, 0x8741 /*PROGRAM_BINARY_LENGTH*/, &size);
    if (size <= 0) return;
    std::vector<uint8_t> data(size_t(size) + 4);
    GLenum format = 0;
    GLsizei got = 0;
    p_glGetProgramBinary(a, size, &got, &format, data.data() + 4);
    if (got <= 0) return;
    std::memcpy(data.data(), &format, 4);
    if (FILE *f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(data.data(), 1, size_t(got) + 4, f);
        std::fclose(f);
    }
}
void w_glPixelStorei(GLenum a, GLint b) { p_glPixelStorei(a, b); }
void w_glPolygonOffset(GLfloat a, GLfloat b) { p_glPolygonOffset(a, b); }
void w_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, u32 p) { p_glReadPixels(x, y, w, h, f, t, hostp<void>(p)); }
void w_glRenderbufferStorage(GLenum a, GLenum b, GLsizei c, GLsizei d) { p_glRenderbufferStorage(a, b, c, d); }
void w_glSampleCoverage(GLfloat a, u32 b) { p_glSampleCoverage(a, B(b)); }
void w_glScissor(GLint a, GLint b, GLsizei c, GLsizei d) { p_glScissor(a, b, c, d); }
void w_glStencilFunc(GLenum a, GLint b, GLuint c) { p_glStencilFunc(a, b, c); }
void w_glStencilMask(GLuint a) { p_glStencilMask(a); }
void w_glStencilOp(GLenum a, GLenum b, GLenum c) { p_glStencilOp(a, b, c); }
void w_glTexImage2D(GLenum t, GLint l, GLint f, GLsizei w, GLsizei h, GLint b, GLenum fmt, GLenum type, u32 p) {
    if (g_texture_hooks.tex_image && g_texture_hooks.tex_image(t, l, f, w, h, fmt, type, host(p))) return;
    {
        const uint64_t px = uint64_t(w) * uint64_t(h);
        if (!p) tex_account(l, {px * 4, px * 2, true});  // render target
        else tex_account(l, {px * 4, fmt == 0x1907 /*RGB*/ ? px / 2 : fmt == 0x1908 /*RGBA*/ ? px : px * 2, false});
    }
    g_stats.tex_uploads++;
    g_stats.tex_bytes += uint64_t(w) * uint64_t(h) * 4;  // (at most)
    p_glTexImage2D(t, l, f, w, h, b, fmt, type, host(p));
    check_upload("glTexImage2D", t, l, GLenum(f), w, h);
}
// Anisotropic filtering (launcher / nova3.ini): added whenever the game picks a mipmapped
// minification filter, so it applies to exactly the textures that have mipmaps.
float g_anisotropy = 1;  // effective level after gl_load (1 = off or unsupported)
void apply_anisotropy(GLenum target, GLenum pname, GLint value) {
    if (g_anisotropy > 1 && pname == 0x2801 /*MIN_FILTER*/ && value >= 0x2700 && value <= 0x2703 /**_MIPMAP_**/)
        p_glTexParameterf(target, 0x84FE /*TEXTURE_MAX_ANISOTROPY_EXT*/, g_anisotropy);
}
void w_glTexParameterf(GLenum a, GLenum b, GLfloat c) { p_glTexParameterf(a, b, c); apply_anisotropy(a, b, GLint(c)); }
void w_glTexParameteri(GLenum a, GLenum b, GLint c) { p_glTexParameteri(a, b, c); apply_anisotropy(a, b, c); }
void w_glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, u32 p) {
    p_glTexSubImage2D(t, l, x, y, w, h, fmt, type, host(p));
}
void w_glUniform1f(GLint a, GLfloat b) { if (uniform_changes(a, 'f', &b, 4)) p_glUniform1f(a, b); }
void w_glUniform1fv(GLint a, GLsizei n, u32 p) { if (uniform_changes(a, 'F', hostp<GLfloat>(p), size_t(n) * 4)) p_glUniform1fv(a, n, In<GLfloat>(p, size_t(n) * 1)); }
void w_glUniform1i(GLint a, GLint b) { if (uniform_changes(a, 'i', &b, 4)) p_glUniform1i(a, b); }
void w_glUniform1iv(GLint a, GLsizei n, u32 p) { if (uniform_changes(a, 'I', hostp<GLint>(p), size_t(n) * 4)) p_glUniform1iv(a, n, In<GLint>(p, size_t(n) * 1)); }
void w_glUniform2fv(GLint a, GLsizei n, u32 p) { if (uniform_changes(a, '2', hostp<GLfloat>(p), size_t(n) * 8)) p_glUniform2fv(a, n, In<GLfloat>(p, size_t(n) * 2)); }
void w_glUniform2iv(GLint a, GLsizei n, u32 p) { if (uniform_changes(a, 'j', hostp<GLint>(p), size_t(n) * 8)) p_glUniform2iv(a, n, In<GLint>(p, size_t(n) * 2)); }
void w_glUniform3fv(GLint a, GLsizei n, u32 p) { if (uniform_changes(a, '3', hostp<GLfloat>(p), size_t(n) * 12)) p_glUniform3fv(a, n, In<GLfloat>(p, size_t(n) * 3)); }
void w_glUniform3iv(GLint a, GLsizei n, u32 p) { if (uniform_changes(a, 'k', hostp<GLint>(p), size_t(n) * 12)) p_glUniform3iv(a, n, In<GLint>(p, size_t(n) * 3)); }
void w_glUniform4fv(GLint a, GLsizei n, u32 p) { if (uniform_changes(a, '4', hostp<GLfloat>(p), size_t(n) * 16)) p_glUniform4fv(a, n, In<GLfloat>(p, size_t(n) * 4)); }
void w_glUniform4iv(GLint a, GLsizei n, u32 p) { if (uniform_changes(a, 'l', hostp<GLint>(p), size_t(n) * 16)) p_glUniform4iv(a, n, In<GLint>(p, size_t(n) * 4)); }
void w_glUniformMatrix4fv(GLint a, GLsizei n, u32 t, u32 p) {
    if (uniform_changes(a, B(t) ? 'T' : 'M', hostp<GLfloat>(p), size_t(n) * 64)) p_glUniformMatrix4fv(a, n, B(t), In<GLfloat>(p, size_t(n) * 16));
}
void w_glUseProgram(GLuint a) {
    a = prog(a);
    g_stats.programs++;
    g_stats.programs_same += g_cur_program == a;
    g_cur_program = a;
    g_cur_uniforms = a ? &g_uniforms[a] : nullptr;
    p_glUseProgram(a);
}
void w_glVertexAttrib4f(GLuint a, GLfloat b, GLfloat c, GLfloat d, GLfloat e) {
    if (a < 32) {
        const GLfloat v[4] = {b, c, d, e};
        if (state_same(g_attr_value_set[a] && std::memcmp(g_attr_value[a], v, sizeof v) == 0)) return;
        g_attr_value_set[a] = true;
        std::memcpy(g_attr_value[a], v, sizeof v);
    }
    p_glVertexAttrib4f(a, b, c, d, e);
}
void w_glViewport(GLint a, GLint b, GLsizei c, GLsizei d) { p_glViewport(a, b, c, d); }
#undef B

uint32_t crc32_png(const uint8_t *p, size_t n, uint32_t c = 0xffffffffu) {
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
    }
    return c;
}

}  // namespace

void gl_trace_frame() { gltrace::frame_done(); }
void gl_set_anisotropy(float level) { g_anisotropy = std::max(1.0f, level); }
void gl_set_texture_hooks(const GlTextureHooks &hooks) { g_texture_hooks = hooks; }

bool gl_load(GlLoader loader) {
    g_loader = loader;
    bool ok = true;
#define LOAD(name, ...)                                       load(p_##name, #name);                                    if (!p_##name) {                                              log("GL function %s not available", #name);               ok = false;                                           }
    GL_FUNCS(LOAD)
#undef LOAD
    load(p_glTexImage3DOES, "glTexImage3DOES");
    load(p_glGetProgramBinary, "glGetProgramBinary");
    if (!p_glGetProgramBinary) load(p_glGetProgramBinary, "glGetProgramBinaryOES");
    load(p_glProgramBinary, "glProgramBinary");
    if (!p_glProgramBinary) load(p_glProgramBinary, "glProgramBinaryOES");
    load(p_glTexSubImage3DOES, "glTexSubImage3DOES");
    load(p_glCompressedTexImage3DOES, "glCompressedTexImage3DOES");
    load(p_glCompressedTexSubImage3DOES, "glCompressedTexSubImage3DOES");
    if (!ok) return false;
    if (const char *dir = std::getenv("NOVA3_GLTRACE"); dir && *dir) gltrace::start(dir);
    if (const char *on = std::getenv("NOVA3_GLPROFILE"); on && *on == '1') glprofile::start();
    log("GL: %s / %s / %s", p_glGetString(0x1F00), p_glGetString(0x1F01), p_glGetString(0x1F02));
    if (const char *ext = p_glGetString(0x1F03))
        log("GL: ETC1 %s, BPTC %s, anisotropic %s", std::strstr(ext, "GL_OES_compressed_ETC1_RGB8_texture") ? "yes" : "no",
            std::strstr(ext, "texture_compression_bptc") ? "yes" : "no", std::strstr(ext, "filter_anisotropic") ? "yes" : "no");
    if (g_anisotropy > 1) {
        const char *ext = p_glGetString(0x1F03);
        GLfloat max_level = 1;
        if (ext && std::strstr(ext, "GL_EXT_texture_filter_anisotropic")) p_glGetFloatv(0x84FF /*MAX_TEXTURE_MAX_ANISOTROPY_EXT*/, &max_level);
        g_anisotropy = std::min(g_anisotropy, max_level);
        log("anisotropic filtering: %gx", g_anisotropy);
    }
    return true;
}

// PNG of the current back buffer (uncompressed "stored" deflate blocks; no dependencies).
bool gl_screenshot(const char *path, int w, int h) {
    std::vector<uint8_t> rgba(size_t(w) * size_t(h) * 4);
    p_glReadPixels(0, 0, w, h, 0x1908 /*RGBA*/, 0x1401 /*UNSIGNED_BYTE*/, rgba.data());
    std::vector<uint8_t> raw;  // per row: filter byte 0 + RGB, top row first
    raw.reserve(size_t(h) * (size_t(w) * 3 + 1));
    for (int y = h - 1; y >= 0; y--) {
        raw.push_back(0);
        for (int x = 0; x < w; x++) {
            const uint8_t *px = &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
            raw.insert(raw.end(), px, px + 3);
        }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (size_t pos = 0; pos < raw.size();) {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back(uint8_t(n));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n));
        z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + ptrdiff_t(pos), raw.begin() + ptrdiff_t(pos + n));
        for (size_t i = pos; i < pos + n; i++) {
            a = (a + raw[i]) % 65521;
            b = (b + a) % 65521;
        }
        pos += n;
    }
    uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back(uint8_t(adler >> s));

    FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    auto be32 = [&](uint32_t v) {
        uint8_t t[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
        std::fwrite(t, 1, 4, f);
    };
    auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
        be32(uint32_t(data.size()));
        std::vector<uint8_t> td(type, type + 4);
        td.insert(td.end(), data.begin(), data.end());
        std::fwrite(td.data(), 1, td.size(), f);
        be32(~crc32_png(td.data(), td.size()));
    };
    const uint8_t signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    std::fwrite(signature, 1, 8, f);
    chunk("IHDR", {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w), uint8_t(h >> 24), uint8_t(h >> 16),
                   uint8_t(h >> 8), uint8_t(h), 8, 2 /*RGB*/, 0, 0, 0});
    chunk("IDAT", z);
    chunk("IEND", {});
    std::fclose(f);
    return true;
}

#define REGISTER(name, ...) RT_IMPORT(name, w_##name)
GL_FUNCS(REGISTER)
#undef REGISTER
RT_IMPORT(glMapBufferOES, w_glMapBufferOES)
RT_IMPORT(glUnmapBufferOES, w_glUnmapBufferOES)
RT_IMPORT(glTexImage3DOES, w_glTexImage3DOES)
RT_IMPORT(glTexSubImage3DOES, w_glTexSubImage3DOES)
RT_IMPORT(glCompressedTexImage3DOES, w_glCompressedTexImage3DOES)
RT_IMPORT(glCompressedTexSubImage3DOES, w_glCompressedTexSubImage3DOES)

}  // namespace rt

namespace rt {
void gl_memory(uint64_t &textures, uint64_t &textures_3ds, uint64_t &targets, uint64_t &buffers) {
    textures = g_tex_bytes, textures_3ds = g_tex_3ds, targets = g_tex_targets, buffers = 0;
    std::lock_guard<std::mutex> l(g_lock);
    for (auto &[name, size] : g_buffer_size) buffers += size;
}
void gl_stats_log(int frames) {
    if (frames <= 0) return;
    const GlStats &s = g_stats;
    const double f = frames;
    if (g_cache_on && (g_cache_hits || g_cache_misses)) {
        log("gl: shader cache: %d programs loaded, %d compiled and saved", g_cache_hits, g_cache_misses);
        g_cache_hits = g_cache_misses = 0;
    }
    if (g_pool_hits) {
        log("gl: %d programs reused (same sources as a deleted one: no compile, no link)", g_pool_hits);
        g_pool_hits = 0;
    }
    log("gl per frame: %.0f draws (%.0f from client memory), %.0f programs (%.0f%% repeated), %.0f textures (%.0f%% repeated), "
        "%.0f enable/disable (%.0f%% repeated), %.0f uniforms (%.0f%% unchanged, skipped), %.0f other state calls (%.0f%% skipped)",
        s.draws / f, s.client_draws / f, s.programs / f, s.programs ? 100.0 * s.programs_same / s.programs : 0.0,
        s.textures / f, s.textures ? 100.0 * s.textures_same / s.textures : 0.0, s.caps / f,
        s.caps ? 100.0 * s.caps_same / s.caps : 0.0, s.uniforms / f, s.uniforms ? 100.0 * s.uniforms_same / s.uniforms : 0.0,
        s.state / f, s.state ? 100.0 * s.state_same / s.state : 0.0);
    if (s.tex_uploads || s.buf_uploads || s.tex_deleted || s.buf_deleted)
        log("gl resources in %d frames: %llu texture uploads (%.1f MB), %llu buffer uploads (%.1f MB), %llu textures and %llu buffers deleted",
            frames, (unsigned long long)s.tex_uploads, s.tex_bytes / 1048576.0, (unsigned long long)s.buf_uploads,
            s.buf_bytes / 1048576.0, (unsigned long long)s.tex_deleted, (unsigned long long)s.buf_deleted);
    g_stats = GlStats{};
    glprofile::report(frames);
}
}  // namespace rt

namespace rt {
void gl_set_shader_cache(const std::string &dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    g_cache_dir = ec ? std::string() : dir;
}

bool gl_prewarm_step() {
    if (g_cache_dir.empty() || cache_active()) return false;  // program binaries cover it there
    load_program_list();
    while (!g_prewarm.empty()) {
        Recorded r = std::move(g_prewarm.front());
        g_prewarm.pop_front();
        bool known = g_pool.count(r.key) && !g_pool[r.key].empty();
        for (const auto &[program, key] : g_program_key) known = known || key == r.key;
        if (known) continue;  // linked already (the game got there first)
        const GLuint program = p_glCreateProgram();
        for (const auto &[type, src] : r.shaders) {
            const GLuint sh = p_glCreateShader(type);
            const char *text = src.c_str();
            const GLint len = GLint(src.size());
            p_glShaderSource(sh, 1, &text, &len);
            p_glCompileShader(sh);
            p_glAttachShader(program, sh);
            p_glDeleteShader(sh);  // (freed with the program)
        }
        p_glLinkProgram(program);
        GLint ok = 0;
        p_glGetProgramiv(program, 0x8B82 /*LINK_STATUS*/, &ok);
        if (!ok) {
            p_glDeleteProgram(program);
            continue;
        }
        g_program_key[program] = r.key;
        g_pool[r.key].push_back(program);
        if (++g_prewarmed % 50 == 0 || g_prewarm.empty()) log("gl: %d programs compiled ahead", g_prewarmed);
        return true;
    }
    return false;
}
}  // namespace rt
