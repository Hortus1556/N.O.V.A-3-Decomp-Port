#include "input.h"
#include "offline.h"

#include "jni.h"
#include "os.h"
#include "runtime.h"

#ifdef __SWITCH__
#include <switch.h>
#endif
#ifdef __3DS__
#include <3ds.h>
#endif
#ifdef __GAMECUBE__
#include <ogc/pad.h>
#endif

#include <algorithm>
#include <atomic>
#include <utility>
#include <cmath>
#include <cstring>
#include <mutex>

namespace input {
namespace {

using settings::Action;
using settings::kMouseBase;
using settings::kPadBase;

std::mutex g_lock;  // event queue and text entry
std::vector<Event> g_queue;
#ifndef NOVA3_CONSOLE
SDL_Window *g_window;
#endif
settings::Settings g_cfg;  // set by configure() before the window exists; game thread: config()
std::string g_cfg_path;
bool g_scripted = false;

// Text entry state (the game's soft-keyboard requests)
bool g_text_active = false;
std::string g_text;
size_t g_text_max = 64;

// Mode and game-side flags
std::atomic<bool> g_gameplay{false}, g_action_phase{false}, g_player_control{false}, g_ingame_frame{false};
std::atomic<bool> g_aiming{false};  // the weapon is aimed down the sights (hooks::apply_controls)
bool g_dpad_buttons;  // the arrow keys and the D-pad press a console's buttons (set_dpad_buttons)
std::atomic<bool> g_last_pad{false};  // the gamepad was used after the keyboard and mouse
std::atomic<bool> g_focused{true};

// Held inputs, written by the main thread (and scripted holds), read by the game thread
std::atomic<bool> g_keys[SDL_SCANCODE_COUNT], g_script_keys[SDL_SCANCODE_COUNT];
std::atomic<uint32_t> g_mouse_buttons{0}, g_script_mouse{0};  // bit n = SDL mouse button n
std::atomic<float> g_mouse_dx{0}, g_mouse_dy{0};
std::atomic<int> g_wheel{0};
std::atomic<int> g_menu_wheel{0};  // menus: wheel notches since take_menu_wheel() (+ = down)
bool g_sprint_on = false;   // [controls] toggle_sprint: running (game thread)
bool g_sprint_key = false;  // a sprint control was down at the last sample
bool g_dragging = false;  // main thread: left button down as touch pointer 0
std::atomic<bool> g_menu_click{false};  // a menu click since the last take_menu_nav()

// Gamepad: raw state from SDL events (main thread; the Switch: libnx, game thread)...
#ifndef NOVA3_CONSOLE
SDL_Gamepad *g_gamepad;
#endif
std::atomic<uint32_t> g_pad_raw{0};  // bit n = settings::PadButton n
std::atomic<uint32_t> g_script_pad{0};  // tests: pad buttons held by script_input
std::atomic<float> g_axis[4];        // left x, left y, right x, right y; [-1, 1], y up
// ...and the game thread's view after dead zones.
struct Pad {
    uint32_t buttons = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;
} g_pad;
int64_t g_last_sample_ns;

bool bound(Action a, int code) {
    const auto &codes = g_cfg.bindings[a];
    return std::find(codes.begin(), codes.end(), code) != codes.end();
}

// Radial dead zone, rescaled so the response starts at 0 just outside it.
void stick(float fx, float fy, float &ox, float &oy) {
    float m = std::sqrt(fx * fx + fy * fy), dz = g_cfg.stick_deadzone;
    if (m <= dz) { ox = oy = 0; return; }
    float k = std::min(1.0f, (m - dz) / (1 - dz)) / m;
    ox = fx * k;
    oy = fy * k;
}

// Android KeyEvent keycodes (menu mode)
int android_keycode(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 29 + int(sc - SDL_SCANCODE_A);  // KEYCODE_A..Z
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return 8 + int(sc - SDL_SCANCODE_1);   // KEYCODE_1..9
    switch (sc) {
    case SDL_SCANCODE_0: return 7;
    case SDL_SCANCODE_ESCAPE: return 4;    // BACK
    case SDL_SCANCODE_UP: return 19;       // DPAD_UP
    case SDL_SCANCODE_DOWN: return 20;
    case SDL_SCANCODE_LEFT: return 21;
    case SDL_SCANCODE_RIGHT: return 22;
    case SDL_SCANCODE_RETURN: return 66;   // ENTER
    case SDL_SCANCODE_SPACE: return 62;
    case SDL_SCANCODE_TAB: return 61;
    case SDL_SCANCODE_BACKSPACE: return 67;  // DEL
    case SDL_SCANCODE_F1: return 82;       // MENU
    default: return 0;
    }
}

// Android Back, the pause binding (opens the pause menu in game). With the pause page's shop open
// it closes the shop instead (offline::take_back_key), down and up.
void post_back(bool down) {
    static bool taken;
    if (down ? (taken = offline::take_back_key()) : std::exchange(taken, false)) return;
    post({Event::Key, down ? 1 : 0, 4 /* KEYCODE_BACK */, 0, 0});
}

// Key event for an input code: the pause binding is Android Back; otherwise only menus get keys.
void post_key(int code, bool down) {
    if (bound(Action::Pause, code)) {
        post_back(down);
        return;
    }
    int k = (!g_gameplay && code < kMouseBase) ? android_keycode(SDL_Scancode(code)) : 0;
    if (k) post({Event::Key, down ? 1 : 0, k, 0, 0});
}

std::atomic<int64_t> g_last_input_ns{0};  // the last touch, key, button or stick movement
void note_input() { g_last_input_ns = rt::os::monotonic_ns(); }
void post_locked(Event e) {
    note_input();
    g_queue.push_back(std::move(e));
}

// One character typed while a text field is active (g_lock held).
void text_char(unsigned ch) {
    if (ch == '\b') {
        if (!g_text.empty()) g_text.pop_back();
    } else if (ch == '\r' || ch == '\n') {
        g_text_active = false;  // "Done"
    } else if (ch >= 32 && ch < 127 && g_text.size() < g_text_max) {
        g_text += char(ch);
    } else {
        return;
    }
    Event e{Event::Text};
    e.text = g_text;
    post_locked(std::move(e));
}

void release_all() {
    for (auto &k : g_keys) k = false;
    g_mouse_buttons = 0;
}

#ifndef NOVA3_CONSOLE
// Main thread (SDL_RunOnMainThread): relative mouse while playing; text input while a
// text field is active.
void apply_mouse_mode(void *) {
    if (g_window) SDL_SetWindowRelativeMouseMode(g_window, g_gameplay && !g_scripted);
}
void apply_text_input(void *) {
    if (!g_window) return;
    bool on;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        on = g_text_active;
    }
    if (on) SDL_StartTextInput(g_window);
    else SDL_StopTextInput(g_window);
}

int pad_button(int b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return settings::PadA;
    case SDL_GAMEPAD_BUTTON_EAST: return settings::PadB;
    case SDL_GAMEPAD_BUTTON_WEST: return settings::PadX;
    case SDL_GAMEPAD_BUTTON_NORTH: return settings::PadY;
    case SDL_GAMEPAD_BUTTON_BACK: return settings::PadBack;
    case SDL_GAMEPAD_BUTTON_START: return settings::PadStart;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return settings::PadL3;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return settings::PadR3;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return settings::PadLB;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return settings::PadRB;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return settings::PadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return settings::PadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return settings::PadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return settings::PadRight;
    default: return -1;
    }
}
#endif
#ifdef NOVA3_CONSOLE
std::atomic<float> g_gyro_yaw{0}, g_gyro_pitch{0};  // degrees since the last take_controls (right / down)
#endif
void set_pad_bit(int bit, bool on) {
    if (bit < 0) return;
    if (on) g_pad_raw |= 1u << bit;
    else g_pad_raw &= ~(1u << bit);
}

