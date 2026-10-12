// Applying display settings while the game runs (the in-game Graphics page): each platform's
// main implements these (host/main.cpp on the PC, host/switch/main.cpp on the Switch).
#pragma once
#include "settings.h"

#include <string>
#include <vector>

namespace display {

// Apply what can change at run time (display mode, resolution, VSync, frame cap, FOV,
// anisotropy); returns false for what needs a restart (it is still saved).
bool apply(const settings::Settings &cfg);
// The resolutions to offer (width x height), the current one included.
std::vector<std::pair<int, int>> resolutions();
// Save the settings to nova3.ini (the file the game started with).
void save(const settings::Settings &cfg);

}  // namespace display
