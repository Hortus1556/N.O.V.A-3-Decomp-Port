#include "fps_overlay.h"

#include "os.h"
#include "runtime.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengles2.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace fps_overlay {
namespace {

// The GLES 2 functions, through SDL (ANGLE on Windows), loaded once.
struct Gl {
    PFNGLGETINTEGERVPROC GetIntegerv;
    PFNGLGETBOOLEANVPROC GetBooleanv;
    PFNGLISENABLEDPROC IsEnabled;
    PFNGLENABLEPROC Enable;
    PFNGLDISABLEPROC Disable;
    PFNGLVIEWPORTPROC Viewport;
    PFNGLCOLORMASKPROC ColorMask;
    PFNGLUSEPROGRAMPROC UseProgram;
    PFNGLBINDBUFFERPROC BindBuffer;
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
    PFNGLGETVERTEXATTRIBIVPROC GetVertexAttribiv;
    PFNGLGETVERTEXATTRIBPOINTERVPROC GetVertexAttribPointerv;
    PFNGLVERTEXATTRIBPOINTERPROC VertexAttribPointer;
    PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray;
    PFNGLDISABLEVERTEXATTRIBARRAYPROC DisableVertexAttribArray;
    PFNGLCREATESHADERPROC CreateShader;
    PFNGLSHADERSOURCEPROC ShaderSource;
    PFNGLCOMPILESHADERPROC CompileShader;
    PFNGLCREATEPROGRAMPROC CreateProgram;
    PFNGLATTACHSHADERPROC AttachShader;
    PFNGLBINDATTRIBLOCATIONPROC BindAttribLocation;
    PFNGLLINKPROGRAMPROC LinkProgram;
    PFNGLGETPROGRAMIVPROC GetProgramiv;
    PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation;
    PFNGLUNIFORM4FPROC Uniform4f;
    PFNGLDRAWARRAYSPROC DrawArrays;
} gl;
bool g_loaded, g_failed;
GLuint g_program;
GLint g_color;

template <class T>
bool load(T &fn, const char *name) {
    fn = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
    return fn != nullptr;
}

bool init() {
    if (g_loaded || g_failed) return g_loaded;
    g_failed = true;
    bool ok = load(gl.GetIntegerv, "glGetIntegerv") && load(gl.GetBooleanv, "glGetBooleanv") &&
              load(gl.IsEnabled, "glIsEnabled") && load(gl.Enable, "glEnable") && load(gl.Disable, "glDisable") &&
              load(gl.Viewport, "glViewport") && load(gl.ColorMask, "glColorMask") &&
              load(gl.UseProgram, "glUseProgram") && load(gl.BindBuffer, "glBindBuffer") &&
              load(gl.BindFramebuffer, "glBindFramebuffer") && load(gl.GetVertexAttribiv, "glGetVertexAttribiv") &&
              load(gl.GetVertexAttribPointerv, "glGetVertexAttribPointerv") &&
              load(gl.VertexAttribPointer, "glVertexAttribPointer") &&
              load(gl.EnableVertexAttribArray, "glEnableVertexAttribArray") &&
              load(gl.DisableVertexAttribArray, "glDisableVertexAttribArray") &&
              load(gl.CreateShader, "glCreateShader") && load(gl.ShaderSource, "glShaderSource") &&
              load(gl.CompileShader, "glCompileShader") && load(gl.CreateProgram, "glCreateProgram") &&
              load(gl.AttachShader, "glAttachShader") && load(gl.BindAttribLocation, "glBindAttribLocation") &&
              load(gl.LinkProgram, "glLinkProgram") && load(gl.GetProgramiv, "glGetProgramiv") &&
              load(gl.GetUniformLocation, "glGetUniformLocation") && load(gl.Uniform4f, "glUniform4f") &&
              load(gl.DrawArrays, "glDrawArrays");
    if (!ok) {
        rt::log("fps counter: GL functions missing");
        return false;
    }
    static const char *vs = "attribute vec2 a_pos; void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }";
    static const char *fs = "precision mediump float; uniform vec4 u_color; void main() { gl_FragColor = u_color; }";
    GLuint v = gl.CreateShader(GL_VERTEX_SHADER), f = gl.CreateShader(GL_FRAGMENT_SHADER);
    gl.ShaderSource(v, 1, &vs, nullptr);
    gl.ShaderSource(f, 1, &fs, nullptr);
    gl.CompileShader(v);
    gl.CompileShader(f);
    g_program = gl.CreateProgram();
    gl.AttachShader(g_program, v);
    gl.AttachShader(g_program, f);
    gl.BindAttribLocation(g_program, 0, "a_pos");
    gl.LinkProgram(g_program);
    GLint linked = 0;
    gl.GetProgramiv(g_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        rt::log("fps counter: shader did not link");
        return false;
    }
    g_color = gl.GetUniformLocation(g_program, "u_color");
    g_failed = false;
    g_loaded = true;
    return true;
}

// 3x5 pixel glyphs, one row per 3 bits (top row first): digits and "FPS".
uint16_t glyph(char c) {
    switch (c) {
    case '0': return 0b111101101101111;
    case '1': return 0b010110010010111;
    case '2': return 0b111001111100111;
    case '3': return 0b111001111001111;
    case '4': return 0b101101111001001;
    case '5': return 0b111100111001111;
    case '6': return 0b111100111101111;
    case '7': return 0b111001010010010;
    case '8': return 0b111101111101111;
    case '9': return 0b111101111001111;
    case 'F': return 0b111100110100100;
    case 'P': return 0b111101111100100;
    case 'S': return 0b111100111001111;
    default: return 0;
    }
}

// two triangles for the pixel rectangle (x, y, w, h) in a width x height drawable, y down
void rect(std::vector<float> &out, float x, float y, float w, float h, int width, int height) {
    const float x0 = x / width * 2 - 1, x1 = (x + w) / width * 2 - 1;
    const float y0 = 1 - y / height * 2, y1 = 1 - (y + h) / height * 2;
    const float v[] = {x0, y0, x1, y0, x0, y1, x1, y0, x1, y1, x0, y1};
    out.insert(out.end(), v, v + 12);
}

int64_t g_window_start;
int g_frames;
char g_text[16] = "";

}  // namespace