#ifdef __SWITCH__
// The Switch: the controllers (handheld, or the first one) and the touchscreen, read with
// libnx on the game thread at every update(). Buttons go by their labels (A is PadA, ...; with
// [gamepad] swap_ab_xy by position, the Xbox layout: B is PadA, A PadB, Y PadX, X PadY),
// ZL / ZR are the triggers, + Start, - Back. In menus the touchscreen is touch pointer 0,
// scaled from its 1280x720 to the game's screen.
PadState g_npad;
bool g_nx_ready = false;
int g_screen_w = 1280, g_screen_h = 720;
bool g_touching = false;
int g_touch_x = 0, g_touch_y = 0;

// Motion and rumble devices of the three ways to hold the controls: handheld, a Pro Controller
// (full key), a pair of Joy-Con (the right one's sensor is used).
HidSixAxisSensorHandle g_six_handheld, g_six_fullkey, g_six_joydual[2];
HidVibrationDeviceHandle g_vib_handheld[2], g_vib_fullkey[2], g_vib_joydual[2];
int64_t g_gyro_last = 0;
std::mutex g_rumble_lock;
int64_t g_rumble_until = 0;  // when the current rumble stops (0: none)

void send_vibration(float low, float high) {
    const HidVibrationValue v{low, 160.0f, high, 320.0f};
    const HidVibrationValue pair[2] = {v, v};
    const u32 style = padGetStyleSet(&g_npad);
    if (style & HidNpadStyleTag_NpadHandheld) hidSendVibrationValues(g_vib_handheld, pair, 2);
    if (style & HidNpadStyleTag_NpadFullKey) hidSendVibrationValues(g_vib_fullkey, pair, 2);
    if (style & HidNpadStyleTag_NpadJoyDual) hidSendVibrationValues(g_vib_joydual, pair, 2);
}

// Gyro: the view turns by the controller's rotation. Yaw is the angular velocity around the
// direction of gravity (from the accelerometer), so it works the same held upright (handheld)
// or flat (a controller); pitch is the rotation around the controller's left-right axis.
void poll_gyro() {
    const int64_t now = rt::os::monotonic_ns();
    const float dt = g_gyro_last ? float(now - g_gyro_last) / 1e9f : 0.0f;
    g_gyro_last = now;
    const u32 style = padGetStyleSet(&g_npad);
    HidSixAxisSensorHandle h = style & HidNpadStyleTag_NpadHandheld ? g_six_handheld
                               : style & HidNpadStyleTag_NpadFullKey ? g_six_fullkey
                                                                      : g_six_joydual[1];
    HidSixAxisSensorState st{};
    if (dt <= 0 || dt > 0.1f || !(style & (HidNpadStyleTag_NpadHandheld | HidNpadStyleTag_NpadFullKey | HidNpadStyleTag_NpadJoyDual)) ||
        !hidGetSixAxisSensorStates(h, &st, 1))
        return;
    const float gx = st.acceleration.x, gy = st.acceleration.y, gz = st.acceleration.z;
    const float g = std::sqrt(gx * gx + gy * gy + gz * gz);
    if (g < 0.5f) return;  // no usable gravity reading
    // angular velocity in revolutions per second: x 360 = degrees per second
    const float wx = st.angular_velocity.x * 360.0f, wy = st.angular_velocity.y * 360.0f, wz = st.angular_velocity.z * 360.0f;
    const float yaw_rate = -(wx * gx + wy * gy + wz * gz) / g;
    g_gyro_yaw = g_gyro_yaw + yaw_rate * dt;
    g_gyro_pitch = g_gyro_pitch - wx * dt;
}

