#include "settings.h"

#include "runtime.h"

#include "sdl.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

namespace settings {
namespace {

struct ActionInfo {
    const char *key;       // name in [keys]
    const char *defaults;  // default binding list
};
constexpr ActionInfo kActions[kActionCount] = {
    {"move_forward", "W, Up"},
    {"move_back", "S, Down"},
    {"move_left", "A, Left"},
    {"move_right", "D, Right"},
    {"fire", "Mouse1, PadRT"},
    {"aim", "Mouse2, PadLT"},
    {"jump", "Space, PadA"},
    {"sprint", "Shift, PadL3"},
    {"reload", "R, PadX"},
    {"grenade", "G, PadLB"},
    {"power", "F, PadRB"},
    {"melee", "V, Mouse3, PadR3"},
    {"interact", "E, PadB"},
    {"next_weapon", "Q, Mouse5, PadY, PadRight"},
    {"prev_weapon", "Mouse4, PadLeft"},
    {"next_power", "C, PadUp"},
    {"next_grenade", "H, PadDown"},
    {"detonate", "X, PadBack"},
    {"pause", "Escape, PadStart"},
};

// Friendly names; anything else goes to SDL_GetScancodeFromName (e.g. "Keypad 5", "Right Ctrl").
struct Alias { const char *name; std::vector<int> codes; };
const std::vector<Alias> &aliases() {
    static const std::vector<Alias> a = {
        {"Mouse1", {kMouseBase + 1}}, {"Mouse2", {kMouseBase + 3}}, {"Mouse3", {kMouseBase + 2}},
        {"Mouse4", {kMouseBase + 4}}, {"Mouse5", {kMouseBase + 5}},
        {"Shift", {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT}}, {"Ctrl", {SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL}},
        {"Alt", {SDL_SCANCODE_LALT, SDL_SCANCODE_RALT}}, {"Enter", {SDL_SCANCODE_RETURN}},
        {"Tilde", {SDL_SCANCODE_GRAVE}}, {"Minus", {SDL_SCANCODE_MINUS}}, {"Equals", {SDL_SCANCODE_EQUALS}},
        {"LBracket", {SDL_SCANCODE_LEFTBRACKET}}, {"RBracket", {SDL_SCANCODE_RIGHTBRACKET}},
        {"Semicolon", {SDL_SCANCODE_SEMICOLON}}, {"Quote", {SDL_SCANCODE_APOSTROPHE}},
        {"Comma", {SDL_SCANCODE_COMMA}}, {"Period", {SDL_SCANCODE_PERIOD}}, {"Slash", {SDL_SCANCODE_SLASH}},
        {"Backslash", {SDL_SCANCODE_BACKSLASH}}, {"CapsLock", {SDL_SCANCODE_CAPSLOCK}},
        {"PadUp", {kPadBase + PadUp}}, {"PadDown", {kPadBase + PadDown}}, {"PadLeft", {kPadBase + PadLeft}},
        {"PadRight", {kPadBase + PadRight}}, {"PadStart", {kPadBase + PadStart}}, {"PadBack", {kPadBase + PadBack}},
        {"PadL3", {kPadBase + PadL3}}, {"PadR3", {kPadBase + PadR3}}, {"PadLB", {kPadBase + PadLB}},
        {"PadRB", {kPadBase + PadRB}}, {"PadA", {kPadBase + PadA}}, {"PadB", {kPadBase + PadB}},
        {"PadX", {kPadBase + PadX}}, {"PadY", {kPadBase + PadY}}, {"PadLT", {kPadBase + PadLT}},
        {"PadRT", {kPadBase + PadRT}},
    };
    return a;
}

bool iequals(const std::string &a, const char *b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; i++)
        if (std::tolower(uint8_t(a[i])) != std::tolower(uint8_t(b[i]))) return false;
    return i == a.size() && !b[i];
}

std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::vector<int> parse_binding(const std::string &list, const char *action) {
    std::vector<int> codes;
    std::stringstream ss(list);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (item.empty()) continue;
        std::vector<int> c = parse_input(item);
        if (c.empty()) rt::log("settings: unknown key '%s' for %s", item.c_str(), action);
        codes.insert(codes.end(), c.begin(), c.end());
    }
    return codes;
}

