// Host-side hooks into game functions (registered in recomp/arm2c.py OVERRIDES) and the
// gameplay control layer built on them.
#pragma once
#include <functional>
#include "input.h"
#include "recomp.h"

namespace hooks {

extern bool g_trace_events;  // --trace-events 1: log every EventManager::raiseSync
extern float g_time_scale;   // --speed (tests): game time per real time, 1 = normal
extern int g_shots;          // shots of the player's weapons (CWeaponComponent::DoFire), for --log-player
extern float g_fov_scale;    // camera field of view relative to the game's own (1 = unchanged)
#ifdef __3DS__
// The width Application::GetFontSize and the glyph outlines are worked out for, instead of the
// screen's (0): the in-game fonts are loaded on the bottom screen (320x240) for the top one.
extern int g_font_width;
#endif

// Game thread, once per frame while in gameplay mode: movement + view from KB/M.
// Menus (outside gameplay): `nav` drives the game's own menu navigation instead.
void apply_controls(Cpu *cpu, const input::Controls &c, const input::MenuNav &nav);
// Game thread, every frame: continue past the loading screen's "Touch the screen to continue".
void auto_continue_loading(Cpu *cpu);
// The top game state (gxGameState*; 0 if none).
uint32_t top_state(Cpu *cpu);
// A loading screen is up (a level or section: GS_Loading; the title menu: GS_LoadMenu).
bool loading(Cpu *cpu);
// A multiplayer game is on (MpManager::IsMultiplayerGame on the GameMpManager instance).
bool multiplayer();
// Game thread, every frame: after a section load, find the zone with ground under the restored
// player (see pc_hook_section_restore).
void watch_section_fall(Cpu *cpu);
// The enemies the level's scripts count (GetNoEnemies): each one's CGameObject.
void for_each_enemy(const std::function<void(uint32_t)> &fn);
// Game thread, every frame: an enemy fallen out of the level dies (see hooks.cpp).
void kill_fallen_enemies(Cpu *cpu);
// Debug (--dump-hud frame): log the in-game interface buttons and controls.
void dump_hud(Cpu *cpu);

}  // namespace hooks