void poll_switch() {
    if (!g_nx_ready) {
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&g_npad);
        hidInitializeTouchScreen();
        hidGetSixAxisSensorHandles(&g_six_handheld, 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
        hidGetSixAxisSensorHandles(&g_six_fullkey, 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
        hidGetSixAxisSensorHandles(g_six_joydual, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
        hidStartSixAxisSensor(g_six_handheld);
        hidStartSixAxisSensor(g_six_fullkey);
        hidStartSixAxisSensor(g_six_joydual[0]);
        hidStartSixAxisSensor(g_six_joydual[1]);
        hidInitializeVibrationDevices(g_vib_handheld, 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
        hidInitializeVibrationDevices(g_vib_fullkey, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
        hidInitializeVibrationDevices(g_vib_joydual, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
        g_nx_ready = true;
        g_last_pad = true;  // the prompts name controller buttons
    }
    padUpdate(&g_npad);
    if (g_cfg.gyro != settings::Settings::Gyro::Off) poll_gyro();
    {
        std::lock_guard<std::mutex> lock(g_rumble_lock);
        if (g_rumble_until && rt::os::monotonic_ns() >= g_rumble_until) {
            g_rumble_until = 0;
            send_vibration(0, 0);
        }
    }
    const u64 held = padGetButtons(&g_npad);
    static const struct { u64 mask; int bit; } kButtons[] = {
        {HidNpadButton_A, settings::PadA},        {HidNpadButton_B, settings::PadB},
        {HidNpadButton_X, settings::PadX},        {HidNpadButton_Y, settings::PadY},
        {HidNpadButton_L, settings::PadLB},       {HidNpadButton_R, settings::PadRB},
        {HidNpadButton_ZL, settings::PadLT},      {HidNpadButton_ZR, settings::PadRT},
        {HidNpadButton_Plus, settings::PadStart}, {HidNpadButton_Minus, settings::PadBack},
        {HidNpadButton_StickL, settings::PadL3},  {HidNpadButton_StickR, settings::PadR3},
        {HidNpadButton_Up, settings::PadUp},      {HidNpadButton_Down, settings::PadDown},
        {HidNpadButton_Left, settings::PadLeft},  {HidNpadButton_Right, settings::PadRight},
    };
    uint32_t raw = 0;
    for (const auto &b : kButtons)
        if (held & b.mask) raw |= 1u << settings::switch_pad_bit(b.bit, g_cfg.swap_ab_xy);
    g_pad_raw = raw;
    const HidAnalogStickState l = padGetStickPos(&g_npad, 0), r = padGetStickPos(&g_npad, 1);
    if (raw || std::abs(l.x) + std::abs(l.y) + std::abs(r.x) + std::abs(r.y) > 10000) note_input();
    g_axis[0] = l.x / 32767.0f;  // libnx: y up, as the game wants
    g_axis[1] = l.y / 32767.0f;
    g_axis[2] = r.x / 32767.0f;
    g_axis[3] = r.y / 32767.0f;

    HidTouchScreenState ts{};
    if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
        const int x = int(ts.touches[0].x) * g_screen_w / 1280, y = int(ts.touches[0].y) * g_screen_h / 720;
        if (!g_gameplay) {
            std::lock_guard<std::mutex> lock(g_lock);
            if (!g_touching) {
                g_menu_click = true;
                post_locked({Event::Touch, 1, x, y, 0});
            } else if (x != g_touch_x || y != g_touch_y) {
                post_locked({Event::Touch, 2, x, y, 0});
            }
        }
        g_touching = true;
        g_touch_x = x;
        g_touch_y = y;
    } else if (g_touching) {
        g_touching = false;
        if (!g_gameplay) {
            std::lock_guard<std::mutex> lock(g_lock);
            post_locked({Event::Touch, 0, g_touch_x, g_touch_y, 0});
        }
    }
}

// The game's text fields (SUtils.ShowKeyboard): the system keyboard, on the game thread
// (it waits while the keyboard is up, as the game does behind its own).
bool console_keyboard(const std::string &initial, size_t max, const char *guide, std::string &out) {
    SwkbdConfig kbd;
    char text[256] = {};
    Result rc = swkbdCreate(&kbd, 0);
    if (R_SUCCEEDED(rc)) {
        swkbdConfigMakePresetDefault(&kbd);
        swkbdConfigSetInitialText(&kbd, initial.c_str());
        if (guide) swkbdConfigSetGuideText(&kbd, guide);
        swkbdConfigSetStringLenMax(&kbd, u32(std::min<size_t>(max, sizeof text - 1)));
        rc = swkbdShow(&kbd, text, sizeof text);
        swkbdClose(&kbd);
    }
    if (R_FAILED(rc)) return false;  // cancelled
    out = text;
    return true;
}

#endif


#ifdef __3DS__
// The New 3DS: buttons, circle pad, C-stick and the bottom touch screen, read with libctru on
// the game thread at every update(). Face buttons go by their labels as on the Switch (A is
// PadA; swap_ab_xy takes them by position). The 3DS lacks stick clicks and has L / R plus
// ZL / ZR, so in gameplay: L aims and R fires (the triggers, PadLT / PadRT), ZL and ZR are
// the bumpers, D-pad up sprints and down melees (PadL3 / PadR3), Select changes power (PadUp).
// In menus (and while a binding waits, so a rebind takes the gameplay meaning) the D-pad
// navigates and Select is Back. The touch screen points at the game's screen (set_screen): 1:1
// while the menus are drawn on it (320x240), scaled to the top screen's 400x240 otherwise.
int g_screen_w = 320, g_screen_h = 240;
bool g_touching = false;
int g_touch_x = 0, g_touch_y = 0;

// Gyro aiming (New 3DS: the console's own gyroscope, [gamepad] gyro): as on the Switch, yaw is
// the angular velocity around gravity (from the accelerometer), so it works however the console
// is tilted; pitch is the rotation around the screen's horizontal axis. Untested on hardware:
// the axis signs may need [gamepad] gyro_invert_x / gyro_invert_y.
float g_gyro_dps = 0;  // degrees per second per raw gyro unit
int64_t g_gyro_last = 0;
void poll_gyro_3ds() {
    if (!g_gyro_dps) {
        if (R_FAILED(HIDUSER_EnableGyroscope()) || R_FAILED(HIDUSER_EnableAccelerometer()) ||
            R_FAILED(HIDUSER_GetGyroscopeRawToDpsCoefficient(&g_gyro_dps)) || !g_gyro_dps) {
            g_gyro_dps = -1;  // no gyroscope: don't ask again
            rt::log("input: no gyroscope");
        }
    }
    if (g_gyro_dps < 0) return;
    const int64_t now = rt::os::monotonic_ns();
    const float dt = g_gyro_last ? float(now - g_gyro_last) / 1e9f : 0.0f;
    g_gyro_last = now;
    if (dt <= 0 || dt > 0.1f) return;
    angularRate w{};
    accelVector a{};
    hidGyroRead(&w);
    hidAccelRead(&a);
    const float gx = a.x, gy = a.y, gz = a.z;
    const float g = std::sqrt(gx * gx + gy * gy + gz * gz);
    if (g < 1) return;  // no usable gravity reading
    const float wx = w.x * g_gyro_dps, wy = w.y * g_gyro_dps, wz = w.z * g_gyro_dps;
    g_gyro_yaw = g_gyro_yaw - (wx * gx + wy * gy + wz * gz) / g * dt;
    g_gyro_pitch = g_gyro_pitch - wx * dt;
}

void poll_3ds() {
    static bool ready = false;
    if (!ready) {
        ready = true;
        g_last_pad = true;  // the prompts name controller buttons
    }
    hidScanInput();
    if (g_cfg.gyro != settings::Settings::Gyro::Off) poll_gyro_3ds();
    const u32 held = hidKeysHeld();
    const bool play = g_gameplay || capturing();
    static const struct { u32 mask; int bit; } kFace[] = {
        {KEY_A, settings::PadA}, {KEY_B, settings::PadB}, {KEY_X, settings::PadX}, {KEY_Y, settings::PadY},
        {KEY_L, settings::PadLT}, {KEY_R, settings::PadRT}, {KEY_ZL, settings::PadLB}, {KEY_ZR, settings::PadRB},
        {KEY_START, settings::PadStart}, {KEY_DLEFT, settings::PadLeft}, {KEY_DRIGHT, settings::PadRight},
    };
    uint32_t raw = 0;
    for (const auto &b : kFace)
        if (held & b.mask) raw |= 1u << settings::switch_pad_bit(b.bit, g_cfg.swap_ab_xy);
    if (held & KEY_DUP) raw |= 1u << (play ? settings::PadL3 : settings::PadUp);
    if (held & KEY_DDOWN) raw |= 1u << (play ? settings::PadR3 : settings::PadDown);
    if (held & KEY_SELECT) raw |= 1u << (play ? settings::PadUp : settings::PadBack);
    g_pad_raw = raw;
    circlePosition l{}, r{};
    hidCircleRead(&l);
    hidCstickRead(&r);  // New 3DS only (reads 0 elsewhere)
    if (raw || std::abs(l.dx) + std::abs(l.dy) + std::abs(r.dx) + std::abs(r.dy) > 20) note_input();
    g_axis[0] = std::clamp(l.dx / 156.0f, -1.0f, 1.0f);  // libctru: y up, as the game wants
    g_axis[1] = std::clamp(l.dy / 156.0f, -1.0f, 1.0f);
    g_axis[2] = std::clamp(r.dx / 156.0f, -1.0f, 1.0f);
    g_axis[3] = std::clamp(r.dy / 156.0f, -1.0f, 1.0f);

    if (held & KEY_TOUCH) {
        touchPosition t{};
        hidTouchRead(&t);
        const int x = int(t.px) * g_screen_w / 320, y = int(t.py) * g_screen_h / 240;
        if (!g_gameplay) {
            std::lock_guard<std::mutex> lock(g_lock);
            if (!g_touching) {
                g_menu_click = true;
                post_locked({Event::Touch, 1, x, y, 0});
            } else if (x != g_touch_x || y != g_touch_y) {
                post_locked({Event::Touch, 2, x, y, 0});
            }
        }
        g_touching = true;
        g_touch_x = x;
        g_touch_y = y;
    } else if (g_touching) {
        g_touching = false;
        if (!g_gameplay) {
            std::lock_guard<std::mutex> lock(g_lock);
            post_locked({Event::Touch, 0, g_touch_x, g_touch_y, 0});
        }
    }
}

// The game's text fields (SUtils.ShowKeyboard): the system keyboard, on the game thread.
bool console_keyboard(const std::string &initial, size_t max, const char *guide, std::string &out) {
    SwkbdState kbd;
    char text[256] = {};
    swkbdInit(&kbd, SWKBD_TYPE_NORMAL, 2, int(std::min<size_t>(max, sizeof text - 1)));
    swkbdSetInitialText(&kbd, initial.c_str());
    if (guide) swkbdSetHintText(&kbd, guide);
    if (swkbdInputText(&kbd, text, sizeof text) != SWKBD_BUTTON_CONFIRM) return false;  // cancelled
    out = text;
    return true;
}
#endif

#ifdef __GAMECUBE__
// The GameCube controller in port 1, read on the game thread at every update(). Face buttons by
// their labels (A is PadA). One bumper (Z) and no Select or stick clicks, so in gameplay: L aims
// and R fires (the analog triggers' digital ends, PadLT / PadRT), Z is the right bumper, D-pad up
// sprints and down melees (PadL3 / PadR3), left is the left bumper and right changes power
// (PadUp). In menus (and while a binding waits) the D-pad navigates and B goes back. Rumble: the
// controller's motor (on or off: it has no strength).
int g_screen_w = 640, g_screen_h = 480;  // (no touch screen: kept for set_screen)
bool g_pad_ready = false;
std::atomic<int64_t> g_motor_until{0};  // rumble(): the motor runs until then
void poll_gc() {
    if (!g_pad_ready) {
        g_pad_ready = true;
        g_last_pad = true;  // the prompts name controller buttons
    }
    PAD_ScanPads();
    if (const int64_t until = g_motor_until.load(); until && rt::os::monotonic_ns() >= until) {
        g_motor_until = 0;
        PAD_ControlMotor(0, PAD_MOTOR_STOP);
    }
    const u16 held = PAD_ButtonsHeld(0);
    const bool play = g_gameplay || capturing();
    static const struct { u16 mask; int bit; } kFace[] = {
        {PAD_BUTTON_A, settings::PadA}, {PAD_BUTTON_B, settings::PadB}, {PAD_BUTTON_X, settings::PadX},
        {PAD_BUTTON_Y, settings::PadY}, {PAD_TRIGGER_L, settings::PadLT}, {PAD_TRIGGER_R, settings::PadRT},
        {PAD_TRIGGER_Z, settings::PadRB}, {PAD_BUTTON_START, settings::PadStart},
    };
    uint32_t raw = 0;
    for (const auto &b : kFace)
        if (held & b.mask) raw |= 1u << settings::switch_pad_bit(b.bit, g_cfg.swap_ab_xy);
    if (held & PAD_BUTTON_UP) raw |= 1u << (play ? settings::PadL3 : settings::PadUp);
    if (held & PAD_BUTTON_DOWN) raw |= 1u << (play ? settings::PadR3 : settings::PadDown);
    if (held & PAD_BUTTON_LEFT) raw |= 1u << (play ? settings::PadLB : settings::PadLeft);
    if (held & PAD_BUTTON_RIGHT) raw |= 1u << (play ? settings::PadUp : settings::PadRight);
    g_pad_raw = raw;
    const int lx = PAD_StickX(0), ly = PAD_StickY(0), rx = PAD_SubStickX(0), ry = PAD_SubStickY(0);
    if (raw || std::abs(lx) + std::abs(ly) + std::abs(rx) + std::abs(ry) > 20) note_input();
    // the sticks reach about +-100 (the main stick's gate) and +-90 (the C-stick's); y is up
    g_axis[0] = std::clamp(lx / 96.0f, -1.0f, 1.0f);
    g_axis[1] = std::clamp(ly / 96.0f, -1.0f, 1.0f);
    g_axis[2] = std::clamp(rx / 84.0f, -1.0f, 1.0f);
    g_axis[3] = std::clamp(ry / 84.0f, -1.0f, 1.0f);
}

// No system keyboard on the GameCube: the game's text fields keep their text.
bool console_keyboard(const std::string &, size_t, const char *, std::string &) { return false; }
#endif

#ifdef NOVA3_CONSOLE
void show_console_keyboard() {
    std::string initial, out;
    size_t max;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        initial = g_text;
        max = g_text_max;
    }
    const bool ok = console_keyboard(initial, max, nullptr, out);
    std::lock_guard<std::mutex> lock(g_lock);
    if (ok) {
        g_text = out;
        if (g_text.size() > g_text_max) g_text.resize(g_text_max);
        Event e{Event::Text};
        e.text = g_text;
        post_locked(std::move(e));
    }
    g_text_active = false;  // "Done"
}
#endif

}  // namespace

#ifdef NOVA3_CONSOLE
bool ask_text(const std::string &initial, size_t max, const char *guide, std::string &out) {
    return console_keyboard(initial, max, guide, out);
}
#endif

void configure(const settings::Settings &s, const std::string &path) {
    g_cfg = s;
    g_cfg_path = path;
}
settings::Settings &config() { return g_cfg; }
void save_config() {
    for (int a = 0; a < settings::kActionCount; a++) g_cfg.binding_text[a] = settings::binding_text(g_cfg.bindings[a]);
    if (g_cfg_path.empty()) return;
    rt::log(settings::save(g_cfg_path, g_cfg) ? "settings: saved %s" : "settings: could not save %s", g_cfg_path.c_str());
}

namespace {
std::atomic<bool> g_capturing{false};
struct InputSnapshot {
    bool keys[SDL_SCANCODE_COUNT] = {};
    uint32_t mouse = 0, pad = 0;
} g_capture_prev;

InputSnapshot snapshot() {
    InputSnapshot s;
    for (int i = 0; i < SDL_SCANCODE_COUNT; i++) s.keys[i] = g_keys[i] || g_script_keys[i];
    s.mouse = g_mouse_buttons | g_script_mouse;
    s.pad = g_pad_raw | g_script_pad;
    return s;
}
}  // namespace

void begin_capture() {
    g_capture_prev = snapshot();  // what is already held (the click that started it) does not count
    g_capturing = true;
}
void end_capture() { g_capturing = false; }
bool capturing() { return g_capturing; }
int take_pressed() {
    const InputSnapshot now = snapshot();
    int code = 0;
    for (int i = 1; i < SDL_SCANCODE_COUNT && !code; i++)
        if (now.keys[i] && !g_capture_prev.keys[i]) code = i;
    for (int b = 1; b <= 5 && !code; b++)
        if (((now.mouse & ~g_capture_prev.mouse) >> b) & 1) code = kMouseBase + b;
    for (int b = 0; b < settings::kPadButtonCount && !code; b++)
        if (((now.pad & ~g_capture_prev.pad) >> b) & 1) code = kPadBase + b;
    g_capture_prev = now;
    return code;
}
void set_scripted(bool on) { g_scripted = on; }
#ifdef NOVA3_CONSOLE
void set_screen(int width, int height) {
    g_screen_w = width;
    g_screen_h = height;
}
#else
void set_window(SDL_Window *window) { g_window = window; }

void handle_event(const SDL_Event &e) {
    switch (e.type) {
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        g_focused = true;
        return;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        g_focused = false;
        release_all();
        return;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!g_gamepad && g_cfg.gamepad) {
            g_gamepad = SDL_OpenGamepad(e.gdevice.which);
            if (g_gamepad) rt::log("gamepad connected: %s", SDL_GetGamepadName(g_gamepad));
        }
        return;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (g_gamepad && SDL_GetGamepadID(g_gamepad) == e.gdevice.which) {
            rt::log("gamepad disconnected");
            SDL_CloseGamepad(g_gamepad);
            g_gamepad = nullptr;
            g_pad_raw = 0;
            for (auto &a : g_axis) a = 0;
        }
        return;
    default:
        break;
    }
    if (g_scripted) return;  // tests: real input is ignored

    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        bool down = e.type == SDL_EVENT_KEY_DOWN;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            if (g_text_active) {  // typing: printable characters come as SDL_EVENT_TEXT_INPUT
                if (down && e.key.scancode == SDL_SCANCODE_BACKSPACE) text_char('\b');
                if (down && (e.key.scancode == SDL_SCANCODE_RETURN || e.key.scancode == SDL_SCANCODE_KP_ENTER)) {
                    text_char('\r');
                    SDL_StopTextInput(g_window);
                }
                return;
            }
        }
        if (e.key.scancode >= SDL_SCANCODE_COUNT) return;
        if (down) g_last_pad = false;
        g_keys[e.key.scancode] = down;
        if (!e.key.repeat) post_key(int(e.key.scancode), down);
        return;
    }
    case SDL_EVENT_TEXT_INPUT: {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_text_active)
            for (const char *p = e.text.text; *p; p++) text_char(uint8_t(*p));
        return;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        uint32_t bit = 1u << e.button.button;
        if (down) g_last_pad = false;
        if (down) g_mouse_buttons |= bit;
        else g_mouse_buttons &= ~bit;
        if (g_gameplay) {
            post_key(kMouseBase + e.button.button, down);
            return;
        }
        if (e.button.button != SDL_BUTTON_LEFT) return;
        // menus: left button is touch pointer 0, in pixels
        float scale = SDL_GetWindowPixelDensity(g_window);
        int x = int(e.button.x * scale), y = int(e.button.y * scale);
        if (down) {
            g_dragging = true;
            g_menu_click = true;
            post({Event::Touch, 1, x, y, 0});
        } else if (g_dragging) {
            g_dragging = false;
            post({Event::Touch, 0, x, y, 0});
        }
        return;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (g_gameplay) {
            if (SDL_GetWindowRelativeMouseMode(g_window)) {
                g_mouse_dx += e.motion.xrel;
                g_mouse_dy += e.motion.yrel;
            }
        } else if (g_dragging) {
            float scale = SDL_GetWindowPixelDensity(g_window);
            post({Event::Touch, 2, int(e.motion.x * scale), int(e.motion.y * scale), 0});
        }
        return;
    case SDL_EVENT_MOUSE_WHEEL:
        if (g_gameplay) {
            int steps = e.wheel.integer_y * (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1);
            g_wheel -= steps;  // wheel down = next weapon
        } else {
            g_menu_wheel -= e.wheel.integer_y * (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1);
        }
        return;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (g_gamepad && SDL_GetGamepadID(g_gamepad) == e.gbutton.which) {
            if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) g_last_pad = true;
            set_pad_bit(pad_button(e.gbutton.button), e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        }
        return;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (!g_gamepad || SDL_GetGamepadID(g_gamepad) != e.gaxis.which) return;
        float v = std::max(-1.0f, e.gaxis.value / 32767.0f);
        if (std::fabs(v) > 0.5f) g_last_pad = true;
        switch (e.gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX: g_axis[0] = v; break;
        case SDL_GAMEPAD_AXIS_LEFTY: g_axis[1] = -v; break;  // SDL: y down
        case SDL_GAMEPAD_AXIS_RIGHTX: g_axis[2] = v; break;
        case SDL_GAMEPAD_AXIS_RIGHTY: g_axis[3] = -v; break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: set_pad_bit(settings::PadLT, v > 30.0f / 255.0f); break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: set_pad_bit(settings::PadRT, v > 30.0f / 255.0f); break;
        default: break;
        }
        return;
    }
    default:
        return;
    }
}
#endif