bool parse_bool(const std::string &v) { return v == "1" || iequals(v, "true") || iequals(v, "yes") || iequals(v, "on"); }

const char *mode_name(DisplayMode m) {
    return m == DisplayMode::Borderless ? "borderless" : m == DisplayMode::Fullscreen ? "fullscreen" : "windowed";
}

const char *yes(bool b) { return b ? "true" : "false"; }

// The whole file, with comments; used for the defaults and by the launcher.
std::string to_file(const Settings &s) {
    std::string out;
    char buf[256];
    auto line = [&](const char *fmt, auto... args) {
        std::snprintf(buf, sizeof buf, fmt, args...);
        out += buf;
        out += '\n';
    };
    out += "; N.O.V.A. 3 settings, also edited by the launcher (nova3.exe -silent skips the launcher).\n";
    out += "; Delete this file to restore the defaults. Command-line options (--size) override these.\n";
    out += "\n[video]\n";
    line("mode = %-14s; windowed, borderless (desktop resolution) or fullscreen (exclusive)", mode_name(s.mode));
    line("display = %-11d; monitor number, 0 = primary", s.display);
    line("width = %-13d; window size, or the fullscreen resolution", s.width);
    line("height = %d", s.height);
    line("refresh = %-11d; fullscreen refresh rate in Hz, 0 = highest", s.refresh);
    line("vsync = %-13s; off, on, or adaptive (syncs when on time, tears instead of stuttering when late)",
         s.vsync == Settings::VSync::Off ? "off" : s.vsync == Settings::VSync::Adaptive ? "adaptive" : "on");
    line("max_fps = %-11d; frame limiter, 0 = unlimited", s.max_fps);
    line("anisotropy = %-8d; anisotropic texture filtering: 1 (off), 2, 4, 8, 16", s.anisotropy);
    line("fov = %-15d; horizontal field of view in degrees (at 16:9), 0 = the game's default", s.fov);
    line("hd_textures = %-7s; upscaled textures from nova3_data/hd_textures.pak (made by tools/texpack.py)",
         yes(s.hd_textures));
    line("glthread = %-10s; Switch: the GL driver's work on another core (Mesa glthread; it stops at every draw from client memory)",
         yes(s.glthread));
    out += "\n[audio]\n";
    line("volume = %-12d; master volume in percent (the game's options have music / effects)", s.volume);
    out += "\n[controls]\n";
    out += "; Also set in the game's Options > Controls.\n";
    line("sensitivity = %-7g; look speed: 1.0 = 0.04 degrees per mouse count / look_speed on the stick",
         double(s.sensitivity));
    line("aim_sensitivity = %-3g; the same while aiming down the sights", double(s.aim_sensitivity));
    line("invert_y = %s", yes(s.invert_y));
    line("aim_assist = %-8s; controller only", yes(s.aim_assist));
    line("auto_leveling = %-6s; controller only: the view levels out while moving", yes(s.auto_leveling));
    line("toggle_sprint = %-6s; press sprint to run, again (or stop, or aim) to walk (any device)",
         yes(s.toggle_sprint));
    out += "\n[gamepad]\n";
    line("enabled = %-11s; Xbox, PlayStation, Switch and other controllers (through SDL)", yes(s.gamepad));
    line("deadzone = %-10g; stick dead zone, fraction of full deflection", double(s.stick_deadzone));
    line("look_speed = %-8g; degrees per second at full right-stick deflection, at sensitivity 1",
         double(s.look_speed));
    static const char *const gyro_names[] = {"off", "aim", "always"};
    line("gyro = %-14s; motion aiming (Switch controllers): off, aim (while aiming down the sights), always",
         gyro_names[int(s.gyro)]);
    line("gyro_sensitivity = %-2g; 1 = the view turns as far as the controller", double(s.gyro_sensitivity));
    line("gyro_invert_x = %-7s; the gyro turns the view the other way left and right", yes(s.gyro_invert_x));
    line("gyro_invert_y = %-7s; and up and down", yes(s.gyro_invert_y));
    line("swap_ab_xy = %-10s; Switch: A / B and X / Y exchanged (the Xbox layout: B jumps, A goes back)",
         yes(s.swap_ab_xy));
    line("rumble = %-12s; controller rumble on shots, hits and explosions", yes(s.rumble));
    line("rumble_strength = %-3g; 0 to 1", double(s.rumble_strength));
    out += "\n[multiplayer]\n";
    line("nickname = %-10s; your name in local Wi-Fi matches (up to 16 characters), empty = the game's",
         s.nickname.c_str());
    line("peers = %-13s; other players' IP addresses (e.g. ZeroTier: 10.147.17.5, 10.147.17.9), also sent the game search",
         s.peers.c_str());
    out += "\n[discord]\n";
    line("enabled = %-11s; Rich Presence: Discord shows what you are playing (menus, chapter, map)", yes(s.discord));
    line("app_id = %-12s; ID of your Discord application (discord.com/developers), named as it should show",
         s.discord_app_id.c_str());
    out += "\n[launcher]\n";
    line("console = %-11s; Windows: show the log in a console window (otherwise it goes to nova3.log)", yes(s.console));
    out += "\n[keys]\n";
    out += "; Gameplay bindings: comma-separated lists of physical keys (named as on a US layout):\n";
    out += "; letters, digits, F1-F24, Space, Enter, Tab, Escape, Backspace, Shift, Ctrl, Alt, CapsLock,\n";
    out += "; Up, Down, Left, Right, Insert, Delete, Home, End, PageUp, PageDown, Tilde, Minus, Equals,\n";
    out += "; LBracket, RBracket, Semicolon, Quote, Comma, Period, Slash, Backslash, any SDL key name\n";
    out += "; (\"Keypad 5\", \"Right Ctrl\"), Mouse1-Mouse5, and gamepad PadA, PadB, PadX, PadY, PadLB, PadRB,\n";
    out += "; PadLT, PadRT, PadL3, PadR3, PadStart, PadBack, PadUp, PadDown, PadLeft, PadRight\n";
    out += "; (PadA/B/X/Y are positions: bottom/right/left/top, whatever the labels).\n";
    out += "; The left stick always moves, the right stick looks and the mouse wheel switches weapons\n";
    out += "; (in a sniper scope it zooms).\n";
    out += "; Menus always use the mouse as touch.\n";
    for (int i = 0; i < kActionCount; i++) line("%-12s = %s", kActions[i].key, s.binding_text[i].c_str());
    return out;
}

}  // namespace

