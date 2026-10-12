// GLES2 on the New 3DS's PICA200 (runtime/gles_pica.cpp): the "driver" under runtime/gles.cpp.
#pragma once
#include <cstdint>
#include <vector>

namespace pica {
struct Stats {
    uint32_t frames = 0, draws = 0, draws_requested = 0, draws_skipped = 0, clears = 0, vertices = 0;
    uint32_t tex_uploads = 0, tex_cow = 0, buf_cow = 0, programs_linked = 0, programs_unmatched = 0;
    uint32_t materials_reused = 0;  // draws whose material was the last draw's (setup skipped most of its work)
    double gpu_ms = 0;   // GPU drawing time (C3D_GetDrawingTime), summed
    double wait_ms = 0;  // waiting for the GPU at frame starts (inside the first draw or clear), summed
    uint32_t ring_peak = 0, ring_overflow = 0;
    uint32_t stride_mismatch = 0;  // draws with a vertex its components do not fill (logged once per layout)
    uint64_t copied_bytes = 0;  // client-memory vertex data copied into the ring
    uint64_t phase_ticks[13] = {};  // PICA_PROFILE builds: where draws' time goes (gles_pica.cpp, PROFILE_MARK)
    float cmdbuf_peak = 0;
    uint64_t tex_bytes = 0, tex_peak = 0, buf_bytes = 0, buf_peak = 0;
};
bool init();                          // citro3d, the top screen, the shaders
void *proc(const char *name);         // the GL functions, for rt::gl_load
void swap();                          // the frame is complete: to the screen
void finish();                        // the process's end: the GPU's work done, citro3d closed (the drawing thread)
// Between frames: draw on the bottom screen (320x240) or the top one (400x240, the start); the
// next frame is not shown (docs/3ds.md, Platform).
void set_screen(bool bottom);
Stats take_stats();                   // since the last call
// The pack of large images made ready for the PICA (tools/3ds_glreplay/picapack.py,
// nova3_data/pica_textures.pak); without it they are converted at load (seconds each).
void set_texture_pack(const char *path);
void read_screen(int x, int y, int w, int h, uint8_t *rgba);  // the last frame (GL rows: bottom first)
// Debugging (tools/3ds_glreplay): the textures rendered to, and an RGBA8 texture's PICA image.
std::vector<uint32_t> render_targets();
bool read_texture(uint32_t name, int &w, int &h, std::vector<uint8_t> &rgba);
// Replay experiments (A/B in one emulator start): 1 the state and uniforms through citro3d's own
// path (C3Di_UpdateContext at every draw), as before send_state.
void set_debug(int flags);
void reset_depth_scale();  // the depth (and fog) scale back to its start: replays independent of the one before
}  // namespace pica

namespace rt {
bool install_pica_gl();  // pica::init + gl_load(pica::proc); false if citro3d could not start
}