void post(const Event &e) {
    std::lock_guard<std::mutex> lock(g_lock);
    post_locked(e);
}

void type_text(const std::string &s) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (!g_text_active) return;
    for (char ch : s) text_char(uint8_t(ch));
}

std::vector<Event> drain() {
    std::lock_guard<std::mutex> lock(g_lock);
    std::vector<Event> out;
    out.swap(g_queue);
    if (g_capturing) out.clear();  // rebinding: the key goes to the binding, not the menu
    return out;
}

void update() {
#ifdef __SWITCH__
    poll_switch();
#elif defined(__3DS__)
    poll_3ds();
#elif defined(__GAMECUBE__)
    poll_gc();
#endif
    // like the keyboard: the pad only counts while the window has focus
    bool use = g_cfg.gamepad && g_focused && !g_scripted;
    uint32_t buttons = (use ? g_pad_raw.load() : 0) | g_script_pad.load();
    stick(use ? g_axis[0].load() : 0, use ? g_axis[1].load() : 0, g_pad.lx, g_pad.ly);
    stick(use ? g_axis[2].load() : 0, use ? g_axis[3].load() : 0, g_pad.rx, g_pad.ry);
    // The pause binding works everywhere (Android Back); menus navigate through take_menu_nav().
    for (int b = 0; b < settings::kPadButtonCount; b++) {
        uint32_t bit = 1u << b;
        if (!((buttons ^ g_pad.buttons) & bit)) continue;
        if (bound(Action::Pause, kPadBase + b)) post_back(buttons & bit);
    }
    g_pad.buttons = buttons;
}

