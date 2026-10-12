// GLSL programs for the drivers of GPUs without fragment shaders: see gl_program.h.
#include "gl_program.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace glprog {
namespace {

std::unordered_map<GLuint, Shader> g_shaders;
std::unordered_map<GLuint, std::unique_ptr<Program>> g_programs;
GLuint g_next_name = 1;
GLuint g_cur_name;
Program *g_cur_program;
GLenum g_error;
Hooks g_hooks;

void set_error(GLenum e) {
    if (!g_error) g_error = e;
}
void changed() {
    if (g_hooks.changed) g_hooks.changed();
}

// ---- GLSL: just enough to know a program's attributes, uniforms and #defines -------------------
int type_comps(GLenum t) {
    switch (t) {
    case 0x8B50: case 0x8B53: case 0x8B57: return 2;            // vec2 ivec2 bvec2
    case 0x8B51: case 0x8B54: case 0x8B58: return 3;
    case 0x8B52: case 0x8B55: case 0x8B59: case 0x8B5A: return 4;  // vec4 ivec4 bvec4 mat2
    case 0x8B5B: return 9;
    case 0x8B5C: return 16;
    default: return 1;
    }
}
GLenum type_of(const std::string &t) {
    static const std::unordered_map<std::string, GLenum> types = {
        {"float", 0x1406}, {"vec2", 0x8B50}, {"vec3", 0x8B51}, {"vec4", 0x8B52}, {"int", 0x1404}, {"ivec2", 0x8B53},
        {"ivec3", 0x8B54}, {"ivec4", 0x8B55}, {"bool", 0x8B56}, {"bvec2", 0x8B57}, {"bvec3", 0x8B58}, {"bvec4", 0x8B59},
        {"mat2", 0x8B5A}, {"mat3", 0x8B5B}, {"mat4", 0x8B5C}, {"sampler2D", 0x8B5E}, {"samplerCube", 0x8B60}};
    auto it = types.find(t);
    return it == types.end() ? 0 : it->second;
}

struct Preproc {
    std::map<std::string, std::string> macros;
    // #if expressions and array sizes: integers, defined(), ! && || comparisons + - * / ( )
    std::vector<std::string> toks;
    size_t at = 0;
    static std::vector<std::string> tokenize(const std::string &s) {
        std::vector<std::string> out;
        for (size_t i = 0; i < s.size();) {
            const char c = s[i];
            if (std::isspace(uint8_t(c))) { i++; continue; }
            if (std::isalpha(uint8_t(c)) || c == '_') {
                size_t j = i;
                while (j < s.size() && (std::isalnum(uint8_t(s[j])) || s[j] == '_')) j++;
                out.push_back(s.substr(i, j - i));
                i = j;
            } else if (std::isdigit(uint8_t(c)) || (c == '.' && i + 1 < s.size() && std::isdigit(uint8_t(s[i + 1])))) {
                size_t j = i;
                while (j < s.size() && (std::isalnum(uint8_t(s[j])) || s[j] == '.')) j++;
                out.push_back(s.substr(i, j - i));
                i = j;
            } else {
                static const char *two[] = {"&&", "||", "==", "!=", "<=", ">="};
                bool done = false;
                for (const char *t : two)
                    if (s.compare(i, 2, t) == 0) { out.emplace_back(t); i += 2; done = true; break; }
                if (!done) out.emplace_back(1, c), i++;
            }
        }
        return out;
    }
    long eval(const std::string &expr) {
        toks = tokenize(expr);
        at = 0;
        return expr_or();
    }
    std::string peek() { return at < toks.size() ? toks[at] : ""; }
    long expr_or() { long v = expr_and(); while (peek() == "||") { at++; long r = expr_and(); v = v || r; } return v; }
    long expr_and() { long v = expr_cmp(); while (peek() == "&&") { at++; long r = expr_cmp(); v = v && r; } return v; }
    long expr_cmp() {
        long v = expr_add();
        for (;;) {
            const std::string op = peek();
            if (op != "==" && op != "!=" && op != "<" && op != ">" && op != "<=" && op != ">=") return v;
            at++;
            const long r = expr_add();
            v = op == "==" ? v == r : op == "!=" ? v != r : op == "<" ? v < r : op == ">" ? v > r : op == "<=" ? v <= r : v >= r;
        }
    }
    long expr_add() {
        long v = expr_mul();
        while (peek() == "+" || peek() == "-") { const bool add = toks[at++] == "+"; long r = expr_mul(); v = add ? v + r : v - r; }
        return v;
    }
    long expr_mul() {
        long v = expr_unary();
        while (peek() == "*" || peek() == "/") { const bool mul = toks[at++] == "*"; long r = expr_unary(); v = mul ? v * r : r ? v / r : 0; }
        return v;
    }
    long expr_unary() {
        const std::string t = peek();
        if (t == "!") { at++; return !expr_unary(); }
        if (t == "-") { at++; return -expr_unary(); }
        if (t == "(") { at++; long v = expr_or(); if (peek() == ")") at++; return v; }
        at++;
        if (t == "defined") {
            bool paren = peek() == "(";
            if (paren) at++;
            const std::string name = peek();
            at++;
            if (paren && peek() == ")") at++;
            return macros.count(name) ? 1 : 0;
        }
        if (!t.empty() && std::isdigit(uint8_t(t[0]))) return std::strtol(t.c_str(), nullptr, 0);
        auto it = macros.find(t);
        if (it != macros.end() && !it->second.empty() && it->second != t) {
            Preproc sub;
            sub.macros = macros;
            sub.macros.erase(t);
            return sub.eval(it->second);
        }
        return 0;
    }
};

// Removes comments, runs the preprocessor and collects declarations.
void parse_glsl(const std::string &src, std::map<std::string, std::string> &macros, std::vector<Var> &attribs,
                std::vector<Var> &uniforms) {
    std::string text;
    text.reserve(src.size());
    for (size_t i = 0; i < src.size(); i++) {  // comments out
        if (src.compare(i, 2, "//") == 0) {
            while (i < src.size() && src[i] != '\n') i++;
            text += '\n';
        } else if (src.compare(i, 2, "/*") == 0) {
            size_t e = src.find("*/", i + 2);
            for (size_t j = i; j < std::min(e, src.size()); j++)
                if (src[j] == '\n') text += '\n';
            i = e == std::string::npos ? src.size() : e + 1;
        } else {
            text += src[i];
        }
    }
    Preproc pp;
    pp.macros = macros;
    struct Cond { bool active, taken, parent; };
    std::vector<Cond> stack;
    auto active = [&] { return stack.empty() || stack.back().active; };
    std::string code;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        size_t s = line.find_first_not_of(" \t\r");
        if (s == std::string::npos) continue;
        if (line[s] != '#') {
            if (active()) code += line + '\n';
            continue;
        }
        std::vector<std::string> t = Preproc::tokenize(line.substr(s + 1));
        if (t.empty()) continue;
        const std::string &d = t[0];
        const std::string rest = line.substr(line.find(d, s) + d.size());
        if (d == "ifdef" || d == "ifndef") {
            const bool def = t.size() > 1 && pp.macros.count(t[1]);
            const bool on = active() && (d == "ifdef" ? def : !def);
            stack.push_back({on, on, active()});
        } else if (d == "if") {
            const bool on = active() && pp.eval(rest) != 0;
            stack.push_back({on, on, active()});
        } else if (d == "elif" && !stack.empty()) {
            Cond &c = stack.back();
            const bool on = c.parent && !c.taken && pp.eval(rest) != 0;
            c.active = on;
            c.taken = c.taken || on;
        } else if (d == "else" && !stack.empty()) {
            Cond &c = stack.back();
            c.active = c.parent && !c.taken;
            c.taken = true;
        } else if (d == "endif" && !stack.empty()) {
            stack.pop_back();
        } else if (d == "define" && active() && t.size() > 1) {
            const size_t n = rest.find(t[1]);
            std::string value = n == std::string::npos ? "" : rest.substr(n + t[1].size());
            value.erase(0, value.find_first_not_of(" \t"));
            while (!value.empty() && std::isspace(uint8_t(value.back()))) value.pop_back();
            pp.macros[t[1]] = value;
        } else if (d == "undef" && active() && t.size() > 1) {
            pp.macros.erase(t[1]);
        }
    }
    macros = pp.macros;
    // declarations: [attribute|uniform] [precision] type name[N] {, name[N]} ;
    std::vector<std::string> toks = Preproc::tokenize(code);
    for (std::string &tk : toks)  // object-like macros used as qualifiers (VCOORD_PRECISION)
        for (int depth = 0; depth < 4; depth++) {
            auto m = pp.macros.find(tk);
            if (m == pp.macros.end() || m->second.empty() || m->second.find(' ') != std::string::npos) break;
            tk = m->second;
        }
    for (size_t i = 0; i < toks.size(); i++) {
        const bool attr = toks[i] == "attribute", unif = toks[i] == "uniform";
        if (!attr && !unif) continue;
        size_t j = i + 1;
        while (j < toks.size() && (toks[j] == "lowp" || toks[j] == "mediump" || toks[j] == "highp")) j++;
        if (j >= toks.size()) break;
        const GLenum type = type_of(toks[j++]);
        if (!type) continue;
        while (j < toks.size() && toks[j] != ";") {
            Var v;
            v.name = toks[j++];
            v.type = type;
            v.comps = type_comps(type);
            if (j < toks.size() && toks[j] == "[") {
                std::string expr;
                for (j++; j < toks.size() && toks[j] != "]"; j++) expr += toks[j] + ' ';
                j++;
                Preproc ev;
                ev.macros = pp.macros;
                v.count = std::max(1, int(ev.eval(expr)));
            }
            std::vector<Var> &list = attr ? attribs : uniforms;
            if (std::none_of(list.begin(), list.end(), [&](const Var &o) { return o.name == v.name; })) list.push_back(v);
            if (j < toks.size() && toks[j] == ",") j++;
        }
        i = j;
    }
}

