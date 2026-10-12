// Game event log: checkpoints, spawns, teleports, trigger scripts and the game's own debug
// console in nova3.log (see eventlog.cpp).
#pragma once

#include "runtime.h"

#include <string>

namespace eventlog {

// Game thread, every frame: the player position every few seconds of gameplay.
void frame(Cpu *cpu);
// Debug (--lua / --place): run a Lua chunk with the level's script manager; move the player.
void run_lua(Cpu *cpu, const char *code);
void place_player(Cpu *cpu, float x, float y, float z);
void restart_checkpoint(Cpu *cpu);
void log_player(Cpu *cpu);  // --log-player FRAME: position, state, jetpack fuel, view angles
void log_enemies(Cpu *cpu, int frame, int every);  // --log-enemies EVERY: the enemies the scripts count
void ray_down(Cpu *cpu, float x, float y);
void dump_zones(Cpu *cpu);
void enter_zone_at(Cpu *cpu, float x, float y, float z);
// Debug (--tour CHAPTER [--tour-shots DIR]): every frame; plays a campaign chapter through,
// checkpoint by checkpoint, then exits (see eventlog.cpp).
void bench(Cpu *cpu, int chapter, int frames, bool profile, bool walk);  // --bench CHAPTER:FRAMES[:walk] [--profile]
void tour(Cpu *cpu, int chapter, const std::string &triggers, const std::string &shots, int width, int height, int stop = -1,
          const std::string &stop_lua = "");  // TeleportInZone(GetZoneFromPos)  // physics ray from z=+200 to z=-400, logs the hit  // CLevel::RestartLevel(true), the pause menu's restart
// --tour-stop SPAWN: the frame number the scripted options (--hold, --look, --screenshot, ...)
// see: counted from the tour's stop at that checkpoint, -1 until then; the frame itself without.
int script_frame(int frame, bool tour_stop);

}  // namespace eventlog