void set_gameplay(bool on) {
    if (on == g_gameplay.exchange(on)) return;
    rt::log("input: %s mode", on ? "gameplay" : "menu");
    g_mouse_dx = 0;
    g_mouse_dy = 0;
    g_sprint_on = false;
    g_wheel = 0;
    g_menu_wheel = 0;
#ifndef NOVA3_CONSOLE
    SDL_RunOnMainThread(apply_mouse_mode, nullptr, false);
#endif
}
bool gameplay() { return g_gameplay; }
bool player_control() { return g_player_control; }
void set_player_control(bool on) { g_player_control = on; }
void set_aiming(bool on) { g_aiming = on; }
void set_dpad_buttons(bool on) { g_dpad_buttons = on; }
bool using_gamepad() { return g_last_pad; }
std::string action_label(settings::Action a, bool gamepad) {
    for (int code : g_cfg.bindings[a])
        if ((code >= settings::kPadBase) == gamepad) return settings::input_label(code, g_cfg.swap_ab_xy);
    return "";
}
void note_ingame_frame() { g_ingame_frame = true; }
bool consume_ingame_frame() { return g_ingame_frame.exchange(false); }
bool action_phase() { return g_action_phase; }

// The arrow keys and the D-pad (on the 3DS its up / down are L3 / R3 in play), as one direction
// each: 0 up, 1 down, 2 left, 3 right; -1 for other codes.
int dpad_direction(int code) {
    switch (code) {
    case SDL_SCANCODE_UP: case kPadBase + settings::PadUp: return 0;
    case SDL_SCANCODE_DOWN: case kPadBase + settings::PadDown: return 1;
    case SDL_SCANCODE_LEFT: case kPadBase + settings::PadLeft: return 2;
    case SDL_SCANCODE_RIGHT: case kPadBase + settings::PadRight: return 3;
#ifdef __3DS__
    case kPadBase + settings::PadL3: return 0;
    case kPadBase + settings::PadR3: return 1;
#endif
    default: return -1;
    }
}