// FOGZ: height fog, (1 - min(height term, camera's)) x clamp(distance / FogZ.z) [x vertex alpha]
// toward FogZColor, mixed after the distance fog; both then go through the combiners, their
// factors per vertex (generic.v.pica's fogzon) in the colour's green and blue, or through the ramp texture on
// unit 1 when the material uses all of the colour (`ramp`).
void height_fog(const Program &p, Recipe &r, bool ramp) {
    auto u = [&](const char *n) { return p.uniform(n); };
    if (!p.defined("FOGZ") || r.fog_kind != 0 || u("FogZ") < 0 || u("FogZColor") < 0 || u("ViewInverse") < 0 || u("WorldViewT") < 0 ||
        u("World") < 0)
        return;
    r.fogz = true, r.fogz_ramp = ramp;
    r.u_fogz = u("FogZ"), r.u_fogz_color = u("FogZColor"), r.u_view_inv = u("ViewInverse"), r.u_wvt = u("WorldViewT");
    r.u_world = u("World");
    if (u("FogDensity") >= 0) r.u_fog_max = u("FogDensity");
    // the world shaders weight it by the vertex alpha; the effects' ADDITIVE darkens instead of mixing
    r.fogz_alpha = !ramp && (!p.defined("GLOW") || (!p.defined("TEX2") && !p.defined("REFLECTION")));
    r.fogz_additive = ramp && p.defined("ADDITIVE");
}