std::vector<int> parse_input(const std::string &name) {
    for (const Alias &a : aliases())
        if (iequals(name, a.name)) return a.codes;
    SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
    if (sc != SDL_SCANCODE_UNKNOWN) return {int(sc)};
    return {};
}

bool save(const std::string &path, const Settings &s) {
    std::ofstream out(path, std::ios::binary);
    out << to_file(s);
    return bool(out);
}

Settings load(const std::string &path) {
    Settings s;
    for (int i = 0; i < kActionCount; i++) {
        s.binding_text[i] = kActions[i].defaults;
        s.bindings[i] = parse_binding(kActions[i].defaults, kActions[i].key);
    }
    std::ifstream in(path);
    if (!in) {
        s.aim_sensitivity = s.sensitivity;
        save(path, s);
        rt::log("settings: wrote defaults to %s", path.c_str());
        return s;
    }
    std::string line, section;
    int n = 0;
    while (std::getline(in, line)) {
        n++;
        line = trim(line.substr(0, line.find(';')));
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            section = trim(line.substr(1, line.find(']') - 1));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            rt::log("settings: %s:%d: expected key = value", path.c_str(), n);
            continue;
        }
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        bool known = true;
        if (iequals(section, "video")) {
            if (iequals(k, "mode")) {
                s.mode = iequals(v, "borderless") ? DisplayMode::Borderless
                         : iequals(v, "fullscreen") ? DisplayMode::Fullscreen
                                                    : DisplayMode::Windowed;
            }
            else if (iequals(k, "fullscreen")) {  // older files: fullscreen = true meant borderless
                if (parse_bool(v)) s.mode = DisplayMode::Borderless;
            }
            else if (iequals(k, "display")) s.display = std::atoi(v.c_str());
            else if (iequals(k, "width")) s.width = std::atoi(v.c_str());
            else if (iequals(k, "height")) s.height = std::atoi(v.c_str());
            else if (iequals(k, "refresh")) s.refresh = std::atoi(v.c_str());
            else if (iequals(k, "vsync"))  // off / on / adaptive (older files: true / false)
                s.vsync = iequals(v, "adaptive") ? Settings::VSync::Adaptive
                          : (iequals(v, "off") || iequals(v, "false") || v == "0") ? Settings::VSync::Off
                                                                                    : Settings::VSync::On;
            else if (iequals(k, "max_fps")) s.max_fps = std::atoi(v.c_str());
            else if (iequals(k, "anisotropy")) s.anisotropy = std::atoi(v.c_str());
            else if (iequals(k, "fov")) s.fov = std::atoi(v.c_str());
            else if (iequals(k, "hd_textures")) s.hd_textures = parse_bool(v);
            else if (iequals(k, "glthread")) s.glthread = parse_bool(v);
            else if (iequals(k, "threaded_gl")) {}  // test builds' name, on by default: now glthread, off
            else known = false;
        } else if (iequals(section, "controls") || iequals(section, "mouse")) {  // [mouse]: older files
            if (iequals(k, "sensitivity")) s.sensitivity = float(std::atof(v.c_str()));
            else if (iequals(k, "aim_sensitivity")) s.aim_sensitivity = float(std::atof(v.c_str()));
            else if (iequals(k, "invert_y")) s.invert_y = parse_bool(v);
            else if (iequals(k, "aim_assist")) s.aim_assist = parse_bool(v);
            else if (iequals(k, "auto_leveling")) s.auto_leveling = parse_bool(v);
            else if (iequals(k, "toggle_sprint")) s.toggle_sprint = parse_bool(v);
            else known = false;
        } else if (iequals(section, "gamepad")) {
            if (iequals(k, "enabled")) s.gamepad = parse_bool(v);
            else if (iequals(k, "deadzone")) s.stick_deadzone = float(std::atof(v.c_str()));
            else if (iequals(k, "look_speed")) s.look_speed = float(std::atof(v.c_str()));
            else if (iequals(k, "gyro")) s.gyro = iequals(v, "always") ? Settings::Gyro::Always
                                                 : iequals(v, "aim")    ? Settings::Gyro::Aim
                                                                        : Settings::Gyro::Off;
            else if (iequals(k, "rumble")) s.rumble = parse_bool(v);
            else if (iequals(k, "rumble_strength")) s.rumble_strength = std::clamp(float(std::atof(v.c_str())), 0.0f, 1.0f);
            else if (iequals(k, "gyro_sensitivity")) s.gyro_sensitivity = std::clamp(float(std::atof(v.c_str())), 0.1f, 5.0f);
            else if (iequals(k, "gyro_invert_x")) s.gyro_invert_x = parse_bool(v);
            else if (iequals(k, "gyro_invert_y")) s.gyro_invert_y = parse_bool(v);
            else if (iequals(k, "swap_ab_xy")) s.swap_ab_xy = parse_bool(v);
            else if (iequals(k, "aim_look_speed") || iequals(k, "invert_y")) {}  // older files: now [controls]
            else known = false;
        } else if (iequals(section, "audio")) {
            if (iequals(k, "volume")) s.volume = std::clamp(std::atoi(v.c_str()), 0, 100);
            else known = false;
        } else if (iequals(section, "multiplayer")) {
            if (iequals(k, "nickname")) s.nickname = v.substr(0, 16);
            else if (iequals(k, "peers")) s.peers = v;
            else known = false;
        } else if (iequals(section, "discord")) {
            if (iequals(k, "enabled")) s.discord = parse_bool(v);
            else if (iequals(k, "app_id")) {
                s.discord_app_id.clear();
                for (char ch : v)
                    if (ch >= '0' && ch <= '9') s.discord_app_id += ch;
            } else known = false;
        } else if (iequals(section, "launcher")) {
            if (iequals(k, "console")) s.console = parse_bool(v);
            else known = false;
        } else if (iequals(section, "keys")) {
            known = false;
            for (int i = 0; i < kActionCount; i++)
                if (iequals(k, kActions[i].key)) {
                    s.binding_text[i] = v;
                    s.bindings[i] = parse_binding(v, kActions[i].key);
                    known = true;
                }
        } else {
            known = false;
        }
        if (!known) rt::log("settings: %s:%d: unknown setting [%s] %s", path.c_str(), n, section.c_str(), k.c_str());
    }
    if (s.width < 320 || s.height < 240) s.width = 1280, s.height = 720;
    if (s.sensitivity <= 0) s.sensitivity = 1;
    if (s.aim_sensitivity <= 0) s.aim_sensitivity = s.sensitivity;
    if (s.look_speed <= 0) s.look_speed = 180;
    if (s.stick_deadzone < 0 || s.stick_deadzone > 0.9f) s.stick_deadzone = 0.2f;
    s.anisotropy = std::clamp(s.anisotropy, 1, 16);
    if (s.fov != 0) s.fov = std::clamp(s.fov, 50, 120);
    s.display = std::max(0, s.display);
    return s;
}