bool code_held(int code) {
    return code < kMouseBase ? g_keys[code] || g_script_keys[code]
           : code < kPadBase ? ((g_mouse_buttons | g_script_mouse) >> (code - kMouseBase)) & 1
                             : (g_pad.buttons >> (code - kPadBase)) & 1;
}
bool action_held(settings::Action a) {
    for (int code : g_cfg.bindings[a])
        if (code_held(code)) return true;
    return false;
}

// Sprint. With [controls] toggle_sprint, a press of any sprint control (key, mouse button or
// controller button) switches running on and the next press off; so do stopping (no forward
// movement) and aiming down the sights, like the game's own touch button.
bool sprint(const Controls &c) {
    bool key = false, pad = false;
    for (int code : g_cfg.bindings[Action::Sprint])
        (code < kPadBase ? key : pad) |= code_held(code) && !(g_dpad_buttons && dpad_direction(code) >= 0);
    const bool held = key || pad;
    if (!g_cfg.toggle_sprint) {
        g_sprint_on = false;
        return held;
    }
    if (held && !g_sprint_key) g_sprint_on = !g_sprint_on;
    g_sprint_key = held;
    if (c.move_y <= 0 || c.aim) g_sprint_on = false;
    return g_sprint_on;
}

Controls take_controls() {
    auto held = [](Action a) {
        for (int code : g_cfg.bindings[a])
            if (code_held(code) && !(g_dpad_buttons && dpad_direction(code) >= 0)) return true;
        return false;
    };
    int64_t now = rt::os::monotonic_ns();
    float dt = g_last_sample_ns ? std::min(0.1f, float(now - g_last_sample_ns) * 1e-9f) : 0;
    g_last_sample_ns = now;

    Controls c;
    c.move_x = float(held(Action::MoveRight) - held(Action::MoveLeft));
    c.move_y = float(held(Action::MoveForward) - held(Action::MoveBack));
    if (c.move_x != 0 && c.move_y != 0) {  // keyboard diagonals: same speed as straight moves
        c.move_x *= 0.70710678f;
        c.move_y *= 0.70710678f;
    }
    if (c.move_x == 0 && c.move_y == 0) {  // analog walk from the left stick
        c.move_x = g_pad.lx;
        c.move_y = g_pad.ly;
    }
    const bool aiming = g_aiming.load();
    const float sensitivity = aiming ? g_cfg.aim_sensitivity : g_cfg.sensitivity;
    const float mouse = 0.04f * sensitivity;
    const float look_speed = g_cfg.look_speed * sensitivity;
    c.look_yaw = g_mouse_dx.exchange(0) * mouse;
    c.look_pitch = g_mouse_dy.exchange(0) * mouse * (g_cfg.invert_y ? -1.0f : 1.0f);
    // right stick: squared response for fine aim near the centre
    auto curve = [](float v) { return v * std::fabs(v); };
    c.look_yaw += curve(g_pad.rx) * look_speed * dt;
    c.look_pitch -= curve(g_pad.ry) * look_speed * dt * (g_cfg.invert_y ? -1.0f : 1.0f);
#ifdef NOVA3_CONSOLE
    {
        const float gyro_yaw = g_gyro_yaw.exchange(0), gyro_pitch = g_gyro_pitch.exchange(0);
        using Gyro = settings::Settings::Gyro;
        if (g_cfg.gyro == Gyro::Always || (g_cfg.gyro == Gyro::Aim && aiming)) {
            c.look_yaw += gyro_yaw * g_cfg.gyro_sensitivity * (g_cfg.gyro_invert_x ? -1.0f : 1.0f);
            c.look_pitch += gyro_pitch * g_cfg.gyro_sensitivity * (g_cfg.gyro_invert_y ? -1.0f : 1.0f);
        }
    }
#endif
    c.fire = held(Action::Fire);
    c.aim = held(Action::Aim);
    c.jump = held(Action::Jump);
    c.sprint = sprint(c);
    c.reload = held(Action::Reload);
    c.grenade = held(Action::Grenade);
    c.melee = held(Action::Melee);
    c.interact = held(Action::Interact);
    c.power = held(Action::Power);
    c.next_weapon = held(Action::NextWeapon);
    c.prev_weapon = held(Action::PrevWeapon);
    c.next_power = held(Action::NextPower);
    c.next_grenade = held(Action::NextGrenade);
    c.detonate = held(Action::Detonate);
    c.weapon_steps = g_wheel.exchange(0);
    static const int kDirections[] = {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
                                      kPadBase + settings::PadUp, kPadBase + settings::PadDown,
                                      kPadBase + settings::PadLeft, kPadBase + settings::PadRight,
#ifdef __3DS__
                                      kPadBase + settings::PadL3, kPadBase + settings::PadR3,
#endif
    };
    bool *dpad[] = {&c.dpad_up, &c.dpad_down, &c.dpad_left, &c.dpad_right};
    for (int code : kDirections)
        if (code_held(code)) *dpad[dpad_direction(code)] = true;
    return c;
}