// The uniforms the material depends on: all those the recipe reads but the ones setup() takes at
// every draw (the matrices that change with each object: position, and the height fog's).
void material_uniforms(Recipe &r, size_t count) {
    r.material.assign(count, 0);
    std::vector<int> u = {r.u_fog_se, r.u_fog_color, r.u_fog_max, r.u_color, r.u_fade, r.u_alpharef, r.u_factor, r.u_tint,
                          r.u_blue, r.u_fogz, r.u_fogz_color, r.u_view_inv, r.u_wit, r.u_lpos, r.u_latt, r.u_lcol,
                          r.u_l1p, r.u_l2p, r.u_l1c, r.u_l2c, r.u_l1i, r.u_l2i, r.u_amb};
    for (int i : r.u_ird) u.push_back(i);
    for (const UnitSpec &s : r.unit) u.insert(u.end(), {s.sampler, s.offset, s.scaleoffset, s.transform});
    if (!r.fogz) u.push_back(r.u_world);  // (the lit shaders light in world space; FOGZ: per draw)
    for (int i : u)
        if (i >= 0 && size_t(i) < count) r.material[size_t(i)] = 1;
}

bool classify(Program &p, Recipe &r) {
    auto u = [&](const char *n) { return p.uniform(n); };
    auto a = [&](const char *n) { return p.attrib(n); };
    r.a_vertex = a("Vertex") >= 0 ? a("Vertex") : a("Position");
    r.a_uv[0] = a("TexCoord0"), r.a_uv[1] = a("TexCoord1"), r.a_uv[2] = a("TexCoord2");
    r.a_color = a("Color0") >= 0 ? a("Color0") : a("Color");
    r.a_normal = a("Normal");
    r.u_mvp = u("WorldViewProjectionMatrix");
    if (r.a_vertex < 0 || r.u_mvp < 0) return false;
    if (p.defined("FOG") && u("FogColor") >= 0 && u("FogStartEnd") >= 0) {
        r.u_fog_se = u("FogStartEnd"), r.u_fog_color = u("FogColor"), r.u_fog_max = u("FogDensity");
    } else if (p.defined("FOG") && u("fogcolor") >= 0 && u("fogstartend") >= 0) {
        r.u_fog_se = u("fogstartend"), r.u_fog_color = u("fogcolor"), r.fog_kind = 1;
    }
    if (p.defined("ALPHA_TEST")) r.u_alpharef = u("alpharef");
    const int diffuse = u("DiffuseTextureSampler"), diffuse_a = u("DiffuseTextureSampler_alpha");
    const int lightmap = u("LightmapTextureSampler"), mask = u("DiffuseMaskSampler");
    const int offset = u("uvOffset");
    auto unit = [&](int n, int sampler, int uv, bool offset_too = false, float rgb = 1) {
        r.unit[n].sampler = sampler, r.unit[n].uv = uv, r.unit[n].rgb = rgb;
        if (offset_too) r.unit[n].offset = offset;
    };
    if (u("ird_cAr") >= 0 && diffuse >= 0 && r.a_normal >= 0) {  // irradiance-lit models
        r.vs = VS_LIT;
        r.tev = TEV_TEXVC;
        unit(0, diffuse, 0, true);
        if (p.defined("COLORMASK") && u("ColorMaskTexture") >= 0 && u("ColorMaskColor") >= 0)
            r.tev = TEV_TINTED, r.u_tint = u("ColorMaskColor"), unit(1, u("ColorMaskTexture"), 0, true, 0);
        if (diffuse_a >= 0) unit(2, diffuse_a, 0, true), r.split_alpha = true;
        r.u_wit = u("WorldInverseTransposed");
        static const char *const ird[7] = {"ird_cAr", "ird_cAg", "ird_cAb", "ird_cBr", "ird_cBg", "ird_cBb", "ird_cC"};
        for (int i = 0; i < 7; i++) r.u_ird[i] = u(ird[i]);
        r.u_lpos = u("Light0positionOS"), r.u_latt = u("Light0attenuation"), r.u_lcol = u("Light0diffusecolormultiplied");
        return r.u_wit >= 0;
    }
    if (u("L1_Pos") >= 0 && diffuse >= 0 && r.a_normal >= 0) {  // models lit by two point lights
        r.vs = VS_POINT2;
        r.tev = TEV_GLOW;
        unit(0, diffuse, 0);
        r.u_world = u("World"), r.u_wit = u("WorldInverseTransposed");
        r.u_l1p = u("L1_Pos"), r.u_l2p = u("L2_Pos");
        r.u_l1c = u("L1_Color"), r.u_l2c = u("L2_Color"), r.u_l1i = u("L1_Intensity"), r.u_l2i = u("L2_Intensity");
        r.u_amb = u("AmbientFactor");
        return r.u_world >= 0 && r.u_wit >= 0;
    }
    if (mask >= 0 && diffuse >= 0) {  // diffuse [x lightmap], alpha from a mask
        r.tev = TEV_MASK;
        unit(0, diffuse, 0, true);
        if (lightmap >= 0) unit(1, lightmap, 1), r.lightmap = true;
        unit(2, mask, 0, true);
        return true;
    }
    if (lightmap >= 0 && diffuse >= 0) {  // the world
        const int diffuse2 = u("Diffuse2TextureSampler");
        unit(0, diffuse, 0);
        unit(1, lightmap, 1, false, 0.5f);
        if (p.defined("TEX2") && diffuse2 >= 0 && r.a_uv[2] >= 0) {
            r.tev = TEV_LIGHTMAP_TEX2;  // (the alpha textures do not fit: three units)
            unit(2, diffuse2, 2);
            r.tex2_alpha = p.defined("GLOW");
        } else {
            r.tev = TEV_LIGHTMAP;
            if (diffuse_a >= 0) unit(2, diffuse_a, 0), r.split_alpha = true;
        }
        height_fog(p, r, false);
        return true;
    }
    const int red = u("DiffuseTextureRedSampler"), green = u("DiffuseTextureGreenSampler");
    if (red >= 0 && green >= 0 && lightmap >= 0) {  // terrain: three maps weighted by the vertex colour
        r.tev = TEV_TERRAIN;  // (the lightmap's UV1 coordinates only reach unit 1)
        unit(0, red, 0);
        unit(1, lightmap, 1, false, 0.5f);
        unit(2, green, 0);
        r.u_blue = u("DiffuseTextureBlueSampler");
        return true;
    }
    if (u("texture1") >= 0 && u("texture2") >= 0) {  // Glitch's two-texture interface programs
        r.tev = TEV_TEX2_MUL;
        unit(0, u("texture1"), 0);
        unit(1, u("texture2"), 0);
        const int a1 = u("texture1_alpha"), a2 = u("texture2_alpha");
        if (a1 >= 0 || a2 >= 0) unit(2, a1 >= 0 ? a1 : a2, 0), r.split_alpha = true;
        return true;
    }
    if (u("texture0") >= 0 && u("texture1") >= 0 && u("factor") >= 0) {
        r.tev = TEV_TEX2_MIX;
        unit(0, u("texture0"), 0);
        unit(1, u("texture1"), 0);
        r.u_factor = u("factor");
        return true;
    }
    // unlit: one texture (and its alpha) x vertex colour
    static const char *const names[] = {"DiffuseTextureSampler", "texture", "MapDiffuseSampler", "texture0", "DiffuseMap", "Texture0",
                                        "Sampler0"};
    int tex = -1;
    std::string tex_name;
    for (const char *n : names)
        if (u(n) >= 0) {
            tex = u(n), tex_name = n;
            break;
        }
    for (size_t i = 0; tex < 0 && i < p.uniforms.size(); i++)  // Glitch renames `texture` in some (#define)
        if (p.uniforms[i].name.rfind("texture_glitchprops", 0) == 0) tex = int(i), tex_name = "texture";
    if (tex >= 0 && !p.defined("SKINNED")) {
        unit(0, tex, 0, true);
        for (GLuint sh : p.shaders)  // the menus' composite of their premultiplied interface layer
            if (auto it = g_shaders.find(sh); it != g_shaders.end() && it->second.type == 0x8B30 &&
                                               it->second.source.find("/ max(fragColor.a") != std::string::npos)
                r.unpremultiply = true;
        if (const int ta = u((tex_name + "_alpha").c_str()); ta >= 0) unit(2, ta, 0, true), r.split_alpha = true;
        for (int n : {0, 2}) {
            if (r.unit[n].sampler < 0) continue;
            if ((p.defined("TEXCOORD_MODIFIER") && p.defines["TEXCOORD_MODIFIER"] == "TEXCOORD_TRANSFORM") || tex_name == "Sampler0")
                r.unit[n].transform = u("TexCoord0_transform");
            else if (p.defined("TEXCOORD_MODIFIER") && p.defines["TEXCOORD_MODIFIER"] == "TEXCOORD_SCALE_OFFSET")
                r.unit[n].scaleoffset = u("TexCoord0_scaleoffset");
        }
        r.tev = TEV_TEXVC;
        if (u("Ambient") >= 0) {
            r.u_color = u("Ambient");
            // the UV-animation shaders: ADDITIVE scales the rgb, the others the alpha; the
            // screen-mask one all of it (its screen-space second texture is left out)
            r.color_mask = u("ViewportSizeInv") >= 0 ? 3 : p.defined("ADDITIVE") ? 1 : 2;
        }
        if (p.defined("FADE_FACTOR")) r.u_fade = u("FadeFactor");
        if (tex_name == "Sampler0" && u("DiffuseColor") >= 0) {  // Glitch's fixed-function emulation (lights left out)
            r.u_color = u("DiffuseColor");
            r.color_mask = 3;
        }
        if (r.unit[1].sampler < 0) height_fog(p, r, true);  // (the UV-animation effects)
        return true;
    }
    if (r.a_color >= 0 || u("DiffuseColor") >= 0) {  // a colour: per vertex, or the material's
        bool samplers = false;
        for (const Var &v : p.uniforms) samplers = samplers || v.type == 0x8B5E || v.type == 0x8B60;
        if (!samplers) {
            r.tev = TEV_VC;
            if (u("DiffuseColor") >= 0) r.u_color = u("DiffuseColor"), r.color_mask = 3;  // (x the vertex colour)
            return true;
        }
    }
    return false;
}

}  // namespace

