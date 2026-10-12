// Headless GLES2 ("null renderer"): accepts every call and answers queries the way a
// capable GLES2 device would, so the engine can boot, compile its shaders and run frames
// without a window. The real backend (window + ANGLE) replaces this in the graphics step;
// it also remains useful for automated runs.
#include "runtime.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <set>
#include <unordered_map>

namespace rt {
namespace {

using u32 = uint32_t;

std::mutex g_lock;
u32 g_next_name = 1;
int g_vp[4] = {0, 0, 1280, 720};
u32 g_bound_buffer[2];                       // GL_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER
std::unordered_map<u32, u32> g_buffer_size;  // name -> size
std::unordered_map<u32, u32> g_buffer_map;   // name -> guest memory for glMapBufferOES
std::set<u32> g_warned;

u32 gen_name() { std::lock_guard<std::mutex> l(g_lock); return g_next_name++; }
void gen(u32 n, u32 out) { for (u32 i = 0; i < n; i++) ST32(out + 4 * i, gen_name()); }
void gl_gen(int32_t n, u32 out) { gen(u32(n), out); }
u32 gl_create(u32) { return gen_name(); }
u32 gl_create_program() { return gen_name(); }
u32 gl_get_error() { return 0; }
u32 gl_check_fb(u32) { return 0x8CD5; }  // GL_FRAMEBUFFER_COMPLETE

u32 guest_string(const char *s) {
    static std::unordered_map<std::string, u32> cache;
    auto it = cache.find(s);
    return it != cache.end() ? it->second : cache[s] = gstrdup(s);
}
u32 gl_get_string(u32 name) {
    switch (name) {
    case 0x1F00: return guest_string("nova3");
    case 0x1F01: return guest_string("nova3 null renderer");
    case 0x1F02: return guest_string("OpenGL ES 2.0 nova3");
    case 0x8B8C: return guest_string("OpenGL ES GLSL ES 1.00");
    case 0x1F03:
        return guest_string("GL_OES_compressed_ETC1_RGB8_texture GL_OES_depth24 GL_OES_packed_depth_stencil "
                            "GL_OES_rgb8_rgba8 GL_OES_standard_derivatives GL_OES_texture_npot GL_OES_mapbuffer "
                            "GL_OES_element_index_uint GL_OES_depth_texture GL_EXT_texture_format_BGRA8888 "
                            "GL_EXT_texture_filter_anisotropic GL_OES_texture_float GL_OES_texture_half_float");
    }
    return 0;
}

int32_t int_param(u32 pname, u32 out) {
    switch (pname) {
    case 0x0D33: case 0x851C: case 0x84E8: return 4096;      // max texture / cube / renderbuffer size
    case 0x8869: return 16;                                    // MAX_VERTEX_ATTRIBS
    case 0x8872: case 0x8B4C: return 16;                       // texture image units (fragment / vertex)
    case 0x8B4D: return 32;                                    // MAX_COMBINED_TEXTURE_IMAGE_UNITS
    case 0x8DFB: return 256;                                   // MAX_VERTEX_UNIFORM_VECTORS
    case 0x8DFD: return 224;                                   // MAX_FRAGMENT_UNIFORM_VECTORS
    case 0x8DFC: return 15;                                    // MAX_VARYING_VECTORS
    case 0x86A2: return 1;                                     // NUM_COMPRESSED_TEXTURE_FORMATS
    case 0x86A3: return 0x8D64;                                // COMPRESSED_TEXTURE_FORMATS: ETC1
    case 0x0D50: case 0x0D52: case 0x0D53: case 0x0D54: return 8;  // red/green/blue/alpha bits
    case 0x0D55: return 8;
    case 0x0D56: return 24;                                    // DEPTH_BITS
    case 0x0D57: return 8;                                     // STENCIL_BITS
    case 0x0BA2:                                               // VIEWPORT
        for (int i = 0; i < 4; i++) ST32(out + 4 * i, u32(g_vp[i]));
        return g_vp[0];
    case 0x8CA6: case 0x8CA7: case 0x8069: case 0x8894: case 0x8895: case 0x8B8D: return 0;  // bindings
    case 0x0D05: case 0x0CF5: return 4;                        // pack / unpack alignment
    }
    std::lock_guard<std::mutex> l(g_lock);
    if (g_warned.insert(pname).second) log("glGetIntegerv(0x%04x): returning 0", pname);
    return 0;
}
void gl_get_integerv(u32 pname, u32 out) {
    int32_t v = int_param(pname, out);
    if (pname != 0x0BA2) ST32(out, u32(v));
}
void gl_get_floatv(u32 pname, u32 out) {
    float v = 0;
    if (pname == 0x84FF) v = 16.0f;                           // MAX_TEXTURE_MAX_ANISOTROPY_EXT
    else if (pname == 0x846E || pname == 0x846D) {            // aliased line width / point size range
        STF(out, 1.0f);
        STF(out + 4, 64.0f);
        return;
    } else v = float(int_param(pname, out));
    STF(out, v);
}
void gl_get_shaderiv(u32, u32 pname, u32 out) {
    ST32(out, pname == 0x8B81 ? 1 : 0);  // COMPILE_STATUS = TRUE, logs empty
}
// Program reflection: the engine looks every program's uniforms and attributes up after linking
// (glGetActiveUniform / glGetActiveAttrib) and draws a material whose program shows none with
// its "pink wireframe" program, which then fails the same way, recursively, until the stack runs
// out. So the declarations are read from the sources (all of them: #if blocks are not evaluated).
struct Active { std::string name; u32 type, size; };
struct Program { std::vector<u32> shaders; std::vector<Active> uniforms, attributes; };
std::unordered_map<u32, std::string> g_shader_source;
std::unordered_map<u32, Program> g_programs;

u32 glsl_type(const std::string &t) {
    static const std::unordered_map<std::string, u32> types = {
        {"float", 0x1406}, {"vec2", 0x8B50}, {"vec3", 0x8B51}, {"vec4", 0x8B52}, {"int", 0x1404}, {"ivec2", 0x8B53},
        {"ivec3", 0x8B54}, {"ivec4", 0x8B55}, {"bool", 0x8B56}, {"bvec2", 0x8B57}, {"bvec3", 0x8B58}, {"bvec4", 0x8B59},
        {"mat2", 0x8B5A}, {"mat3", 0x8B5B}, {"mat4", 0x8B5C}, {"sampler2D", 0x8B5E}, {"samplerCube", 0x8B60}};
    auto it = types.find(t);
    return it == types.end() ? 0 : it->second;
}

// `uniform` / `attribute` declarations: [precision] type name[[n]] {, name[[n]]} ;
void reflect(const std::string &src, std::vector<Active> &uniforms, std::vector<Active> &attributes) {
    std::string text;
    for (size_t i = 0; i < src.size(); i++) {  // without comments
        if (src.compare(i, 2, "//") == 0) i = std::min(src.find('\n', i), src.size()) - 1;  // (keeps the line break)
        else if (src.compare(i, 2, "/*") == 0) { i = std::min(src.find("*/", i) + 1, src.size() - 1); text += ' '; }
        else text += src[i];
    }
    auto word = [&](size_t &i) {
        while (i < text.size() && (std::isspace(uint8_t(text[i])) || text[i] == ',')) i++;
        size_t b = i;
        while (i < text.size() && (std::isalnum(uint8_t(text[i])) || text[i] == '_')) i++;
        return text.substr(b, i - b);
    };
    for (size_t i = 0; i < text.size();) {
        std::string w = word(i);
        if (w.empty()) { i++; continue; }
        if (w != "uniform" && w != "attribute") continue;
        auto &list = w == "uniform" ? uniforms : attributes;
        std::string t = word(i);
        if (t == "lowp" || t == "mediump" || t == "highp") t = word(i);
        const u32 type = glsl_type(t);
        if (!type) continue;
        while (i < text.size() && text[i] != ';') {
            std::string name = word(i);
            if (name.empty()) { i++; continue; }
            u32 size = 1;
            while (i < text.size() && std::isspace(uint8_t(text[i]))) i++;
            if (i < text.size() && text[i] == '[') {
                size = u32(std::strtoul(text.c_str() + i + 1, nullptr, 10));
                i = text.find(']', i) + 1;
                name += "[0]";
            }
            bool seen = false;
            for (const Active &a : list) seen |= a.name == name;
            if (!seen && name.compare(0, 3, "gl_") != 0) list.push_back({name, type, size ? size : 1});
        }
    }
}

void gl_shader_source(u32 shader, int32_t count, u32 strings, u32 lengths) {
    std::string src;
    for (int32_t i = 0; i < count; i++) {
        const char *p = str(LD32(strings + 4 * i));
        const int32_t n = lengths ? int32_t(LD32(lengths + 4 * i)) : -1;
        if (p) src += n < 0 ? std::string(p) : std::string(p, size_t(n));
    }
    std::lock_guard<std::mutex> l(g_lock);
    g_shader_source[shader] = src;
}
void gl_attach_shader(u32 program, u32 shader) {
    std::lock_guard<std::mutex> l(g_lock);
    g_programs[program].shaders.push_back(shader);
}
void gl_link_program(u32 program) {
    std::lock_guard<std::mutex> l(g_lock);
    Program &p = g_programs[program];
    p.uniforms.clear();
    p.attributes.clear();
    for (u32 s : p.shaders) reflect(g_shader_source[s], p.uniforms, p.attributes);
}
void gl_get_programiv(u32 program, u32 pname, u32 out) {
    std::lock_guard<std::mutex> l(g_lock);
    const Program &p = g_programs[program];
    auto longest = [](const std::vector<Active> &v) { size_t n = 0; for (const Active &a : v) n = std::max(n, a.name.size() + 1); return u32(n); };
    switch (pname) {
    case 0x8B82: case 0x8B83: ST32(out, 1); return;  // LINK_STATUS / VALIDATE_STATUS
    case 0x8B85: ST32(out, u32(p.shaders.size())); return;  // ATTACHED_SHADERS
    case 0x8B86: ST32(out, u32(p.uniforms.size())); return;  // ACTIVE_UNIFORMS
    case 0x8B87: ST32(out, longest(p.uniforms)); return;
    case 0x8B89: ST32(out, u32(p.attributes.size())); return;  // ACTIVE_ATTRIBUTES
    case 0x8B8A: ST32(out, longest(p.attributes)); return;
    }
    ST32(out, 0);
}
void get_active(const std::vector<Active> &list, u32 index, int32_t max, u32 len, u32 size, u32 type, u32 name) {
    const Active a = index < list.size() ? list[index] : Active{"", 0x1406, 1};
    const size_t n = max > 0 ? std::min(a.name.size(), size_t(max - 1)) : 0;
    if (name && max > 0) { std::memcpy(ptr(name), a.name.data(), n); ST8(name + u32(n), 0); }
    if (len) ST32(len, u32(n));
    if (size) ST32(size, a.size);
    if (type) ST32(type, a.type);
}
void gl_get_active_uniform(u32 program, u32 index, int32_t max, u32 len, u32 size, u32 type, u32 name) {
    std::lock_guard<std::mutex> l(g_lock);
    get_active(g_programs[program].uniforms, index, max, len, size, type, name);
}
void gl_get_active_attrib(u32 program, u32 index, int32_t max, u32 len, u32 size, u32 type, u32 name) {
    std::lock_guard<std::mutex> l(g_lock);
    get_active(g_programs[program].attributes, index, max, len, size, type, name);
}
void gl_delete_program(u32 program) {
    std::lock_guard<std::mutex> l(g_lock);
    g_programs.erase(program);
}
void gl_delete_shader(u32 shader) {
    std::lock_guard<std::mutex> l(g_lock);
    g_shader_source.erase(shader);
}
void gl_get_info_log(u32, int32_t max, u32 len, u32 buf) {
    if (len) ST32(len, 0);
    if (max > 0 && buf) ST8(buf, 0);
}
int32_t gl_get_location(u32, u32) { return int32_t(gen_name() & 0x3ff); }
void gl_viewport(int32_t x, int32_t y, int32_t w, int32_t h) { g_vp[0] = x, g_vp[1] = y, g_vp[2] = w, g_vp[3] = h; }
void gl_bind_buffer(u32 target, u32 name) { g_bound_buffer[target == 0x8893] = name; }
void gl_buffer_data(u32 target, int32_t size, u32, u32) {
    std::lock_guard<std::mutex> l(g_lock);
    u32 b = g_bound_buffer[target == 0x8893];
    g_buffer_size[b] = u32(size);
    auto it = g_buffer_map.find(b);
    if (it != g_buffer_map.end()) { gfree(it->second); g_buffer_map.erase(it); }
}
u32 gl_map_buffer(u32 target, u32) {
    std::lock_guard<std::mutex> l(g_lock);
    u32 b = g_bound_buffer[target == 0x8893];
    u32 &mem = g_buffer_map[b];
    if (!mem) mem = gcalloc(1, std::max<u32>(g_buffer_size[b], 16u));
    return mem;
}
u32 gl_unmap_buffer(u32) { return 1; }

}  // namespace

// Selected with --headless: replaces every gl* import with a no-op, then answers the
// queries the engine depends on.
void install_null_gl() {
    for (u32 i = 0; i < g_import_count; i++)
        if (!std::strncmp(g_import_names[i], "gl", 2)) set_import(g_import_names[i], [](Cpu *c) { c->r[0] = 0; });
    set_import("glGenBuffers", &thunk<gl_gen>);
    set_import("glGenFramebuffers", &thunk<gl_gen>);
    set_import("glGenRenderbuffers", &thunk<gl_gen>);
    set_import("glGenTextures", &thunk<gl_gen>);
    set_import("glCreateShader", &thunk<gl_create>);
    set_import("glCreateProgram", &thunk<gl_create_program>);
    set_import("glGetError", &thunk<gl_get_error>);
    set_import("glCheckFramebufferStatus", &thunk<gl_check_fb>);
    set_import("glGetString", &thunk<gl_get_string>);
    set_import("glGetIntegerv", &thunk<gl_get_integerv>);
    set_import("glGetFloatv", &thunk<gl_get_floatv>);
    set_import("glGetShaderiv", &thunk<gl_get_shaderiv>);
    set_import("glGetProgramiv", &thunk<gl_get_programiv>);
    set_import("glShaderSource", &thunk<gl_shader_source>);
    set_import("glAttachShader", &thunk<gl_attach_shader>);
    set_import("glLinkProgram", &thunk<gl_link_program>);
    set_import("glGetActiveUniform", &thunk<gl_get_active_uniform>);
    set_import("glGetActiveAttrib", &thunk<gl_get_active_attrib>);
    set_import("glDeleteProgram", &thunk<gl_delete_program>);
    set_import("glDeleteShader", &thunk<gl_delete_shader>);
    set_import("glGetShaderInfoLog", &thunk<gl_get_info_log>);
    set_import("glGetProgramInfoLog", &thunk<gl_get_info_log>);
    set_import("glGetAttribLocation", &thunk<gl_get_location>);
    set_import("glGetUniformLocation", &thunk<gl_get_location>);
    set_import("glViewport", &thunk<gl_viewport>);
    set_import("glBindBuffer", &thunk<gl_bind_buffer>);
    set_import("glBufferData", &thunk<gl_buffer_data>);
    set_import("glMapBufferOES", &thunk<gl_map_buffer>);
    set_import("glUnmapBufferOES", &thunk<gl_unmap_buffer>);
}

}  // namespace rt
