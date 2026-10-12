// Host input -> the game, from SDL events.
//
// Two modes:
//  * menus: the mouse is a touch pointer (pointer 0), keys become Android keycodes, and the
//    physical keyboard replaces the soft keyboard for text fields;
//  * gameplay: no touch input at all. The mouse is in relative mode (hidden, unaccelerated
//    motion), and held keys / mouse buttons / gamepad state form a control state (Controls)
//    through the [keys] bindings.
// The main thread feeds SDL events in; the game thread drains events and samples controls
// before each frame.
#pragma once
#include "platform.h"
#include "settings.h"

#include "sdl.h"

#include <cstdint>
#include <string>
#include <vector>

namespace input {

struct Event {
    enum Kind { Touch, Key, Text } kind;
    int a = 0, b = 0, c = 0, d = 0;  // Touch: action (1 down, 0 up, 2 move), x, y, pointer id. Key: down, keycode.
    std::string text;                // Text: current contents of the text field
};

// Gameplay control state, sampled once per frame on the game thread.
struct Controls {
    float move_x = 0, move_y = 0;          // strafe right / forward, each in [-1, 1]
    float look_yaw = 0, look_pitch = 0;    // view change in degrees since the last sample (right / down)
    bool fire = false, aim = false, jump = false, sprint = false, reload = false, grenade = false;
    bool melee = false, interact = false, power = false, next_weapon = false, prev_weapon = false, next_power = false,
         next_grenade = false, detonate = false;
    int weapon_steps = 0;                  // mouse wheel notches since the last sample (+ = next)
    // the arrow keys and the D-pad themselves, whatever they are bound to (set_dpad_buttons)
    bool dpad_up = false, dpad_down = false, dpad_left = false, dpad_right = false;
};

// Menu navigation state (menus only): arrows / D-pad / left stick, Enter / pad A, pad B.
struct MenuNav {
    bool up = false, down = false, left = false, right = false, confirm = false, back = false;
    bool pointer = false;  // the mouse was clicked in a menu since the last sample
};

// Before the window exists: bindings, mouse and gamepad tuning, and the file they are saved to.
void configure(const settings::Settings &s, const std::string &path);
// Game thread: the live settings, which the game's Controls page changes ([keys] bindings of
// every action but pause, [controls]); save_config() writes them to the file. The main thread
// only reads the pause binding, which never changes.
settings::Settings &config();
void save_config();
// Game thread, rebinding a control: while capturing, events and menu navigation are held back
// from the game, and take_pressed() returns the first key, mouse button or gamepad button
// pressed since the capture began or the last call, or 0.
void begin_capture();
void end_capture();
bool capturing();
int take_pressed();
// Scripted runs (tests): real input is ignored and the mouse is never captured.
void set_scripted(bool on);
#ifndef NOVA3_CONSOLE
// Main thread: the window, then every SDL event.
void set_window(SDL_Window *window);
void handle_event(const SDL_Event &e);
#else
// Consoles: the game's screen size, for the touchscreen (touches come in 1280x720 coordinates
// on the Switch, 320x240 on the 3DS's bottom screen). Controllers and the touchscreen are read
// in update(), on the game thread (libnx, libctru).
void set_screen(int width, int height);
#endif

// Any thread: queue an event (scripted input uses this).
void post(const Event &e);
// Scripted typing into an active text field.
void type_text(const std::string &s);
// Game thread, once per frame before drain(): gamepad dead zones and Back/Pause buttons.
void update();
// Game thread: take all pending events.
std::vector<Event> drain();

// Game thread: switch between menu and gameplay modes (captures/releases the mouse).
void set_gameplay(bool on);
bool gameplay();
bool action_phase();  // the game's own "action phase" flag (GL2JNILib.SetIsActionPhase)
bool player_control();  // CPlayerControl::EnablePlayerControl state (hooks.cpp)
void set_player_control(bool on);
// Game thread: whether the player's weapon is aimed down the sights ([controls]
// aim_sensitivity applies then).
void set_aiming(bool on);
// Game thread: while on, the arrow keys and the D-pad leave their bindings (moving, weapons,
// powers) and only report themselves (Controls::dpad_*): they press the buttons of a console
// that has four (hooks.cpp).
void set_dpad_buttons(bool on);
// Whether the gamepad was used more recently than the keyboard and mouse.
bool using_gamepad();
bool action_held(settings::Action a);  // any of the action's controls held (keys, mouse, pad), in any mode
// Time since the last touch, key, button or stick movement.
int64_t idle_ns();
// The first binding of an action on the given device ("F", "RB"); "" if it has none there.
std::string action_label(settings::Action a, bool gamepad);
// Game thread: GS_InGame::Update ran this frame / did it run since the last call?
void note_ingame_frame();
bool consume_ingame_frame();
// Game thread: current controls (keyboard, mouse and gamepad); mouse motion is consumed.
Controls take_controls();
// Game thread, in menus: current navigation keys / buttons.
MenuNav take_menu_nav();
// Game thread, in menus: mouse wheel notches since the last call (+ = down, toward the end
// of a list).
int take_menu_wheel();
// Scripted testing: hold an input code (settings::parse_input) / inject mouse motion.
void script_input(int code, bool down);
// Controller rumble ([gamepad] rumble / rumble_strength): low- and high-frequency motor
// strength 0..1, for `ms`; a new rumble replaces the current one. Any thread.
void rumble(float low, float high, int ms);

#ifdef NOVA3_CONSOLE
// The system keyboard for one line of text (game thread; it waits while the keyboard is up).
// False when cancelled.
bool ask_text(const std::string &initial, size_t max, const char *guide, std::string &out);
#endif
void script_look(int dx, int dy);
void script_wheel(int steps);  // + = down

// Java-side input methods (SUtils.ShowKeyboard etc.); call after jni::init().
void register_java();

}  // namespace input