void set_hooks(const Hooks &h) { g_hooks = h; }
Program *current() { return g_cur_program; }
GLuint current_name() { return g_cur_name; }
const Shader *shader(GLuint name) {
    auto it = g_shaders.find(name);
    return it == g_shaders.end() ? nullptr : &it->second;
}
GLenum take_error() {
    const GLenum e = g_error;
    g_error = 0;
    return e;
}

void glAttachShader(GLuint p, GLuint s) {
    auto it = g_programs.find(p);
    if (it != g_programs.end()) it->second->shaders.push_back(s);
}

void glCompileShader(GLuint) {}

GLuint glCreateProgram() {
    const GLuint n = g_next_name++;
    g_programs[n] = std::make_unique<Program>();
    return n;
}

GLuint glCreateShader(GLenum type) {
    const GLuint n = g_next_name++;
    g_shaders[n].type = type;
    return n;
}

void glDeleteProgram(GLuint p) {
    auto it = g_programs.find(p);
    if (it == g_programs.end()) return;
    if (g_cur_name == p) it->second->deleted = true;  // in use: deleted when it no longer is
    else g_programs.erase(it), changed();
}

void glDeleteShader(GLuint s) { g_shaders.erase(s); }

Program *program(GLuint p) {
    auto it = g_programs.find(p);
    return it == g_programs.end() ? nullptr : it->second.get();
}