MenuNav take_menu_nav() {
    if (g_capturing) return {};
    auto key = [](SDL_Scancode sc) { return g_keys[sc] || g_script_keys[sc]; };
    auto pad = [](int b) { return (g_pad.buttons >> b) & 1; };
    MenuNav n;
    n.up = key(SDL_SCANCODE_UP) || pad(settings::PadUp) || g_pad.ly > 0.5f;
    n.down = key(SDL_SCANCODE_DOWN) || pad(settings::PadDown) || g_pad.ly < -0.5f;
    n.left = key(SDL_SCANCODE_LEFT) || pad(settings::PadLeft) || g_pad.lx < -0.5f;
    n.right = key(SDL_SCANCODE_RIGHT) || pad(settings::PadRight) || g_pad.lx > 0.5f;
    n.confirm = key(SDL_SCANCODE_RETURN) || key(SDL_SCANCODE_KP_ENTER) || pad(settings::PadA);
    n.back = pad(settings::PadB);
    n.pointer = g_menu_click.exchange(false);
    return n;
}

int64_t idle_ns() { return rt::os::monotonic_ns() - g_last_input_ns; }
void script_input(int code, bool down) {
    note_input();
    if (code < kMouseBase) g_script_keys[code] = down;
    else if (code < kPadBase) {
        uint32_t bit = 1u << (code - kMouseBase);
        if (down) g_script_mouse |= bit;
        else g_script_mouse &= ~bit;
    } else {  // through the pad state, as a controller's button
        uint32_t bit = 1u << (code - kPadBase);
        if (down) g_script_pad |= bit;
        else g_script_pad &= ~bit;
        return;
    }
    post_key(code, down);
}
void rumble(float low, float high, int ms) {
    if (!g_cfg.rumble || g_cfg.rumble_strength <= 0 || ms <= 0) return;
    low = std::clamp(low * g_cfg.rumble_strength, 0.0f, 1.0f);
    high = std::clamp(high * g_cfg.rumble_strength, 0.0f, 1.0f);
#ifdef __SWITCH__
    if (!g_nx_ready) return;
    std::lock_guard<std::mutex> lock(g_rumble_lock);
    send_vibration(low, high);
    g_rumble_until = rt::os::monotonic_ns() + int64_t(ms) * 1'000'000;
#elif defined(__3DS__)
    (void)ms;  // the 3DS has no rumble
#elif defined(__GAMECUBE__)
    // the motor is on or off: on for a strong enough request, off once it has run (poll_gc)
    if (std::max(low, high) < 0.3f) return;
    PAD_ControlMotor(0, PAD_MOTOR_RUMBLE);
    g_motor_until = rt::os::monotonic_ns() + int64_t(ms) * 1'000'000;
#else
    struct Request { Uint16 low, high; Uint32 ms; };
    SDL_RunOnMainThread([](void *p) {  // the gamepad belongs to the main thread
        auto *r = static_cast<Request *>(p);
        if (g_gamepad) SDL_RumbleGamepad(g_gamepad, r->low, r->high, r->ms);
        delete r;
    }, new Request{Uint16(low * 65535), Uint16(high * 65535), Uint32(ms)}, false);
#endif
}

