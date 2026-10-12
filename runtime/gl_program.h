// GLSL programs for the GL drivers whose GPUs have no fragment shaders (the New 3DS's PICA200,
// runtime/gles_pica.cpp; the GameCube's Flipper, runtime/gles_gx.cpp): shaders, programs and
// their uniforms as GL keeps them, and the "material" each linked program is drawn with.
//
// Neither GPU can run the game's GLSL, so every program is recognised from its sources: just
// enough of the GLSL (preprocessor, attribute and uniform declarations) is parsed to answer GL's
// queries and to pick a Recipe (classify): which kind of vertex transform and lighting, which
// sampler goes to which texture unit with which coordinates, how the units are combined. The
// drivers turn a recipe into their GPU's state at each draw. Uniform values are kept as floats
// in the program (ints and samplers too).
//
// The GL entry points here take host-order arrays (runtime/gles.cpp converts the game's) and
// report errors through glprog::take_error().
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace glprog {

using GLenum = uint32_t;
using GLuint = uint32_t;
using GLint = int;
using GLsizei = int;
using GLfloat = float;
using GLboolean = uint8_t;

// An attribute or uniform as declared.
struct Var {
    std::string name;
    GLenum type = 0;
    int count = 1;     // array size
    int loc = -1;      // first location (uniforms: one per array element)
    int offset = 0;    // uniforms: first float in the program's storage
    int comps = 1;     // floats per element
};

struct Shader {
    GLenum type = 0;
    std::string source;
    bool deleted = false;
};

// The vertex transform and lighting a material needs.
enum Vs : uint8_t {
    VS_GENERIC,  // position by the MVP matrix, up to three texture coordinates, the vertex colour
    VS_LIT,      // irradiance (second-order spherical harmonics on the world normal) + one point light
    VS_POINT2,   // two world-space point lights and an ambient term
};
// How the texture units and the vertex colour are combined.
enum Tev : uint8_t {
    TEV_TEXVC,         // texture x vertex colour (UI, particles, unlit and lit models)
    TEV_LIGHTMAP,      // diffuse x lightmap x 2 (the world)
    TEV_LIGHTMAP_TEX2, // mix(diffuse, diffuse2, vertex red) x lightmap x 2
    TEV_MASK,          // diffuse [x lightmap x 2], alpha = mask green x vertex alpha
    TEV_VC,            // vertex colour
    TEV_TERRAIN,       // (red map x vertex red + green map x vertex green + blue map x vertex blue) x lightmap x 2
                       // (the PICA's three units: the blue map in a second pass)
    TEV_TEX2_MUL,      // texture1 x texture2 x vertex colour (alpha: one of their alpha maps)
    TEV_TEX2_MIX,      // mix(texture0, texture1, factor)
    TEV_TINTED,        // texture x mix(1, tint, mask red) x vertex colour (COLORMASK: team colours)
    TEV_GLOW,          // diffuse x vertex colour + diffuse x (1 - diffuse alpha), alpha = vertex alpha
};
struct UnitSpec {
    int sampler = -1;      // uniform index of its sampler
    int uv = 0;            // TexCoord0, 1 or 2
    int offset = -1;       // uniform vec2 added to the coordinates (uvOffset)
    int scaleoffset = -1;  // uniform vec4: coordinates * xy + zw (TexCoord0_scaleoffset)
    int transform = -1;    // uniform mat4 on (u, v, 1, 0) (TexCoord0_transform)
    float rgb = 1, a = 1;  // stand-in when no texture is bound
};
struct Recipe {
    Vs vs = VS_GENERIC;
    Tev tev = TEV_TEXVC;
    UnitSpec unit[3];
    bool split_alpha = false;  // the alpha is unit 2's red (SPLIT_ALPHA: ETC1 has none)
    bool lightmap = false;     // TEV_MASK: unit 1 is a lightmap
    bool unpremultiply = false;  // the shader divides rgb by alpha (blending with SRC_ALPHA then: ONE)
    int a_vertex = -1, a_uv[3] = {-1, -1, -1}, a_color = -1, a_normal = -1;  // attributes
    int u_mvp = -1;
    int u_fog_se = -1, u_fog_color = -1, u_fog_max = -1;
    int fog_kind = 0;          // 0: clamp((d - x) * z, 0, max) (FogStartEnd); 1: (-d - x) / (y - x) (fogstartend)
    int u_color = -1;          // uniform vec4 multiplying the vertex colour (Ambient)
    uint8_t color_mask = 0;    // which of its components: 1 rgb, 2 alpha
    int u_fade = -1;           // float multiplying the rgb (FadeFactor)
    int u_alpharef = -1;       // alpha test (ALPHA_TEST)
    int u_factor = -1;         // TEV_TEX2_MIX
    int u_tint = -1;           // TEV_TINTED: ColorMaskColor
    int u_blue = -1;           // TEV_TERRAIN: the blue map's sampler
    // FOGZ (height fog): the distance fog and the height fog per vertex, mixed by the combiners
    bool fogz = false, fogz_alpha = false;  // (fogz_alpha: the height fog x the vertex alpha)
    bool fogz_ramp = false, fogz_additive = false;
    bool tex2_alpha = false;   // TEV_LIGHTMAP_TEX2: the vertex alpha blends the two maps (GLOW), not red
    int u_fogz = -1, u_fogz_color = -1, u_view_inv = -1, u_wvt = -1;
    std::vector<uint8_t> material;  // per uniform: read by the material (its changes count in Program::serial)
    // VS_LIT
    int u_wit = -1, u_ird[7] = {-1, -1, -1, -1, -1, -1, -1}, u_lpos = -1, u_latt = -1, u_lcol = -1;
    // VS_POINT2: two point lights in world space
    int u_world = -1, u_l1p = -1, u_l2p = -1, u_l1c = -1, u_l2c = -1, u_l1i = -1, u_l2i = -1, u_amb = -1;
};

struct Program {
    std::vector<GLuint> shaders;
    bool linked = false, deleted = false;
    std::vector<Var> attribs, uniforms;
    std::map<std::string, std::string> defines;  // the preprocessor's macros after both shaders
    std::vector<float> values;                   // uniform storage
    std::vector<std::pair<int16_t, int16_t>> locs;  // location -> (uniform, element)
    std::string signature;                       // what the material was chosen from (logs)
    bool reported = false;                       // (its draws were skipped: logged once)
    bool warmup = false;                         // Glitch's texture warm-up pass: drawn as nothing, silently
    std::unique_ptr<Recipe> recipe;              // how it is drawn (null: not at all)
    uint32_t serial = 1;                         // bumped by every uniform change the material reads
    int uniform(const char *name) const {
        for (size_t i = 0; i < uniforms.size(); i++)
            if (uniforms[i].name == name) return int(i);
        return -1;
    }
    int attrib(const char *name) const {
        for (size_t i = 0; i < attribs.size(); i++)
            if (attribs[i].name == name) return int(i);
        return -1;
    }
    bool defined(const char *name) const { return defines.count(name) != 0; }
    const float *value(int u) const { return u >= 0 ? &values[size_t(uniforms[size_t(u)].offset)] : nullptr; }
};

// The driver's part: called when a program it may have cached state for is deleted or relinked,
// and after each link (matched: a recipe was found; the driver logs what it cannot draw).
struct Hooks {
    void (*changed)() = nullptr;
    void (*linked)(const Program &p, bool matched) = nullptr;
};
void set_hooks(const Hooks &h);

Program *current();      // the program in use (glUseProgram), or null
GLuint current_name();
Program *program(GLuint name);
const Shader *shader(GLuint name);
GLenum take_error();      // the first error since the last call (0: none)

// The GL entry points (the drivers list them in their procedure tables)
void glAttachShader(GLuint p, GLuint s);
void glCompileShader(GLuint);
GLuint glCreateProgram();
GLuint glCreateShader(GLenum type);
void glDeleteProgram(GLuint p);
void glDeleteShader(GLuint s);
void glGetActiveAttrib(GLuint p, GLuint i, GLsizei n, GLsizei *len, GLint *size, GLenum *type, char *name);
void glGetActiveUniform(GLuint p, GLuint i, GLsizei n, GLsizei *len, GLint *size, GLenum *type, char *name);
GLint glGetAttribLocation(GLuint p, const char *name);
void glGetProgramInfoLog(GLuint, GLsizei n, GLsizei *len, char *log);
void glGetProgramiv(GLuint p, GLenum pname, GLint *out);
void glGetShaderInfoLog(GLuint, GLsizei n, GLsizei *len, char *log);
void glGetShaderSource(GLuint s, GLsizei n, GLsizei *len, char *src);
void glGetShaderiv(GLuint s, GLenum pname, GLint *out);
GLint glGetUniformLocation(GLuint p, const char *name);
void glLinkProgram(GLuint p);
void glShaderSource(GLuint s, GLsizei count, const char *const *strings, const GLint *lengths);
void glUniform1f(GLint l, GLfloat v);
void glUniform1fv(GLint l, GLsizei n, const GLfloat *v);
void glUniform1i(GLint l, GLint v);
void glUniform1iv(GLint l, GLsizei n, const GLint *v);
void glUniform2fv(GLint l, GLsizei n, const GLfloat *v);
void glUniform2iv(GLint l, GLsizei n, const GLint *v);
void glUniform3fv(GLint l, GLsizei n, const GLfloat *v);
void glUniform3iv(GLint l, GLsizei n, const GLint *v);
void glUniform4fv(GLint l, GLsizei n, const GLfloat *v);
void glUniform4iv(GLint l, GLsizei n, const GLint *v);
void glUniformMatrix4fv(GLint l, GLsizei n, GLboolean transpose, const GLfloat *v);
void glUseProgram(GLuint p);

}  // namespace glprog