static void active_var(const std::vector<Var> &list, GLuint i, GLsizei n, GLsizei *len, GLint *size, GLenum *type, char *name) {
    if (i >= list.size()) return set_error(0x0501);
    const Var &v = list[i];
    std::string nm = v.name + (v.count > 1 ? "[0]" : "");
    if (size) *size = v.count;
    if (type) *type = v.type;
    if (name && n > 0) {
        const size_t k = std::min(size_t(n - 1), nm.size());
        std::memcpy(name, nm.data(), k);
        name[k] = 0;
        if (len) *len = GLsizei(k);
    } else if (len) {
        *len = 0;
    }
}

void glGetActiveAttrib(GLuint p, GLuint i, GLsizei n, GLsizei *len, GLint *size, GLenum *type, char *name) {
    if (Program *pr = program(p)) active_var(pr->attribs, i, n, len, size, type, name);
}

void glGetActiveUniform(GLuint p, GLuint i, GLsizei n, GLsizei *len, GLint *size, GLenum *type, char *name) {
    if (Program *pr = program(p)) active_var(pr->uniforms, i, n, len, size, type, name);
}

GLint glGetAttribLocation(GLuint p, const char *name) {
    Program *pr = program(p);
    if (!pr || !name) return -1;
    const int a = pr->attrib(name);
    return a < 0 ? -1 : pr->attribs[size_t(a)].loc;
}

