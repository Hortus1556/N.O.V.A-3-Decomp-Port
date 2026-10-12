// Launcher window shown before the game (skipped with -silent): display, graphics and
// control settings. Uses Dear ImGui on an SDL renderer; SDL video must be initialised.
#pragma once
#include "settings.h"

#include <string>

namespace launcher {

// Shows the launcher. Returns true when the player pressed Play: `cfg` then holds the
// chosen settings, already saved to `config_path`. Returns false on Quit / window close.
bool run(settings::Settings &cfg, const std::string &config_path);
// Whether the HD texture pack is there (its option is greyed out otherwise).
void set_hd_textures_found(bool found);
// Debug: render the launcher once into a PNG (window never takes focus), without playing.
void screenshot(settings::Settings cfg, const std::string &config_path, const char *png_path);

}  // namespace launcher
