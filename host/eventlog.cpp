// Game event log (nova3.log, lines starting with "event:", "script:" and "game:"), for
// tracking down level-flow problems such as a respawn outside the map:
//  * "game:" the game's own debug console (glf::Console::PrintRaw: save/checkpoint notes,
//    asserts, script errors), which goes nowhere on a release Android build;
//  * "script:" Lua handlers the level starts (trigger callbacks such as Obj12_OnEnter);
//  * "event:" checkpoints, campaign saves and loads, spawns, respawns, teleports and
//    cinematics, each with the player position after the call;
//  * "event: position" every 5 s of gameplay.
// Each hook is an OVERRIDES wrap in recomp/arm2c.py and only logs around the original.
#include "eventlog.h"

#include "hooks.h"
#include "input.h"
#include "os.h"
#include "runtime.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// Player position "(x, y, z)", or "" outside a level. Calls into the game with the CPU state
// saved and restored, so it can run after an original returned.
std::string player_position(Cpu *cpu) {
    static const uint32_t get_level = rt::export_address("_ZN6CLevel8GetLevelEv");
    static const uint32_t get_player = rt::export_address("_ZN6CLevel9GetPlayerEv");
    if (!get_level || !get_player) return "";
    uint32_t level = rt::call_nested(cpu, get_level, {});
    uint32_t player = level ? rt::call_nested(cpu, get_player, {level}) : 0;
    if (!player) return "";
    float p[3];
    for (int i = 0; i < 3; i++) {
        uint32_t bits = LD32(player + 0x54 + 4 * i);
        std::memcpy(&p[i], &bits, 4);
    }
    char buf[96];
    std::snprintf(buf, sizeof buf, " at (%.1f, %.1f, %.1f)", p[0], p[1], p[2]);
    return buf;
}

int g_section_loads = 0;      // LoadLevelSections calls so far (the tour waits for the one its trigger runs)
int64_t g_section_load_ns = 0;  // when the last one was called

void event(Cpu *cpu, const char *what, const std::string &detail = "") {
    rt::log("event: %s%s%s", what, detail.c_str(), player_position(cpu).c_str());
}

std::string arg_int(const char *name, uint32_t v) { return std::string(" ") + name + "=" + std::to_string(int32_t(v)); }

// Lua function references (CLuaScriptManager::GetFuncRef) -> names
std::mutex g_refs_lock;
uint32_t g_lua_manager;  // the CLuaScriptManager seen in the hooks below (for --lua)
std::unordered_map<uint32_t, std::string> g_ref_names;

// one line per script name at most every 2 s (some handlers run every frame while a
// condition holds)
void script(const std::string &name, const char *how) {
    static std::unordered_map<std::string, int64_t> last;
    const int64_t now = rt::os::monotonic_ns();
    auto it = last.find(name);
    if (it != last.end() && now - it->second < 2'000'000'000) return;
    last[name] = now;
    rt::log("script: %s %s", how, name.c_str());
}

}  // namespace