static void info_log(GLsizei n, GLsizei *len, char *log) {
    if (len) *len = 0;
    if (log && n > 0) log[0] = 0;
}

void glGetProgramInfoLog(GLuint, GLsizei n, GLsizei *len, char *log) { info_log(n, len, log); }

void glGetProgramiv(GLuint p, GLenum pname, GLint *out) {
    Program *pr = program(p);
    if (!pr || !out) return set_error(0x0501);
    auto max_len = [](const std::vector<Var> &l) {
        size_t m = 0;
        for (const Var &v : l) m = std::max(m, v.name.size() + (v.count > 1 ? 4 : 1));
        return GLint(m);
    };
    switch (pname) {
    case 0x8B80: *out = pr->deleted; break;           // DELETE_STATUS
    case 0x8B82: case 0x8B83: *out = pr->linked; break;  // LINK_STATUS, VALIDATE_STATUS
    case 0x8B84: *out = 0; break;                     // INFO_LOG_LENGTH
    case 0x8B85: *out = GLint(pr->shaders.size()); break;
    case 0x8B86: *out = GLint(pr->uniforms.size()); break;
    case 0x8B87: *out = max_len(pr->uniforms); break;
    case 0x8B89: *out = GLint(pr->attribs.size()); break;
    case 0x8B8A: *out = max_len(pr->attribs); break;
    default: *out = 0; break;
    }
}