void draw(int width, int height) {
    // the rate over the last half second
    const int64_t now = rt::os::monotonic_ns();
    if (!g_window_start) g_window_start = now;
    g_frames++;
    if (now - g_window_start >= 500000000) {
        const double fps = g_frames * 1e9 / double(now - g_window_start);
        std::snprintf(g_text, sizeof g_text, "%d FPS", int(fps + 0.5));
        g_window_start = now;
        g_frames = 0;
    }
    if (!g_text[0] || width <= 0 || height <= 0 || !init()) return;

    // the quads: a dark box, then the glyphs' pixels
    const int px = std::max(2, height / 200);  // one font pixel: 2 at 360 lines, 5 at 1080
    const int len = int(std::strlen(g_text));
    const float text_w = float(len * 4 - 1) * px, text_h = 5.0f * px, pad = float(px) * 2;
    const float box_w = text_w + pad * 2, box_h = text_h + pad * 2;
    const float bx = width - box_w - px * 2, by = float(px) * 2;
    std::vector<float> box, text;
    rect(box, bx, by, box_w, box_h, width, height);
    for (int i = 0; i < len; i++) {
        const uint16_t bits = glyph(g_text[i]);
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if (bits & (1u << (14 - row * 3 - col)))
                    rect(text, bx + pad + float(i * 4 + col) * px, by + pad + float(row) * px, float(px), float(px), width,
                         height);
    }

    // save every state touched here
    GLint program, array_buffer, framebuffer, viewport[4];
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &program);
    gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
    gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    gl.GetIntegerv(GL_VIEWPORT, viewport);
    GLboolean color_mask[4];
    gl.GetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    const GLenum caps[] = {GL_BLEND, GL_DEPTH_TEST, GL_SCISSOR_TEST, GL_STENCIL_TEST, GL_CULL_FACE};
    GLboolean enabled[5];
    for (int i = 0; i < 5; i++) enabled[i] = gl.IsEnabled(caps[i]);
    GLint a_enabled, a_size, a_type, a_normalized, a_stride, a_buffer;
    void *a_pointer = nullptr;
    gl.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a_enabled);
    gl.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a_size);
    gl.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a_type);
    gl.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a_normalized);
    gl.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a_stride);
    gl.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a_buffer);
    gl.GetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a_pointer);

    // draw into the window
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.Viewport(0, 0, width, height);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    for (GLenum cap : caps) gl.Disable(cap);
    gl.UseProgram(g_program);
    gl.BindBuffer(GL_ARRAY_BUFFER, 0);
    gl.EnableVertexAttribArray(0);
    gl.Uniform4f(g_color, 0.0f, 0.0f, 0.0f, 1.0f);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, box.data());
    gl.DrawArrays(GL_TRIANGLES, 0, GLsizei(box.size() / 2));
    gl.Uniform4f(g_color, 0.55f, 1.0f, 0.55f, 1.0f);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, text.data());
    gl.DrawArrays(GL_TRIANGLES, 0, GLsizei(text.size() / 2));

    // and put it all back
    gl.BindBuffer(GL_ARRAY_BUFFER, GLuint(a_buffer));
    gl.VertexAttribPointer(0, a_size, GLenum(a_type), GLboolean(a_normalized), a_stride, a_pointer);
    if (!a_enabled) gl.DisableVertexAttribArray(0);
    gl.BindBuffer(GL_ARRAY_BUFFER, GLuint(array_buffer));
    gl.UseProgram(GLuint(program));
    for (int i = 0; i < 5; i++) {
        if (enabled[i]) gl.Enable(caps[i]);
        else gl.Disable(caps[i]);
    }
    gl.ColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    gl.Viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    gl.BindFramebuffer(GL_FRAMEBUFFER, GLuint(framebuffer));
}

}  // namespace fps_overlay
