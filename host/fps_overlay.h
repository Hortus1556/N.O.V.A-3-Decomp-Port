// A small frame-rate counter in the top-right corner of the game window ([video] fps_counter),
// drawn with GLES 2 just before each swap. Every GL state it touches is saved and restored
// exactly: the runtime skips the game's GL calls that would not change its cached state, so
// state left changed here would break the game's rendering.
#pragma once

namespace fps_overlay {

// Game thread, GL context current, just before the swap: count the frame, draw the counter
// (the frame rate over the last half second) into a width x height drawable.
void draw(int width, int height);

}  // namespace fps_overlay