void glGetShaderInfoLog(GLuint, GLsizei n, GLsizei *len, char *log) { info_log(n, len, log); }

void glGetShaderSource(GLuint s, GLsizei n, GLsizei *len, char *src) {
    auto it = g_shaders.find(s);
    const std::string text = it == g_shaders.end() ? "" : it->second.source;
    if (src && n > 0) {
        const size_t k = std::min(size_t(n - 1), text.size());
        std::memcpy(src, text.data(), k);
        src[k] = 0;
        if (len) *len = GLsizei(k);
    }
}

void glGetShaderiv(GLuint s, GLenum pname, GLint *out) {
    auto it = g_shaders.find(s);
    if (it == g_shaders.end() || !out) return set_error(0x0501);
    switch (pname) {
    case 0x8B4F: *out = GLint(it->second.type); break;  // SHADER_TYPE
    case 0x8B81: *out = 1; break;                        // COMPILE_STATUS
    case 0x8B88: *out = GLint(it->second.source.size() + 1); break;
    default: *out = 0; break;
    }
}

GLint glGetUniformLocation(GLuint p, const char *name) {
    Program *pr = program(p);
    if (!pr || !name) return -1;
    std::string n = name;
    int element = 0;
    if (const size_t b = n.find('['); b != std::string::npos) {
        element = std::atoi(n.c_str() + b + 1);
        n.resize(b);
    }
    const int u = pr->uniform(n.c_str());
    if (u < 0 || element >= pr->uniforms[size_t(u)].count) return -1;
    return pr->uniforms[size_t(u)].loc + element;
}

void glLinkProgram(GLuint p) {
    Program *pr = program(p);
    if (!pr) return set_error(0x0501);
    pr->attribs.clear(), pr->uniforms.clear(), pr->values.clear(), pr->locs.clear(), pr->defines.clear();
    std::map<std::string, std::string> vs_macros, fs_macros;
    std::vector<Var> unused;
    for (GLuint s : pr->shaders) {
        auto it = g_shaders.find(s);
        if (it == g_shaders.end()) continue;
        // (GLSL ES's predefined GL_ES is left undefined: with it, Glitch's texture warm-up programs
        // get their sampler, but chapter 3's NormalRimDistort material became invalid and the game
        // hung in CMaterialRendererManager::endMaterialRenderer while loading the level)
        std::map<std::string, std::string> macros;
        if (it->second.type == 0x8B31) parse_glsl(it->second.source, macros, pr->attribs, pr->uniforms);
        else parse_glsl(it->second.source, macros, unused, pr->uniforms);
        for (auto &[k, v] : macros) pr->defines[k] = v;
    }
    int loc = 0, offset = 0;
    for (Var &a : pr->attribs) a.loc = loc++;
    loc = 0;
    for (size_t i = 0; i < pr->uniforms.size(); i++) {
        Var &u = pr->uniforms[i];
        u.loc = loc;
        u.offset = offset;
        for (int e = 0; e < u.count; e++) pr->locs.emplace_back(int16_t(i), int16_t(e));
        loc += u.count;
        offset += u.count * u.comps;
    }
    pr->values.assign(size_t(offset), 0.0f);
    pr->linked = true;
    pr->serial++;
    std::string defs;
    for (auto &[k, v] : pr->defines)
        if (!k.empty() && std::isupper(uint8_t(k[0])) && k.find("GLITCH") == std::string::npos && (v.empty() || v == "1")) defs += k + ' ';
    std::string samplers;
    for (const Var &u : pr->uniforms)
        if (u.type == 0x8B5E || u.type == 0x8B60) samplers += u.name + ' ';
    pr->signature = "samplers: " + samplers + "defines: " + defs;
    pr->recipe = std::make_unique<Recipe>();
    if (!classify(*pr, *pr->recipe)) pr->recipe.reset();
    else material_uniforms(*pr->recipe, pr->uniforms.size());
    changed();
    // ForceCommitTexture: a texture drawn in clip space at load time, so that a phone's driver
    // uploads it; the PICA reads textures where they are
    pr->warmup = !pr->recipe && pr->defined("TEXTURE_TYPE") && pr->uniform("WorldViewProjectionMatrix") < 0 && pr->uniform("texture") >= 0;
    if (g_hooks.linked) g_hooks.linked(*pr, pr->recipe || pr->warmup);
}

