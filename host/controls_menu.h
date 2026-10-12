// The game's Controls page (in-game menu page 7, also opened from the title menu), its key
// binding page and the Graphics page, built from the game's own menu widgets
// (host/controls_menu.cpp).
#pragma once
#include "runtime.h"

#include <cstdint>

namespace controls_menu {

// Game thread, every frame: applies the settings the game reads itself (aim assist and
// auto-leveling with a controller only) and saves changed settings to nova3.ini.
void frame(Cpu *cpu);
// Application::GetString: guest text replacing game string `id` on these pages, or 0.
uint32_t string(int id);
// Font DrawString wraps: leave this text out (the title menu's, under the page opened from it).
bool hide_text();

}  // namespace controls_menu
