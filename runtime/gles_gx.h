// GLES2 on the GameCube's Flipper (runtime/gles_gx.cpp): the "driver" under runtime/gles.cpp.
#pragma once
#include <cstdint>
namespace gx {
struct Stats {
    uint32_t frames = 0, draws = 0, draws_requested = 0, clears = 0, vertices = 0;
    uint32_t tex_uploads = 0, programs_linked = 0;
    uint32_t tex_fills = 0, tex_evictions = 0;  // the pool: textures brought back from ARAM, sent out
    uint64_t tex_bytes = 0;  // the texture pool in use
};
// The screen (the preferred video mode, two XFBs), the GPU's FIFO; `pool`: MEM1 for textures.
bool init(void *pool, uint32_t pool_size);
void *proc(const char *name);  // the GL functions, for rt::gl_load
void swap();                   // the frame is complete: copied to the screen
int screen_width();
int screen_height();
Stats take_stats();  // since the last call
Stats peek_stats();  // the same, without starting over
}  // namespace gx
namespace rt {
bool install_gx_gl(void *pool, uint32_t pool_size);  // gx::init + gl_load(gx::proc)
}