void glShaderSource(GLuint s, GLsizei count, const char *const *strings, const GLint *lengths) {
    auto it = g_shaders.find(s);
    if (it == g_shaders.end()) return set_error(0x0501);
    std::string all;
    for (GLsizei i = 0; i < count; i++) all += lengths && lengths[i] >= 0 ? std::string(strings[i], size_t(lengths[i])) : std::string(strings[i]);
    it->second.source = std::move(all);
}

// Uniforms: stored as floats in the program (ints and samplers too).
template <int N, class T> static void uniform(GLint loc, GLsizei count, const T *v) {
    Program *p = g_cur_program;
    if (!p || loc < 0 || size_t(loc) >= p->locs.size() || !v) return;
    const auto [u, e] = p->locs[size_t(loc)];
    const Var &var = p->uniforms[size_t(u)];
    const int n = std::min<int>(count, var.count - e);
    float *dst = &p->values[size_t(var.offset + e * var.comps)];
    const int per = std::min(N, var.comps);
    for (int i = 0; i < n; i++)
        for (int k = 0; k < per; k++) dst[i * var.comps + k] = float(v[i * N + k]);
    if (!p->recipe || p->recipe->material[size_t(u)]) p->serial++;
}
void glUniform1f(GLint l, GLfloat v) { uniform<1>(l, 1, &v); }
void glUniform1fv(GLint l, GLsizei n, const GLfloat *v) { uniform<1>(l, n, v); }
void glUniform1i(GLint l, GLint v) { uniform<1>(l, 1, &v); }
void glUniform1iv(GLint l, GLsizei n, const GLint *v) { uniform<1>(l, n, v); }
void glUniform2fv(GLint l, GLsizei n, const GLfloat *v) { uniform<2>(l, n, v); }
void glUniform2iv(GLint l, GLsizei n, const GLint *v) { uniform<2>(l, n, v); }
void glUniform3fv(GLint l, GLsizei n, const GLfloat *v) { uniform<3>(l, n, v); }
void glUniform3iv(GLint l, GLsizei n, const GLint *v) { uniform<3>(l, n, v); }
void glUniform4fv(GLint l, GLsizei n, const GLfloat *v) { uniform<4>(l, n, v); }
void glUniform4iv(GLint l, GLsizei n, const GLint *v) { uniform<4>(l, n, v); }
void glUniformMatrix4fv(GLint l, GLsizei n, GLboolean transpose, const GLfloat *v) {
    if (!transpose) return uniform<16>(l, n, v);
    std::vector<float> t(size_t(n) * 16);
    for (GLsizei i = 0; i < n; i++)
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) t[size_t(i) * 16 + size_t(c * 4 + r)] = v[i * 16 + r * 4 + c];
    uniform<16>(l, n, t.data());
}

void glUseProgram(GLuint p) {
    if (g_cur_name && g_cur_name != p) {
        auto it = g_programs.find(g_cur_name);
        if (it != g_programs.end() && it->second->deleted) g_programs.erase(it), changed();
    }
    g_cur_name = p;
    g_cur_program = program(p);
}

}  // namespace glprog