std::string binding_text(const std::vector<int> &codes) {
    std::vector<std::string> names;
    for (int code : codes) {
        std::string name;
        for (const Alias &a : aliases())
            if (std::find(a.codes.begin(), a.codes.end(), code) != a.codes.end()) {
                name = a.name;
                break;
            }
        if (name.empty() && code < kMouseBase) {
            const char *n = SDL_GetScancodeName(SDL_Scancode(code));
            if (n && *n) name = n;
        }
        if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    std::string out;
    for (const std::string &n : names) out += (out.empty() ? "" : ", ") + n;
    return out;
}

std::string input_label(int code, bool swap_ab_xy) {
    if (code >= kPadBase) {
#if defined(__3DS__)
        // the gameplay meaning of the 3DS's buttons (input.cpp poll_3ds); none gives "next grenade"
        static const char *const pad[kPadButtonCount] = {"SELECT", "-", "D-Pad Left", "D-Pad Right", "START",
                                                         "SELECT", "D-Pad Up", "D-Pad Down", "ZL", "ZR", "A", "B", "X",
                                                         "Y", "L", "R"};
        const int bit = code - kPadBase < kPadButtonCount ? switch_pad_bit(code - kPadBase, swap_ab_xy) : kPadButtonCount;
#elif defined(__SWITCH__)
        static const char *const pad[kPadButtonCount] = {"D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right",
                                                         "+", "-", "Left Stick", "Right Stick", "L", "R", "A", "B", "X",
                                                         "Y", "ZL", "ZR"};
        const int bit = code - kPadBase < kPadButtonCount ? switch_pad_bit(code - kPadBase, swap_ab_xy) : kPadButtonCount;
#else
        static const char *const pad[kPadButtonCount] = {"D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right",
                                                         "Start", "Back", "L3", "R3", "LB", "RB", "A", "B", "X", "Y",
                                                         "LT", "RT"};
        (void)swap_ab_xy;
        const int bit = code - kPadBase;
#endif
        return bit < kPadButtonCount ? pad[bit] : "?";
    }
    if (code >= kMouseBase) {
        switch (code - kMouseBase) {
        case 1: return "Left Mouse Button";
        case 2: return "Middle Mouse Button";
        case 3: return "Right Mouse Button";
        default: return "Mouse " + std::to_string(code - kMouseBase);
        }
    }
    switch (code) {  // one name for both sides, as the bindings write them
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return "Shift";
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return "Ctrl";
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return "Alt";
    default: break;
    }
    const char *name = SDL_GetScancodeName(SDL_Scancode(code));
    return name && *name ? name : "?";
}

}  // namespace settings