namespace eventlog {

void run_lua(Cpu *cpu, const char *code) {
    static const uint32_t load_string = rt::export_address("_ZN17CLuaScriptManager14LoadFromStringEPKc");
    if (!g_lua_manager || !load_string) {
        rt::log("lua: no script manager yet");
        return;
    }
    uint32_t text = rt::gstrdup(code);
    uint32_t ok = rt::call_nested(cpu, load_string, {g_lua_manager, text});
    rt::log("lua: %s -> %u", code, ok);
}

void place_player(Cpu *cpu, float x, float y, float z) {
    static const uint32_t get_level = rt::export_address("_ZN6CLevel8GetLevelEv");
    static const uint32_t get_player = rt::export_address("_ZN6CLevel9GetPlayerEv");
    static const uint32_t set_position = rt::export_address("_ZN11CGameObject11SetPositionERKN6glitch4core8vector3dIfEE");
    static const uint32_t pos = rt::gcalloc(1, 12);
    uint32_t level = rt::call_nested(cpu, get_level, {});
    uint32_t player = level ? rt::call_nested(cpu, get_player, {level}) : 0;
    if (!player) return;
    const float p[3] = {x, y, z};
    STFV(pos, p, 3);
    rt::call_nested(cpu, set_position, {player, pos});
    event(cpu, "debug: player placed");
}

// Debug (--log-enemies EVERY): the enemies the level's scripts wait for (hooks::for_each_enemy):
// each one's object id (+0x50), position (+0x54), zone (+0xe8, its id at +0xb0), health and
// dead / targetable flags. Logged every `every` frames and whenever the count changes.
void log_enemies(Cpu *cpu, int frame, int every) {
    static const uint32_t is_dead = rt::export_address("_ZNK11CGameObject6IsDeadEv");
    static const uint32_t health = rt::export_address("_ZNK11CGameObject9GetHealthEv");
    static const uint32_t targetable = rt::export_address("_ZNK11CGameObject12IsTargetableEv");
    static int last_count = -1;
    if (!is_dead || !health || !targetable) return;
    std::string list;
    int count = 0;
    hooks::for_each_enemy([&](uint32_t o) {
        count++;
        float p[3];
        for (int i = 0; i < 3; i++) {
            const uint32_t bits = LD32(o + 0x54 + 4 * i);
            std::memcpy(&p[i], &bits, 4);
        }
        const uint32_t hp = rt::call_nested(cpu, health, {o});  // (a float)
        float h;
        std::memcpy(&h, &hp, 4);
        const uint32_t zone = LD32(o + 0xe8);
        char item[160];
        std::snprintf(item, sizeof item, "\n  enemy %d at (%.1f, %.1f, %.1f) zone %d health %.0f%s%s", int32_t(LD32(o + 0x50)),
                      p[0], p[1], p[2], zone ? int32_t(LD32(zone + 0xb0)) : 0, h,
                      rt::call_nested(cpu, is_dead, {o}) & 0xff ? " dead" : "",
                      rt::call_nested(cpu, targetable, {o}) & 0xff ? "" : " not targetable");
        list += item;
    });
    if (count == last_count && (every <= 0 || frame % every)) return;
    last_count = count;
    rt::log("enemies: %d counted%s%s", count, player_position(cpu).c_str(), list.c_str());
}

// Debug (--log-player FRAME): one line of the player's state: position, PlayerComponent state
// (+0x130: 0 normal, 1 in the air, 2 jetpack, 3 cinematic, ...), jetpack fuel (+0x398), view angles
// (the player object's +0x60 pitch / +0x68 yaw, degrees) and, on a vehicle, the jeep component's yaw / pitch (radians).
void log_player(Cpu *cpu) {
    static const uint32_t get_level = rt::export_address("_ZN6CLevel8GetLevelEv");
    static const uint32_t get_player = rt::export_address("_ZN6CLevel9GetPlayerEv");
    static const uint32_t get_component = rt::export_address("_ZN6CLevel18GetPlayerComponentEv");
    static const uint32_t get_jeep = rt::export_address("_ZN6CLevel22GetPlayerJeepComponentEv");
    const uint32_t level = rt::call_nested(cpu, get_level, {});
    const uint32_t player = level ? rt::call_nested(cpu, get_player, {level}) : 0;
    const uint32_t pc = level ? rt::call_nested(cpu, get_component, {level}) : 0;
    if (!player || !pc) return;
    auto f = [](uint32_t a) {
        float v;
        uint32_t bits = LD32(a);
        std::memcpy(&v, &bits, 4);
        return double(v);
    };
    {  // HUD buttons of the interface (CLevelInterfaceNormal, CLevel +0x188) up (+0x28): leave
       // (+0x10), detonate (+0x68), heal (+0x6c), the zoom slider (+0x78, value +0xac)
        const uint32_t iface = LD32(level + 0x188);
        auto up = [&](uint32_t off) { const uint32_t b = LD32(iface + off); return b ? LD8(b + 0x28) : 0; };
        if (iface)
            rt::log("player: hud leave %d detonate %d heal %d zoom %d (%.2f) seats %d/%d pedals %d/%d", up(0x10), up(0x68),
                    up(0x6c), up(0x78), LD32(iface + 0x78) ? f(LD32(iface + 0x78) + 0xac) : 0.0, up(0x20), up(0x24),
                    up(0x28), up(0x2c));
    }
    const uint32_t jeep = get_jeep ? rt::call_nested(cpu, get_jeep, {level}) : 0;
    char extra[128] = "";
    static const uint32_t control_type = rt::export_address("_ZNK20CPlayerJeepComponent14GetControlTypeEv");
    if (jeep && LD32(jeep + 0x17c)) {  // in a vehicle (its CVehicleCar)
        rt::log("player: vehicle control type %d", int32_t(rt::call_nested(cpu, control_type, {jeep})));
        std::snprintf(extra, sizeof extra, ", jeep yaw %.3f pitch %.3f fire %d%d%d %d%d%d seat %d%d", f(jeep + 0x164),
                      f(jeep + 0x16c), LD8(jeep + 0x30), LD8(jeep + 0x31), LD8(jeep + 0x32), LD8(jeep + 0x33),
                      LD8(jeep + 0x34), LD8(jeep + 0x35), LD8(jeep + 0x1ac), LD8(jeep + 0x1ad));
    }
    const uint32_t near = LD32(pc + 0x1a0);  // the interactive object in reach (its id at +0x50)
    {  // the current weapon (CWeaponManager +0x148 list of 24-byte entries, index +0x160): its object
       // id, aim mode (+0xac, -1: none), aimed flag (+0x61) and fire modes (template +0x14: a
       // vector of 44-byte modes at +0x1c..+0x20; two or more: secondary fire)
        const uint32_t weapons = LD32(pc + 0x170), list = weapons ? LD32(weapons + 0x148) : 0;
        const uint32_t weapon = list ? LD32(list + LD32(weapons + 0x160) * 24 + 0xc) : 0;
        const uint32_t tpl = weapon ? LD32(weapon + 0x14) : 0;
        static const uint32_t special = rt::export_address("_ZN13CPowerManager27GetCurrentSpecialPowerIndexEv");
        const uint32_t powers = LD32(pc + 0x174);  // CPowerManager
        if (weapon && tpl)
            rt::log("player: weapon %d aim %d aimed %d fire modes %d, special power %d", int32_t(LD32(weapons + 0x160)),
                    int32_t(LD32(weapon + 0xac)), LD8(weapon + 0x61), int((LD32(tpl + 0x20) - LD32(tpl + 0x1c)) / 44),
                    powers && special ? int32_t(rt::call_nested(cpu, special, {powers})) : -1);
    }
    rt::log("player: state %d fuel %.1f pitch %.2f yaw %.2f near %d shots %d%s%s", int32_t(LD32(pc + 0x130)),
            f(pc + 0x398), f(player + 0x60), f(player + 0x68), near ? int32_t(LD32(near + 0x50)) : 0, hooks::g_shots,
            extra, player_position(cpu).c_str());
}

void restart_checkpoint(Cpu *cpu) {
    static const uint32_t get_level = rt::export_address("_ZN6CLevel8GetLevelEv");
    static const uint32_t restart = rt::export_address("_ZN6CLevel12RestartLevelEb");
    uint32_t level = rt::call_nested(cpu, get_level, {});
    if (!level || !restart) return;
    event(cpu, "debug: restart from checkpoint");
    rt::call_nested(cpu, restart, {level, 1});
}

void ray_down(Cpu *cpu, float x, float y) {
    static const uint32_t singleton = rt::export_address("_ZN15CPhysicsManager9SingletonE");
    static const uint32_t ray = rt::export_address(
        "_ZN15CPhysicsManager19getClosestRayResultERKN6glitch4core8vector3dIfEES5_RS3_S6_ii");
    static const uint32_t buf = rt::gcalloc(1, 48);
    uint32_t mgr = singleton ? LD32(singleton) : 0;
    if (!mgr || !ray) return;
    const float v[12] = {x, y, 200, x, y, -400, 0, 0, 0, 0, 0, 0};
    STFV(buf, v, 12);
    uint32_t hit = rt::call_nested(cpu, ray, {mgr, buf, buf + 12, buf + 24, buf + 36, 0xffffffffu, 0xffffffffu});
    float h[3];
    LDFV(buf + 24, h, 3);
    rt::log("ray: (%.1f, %.1f) %s z=%.1f", x, y, hit & 0xff ? "hit" : "no hit", h[2]);
}

// Zone changes: the player object's zone (CGameObject+0xe8, switched when the player
// crosses a zone portal) and the zone manager's current zone (CZonesManager+0, the view
// zone; collision follows the visible zones).
static void log_zone_changes(Cpu *cpu) {
    static const uint32_t get_level = rt::export_address("_ZN6CLevel8GetLevelEv");
    static const uint32_t get_player = rt::export_address("_ZN6CLevel9GetPlayerEv");
    static uint32_t last_player_zone, last_view_zone;
    uint32_t level = get_level ? rt::call_nested(cpu, get_level, {}) : 0;
    uint32_t player = level ? rt::call_nested(cpu, get_player, {level}) : 0;
    uint32_t zones = level ? LD32(level + 0x2bc) : 0;
    if (!player || !zones) return;
    const uint32_t player_zone = LD32(player + 0xe8), view_zone = LD32(zones);
    auto id = [](uint32_t zone) { return zone ? int32_t(LD32(zone + 0xb0)) : 0; };
    if (player_zone != last_player_zone || view_zone != last_view_zone)
        rt::log("event: zone player %d, view %d%s", id(player_zone), id(view_zone), player_position(cpu).c_str());
    last_player_zone = player_zone;
    last_view_zone = view_zone;
}

void dump_zones(Cpu *cpu) {
    static const uint32_t get_level = rt::export_address("_ZN6CLevel8GetLevelEv");
    static const uint32_t get_player = rt::export_address("_ZN6CLevel9GetPlayerEv");
    uint32_t level = rt::call_nested(cpu, get_level, {});
    uint32_t player = level ? rt::call_nested(cpu, get_player, {level}) : 0;
    uint32_t zones = level ? LD32(level + 0x2bc) : 0;
    if (!player || !zones) return;
    auto id = [](uint32_t zone) { return zone ? int32_t(LD32(zone + 0xb0)) : 0; };
    rt::log("zones: player %d, view %d%s", id(LD32(player + 0xe8)), id(LD32(zones)), player_position(cpu).c_str());
    for (uint32_t it = LD32(zones + 0x18); it < LD32(zones + 0x1c); it += 4) {
        const uint32_t zone = LD32(it);
        float b[6];
        LDFV(zone + 0x12c, b, 6);
        rt::log("zones: %d (%.1f, %.1f, %.1f) - (%.1f, %.1f, %.1f) portals %u", id(zone), b[0], b[1], b[2], b[3], b[4],
                b[5], (LD32(zone + 0x118) - LD32(zone + 0x114)) / 4);
    }
}

void frame(Cpu *cpu) {
    static int64_t next;
    if (!input::gameplay()) return;
    log_zone_changes(cpu);
    const int64_t now = rt::os::monotonic_ns();
    if (now < next) return;
    next = now + 5'000'000'000;
    std::string at = player_position(cpu);
    if (!at.empty()) rt::log("event: position%s", at.c_str());
}

// Benchmark (--bench CHAPTER:FRAMES): starts the chapter as the tour does, skips its cutscenes,
// lets the scene settle for 5 s of gameplay, then times FRAMES frames of standing still and
// exits: frames per second and the game thread's CPU time per frame (with --profile, the
// hottest code too). CHAPTER:FRAMES:walk walks forward and turns instead of standing.
void bench(Cpu *cpu, int chapter, int frames, bool profile, bool walk) {
    static const uint32_t menu_singleton = rt::export_address("_ZN12CMenuManager9SingletonE");
    static const uint32_t get_level = rt::export_address("_ZN6CLevel8GetLevelEv");
    static const uint32_t levels_singleton = rt::export_address("_ZN14CLevelsManager9SingletonE");
    static const uint32_t unlock_all = rt::export_address("_ZN14CLevelsManager9UnlockAllEv");
    static int frame = 0, started = 0, space = 0, measured = 0;
    static int64_t settled = 0, last_space = 0, t0 = 0, c0 = 0;
    frame++;
    if (space && --space == 0) input::script_input(44 /*SDL_SCANCODE_SPACE*/, false);
    const int64_t now = rt::os::monotonic_ns();
    if (!started) {
        const uint32_t menu = menu_singleton ? LD32(menu_singleton) : 0;
        if (!menu || frame < 600 || !g_lua_manager) return;
        if (const uint32_t lm = levels_singleton ? LD32(levels_singleton) : 0) {
            static const uint32_t file_name = rt::export_address("_ZNK14CLevelsManager16GetLevelFileNameEi");
            const uint32_t name = file_name ? rt::call_nested(cpu, file_name, {lm, uint32_t(chapter)}) : 0;
            rt::log("bench: chapter %d: %s", chapter, name && LD32(name) ? rt::str(LD32(name)) : "?");
            rt::call_nested(cpu, unlock_all, {lm});
        }
        char code[96];
        std::snprintf(code, sizeof code, "Menu_SetLevel(%d) Menu_SendStateEvent(\"StartLevel\")", chapter);
        run_lua(cpu, code);
        started = frame;
        t0 = now;  // (the level load's start, until gameplay)
        if (std::getenv("NOVA3_PROFILE_LOAD")) rt::os::start_profiler();  // the load's hottest code (Windows)
        return;
    }
    if (!measured) {
        if (!rt::call_nested(cpu, get_level, {}) || !input::gameplay()) {  // loading, a cutscene, a dialog: Space
            settled = 0;
            if (now - last_space > 1'000'000'000 && !space) {
                input::script_input(44, true);
                space = 6;
                last_space = now;
            }
            return;
        }
        if (!settled) {
            settled = now;
            static bool loaded = false;
            if (!loaded) rt::log("bench: chapter %d loaded, in play %.1f s after the start", chapter, (now - t0) / 1e9);
            if (!loaded && std::getenv("NOVA3_PROFILE_LOAD")) rt::os::report_profiler();
            loaded = true;
        }
        if (now - settled < 5'000'000'000LL) return;
        measured = frame;
        t0 = now;
        c0 = rt::os::thread_cpu_ns();
        if (profile) rt::os::start_profiler();
        if (walk) input::script_input(26 /*SDL_SCANCODE_W*/, true);
        return;
    }
    if (walk) input::script_look(3, 0);  // walking forward, turning slowly: the level streams in
    if (frame - measured < frames) return;
    const double secs = double(now - t0) / 1e9;
    const int64_t c1 = rt::os::thread_cpu_ns();
    rt::log("bench: chapter %d: %d frames in %.2f s: %.1f fps, game thread CPU %.3f ms/frame", chapter, frames, secs,
            frames / secs, c0 >= 0 && c1 >= 0 ? double(c1 - c0) / 1e6 / frames : -1.0);
    if (profile) rt::os::report_profiler();
    rt::os::exit_process(0);
}

// Debug (--tour CHAPTER): a campaign chapter from start to finish, for crashes and broken
// areas. Starts it from the title menu as the chapter slideshow does (Menu_SetLevel, then the
// menu state event "StartLevel"; every chapter unlocked first). Then, at each checkpoint once
// gameplay has run for a second: logs it with the player's position and state, takes a screenshot
// (--tour-shots), and moves on:
//  * along the checkpoint chain, with the game's own CLevel::SkipToNextCheckpoint (the cheats
//    page: the next spawn point, saved as the checkpoint, the level restarted there);
//  * at its end, to a spawn point of the loaded rooms not visited yet (the same steps by hand:
//    CLevel +0x288, Application::SaveCheckPoint, CLevel::RestartLevel(true));
//  * when there is none, into the next section: the player is put at the trigger volume whose
//    script loads it (--tour-triggers, from tools/tourplan.py: object:handler:section), into
//    its zone, and its handler is run as walking in does (Enable(id), as the story does on the
//    way, then e.g. Obj13045_WhileIn(): SaveGame(12814) LoadLevelSections(2)); the game loads
//    the section and sets its checkpoint.
// Cutscenes and dialogs are skipped with Space. The process exits with 0 when the triggers are
// used up and nothing new is found, 2 when gameplay does not come back for a minute.
// With --tour-stop SPAWN the tour stops at that checkpoint and leaves the game to the scripted
// options, whose frame numbers then count from the stop (script_frame).
namespace {
int g_main_frame;           // the main loop's frame, as script_frame last saw it
int g_script_origin = -1;   // the main loop's frame at the tour's stop
}  // namespace

int script_frame(int frame, bool tour_stop) {
    g_main_frame = frame;
    if (!tour_stop) return frame;
    return g_script_origin < 0 ? -1 : frame - g_script_origin;
}

void tour(Cpu *cpu, int chapter, const std::string &trigger_list, const std::string &shots, int width, int height,
          int stop, const std::string &stop_lua) {
    static const uint32_t menu_singleton = rt::export_address("_ZN12CMenuManager9SingletonE");  // the title menu is up
    static const uint32_t get_level = rt::export_address("_ZN6CLevel8GetLevelEv");
    static const uint32_t get_player = rt::export_address("_ZN6CLevel9GetPlayerEv");
    static const uint32_t is_dead = rt::export_address("_ZNK11CGameObject6IsDeadEv");
    static const uint32_t skip = rt::export_address("_ZN6CLevel20SkipToNextCheckpointEv");
    static const uint32_t find_spawn = rt::export_address("_ZN6CLevel21FindSpawnPointInRoomsEi");
    static const uint32_t restart = rt::export_address("_ZN6CLevel12RestartLevelEb");
    static const uint32_t get_app = rt::export_address("_ZN11Application11GetInstanceEv");
    static const uint32_t save_checkpoint = rt::export_address("_ZN11Application14SaveCheckPointEv");
    static const uint32_t levels_singleton = rt::export_address("_ZN14CLevelsManager9SingletonE");
    static const uint32_t levels_count = rt::export_address("_ZNK14CLevelsManager14GetLevelsCountEv");
    static const uint32_t unlock_all = rt::export_address("_ZN14CLevelsManager9UnlockAllEv");
    static const uint32_t find_in_rooms = rt::export_address("_ZN6CLevel17FindObjectInRoomsEiRP5CZone");
    static const uint32_t set_zone = rt::export_address("_ZN11CGameObject7SetZoneEP5CZone");
    static const uint32_t teleport = rt::export_address("_ZN13CZonesManager14TeleportInZoneEP5CZone");
    static int frame = 0, started = 0, visits = 0, space = 0, section = 1, hold = 0;
    // after a section trigger: its LoadLevelSections not run yet (-1: not waiting), loading seen
    static int awaiting_loads = -1;
    static bool loading_seen = false;
    static int64_t awaiting_since = 0;
    static float hold_at[3];
    static std::vector<int32_t> visited;
    struct Trigger { int object, section; std::string handler; };
    static std::vector<Trigger> triggers;
    static size_t next_trigger = 0;
    if (frame == 0)
        for (size_t pos = 0; pos < trigger_list.size();) {
            const size_t end = std::min(trigger_list.find(',', pos), trigger_list.size());
            int object = 0, sec = 0;
            char handler[16] = {};
            if (std::sscanf(trigger_list.substr(pos, end - pos).c_str(), "%d:%15[A-Za-z]:%d", &object, handler, &sec) == 3)
                triggers.push_back({object, sec, handler});
            pos = end + 1;
        }
    frame++;
    if (space && --space == 0) input::script_input(44 /*SDL_SCANCODE_SPACE*/, false);
    if (g_script_origin >= 0) return;  // stopped (--tour-stop): the scripted options play on
    const uint32_t menu = menu_singleton ? LD32(menu_singleton) : 0;
    const uint32_t level = rt::call_nested(cpu, get_level, {});
    if (!started) {
        if (!menu || frame < 600 || !g_lua_manager) return;
        const uint32_t lm = levels_singleton ? LD32(levels_singleton) : 0;
        // GetLevelFileName: a reference to the chapter's std::string (libstdc++: one char pointer)
        static const uint32_t file_name = rt::export_address("_ZNK14CLevelsManager16GetLevelFileNameEi");
        const uint32_t name = lm && file_name ? rt::call_nested(cpu, file_name, {lm, uint32_t(chapter)}) : 0;
        static const uint32_t mp_count = rt::export_address("_ZNK14CLevelsManager16GetMPLevelsCountEv");
        rt::log("tour: chapter %d of %d: %s (multiplayer maps: %d)", chapter,
                lm ? int(rt::call_nested(cpu, levels_count, {lm})) : -1, name && LD32(name) ? rt::str(LD32(name)) : "?",
                lm ? int(rt::call_nested(cpu, mp_count, {lm})) : -1);
        if (lm) rt::call_nested(cpu, unlock_all, {lm});  // so no chapter is locked
        char code[96];  // what the chapter slideshow does (Obj150042_OnSelectionConfirmed)
        std::snprintf(code, sizeof code, "Menu_SetLevel(%d) Menu_SendStateEvent(\"StartLevel\")", chapter);
        run_lua(cpu, code);
        started = frame;
        return;
    }
    const int64_t now = rt::os::monotonic_ns();
    static int64_t idle_since = 0, last_space = 0, settled_since = 0;
    if (!level || !input::gameplay()) {
        settled_since = 0;
        if (awaiting_loads >= 0 && g_section_loads > awaiting_loads) loading_seen = true;
        {  // the end of the chapter: the results screen ("Congratulations!", GS_EndLevel)
            static const uint32_t end_vtable = rt::export_address("_ZTV11GS_EndLevel") + 8;
            static const uint32_t current = rt::export_address("_ZN12gxStateStack12CurrentStateEv");
            const uint32_t app = rt::call_nested(cpu, get_app, {});
            const uint32_t state = app ? rt::call_nested(cpu, current, {app + 0x18400 + 0x33c}) : 0;
            if (state && LD32(state) == end_vtable) {
                rt::log("tour: chapter %d done: the level ended (results screen) after %d checkpoints", chapter, visits);
                if (!shots.empty()) rt::gl_screenshot((shots + "/end.png").c_str(), width, height);
                rt::os::exit_process(0);
            }
        }
        if (!idle_since) idle_since = now;
        if (now - last_space > 1'000'000'000 && !space) {  // a cutscene, a dialog, "tap to continue": Space
            input::script_input(44, true);
            space = 6;
            last_space = now;
        }
        if (now - idle_since > 60'000'000'000LL) {
            rt::log("tour: stuck after %d checkpoints (no gameplay for a minute)%s", visits, player_position(cpu).c_str());
            if (!shots.empty()) rt::gl_screenshot((shots + "/stuck.png").c_str(), width, height);
            rt::os::exit_process(2);
        }
        return;
    }
    idle_since = 0;
    if (hold && --hold > 0) place_player(cpu, hold_at[0], hold_at[1], hold_at[2]);  // in the volume
    // The trigger's handler calls LoadLevelSections, and the frame after it still has the old
    // rooms: until the loading has been through (a frame out of gameplay, or 3 s since the call;
    // at most 20 s after the trigger), no spawn point is looked for (chapter 0's section 4 was
    // ended there as "done": no trigger 91880 in the section 3 rooms).
    if (awaiting_loads >= 0) {
        if (g_section_loads > awaiting_loads && (loading_seen || now - g_section_load_ns > 3'000'000'000LL)) {
            awaiting_loads = -1;
        } else if (now - awaiting_since > 20'000'000'000LL) {
            rt::log("tour: no section loaded 20 s after the trigger");
            awaiting_loads = -1;
        } else {
            return;
        }
    }
    if (!settled_since) settled_since = now;
    // a second of play here (more game time with --speed; NOVA3_TOUR_SETTLE_MS for slow machines)
    static const int64_t settle_ns = std::getenv("NOVA3_TOUR_SETTLE_MS") ? std::atoll(std::getenv("NOVA3_TOUR_SETTLE_MS")) * 1'000'000 : 1'000'000'000;
    if (now - settled_since < settle_ns) return;
    settled_since = 0;
    const int32_t id = int32_t(LD32(level + 0x288));
    if (std::find(visited.begin(), visited.end(), id) == visited.end()) {
        visited.push_back(id);
        const uint32_t player = rt::call_nested(cpu, get_player, {level});
        const bool dead = player && (rt::call_nested(cpu, is_dead, {player}) & 0xff);
        rt::log("tour: checkpoint %d: spawn %d, section %d%s%s", visits, id, section, player_position(cpu).c_str(),
                dead ? " DEAD" : "");
        if (!shots.empty()) {
            char name[64];
            std::snprintf(name, sizeof name, "/ch%02d_%02d_spawn%d.png", chapter, visits, id);
            rt::gl_screenshot((shots + name).c_str(), width, height);
        }
        visits++;
        if (id == stop) {
            g_script_origin = g_main_frame + 1;  // the next frame is the scripted options' frame 0
            hooks::g_time_scale = 1;              // --speed was for getting here
            rt::log("tour: stopped at spawn %d (frame %d): scripted input from here", id, g_script_origin);
            return;
        }
    }
    auto jump = [&](int32_t spawn) {
        rt::log("tour: jumping to spawn %d", spawn);
        hold = 0;  // (a trigger's hold would put the player back in its volume after the jump)
        // --tour-lua: what the skipped part of the level would have done (running unlocked:
        // PlayerEnableRush(true)), before the checkpoint is saved, so restarting it keeps it
        if (spawn == stop && !stop_lua.empty()) run_lua(cpu, stop_lua.c_str());
        ST32(level + 0x288, uint32_t(spawn));
        rt::call_nested(cpu, save_checkpoint, {rt::call_nested(cpu, get_app, {})});
        rt::call_nested(cpu, restart, {level, 1});
    };
    // --tour-stop: no other checkpoint, only the section triggers until the stop's room is loaded,
    // then straight there. The checkpoints on the way run their scripts, and their list is not the
    // story's order: section 2 of Lost Ark (--tour 2) passed spawns after the generator room (15723, 16620)
    // on the way to 10140, which woke the upstairs enemies there. (A spawn a trigger saves, such
    // as 17268 by Obj17269_OnEnter, is no stop: jumped to, it comes without the section it loads.)
    const bool direct = stop >= 0;
    if (direct && id != stop && rt::call_nested(cpu, find_spawn, {level, uint32_t(stop)})) {
        jump(stop);
        return;
    }
    // along the chain (SkipToNextCheckpoint reads the current spawn point without a check)
    if (!direct && rt::call_nested(cpu, find_spawn, {level, uint32_t(id)})) {
        rt::call_nested(cpu, skip, {level});
        if (int32_t(LD32(level + 0x288)) != id) return;
    }
    // a spawn point of the loaded rooms not visited yet (object IDs go past 90000: the rooms are
    // searched once per section)
    static std::vector<int32_t> spawns;
    static int scanned_section = 0;
    if (scanned_section != section) {
        scanned_section = section;
        spawns.clear();
        for (int32_t candidate = 1; candidate < 0x40000; candidate++)
            if (rt::call_nested(cpu, find_spawn, {level, uint32_t(candidate)})) spawns.push_back(candidate);
        rt::log("tour: %zu spawn points in the loaded rooms", spawns.size());
    }
    for (int32_t candidate : spawns) {
        if (direct) break;
        if (std::find(visited.begin(), visited.end(), candidate) != visited.end()) continue;
        if (!rt::call_nested(cpu, find_spawn, {level, uint32_t(candidate)})) continue;
        jump(candidate);
        return;
    }
    // the next section: into the trigger volume whose script loads it
    while (next_trigger < triggers.size()) {
        const Trigger t = triggers[next_trigger++];
        static const uint32_t zone_out = rt::gcalloc(1, 4);
        ST32(zone_out, 0);
        const uint32_t object = rt::call_nested(cpu, find_in_rooms, {level, uint32_t(t.object), zone_out});
        const uint32_t player = rt::call_nested(cpu, get_player, {level});
        if (!object || !player) {
            rt::log("tour: trigger %d (section %d) not in the loaded rooms", t.object, t.section);
            continue;
        }
        float p[3];
        LDFV(object + 0x54, p, 3);
        rt::log("tour: into trigger %d for section %d at (%.1f, %.1f, %.1f)", t.object, t.section, p[0], p[1], p[2]);
        place_player(cpu, p[0], p[1], p[2]);
        std::memcpy(hold_at, p, 12);
        hold = 240;
        if (const uint32_t zone = LD32(zone_out)) {
            rt::call_nested(cpu, set_zone, {player, zone});
            if (const uint32_t zones = LD32(level + 0x2bc)) rt::call_nested(cpu, teleport, {zones, zone});
        }
        // what walking in runs: the story enables the trigger on the way (Obj100338_WhileOut:
        // Enable(13045)), then its handler fires (its volume's extent is not known here)
        // started as the game starts handlers: a coroutine (they Wait())
        static const uint32_t get_ref = rt::export_address("_ZN17CLuaScriptManager10GetFuncRefEPKc");
        static const uint32_t start_function = rt::export_address("_ZN17CLuaScriptManager13StartFunctionEiiP11ScriptParami");
        awaiting_loads = g_section_loads, loading_seen = false, awaiting_since = now;  // (the handler may load at once)
        char code[48];
        std::snprintf(code, sizeof code, "Enable(%d)", t.object);
        run_lua(cpu, code);
        std::snprintf(code, sizeof code, "Obj%d_%s", t.object, t.handler.c_str());
        const uint32_t ref = g_lua_manager ? rt::call_nested(cpu, get_ref, {g_lua_manager, rt::gstrdup(code)}) : 0;
        if (int32_t(ref) > 0) rt::call_nested(cpu, start_function, {g_lua_manager, ref, 0, 0, 0xffffffffu});
        else rt::log("tour: no script function %s", code);
        section = t.section;
        return;
    }
    rt::log("tour: chapter %d done: %d checkpoints", chapter, visits);
    rt::os::exit_process(0);
}

}  // namespace eventlog

// glf::Console::PrintRaw(const char *): the game's debug console. Resource loading chatter
// (file probes along the search paths, texture and shader setup: ~80% of the lines) is left
// out; repeats of the same line are counted instead of printed.
static bool is_loading_noise(const std::string &line) {
    static const char *const prefixes[] = {
        "customCreateAndOpenFile", "createAndOpenFile", "Error: Cannot open file", "Could not find texture file",
        "parameter type mismatch", "INFO: Weapon type", "INFO: Grenade type", "finalizing renderer",
        "compiling GLSL", "Loaded texture", "checking ", "creating ", "Buffer released is not in VRAM",
    };
    for (const char *p : prefixes)
        if (line.compare(0, std::strlen(p), p) == 0) return true;
    return false;
}

extern "C" void pc_log_console(Cpu *cpu, uint32_t entry, GuestFn original) {
    static std::string last;
    static int repeats;
    if (const char *text = rt::str(cpu->r[0]); text && !is_loading_noise(text)) {
        std::string line = text;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line == last) {
            if (++repeats % 1000 == 0) rt::log("game: (last line repeated %d times)", repeats);
        } else {
            if (repeats > 1 && repeats % 1000) rt::log("game: (last line repeated %d times)", repeats);
            repeats = 1;
            last = line;
            rt::log("game: %s", line.c_str());
        }
    }
    original(cpu, entry);
}

// CLuaScriptManager::GetFuncRef(const char *name) -> reference
extern "C" void pc_log_func_ref(Cpu *cpu, uint32_t entry, GuestFn original) {
    g_lua_manager = cpu->r[0];
    std::string name = cpu->r[1] ? rt::str(cpu->r[1]) : "";
    original(cpu, entry);
    std::lock_guard<std::mutex> lock(g_refs_lock);
    g_ref_names[cpu->r[0]] = name;
}

// CLuaScriptManager::StartFunction / StartFunctionOnThread(int ref, ...)
extern "C" void pc_log_start_function(Cpu *cpu, uint32_t entry, GuestFn original) {
    g_lua_manager = cpu->r[0];
    std::string name;
    {
        std::lock_guard<std::mutex> lock(g_refs_lock);
        auto it = g_ref_names.find(cpu->r[1]);
        name = it != g_ref_names.end() ? it->second : "ref " + std::to_string(int32_t(cpu->r[1]));
    }
    script(name, "start");
    original(cpu, entry);
}

// CLuaScriptManager::CallFunction(const char *name, ...)
extern "C" void pc_log_call_function(Cpu *cpu, uint32_t entry, GuestFn original) {
    if (const char *name = rt::str(cpu->r[1])) script(name, "call");
    original(cpu, entry);
}

// Level-flow events, logged after the original with the player position.
#define EVENT_HOOK(fn, label, detail)                                          \
    extern "C" void fn(Cpu *cpu, uint32_t entry, GuestFn original) {          \
        const uint32_t a1 = cpu->r[1];                                         \
        (void)a1;                                                              \
        original(cpu, entry);                                                  \
        event(cpu, label, detail);                                             \
    }
EVENT_HOOK(pc_log_save_checkpoint, "checkpoint saved", "")
EVENT_HOOK(pc_log_load_checkpoint, "checkpoint loaded", "")
EVENT_HOOK(pc_log_save_campaign, "campaign saved", "")
EVENT_HOOK(pc_log_load_campaign, "campaign loaded", "")
EVENT_HOOK(pc_log_save_section, "level section saved", arg_int("section", a1))
EVENT_HOOK(pc_log_skip_checkpoint, "skip to next checkpoint", "")
EVENT_HOOK(pc_log_respawn_init, "level respawn init", "")
EVENT_HOOK(pc_log_spawn_player, "player spawned", "")
EVENT_HOOK(pc_log_respawn_player, "player respawned", "")
EVENT_HOOK(pc_log_player_save, "player checkpoint (spawn point)", "")
EVENT_HOOK(pc_log_teleport_reset, "player teleport reset", "")
EVENT_HOOK(pc_log_teleport_zone, "teleport into zone", "")
EVENT_HOOK(pc_log_lua_teleport_zone, "script TeleportPlayerToZone", "")
EVENT_HOOK(pc_log_lua_teleport_wp, "script TeleportToWaypoint", "")
// LoadLevelSections(section, flag): loads another part of the level (GS_Loading), keeping the
// player position and rotation
extern "C" void pc_log_lua_load_sections(Cpu *cpu, uint32_t entry, GuestFn original) {
    static const uint32_t to_integer = rt::export_address("lua_tointeger");
    static const uint32_t to_boolean = rt::export_address("lua_toboolean");
    const uint32_t L = cpu->r[0];
    std::string detail;
    if (to_integer && to_boolean)
        detail = arg_int("section", rt::call_nested(cpu, to_integer, {L, 1})) +
                 arg_int("flag", rt::call_nested(cpu, to_boolean, {L, 2}));
    event(cpu, "script LoadLevelSections", detail);
    g_section_loads++;
    g_section_load_ns = rt::os::monotonic_ns();
    original(cpu, entry);
}
EVENT_HOOK(pc_log_cinematic_start, "cinematic start", "")
EVENT_HOOK(pc_log_cinematic_end, "cinematic end", "")

