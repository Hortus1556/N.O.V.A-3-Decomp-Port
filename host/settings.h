// nova3.ini: video options, mouse/gamepad tuning and the gameplay key bindings.
// The file is created with the defaults (and comments) when missing; command-line options
// override it.
#pragma once
#include <array>
#include <string>
#include <vector>

namespace settings {

enum Action {
    MoveForward, MoveBack, MoveLeft, MoveRight,
    Fire, Aim, Jump, Sprint, Reload, Grenade, Power, Melee, Interact, NextWeapon, PrevWeapon, NextPower, NextGrenade,
    Detonate, Pause,
    kActionCount
};

// Input codes in bindings: keyboard keys are SDL scancodes (physical keys, so WASD stays
// put on any layout), kMouseBase + n is mouse button n (1-5), kPadBase + n is gamepad
// button n (PadButton).
constexpr int kMouseBase = 0x400;
constexpr int kPadBase = 0x500;
enum PadButton {
    PadUp, PadDown, PadLeft, PadRight, PadStart, PadBack, PadL3, PadR3,
    PadLB, PadRB, PadA, PadB, PadX, PadY, PadLT, PadRT,
    kPadButtonCount
};
// The Switch's face buttons go to PadA..PadY by their labels (A is PadA), or with swap_ab_xy by
// position as on an Xbox controller (the bottom button, B, is PadA). The mapping is its own
// inverse: it also gives the label of the button behind a pad bit.
constexpr int switch_pad_bit(int bit, bool swap_ab_xy) {
    if (!swap_ab_xy) return bit;
    switch (bit) {
    case PadA: return PadB;
    case PadB: return PadA;
    case PadX: return PadY;
    case PadY: return PadX;
    default: return bit;
    }
}

enum class DisplayMode { Windowed, Borderless, Fullscreen };

struct Settings {
    // [video]
    DisplayMode mode = DisplayMode::Windowed;  // borderless: desktop resolution; fullscreen: exclusive mode
    int display = 0;                 // monitor index (SDL display order)
    int width = 1280, height = 720;  // window size, or the exclusive fullscreen resolution
    int refresh = 0;                 // exclusive fullscreen refresh rate in Hz, 0 = highest
    enum class VSync { Off, On, Adaptive } vsync = VSync::On;  // adaptive: tear instead of stutter when late
#ifdef __3DS__
    int max_fps = 30;                // frame limiter: 30 (the target) or 60 on the 3DS
#else
    int max_fps = 0;                 // frame limiter, 0 = off
#endif
    int anisotropy = 16;             // anisotropic texture filtering: 1 (off), 2, 4, 8, 16
    int fov = 0;                     // horizontal field of view in degrees (16:9), 0 = the game's own
    bool hd_textures = true;         // upscaled textures from hd_textures.pak, when it is there
    // Switch: Mesa runs the GL driver's work on another core (glthread). Off: this Mesa (20.1)
    // waits for that core at every draw from client memory, about half the game's draws
    bool glthread = false;
    // [audio]
    int volume = 100;                // master volume in percent
    // [controls] (also in the game's Options > Controls)
    float sensitivity = 1.0f;        // look speed factor: mouse 0.04 degrees per count, stick look_speed
    float aim_sensitivity = -1;      // the same while aiming down the sights; < 0: as sensitivity
    bool invert_y = false;
    bool aim_assist = true;          // the game's aim assist, with a controller only
    bool auto_leveling = false;      // the view levels out while moving, with a controller only
#ifdef __3DS__
    bool toggle_sprint = true;       // (the 3DS sprints with the D-pad, under the thumb on the circle pad)
#else
    bool toggle_sprint = false;      // a press of sprint (any device) switches running on and off
#endif
    // [gamepad]
    bool gamepad = true;
    float stick_deadzone = 0.2f;     // fraction of full deflection
    float look_speed = 180.0f;       // degrees per second at full right-stick deflection, sensitivity 1
    enum class Gyro { Off, Aim, Always } gyro = Gyro::Off;  // motion aiming (Switch controllers)
    float gyro_sensitivity = 1.0f;   // 1 = the view turns as far as the controller
    bool gyro_invert_x = false, gyro_invert_y = false;  // turn the other way left-right / up-down
    bool swap_ab_xy = false;         // Switch: A / B and X / Y exchanged (B jumps, A interacts and goes back)
    bool rumble = true;              // controller rumble on shots, hits, explosions
    float rumble_strength = 1.0f;    // 0..1
    // [multiplayer]
    std::string nickname;            // name shown to other players (local Wi-Fi); empty: the game's own
    std::string peers;               // other players' IPv4 addresses, for networks without broadcasts
    // [discord]
    bool discord = true;             // Rich Presence (needs discord_app_id)
    std::string discord_app_id;      // the Discord application whose name and icon are shown
    // [launcher]
    bool console = false;            // Windows: show the log in a console window (else nova3.log)
    // [keys]
    std::array<std::vector<int>, kActionCount> bindings;
    std::array<std::string, kActionCount> binding_text;  // as written in the file (saved back verbatim)
};

// Reads `path`, writing a commented default file first when it does not exist.
Settings load(const std::string &path);
// Writes all settings (with comments) to `path`.
bool save(const std::string &path, const Settings &s);

// One binding name ("W", "Space", "Shift", "Mouse1", "PadA", any SDL key name) -> input
// codes ("Shift" etc. cover both sides); empty if unknown.
std::vector<int> parse_input(const std::string &name);
// Binding list as the file writes it ("Q, Mouse5, PadY"; both sides of Shift etc. as one).
std::string binding_text(const std::vector<int> &codes);
// An input code as players know it ("Space", "Left Mouse Button", "RT", "D-Pad Up"; on the
// Switch its buttons: "ZR", "+", and A / B / X / Y as labelled, after swap_ab_xy).
std::string input_label(int code, bool swap_ab_xy);

}  // namespace settings
