// Discord Rich Presence: what the player is doing, shown on their Discord profile
// (host/discord.cpp).
#pragma once
#include "runtime.h"
#include "settings.h"

namespace discord {

// Before the game starts: connects to the Discord client in the background when [discord] is
// enabled with an application ID (Discord shows that application's name and icon). Test runs
// (connect = false) only log the activity.
void start(const settings::Settings &s, bool connect);
// Game thread, every frame: reads the game's state about once a second.
void frame(Cpu *cpu);

}  // namespace discord