int take_menu_wheel() { return g_capturing ? 0 : g_menu_wheel.exchange(0); }
void script_wheel(int steps) {
    if (g_gameplay) g_wheel += steps;
    else g_menu_wheel += steps;
}
void script_look(int dx, int dy) {
    g_mouse_dx += float(dx);
    g_mouse_dy += float(dy);
}

void register_java() {
    using jni::Call;
    using jni::Value;
    const std::string pkg = "com/gameloft/android/ANMP/GloftNAHM/";
    auto boolean = [](bool b) { Value v{}; v.i = b ? 1 : 0; return v; };
    jni::register_method(pkg + "GLUtils/SUtils.ShowKeyboard(Ljava/lang/String;)V", [](Call &c) {
        {
            std::lock_guard<std::mutex> lock(g_lock);
            g_text = jni::string_of(c.args[0].l);
            if (g_text.size() > g_text_max) g_text.resize(g_text_max);
            g_text_active = true;
        }
#ifdef NOVA3_CONSOLE
        show_console_keyboard();
#else
        SDL_RunOnMainThread(apply_text_input, nullptr, false);
#endif
        return Value{};
    });
    jni::register_method(pkg + "GLUtils/SUtils.HideKeyboard()V", [](Call &) {
        {
            std::lock_guard<std::mutex> lock(g_lock);
            g_text_active = false;
        }
#ifndef NOVA3_CONSOLE
        SDL_RunOnMainThread(apply_text_input, nullptr, false);
#endif
        return Value{};
    });
    jni::register_method(pkg + "GLUtils/SUtils.IsKeyboardVisible()Z", [boolean](Call &) {
        std::lock_guard<std::mutex> lock(g_lock);
        return boolean(g_text_active);
    });
    jni::register_method(pkg + "GLUtils/SUtils.GetVirtualKeyboardText()Ljava/lang/String;", [](Call &) {
        std::lock_guard<std::mutex> lock(g_lock);
        Value v{};
        v.l = jni::new_string(g_text);
        return v;
    });
    // SetInputFilters(maxLength, inputType, flag): only the length limit matters here.
    jni::register_method(pkg + "GL2JNILib.SetInputFilters(IIZ)V", [](Call &c) {
        std::lock_guard<std::mutex> lock(g_lock);
        if (c.args[0].i > 0) g_text_max = size_t(c.args[0].i);
        return Value{};
    });
    // The game tells Java when player-controlled action starts/stops.
    jni::register_method(pkg + "GL2JNILib.SetIsActionPhase(I)V", [](Call &c) {
        g_action_phase = c.args[0].i != 0;
        rt::log("action phase: %d", c.args[0].i);
        return Value{};
    });
}

}  // namespace input
