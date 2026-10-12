// One Controls page for the whole game, built from the game's own menu widgets.
//
// The in-game menu (GS_InGameMenu, C++ pages of CPanel / CSlider / CButtonSpr widgets) has
// page 7, Controls; the title menu (Lua screens from menus.lvc) had its own, different
// Controls screen (150509). That screen now opens the in-game menu at page 7 instead
// (Menu_GoToMenu), so both menus show the same page. Its widgets are re-purposed:
//
//   Sensitivity slider            -> [controls] sensitivity
//   Aim Acceleration slider       -> "Aim Sensitivity", [controls] aim_sensitivity
//   Control Scheme button         -> "Keyboard & Mouse": the binding page
//   Customize Interface button    -> "Controller": the binding page for the gamepad
//   Vehicle controls option       -> unchanged (CGameSettings +0x14)
//   LEFT-HANDED switch            -> "AIM ASSIST" (controller only)
//   INVERT Y-AXIS switch          -> [controls] invert_y, mouse and stick
//   GYROSCOPE switch              -> "AUTO-LEVELING" (controller only); the row is only built on
//                                    devices with a gyroscope, so HasGyroscope says yes there
//
// Rows added below them: TOGGLE RUN, RUMBLE, Rumble Strength, and on the Switch GYRO AIMING,
// Gyro Sensitivity, the gyro's two INVERT rows and SWAP A/B, X/Y.
//
// The binding page is page 9 (Control Scheme, which pops back to page 7): a panel of label +
// button rows, one per action, drawn by the Controls page's renderer with that panel in
// place. Clicking a row waits for the next key / mouse button (gamepad: button); Escape,
// Start / Back or the other device cancel. A control taken from another action gives that
// action the old one.
//
// The Graphics page is page 9 too, with a panel of its own: the Options page's Advanced button
// (renamed Graphics) and the title menu's Advanced screen open it (display mode, resolution,
// VSync, frame cap, field of view, texture filtering, HD textures; host/display.h applies them).
// Settings are saved to nova3.ini.
#include "controls_menu.h"
#include "platform.h"

#include "display.h"
#include "input.h"
#include "offline.h"
#include "os.h"

#include "sdl.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using settings::Action;

constexpr int kTitleControlsScreen = 150509;  // menus.lvc: Options > Controls
constexpr int kTitleAdvancedScreen = 150516;  // menus.lvc: Options > Advanced, now Graphics
constexpr int kTitleOptionsScreen = 150498;   // menus.lvc: Options (multiplayer's match setup has an Advanced button too)
constexpr uint32_t kPageAdvanced = 8;         // InGameMenuPage: Advanced (pops back to Options)
constexpr uint32_t kPageControls = 7, kPageBindings = 9;  // InGameMenuPage

// GS_InGameMenu fields
constexpr uint32_t kSprite = 0x20, kScrollBar = 0x5c, kControlsPanel = 0x80, kOptionsPanel = 0x78;
constexpr uint32_t kSensLabel = 0xfc, kSensSlider = 0x100, kAimLabel = 0x104, kAimSlider = 0x108;
constexpr uint32_t kAssistLabel = 0x10c, kAssistSwitch = 0x110, kInvertLabel = 0x114, kInvertSwitch = 0x118;
constexpr uint32_t kLevelSwitch = 0x120, kKeyboardButton = 0x124, kPadButton = 0x128, kVehicleOption = 0x12c;
constexpr uint32_t kCustomerCare = 0xc0, kSoundButton = 0xc4, kControlsButton = 0xc8, kAdvancedButton = 0xcc;
constexpr uint32_t kPage = 0x1e0, kPagesRect = 0x330, kSwitchColor = 0x364, kLabelColor = 0x368;
// CButton: position; CSlider: range and value; CScrollBar: range and position
constexpr uint32_t kPosX = 0x50, kPosY = 0x52, kSliderMin = 0xa4, kSliderMax = 0xa8, kSliderValue = 0xac;
constexpr uint32_t kBarMin = 0xa0, kBarMax = 0xa4, kBarPos = 0xa8;
// game strings
constexpr int kStrAimAcceleration = 0x18b, kStrLeftHanded = 0x4a5, kStrGyroscope = 0x4a7, kStrControlsTitle = 0x1be;
constexpr int kStrOn = 0x49d, kStrOff = 0x49e, kStrAdvanced = 0x1bf;
// CGameSettings fields
constexpr uint32_t kGsAimAssist = 0x10, kGsVehicleControls = 0x14, kGsAutoLeveling = 0x18, kGsLeftHanded = 0x44,
                   kGsInvertY = 0x45, kGsGyroscope = 0x57;

struct Fns {
    static uint32_t f(const char *name) { return rt::export_address(name); }
    uint32_t alloc = f("_Znwj"), get_instance = f("_ZN11Application11GetInstanceEv");
    uint32_t menu_ctor = f("_ZN13GS_InGameMenuC1Ev"), push_state = f("_ZN12gxStateStack9PushStateEP11gxGameStateb");
    uint32_t pop_state = f("_ZN12gxStateStack8PopStateEb");
    uint32_t push_page = f("_ZN13GS_InGameMenu8PushPageE14InGameMenuPage");
    uint32_t render_controls = f("_ZN13GS_InGameMenu18RenderControlsPageEv");
    uint32_t feedback = f("_ZN13GS_InGameMenu17PlayFeedbackSoundEv");
    uint32_t panel_ctor = f("_ZN6CPanelC2ERKN6glitch4core4rectIiEE"), add = f("_ZN6CPanel10AddControlEP7CButton");
    uint32_t scroll = f("_ZN6CPanel18AssociateScrollBarEP10CScrollBar"), panel_update = f("_ZN6CPanel6UpdateEv");
    uint32_t text_ctor = f("_ZN11CButtonTextC2EiiPcbi");
    uint32_t text_color = f("_ZN11CButtonText8SetColorEbN6glitch5video6SColorE");
    uint32_t spr_ctor = f("_ZN10CButtonSprC1EiiP7CSpriteiiiiiii"), spr_text = f("_ZN10CButtonSpr7SetTextEPKc");
    uint32_t spr_text_id = f("_ZN10CButtonSpr7SetTextEi"), spr_frame = f("_ZN10CButtonSpr8SetFrameEii");
    uint32_t set_pos_y = f("_ZN7CButton7SetPosYEs"), navig_up = f("_ZN7CButton11SetNavig_UpEPS_");
    uint32_t navig_down = f("_ZN7CButton13SetNavig_DownEPS_");
    uint32_t slider_pos = f("_ZN7CSlider26CalculatePositionFromValueEv");
    uint32_t slider_ctor = f("_ZN7CSliderC1EiiP7CSpriteiiiifffii");
    uint32_t game_settings = f("_ZN13CGameSettings9SingletonE");
    uint32_t load_fonts = f("_ZN11Application9LoadFontsEb");
};
const Fns &fns() {
    static const Fns f;
    return f;
}

uint32_t call(Cpu *cpu, uint32_t f, std::initializer_list<uint32_t> args) { return rt::call_nested(cpu, f, args); }
// virtual call with no or one argument
uint32_t virt(Cpu *cpu, uint32_t object, uint32_t slot) { return call(cpu, LD32(LD32(object) + slot), {object}); }
uint32_t virt(Cpu *cpu, uint32_t object, uint32_t slot, uint32_t arg) {
    return call(cpu, LD32(LD32(object) + slot), {object, arg});
}
int16_t pos_x(uint32_t b) { return int16_t(LD16(b + kPosX)); }
int16_t pos_y(uint32_t b) { return int16_t(LD16(b + kPosY)); }
float ldf(uint32_t a) { return LDF(a); }
void stf(uint32_t a, float f) { STF(a, f); }

// Guest copies of the texts given to widgets; kept, widgets keep the pointer.
uint32_t guest_text(const std::string &s) {
    static std::unordered_map<std::string, uint32_t> texts;
    uint32_t &g = texts[s];
    if (!g) g = rt::gstrdup(s.c_str());
    return g;
}

bool clicked(Cpu *cpu, uint32_t button) { return button && (virt(cpu, button, 0x34) & 0xff); }

void set_switch(Cpu *cpu, uint32_t button, bool on) {
    if (!button) return;
    call(cpu, fns().spr_frame, {button, on ? 0x7bu : 0x7cu, 0x10a});
    call(cpu, fns().spr_text_id, {button, uint32_t(on ? kStrOn : kStrOff)});
}

// Sliders: the game's range maps to sensitivity 0.25 .. 4 (logarithmic, 1 in the middle).
float sensitivity_of(uint32_t slider) {
    const float lo = ldf(slider + kSliderMin), hi = ldf(slider + kSliderMax);
    const float t = std::clamp((ldf(slider + kSliderValue) - lo) / std::max(hi - lo, 1e-6f), 0.0f, 1.0f);
    return std::round(0.25f * std::pow(16.0f, t) * 100) / 100;
}
void set_slider(Cpu *cpu, uint32_t slider, float sensitivity) {
    const float lo = ldf(slider + kSliderMin), hi = ldf(slider + kSliderMax);
    const float t = std::clamp(std::log(sensitivity / 0.25f) / std::log(16.0f), 0.0f, 1.0f);
    stf(slider + kSliderValue, lo + t * (hi - lo));
    call(cpu, fns().slider_pos, {slider});
}

// The mouse wheel: a row per notch. Both pages scroll with the menu's scroll bar (CPanel::Update
// takes the panel's offset from its position), as a drag on the bar does. The notches are
// taken each frame (frame()), so those turned elsewhere are dropped.
int g_wheel_steps = 0;
void wheel_scroll(uint32_t menu) {
    const int steps = std::exchange(g_wheel_steps, 0);
    const uint32_t bar = LD32(menu + kScrollBar), a = LD32(menu + kAssistLabel), b = LD32(menu + kInvertLabel);
    if (!steps || !bar || !a || !b) return;
    const int32_t pos = int32_t(LD32(bar + kBarPos)) + steps * (pos_y(b) - pos_y(a));
    ST32(bar + kBarPos, uint32_t(std::clamp(pos, int32_t(LD32(bar + kBarMin)), int32_t(LD32(bar + kBarMax)))));
}

// Settings changed on the pages; saved once a slider drag has settled.
bool g_dirty = false;
int64_t g_changed_at = 0;
void changed() {
    g_dirty = true;
    g_changed_at = rt::os::monotonic_ns();
}

// --- widgets made here (rows added to the pages) -------------------------------------------

// A text label, as the game makes the switch rows' (align 0x21) and the sliders' (0x42) labels.
uint32_t make_label(Cpu *cpu, uint32_t menu, uint32_t panel, int x, int y, const char *text, uint32_t align) {
    const Fns &f = fns();
    const uint32_t label = call(cpu, f.alloc, {0xa0});
    call(cpu, f.text_ctor, {label, uint32_t(x), uint32_t(y), guest_text(text), 0, align});
    call(cpu, f.text_color, {label, 1, LD32(menu + kLabelColor)});
    call(cpu, f.add, {panel, label});
    return label;
}

// A switch button (the Controls page's ON / OFF look); also the value buttons, with their text.
uint32_t make_switch(Cpu *cpu, uint32_t menu, uint32_t panel, int x, int y) {
    const Fns &f = fns();
    const uint32_t button = call(cpu, f.alloc, {0xbc});
    call(cpu, f.spr_ctor, {button, uint32_t(x), uint32_t(y), LD32(menu + kSprite), 0x7b, 0x10a, uint32_t(kStrOff), 6,
                           0x22, 0, 0});
    ST32(button + 0x74, LD32(menu + kSwitchColor));
    ST32(button + 4, 1);
    call(cpu, f.add, {panel, button});
    return button;
}

// A wide button, as the Controls page's Keyboard & Mouse / Controller buttons; the value rows
// show "Name: value" on it.
uint32_t make_wide_button(Cpu *cpu, uint32_t menu, uint32_t panel, int x, int y) {
    const Fns &f = fns();
    const uint32_t button = call(cpu, f.alloc, {0xbc});
    call(cpu, f.spr_ctor, {button, uint32_t(x), uint32_t(y), LD32(menu + kSprite), 0x1d, 0x1e, 0x18f, 5, 0x22, 0, 0});
    ST32(button + 0x74, LD32(menu + kLabelColor));
    ST8(button + 0x65, 1);
    ST32(button + 4, 1);
    call(cpu, f.add, {panel, button});
    return button;
}

// A slider over 0..1, made as CreateControlsPage makes the sensitivity sliders (sprite frames,
// width 60 x the menu's scale +0x1e4).
uint32_t make_slider(Cpu *cpu, uint32_t menu, uint32_t panel, int x, int y) {
    const Fns &f = fns();
    const uint32_t slider = call(cpu, f.alloc, {0xbc});
    const int width = int(ldf(menu + 0x1e4) * 60.0f);
    auto bits = [](float v) {
        uint32_t u;
        std::memcpy(&u, &v, 4);
        return u;
    };
    call(cpu, f.slider_ctor, {slider, uint32_t(x), uint32_t(y), LD32(menu + kSprite), 0x7f, 0x7f, 0x80, 0x81, bits(0.0f),
                              bits(1.0f), bits(0.5f), 7, uint32_t(width)});
    ST32(slider + 4, 1);
    call(cpu, f.add, {panel, slider});
    return slider;
}

// Arrow / D-pad links down a column of buttons (CPanel::NavigUp / NavigDown follow them).
void link(Cpu *cpu, const std::vector<uint32_t> &column) {
    for (size_t i = 0; i + 1 < column.size(); i++) {
        if (!column[i] || !column[i + 1]) continue;
        call(cpu, fns().navig_down, {column[i], column[i + 1]});
        call(cpu, fns().navig_up, {column[i + 1], column[i]});
    }
}

// Left / right on the selected button (keys, D-pad), on release: CAccessory's input status, a
// ButtonInputStatus {held, (pending), released} per slot, slots 6 / 7 left / right (CPanel::Update
// moves sliders while they are held). -1, +1 or 0.
int left_right(uint32_t button) {
    static const uint32_t status = rt::export_address("_ZN10CAccessory13m_inputStatusE");
    if (!button || !status || !LD8(button + 0x24)) return 0;  // CButton +0x24: selected
    return LD8(status + 7 * 3 + 2) ? 1 : LD8(status + 6 * 3 + 2) ? -1 : 0;
}

// Linear sliders (0..1 over the slider's range).
float slider_t(uint32_t slider) {
    const float lo = ldf(slider + kSliderMin), hi = ldf(slider + kSliderMax);
    return std::clamp((ldf(slider + kSliderValue) - lo) / std::max(hi - lo, 1e-6f), 0.0f, 1.0f);
}
void set_slider_t(Cpu *cpu, uint32_t slider, float t) {
    const float lo = ldf(slider + kSliderMin), hi = ldf(slider + kSliderMax);
    stf(slider + kSliderValue, lo + std::clamp(t, 0.0f, 1.0f) * (hi - lo));
    call(cpu, fns().slider_pos, {slider});
}

// The panel's scroll range from its rows (CPanel::AssociateScrollBar does it once; the pages
// share the menu's scroll bar).
void update_scroll_range(Cpu *cpu, uint32_t panel) {
    static const uint32_t update = rt::export_address("_ZN6CPanel19UpdateScrollBarDataEv");
    if (panel && update) call(cpu, update, {panel});
}

// --- the binding page ----------------------------------------------------------------------

struct Row {
    Action action;
    const char *name;
};
const Row kRows[] = {
    {settings::MoveForward, "MOVE FORWARD"},   {settings::MoveBack, "MOVE BACK"},
    {settings::MoveLeft, "MOVE LEFT"},         {settings::MoveRight, "MOVE RIGHT"},
    {settings::Fire, "FIRE"},                  {settings::Aim, "AIM"},
    {settings::Jump, "JUMP / SKIP"},           {settings::Sprint, "SPRINT"},
    {settings::Reload, "RELOAD"},              {settings::Grenade, "GRENADE"},
    {settings::Power, "ABILITY"},              {settings::Melee, "MELEE"},
    {settings::Interact, "INTERACT"},          {settings::NextWeapon, "NEXT WEAPON"},
    {settings::PrevWeapon, "PREVIOUS WEAPON"}, {settings::NextPower, "NEXT ABILITY"},
    {settings::NextGrenade, "NEXT GRENADE"},    {settings::Detonate, "DETONATE MINES"},
};
constexpr int kRowCount = int(sizeof kRows / sizeof kRows[0]);

struct BindingPage {
    uint32_t panel = 0;
    uint32_t buttons[kRowCount] = {};
};
std::unordered_map<uint32_t, BindingPage> g_pages;  // GS_InGameMenu -> its binding page
bool g_pad_page = false;                             // which device the binding page shows
int g_capture_row = -1;                              // the row waiting for a key

bool is_pad(int code) { return code >= settings::kPadBase; }
bool movement(Action a) { return a <= settings::MoveRight; }

std::string short_label(int code) {
    if (code >= settings::kMouseBase && code < settings::kPadBase) {
        switch (code - settings::kMouseBase) {
        case 1: return "LMB";
        case 2: return "MMB";
        case 3: return "RMB";
        default: return "Mouse " + std::to_string(code - settings::kMouseBase);
        }
    }
    return settings::input_label(code, input::config().swap_ab_xy);
}

// The action's first control on the page's device (more can be set in nova3.ini; rebinding
// replaces them all).
std::string binding_label(Action a) {
    if (g_pad_page && movement(a)) return "L Stick";
    for (int code : input::config().bindings[a])
        if (is_pad(code) == g_pad_page) return short_label(code);
    return "-";
}

// Value buttons: the switch's ON look (blue); OFF (red) while waiting for a key.
void set_row(Cpu *cpu, uint32_t button, const std::string &text, bool waiting) {
    call(cpu, fns().spr_frame, {button, waiting ? 0x7cu : 0x7bu, 0x10a});
    call(cpu, fns().spr_text, {button, guest_text(text)});
}

void refresh_rows(Cpu *cpu, const BindingPage &page) {
    for (int i = 0; i < kRowCount; i++)
        if (page.buttons[i]) set_row(cpu, page.buttons[i], binding_label(kRows[i].action), false);
}

// Gives `code` to action `a` on the code's device. An action that had the code gets a's
// previous controls of that device instead.
void assign(Action a, int code) {
    auto &b = input::config().bindings;
    std::vector<int> previous;
    for (int c : b[a])
        if (is_pad(c) == is_pad(code)) previous.push_back(c);
    for (int other = 0; other < settings::kActionCount; other++) {
        if (other == a || other == settings::Pause) continue;
        auto &codes = b[other];
        if (std::find(codes.begin(), codes.end(), code) == codes.end()) continue;
        codes.erase(std::remove(codes.begin(), codes.end(), code), codes.end());
        for (int c : previous)
            if (std::find(codes.begin(), codes.end(), c) == codes.end()) codes.push_back(c);
    }
    b[a].erase(std::remove_if(b[a].begin(), b[a].end(), [&](int c) { return is_pad(c) == is_pad(code); }), b[a].end());
    b[a].push_back(code);
    rt::log("controls: action %d bound to %s", int(a), settings::binding_text(b[a]).c_str());
}

// Rows laid out like the Controls page's switch rows (label left, button right), from its
// first row down.
void build_binding_page(Cpu *cpu, uint32_t menu) {
    const Fns &f = fns();
    const uint32_t label0 = LD32(menu + kAssistLabel), switch0 = LD32(menu + kAssistSwitch);
    const uint32_t label1 = LD32(menu + kInvertLabel), first = LD32(menu + kSensLabel);
    if (!label0 || !switch0 || !label1 || !first) return;
    const int pitch = pos_y(label1) - pos_y(label0), dy = pos_y(switch0) - pos_y(label0);
    BindingPage &page = g_pages[menu];
    page.panel = call(cpu, f.alloc, {0xa4});
    call(cpu, f.panel_ctor, {page.panel, menu + kPagesRect});
    call(cpu, f.scroll, {page.panel, LD32(menu + kScrollBar)});
    for (int i = 0; i < kRowCount; i++) {
        const int y = pos_y(first) + i * pitch;
        make_label(cpu, menu, page.panel, pos_x(label0), y, kRows[i].name, 0x21);
        page.buttons[i] = make_switch(cpu, menu, page.panel, pos_x(switch0), y + dy);
    }
    // arrows / D-pad: from row to row (CPanel::NavigUp / NavigDown follow the links and scroll
    // the selection into view); the first row is selected, as the game's pages start
    link(cpu, std::vector<uint32_t>(page.buttons, page.buttons + kRowCount));
    virt(cpu, page.buttons[0], 0x78, 1);  // CButton::SetSelected
}

// --- the Graphics page ---------------------------------------------------------------------
//
// Also page 9 (its update and render are the host's): the Options page's Advanced button
// (PushPage(8), turned into page 9 with the Graphics panel) and the title menu's Advanced
// screen (150516) open it; Back returns to Options (page 8 pops to 2). Rows of label + value
// button, built like the binding page; a click (Enter, pad A) takes the next value, left /
// right the previous / next. Changes apply at once (display::apply) and are saved
// (display::save).

enum class Item { Mode, Resolution, VSync, FrameCap, Fov, Anisotropy, HdTextures };
struct GraphicsRow {
    Item item;
    const char *name;
};
#if defined(__3DS__)
// the top screen is 400x240 and always synchronised; the PICA has no anisotropic filtering
const GraphicsRow kGraphicsRows[] = {{Item::FrameCap, "Frame Cap"}, {Item::Fov, "Field of View"}};
#elif defined(__SWITCH__)
// docked / handheld (and so the resolution) is automatic
const GraphicsRow kGraphicsRows[] = {
    {Item::FrameCap, "Frame Cap"}, {Item::VSync, "VSync"}, {Item::Fov, "Field of View"},
    {Item::Anisotropy, "Anisotropic Filtering"},
};
#else
const GraphicsRow kGraphicsRows[] = {
    {Item::Mode, "Display Mode"},  {Item::Resolution, "Resolution"},    {Item::VSync, "VSync"},
    {Item::FrameCap, "Frame Cap"}, {Item::Fov, "Field of View"},        {Item::Anisotropy, "Anisotropic Filtering"},
    {Item::HdTextures, "HD Textures (after restart)"},
};
#endif
constexpr int kGraphicsCount = int(sizeof kGraphicsRows / sizeof kGraphicsRows[0]);

struct Choice {
    int value;
    std::string text;
};

int size_value(int w, int h) { return (w << 16) | h; }

// The values offered for a row (the launcher's choices).
std::vector<Choice> choices(Item item) {
    using VSync = settings::Settings::VSync;
    std::vector<Choice> v;
    switch (item) {
    case Item::Mode:
        v = {{int(settings::DisplayMode::Windowed), "Windowed"},
             {int(settings::DisplayMode::Borderless), "Borderless"},
             {int(settings::DisplayMode::Fullscreen), "Fullscreen"}};
        break;
    case Item::Resolution: {
        auto sizes = display::resolutions();
        std::sort(sizes.begin(), sizes.end(), [](auto a, auto b) { return a.first * a.second < b.first * b.second; });
        for (auto [w, h] : sizes) v.push_back({size_value(w, h), std::to_string(w) + " x " + std::to_string(h)});
        break;
    }
    case Item::VSync:
#ifndef __SWITCH__
        v.push_back({int(VSync::Off), "Off"});
#endif
        v.push_back({int(VSync::On), "On"});
        v.push_back({int(VSync::Adaptive), "Adaptive"});
        break;
    case Item::FrameCap:
#ifdef NOVA3_CONSOLE
        v = {{30, "30 fps"}, {60, "60 fps"}};
#else
        v.push_back({0, "Unlimited"});
        for (int fps : {30, 60, 120, 144, 165, 240}) v.push_back({fps, std::to_string(fps) + " fps"});
#endif
        break;
    case Item::Fov:
        v.push_back({0, "Default"});
        for (int deg = 60; deg <= 110; deg += 5) v.push_back({deg, std::to_string(deg)});
        break;
    case Item::Anisotropy:
        v = {{1, "Off"}, {2, "2x"}, {4, "4x"}, {8, "8x"}, {16, "16x"}};
        break;
    case Item::HdTextures:
        v = {{0, "Off"}, {1, "On"}};
        break;
    }
    return v;
}

int item_value(Item item, const settings::Settings &c) {
    switch (item) {
    case Item::Mode: return int(c.mode);
    case Item::Resolution:
        if (c.mode == settings::DisplayMode::Borderless) {  // the desktop's size
            const auto sizes = display::resolutions();
            if (!sizes.empty()) return size_value(sizes[0].first, sizes[0].second);
        }
        return size_value(c.width, c.height);
    case Item::VSync: return int(c.vsync);
#if defined(__3DS__)
    case Item::FrameCap: return c.max_fps == 60 ? 60 : 30;  // 30 unless 60 was chosen
#elif defined(__SWITCH__)
    case Item::FrameCap: return c.max_fps > 0 && c.max_fps <= 30 ? 30 : 60;
#else
    case Item::FrameCap: return c.max_fps;
#endif
    case Item::Fov: return c.fov;
    case Item::Anisotropy: return c.anisotropy;
    case Item::HdTextures: return c.hd_textures;
    }
    return 0;
}

void set_item(Item item, settings::Settings &c, int v) {
    switch (item) {
    case Item::Mode: c.mode = settings::DisplayMode(v); break;
    case Item::Resolution:
        c.width = v >> 16;
        c.height = v & 0xffff;
        break;
    case Item::VSync: c.vsync = settings::Settings::VSync(v); break;
    case Item::FrameCap: c.max_fps = v; break;
    case Item::Fov: c.fov = v; break;
    case Item::Anisotropy: c.anisotropy = v; break;
    case Item::HdTextures: c.hd_textures = v != 0; break;
    }
}

// The value `step` places from `current` (wrapping); a value not in the list (set in
// nova3.ini) steps to its neighbours in the list.
int step_value(const std::vector<Choice> &v, int current, int step) {
    if (v.empty()) return current;
    const int n = int(v.size());
    int i = -1;
    for (int k = 0; k < n; k++)
        if (v[k].value == current) i = k;
    if (i < 0) {  // the lists are in ascending order (resolutions: by area)
        auto key = [](int value) { return value > 0xffff ? (value >> 16) * (value & 0xffff) : value; };
        int above = n;
        for (int k = n - 1; k >= 0; k--)
            if (key(v[k].value) > key(current)) above = k;
        i = step > 0 ? above - 1 : above;  // so that +1 lands on the next larger, -1 on the next smaller
    }
    return v[((i + step) % n + n) % n].value;
}

struct GraphicsPage {
    uint32_t panel = 0;
    uint32_t buttons[kGraphicsCount] = {};
};
std::unordered_map<uint32_t, GraphicsPage> g_graphics;  // GS_InGameMenu -> its Graphics page
bool g_page9_graphics = false;                           // page 9 shows the Graphics page (else bindings)

void refresh_graphics(Cpu *cpu, const GraphicsPage &page) {
    const settings::Settings &c = input::config();
    for (int i = 0; i < kGraphicsCount; i++) {
        const Item item = kGraphicsRows[i].item;
        const int value = item_value(item, c);
        std::string text;
        for (const Choice &ch : choices(item))
            if (ch.value == value) text = ch.text;
        if (text.empty())  // not one of the choices (nova3.ini)
            text = item == Item::Resolution ? std::to_string(value >> 16) + " x " + std::to_string(value & 0xffff)
                   : item == Item::FrameCap ? std::to_string(value) + " fps"
                   : item == Item::VSync    ? std::string("Off")  // (the Switch offers on / adaptive)
                                            : std::to_string(value);
        call(cpu, fns().spr_text, {page.buttons[i], guest_text(std::string(kGraphicsRows[i].name) + ":  " + text)});
    }
}

// A column of wide buttons, spaced as the Keyboard & Mouse and Controller buttons.
void build_graphics_page(Cpu *cpu, uint32_t menu) {
    const Fns &f = fns();
    const uint32_t kb = LD32(menu + kKeyboardButton), pad = LD32(menu + kPadButton), first = LD32(menu + kSensLabel);
    if (!kb || !pad || !first) return;
    const int pitch = pos_y(pad) - pos_y(kb);
    GraphicsPage &page = g_graphics[menu];
    page.panel = call(cpu, f.alloc, {0xa4});
    call(cpu, f.panel_ctor, {page.panel, menu + kPagesRect});
    call(cpu, f.scroll, {page.panel, LD32(menu + kScrollBar)});
    for (int i = 0; i < kGraphicsCount; i++)
        page.buttons[i] = make_wide_button(cpu, menu, page.panel, pos_x(kb), pos_y(first) + i * pitch);
    link(cpu, std::vector<uint32_t>(page.buttons, page.buttons + kGraphicsCount));
    virt(cpu, page.buttons[0], 0x78, 1);  // CButton::SetSelected
}

void update_graphics_page(Cpu *cpu, uint32_t menu, GraphicsPage &page) {
    call(cpu, fns().panel_update, {page.panel});
    for (int i = 0; i < kGraphicsCount; i++) {
        const uint32_t b = page.buttons[i];
        const int step = clicked(cpu, b) ? 1 : left_right(b);
        if (!step) continue;
        settings::Settings &c = input::config();
        const Item item = kGraphicsRows[i].item;
        const int before = item_value(item, c), after = step_value(choices(item), before, step);
        if (after == before) continue;
        set_item(item, c, after);
        call(cpu, fns().feedback, {menu});
        display::apply(c);
        display::save(c);
        refresh_graphics(cpu, page);  // a new display mode offers other resolutions
        break;
    }
}

// --- the Controls page ---------------------------------------------------------------------
//
// Rows added below the game's (after AUTO-LEVELING), laid out like them: TOGGLE RUN (sprint
// toggles on any device), RUMBLE and its strength (the controller), and on the Switch GYRO
// AIMING (OFF / AIM: while aiming / ALWAYS) and its sensitivity, the gyro's INVERT X-AXIS and
// INVERT Y-AXIS, and SWAP A/B, X/Y (the Xbox layout).

struct ControlsExtra {
    uint32_t toggle_run = 0, rumble = 0, rumble_strength = 0;
    uint32_t gyro = 0, gyro_sensitivity = 0, gyro_invert_x = 0, gyro_invert_y = 0, swap_ab_xy = 0;  // Switch
};
std::unordered_map<uint32_t, ControlsExtra> g_extra;  // GS_InGameMenu -> its added rows

// GYRO AIMING's switch shows its three values (lit unless off)
void set_gyro_switch(Cpu *cpu, uint32_t button, settings::Settings::Gyro g) {
    using Gyro = settings::Settings::Gyro;
    if (!button) return;
    call(cpu, fns().spr_frame, {button, g != Gyro::Off ? 0x7bu : 0x7cu, 0x10a});
    call(cpu, fns().spr_text, {button, guest_text(g == Gyro::Always ? "ALWAYS" : g == Gyro::Aim ? "AIM" : "OFF")});
}

void build_controls_extra(Cpu *cpu, uint32_t menu) {
    const uint32_t panel = LD32(menu + kControlsPanel);
    const uint32_t label0 = LD32(menu + kAssistLabel), switch0 = LD32(menu + kAssistSwitch);
    const uint32_t label1 = LD32(menu + kInvertLabel), last_label = LD32(menu + kLevelSwitch - 4);
    const uint32_t last = LD32(menu + kLevelSwitch);
    const uint32_t sens_label = LD32(menu + kSensLabel), sens = LD32(menu + kSensSlider);
    const uint32_t aim_label = LD32(menu + kAimLabel);
    if (!panel || !label0 || !switch0 || !label1 || !last_label || !last || !sens_label || !sens || !aim_label) return;
    const int pitch = pos_y(label1) - pos_y(label0), dy = pos_y(switch0) - pos_y(label0);
    // slider rows: label, then the slider below it, a slider row's height apart
    const int slider_dx = pos_x(sens) - pos_x(sens_label), slider_dy = pos_y(sens) - pos_y(sens_label);
    const int slider_pitch = std::max(pos_y(aim_label) - pos_y(sens_label), pitch);
    ControlsExtra &x = g_extra[menu];
    int y = pos_y(last_label) + pitch;
    auto switch_row = [&](const char *name) {
        make_label(cpu, menu, panel, pos_x(label0), y, name, 0x21);
        const uint32_t b = make_switch(cpu, menu, panel, pos_x(switch0), y + dy);
        y += pitch;
        return b;
    };
    auto slider_row = [&](const char *name) {
        make_label(cpu, menu, panel, pos_x(sens_label), y, name, 0x42);
        const uint32_t s = make_slider(cpu, menu, panel, pos_x(sens_label) + slider_dx, y + slider_dy);
        y += slider_pitch;
        return s;
    };
    x.toggle_run = switch_row("TOGGLE RUN");
#ifndef __3DS__  // (no rumble motor)
    x.rumble = switch_row("RUMBLE");
    x.rumble_strength = slider_row("Rumble Strength");
#endif
#ifdef NOVA3_CONSOLE
    x.gyro = switch_row("GYRO AIMING");
    x.gyro_sensitivity = slider_row("Gyro Sensitivity");
    x.gyro_invert_x = switch_row("GYRO INVERT X-AXIS");
    x.gyro_invert_y = switch_row("GYRO INVERT Y-AXIS");
#endif
#ifdef NOVA3_CONSOLE
    x.swap_ab_xy = switch_row("SWAP A/B, X/Y");
#endif
    link(cpu, {last, x.toggle_run, x.rumble, x.rumble_strength, x.gyro, x.gyro_sensitivity, x.gyro_invert_x,
               x.gyro_invert_y, x.swap_ab_xy});
    update_scroll_range(cpu, panel);
}

void sync_controls_page(Cpu *cpu, uint32_t menu) {
    const settings::Settings &c = input::config();
    if (uint32_t s = LD32(menu + kSensSlider)) set_slider(cpu, s, c.sensitivity);
    if (uint32_t s = LD32(menu + kAimSlider)) set_slider(cpu, s, c.aim_sensitivity);
    set_switch(cpu, LD32(menu + kAssistSwitch), c.aim_assist);
    set_switch(cpu, LD32(menu + kInvertSwitch), c.invert_y);
    set_switch(cpu, LD32(menu + kLevelSwitch), c.auto_leveling);
    auto it = g_extra.find(menu);
    if (it == g_extra.end()) return;
    const ControlsExtra &x = it->second;
    set_switch(cpu, x.toggle_run, c.toggle_sprint);
    set_switch(cpu, x.rumble, c.rumble);
    if (x.rumble_strength) set_slider_t(cpu, x.rumble_strength, c.rumble_strength);
    set_gyro_switch(cpu, x.gyro, c.gyro);
    if (x.gyro_sensitivity) set_slider(cpu, x.gyro_sensitivity, c.gyro_sensitivity);
    set_switch(cpu, x.gyro_invert_x, c.gyro_invert_x);
    set_switch(cpu, x.gyro_invert_y, c.gyro_invert_y);
    set_switch(cpu, x.swap_ab_xy, c.swap_ab_xy);
}

// The added rows (pc_hook_igm_update_controls, after the panel's update).
void update_controls_extra(Cpu *cpu, uint32_t menu) {
    auto it = g_extra.find(menu);
    if (it == g_extra.end()) return;
    const ControlsExtra &x = it->second;
    settings::Settings &c = input::config();
    for (auto [button, value] : {std::pair{x.toggle_run, &c.toggle_sprint}, std::pair{x.rumble, &c.rumble},
                                 std::pair{x.gyro_invert_x, &c.gyro_invert_x}, std::pair{x.gyro_invert_y, &c.gyro_invert_y},
                                 std::pair{x.swap_ab_xy, &c.swap_ab_xy}}) {
        if (!button || !(clicked(cpu, button) || left_right(button))) continue;
        *value = !*value;
        set_switch(cpu, button, *value);
        call(cpu, fns().feedback, {menu});
        changed();
        if (value == &c.rumble && c.rumble) input::rumble(c.rumble_strength, c.rumble_strength, 200);
    }
    if (const uint32_t s = x.rumble_strength; s && std::fabs(slider_t(s) - c.rumble_strength) > 0.004f) {
        c.rumble_strength = std::round(slider_t(s) * 100) / 100;
        changed();
        input::rumble(c.rumble_strength, c.rumble_strength, 150);  // feel it
    }
    if (const int step = clicked(cpu, x.gyro) ? 1 : left_right(x.gyro)) {
        c.gyro = settings::Settings::Gyro((int(c.gyro) + step + 3) % 3);
        set_gyro_switch(cpu, x.gyro, c.gyro);
        call(cpu, fns().feedback, {menu});
        changed();
    }
    if (const uint32_t s = x.gyro_sensitivity; s && std::fabs(sensitivity_of(s) - c.gyro_sensitivity) > 0.004f) {
        c.gyro_sensitivity = sensitivity_of(s);
        changed();
    }
}

void entered_page(Cpu *cpu, uint32_t menu) {
    const uint32_t page = LD32(menu + kPage);
    if (page == kPageControls) {
        sync_controls_page(cpu, menu);
        update_scroll_range(cpu, LD32(menu + kControlsPanel));
    }
    auto it = g_pages.find(menu);
    if (it != g_pages.end() && it->second.panel) {
        const bool shown = page == kPageBindings && !g_page9_graphics;
        virt(cpu, it->second.panel, 0xb4, shown);  // shown with its page, as UpdatePageUIs does
        if (shown) {
            refresh_rows(cpu, it->second);
            update_scroll_range(cpu, it->second.panel);
        }
    }
    auto g = g_graphics.find(menu);
    if (g != g_graphics.end() && g->second.panel) {
        const bool shown = page == kPageBindings && g_page9_graphics;
        virt(cpu, g->second.panel, 0xb4, shown);
        if (shown) {
            refresh_graphics(cpu, g->second);
            update_scroll_range(cpu, g->second.panel);
        }
    }
    if (page != kPageBindings && g_capture_row >= 0) {
        g_capture_row = -1;
        input::end_capture();
    }
}

// --- the title menu ------------------------------------------------------------------------

uint32_t g_title_menu = 0;  // the GS_InGameMenu opened from the title menu, while open
int g_title_screen = 0;      // the title menu's screen (Menu_GoToMenu)
uint32_t g_app = 0;         // Application (its string tables)
bool g_creating_for_title = false;
bool g_creating_controls = false;
int64_t g_title_input_after = 0;  // monotonic ns: the title menu takes input again from then
bool g_render_bindings = false;   // the binding page is being drawn (its title)
bool g_title_render = false;      // CMenuManager::Render is drawing the title menu

uint32_t state_stack(Cpu *cpu) { return call(cpu, fns().get_instance, {}) + 0x18400 + 0x33c; }

// The title menu uses its fonts at menu sizes (Application::LoadFonts(false), GS_LoadMenu);
// levels, and so the in-game menu's pages, at in-game sizes (LoadFonts(true), GS_Loading),
// about twice as large at 4K. The page opened from the title gets the in-game fonts, and the
// menu ones come back when it closes (the game reloads fonts in the menus on a language change
// too).
void load_fonts(Cpu *cpu, bool in_game) {
    call(cpu, fns().load_fonts, {call(cpu, fns().get_instance, {}), in_game ? 1u : 0u});
}

// The title menu is a stack of screens in depth (CMenuManager +0x30 .. +0x34): pushing one
// moves every screen a level back (CMenuScreen::PushScreenForward: target depth +0x60 + 1),
// and the menu's update animates them there (+0x5c follows +0x60 over a quarter second, as
// the screens move back and dim); going back moves them forward again. The page opened from
// the title moves them as a screen of its own would.
void move_title_screens(Cpu *cpu, bool back) {
    static const uint32_t singleton = rt::export_address("_ZN12CMenuManager9SingletonE");
    static const uint32_t forward = rt::export_address("_ZN11CMenuScreen17PushScreenForwardEv");
    static const uint32_t backward = rt::export_address("_ZN11CMenuScreen18PushScreenBackwardEv");
    const uint32_t manager = singleton ? LD32(singleton) : 0;
    if (!manager || !forward || !backward) return;
    for (uint32_t it = LD32(manager + 0x30); it < LD32(manager + 0x34); it += 4)
        call(cpu, back ? forward : backward, {LD32(it)});
}

// Opens the in-game menu at the Controls page, or the Graphics page.
void open_from_title(Cpu *cpu, bool graphics) {
    const Fns &f = fns();
    load_fonts(cpu, true);
    move_title_screens(cpu, true);
    const uint32_t menu = call(cpu, f.alloc, {0x594});
    call(cpu, f.menu_ctor, {menu});
    g_creating_for_title = true;
    call(cpu, f.push_state, {state_stack(cpu), menu, 1});  // creates the pages
    g_creating_for_title = false;
    g_title_menu = menu;
    call(cpu, f.push_page, {menu, graphics ? kPageAdvanced : kPageControls});  // (Advanced: the Graphics page)
    rt::log("controls: %s opened from the title menu", graphics ? "Graphics" : "Controls");
}

// The title menu (CMenuManager) listens to touches globally and reads the controller
// navigation itself, so it would also react to clicks on the page above it.
bool title_input_blocked() { return g_title_menu || rt::os::monotonic_ns() < g_title_input_after; }

// --- the multiplayer Advanced screen: PLAYER IP (Switch) ------------------------------------
//
// Match setup > Advanced (a Lua screen, 150358) has a fifth switch row that the game shows only
// online: Private Match (150360), in the slot under Killchains. On the Switch it is the
// [multiplayer] peers row (players' addresses, for networks that drop broadcasts): the label
// names them, the button says EDIT and opens the system keyboard. A press flips the switch's
// state (+0x40), which is how the click is seen; its texts are string ids (label +0x74, the
// ON / OFF values +0x78 / +0x7c) the GetString wrap answers. (On the PC the launcher has it.)
constexpr int kStrPeersLabel = 0x7f000001, kStrPeersValue = 0x7f000002;  // not the game's

#ifdef NOVA3_CONSOLE
constexpr int kMpAdvancedScreen = 150358;
void update_peers_row(Cpu *cpu) {
    constexpr uint32_t kRow = 150360, kAbove = 9006, kTop = 150361;  // Private Match, Killchains, Weapons Stay
    static uint32_t row = 0;
    static uint8_t state = 0;
    static const uint32_t singleton = rt::export_address("_ZN12CMenuManager9SingletonE");
    static const uint32_t top_screen = rt::export_address("_ZN12CMenuManager14GetTopScreenIdEv");
    static const uint32_t find = rt::export_address("_ZN12CMenuManager10FindObjectEi");
    static const uint32_t set_state = rt::export_address("_ZN11CMenuSwitch13SetOnOffStateEb");
    const uint32_t manager = singleton ? LD32(singleton) : 0;
    if (!manager || !top_screen || !find || !set_state || int32_t(call(cpu, top_screen, {manager})) != kMpAdvancedScreen) {
        row = 0;  // (the menus may be unloaded in play: nothing is kept)
        return;
    }
    const settings::Settings &c = input::config();
    if (!row) {  // entering the screen
        row = call(cpu, find, {manager, kRow});
        const uint32_t above = row ? call(cpu, find, {manager, kAbove}) : 0, top = row ? call(cpu, find, {manager, kTop}) : 0;
        if (!row || !above || !top) return;
        virt(cpu, row, 0x44, 1);  // Show(true), Enable(true)
        virt(cpu, row, 0x40, 1);
        // controller navigation (object ids: +0x18 up, +0x1c down): Killchains -> this -> the top row
        ST32(above + 0x1c, kRow);
        ST32(row + 0x18, kAbove);
        ST32(row + 0x1c, kTop);
        ST32(top + 0x18, kRow);
        call(cpu, set_state, {row, !c.peers.empty()});
        state = LD8(row + 0x40);
    }
    ST32(row + 0x74, uint32_t(kStrPeersLabel));
    ST32(row + 0x78, uint32_t(kStrPeersValue));
    ST32(row + 0x7c, uint32_t(kStrPeersValue));
    if (LD8(row + 0x40) == state) return;
    std::string text;
    if (input::ask_text(c.peers, 120, "Other players' IP addresses, separated by commas", text)) {
        input::config().peers = text;
        rt::net_set_peers(text);
        changed();
    }
    call(cpu, set_state, {row, !input::config().peers.empty()});
    state = LD8(row + 0x40);
}
#endif

}  // namespace

namespace controls_menu {

void frame(Cpu *cpu) {
    const uint32_t singleton = fns().game_settings;
    if (const uint32_t gs = singleton ? LD32(singleton) : 0) {
        const settings::Settings &c = input::config();
        const bool pad = input::using_gamepad();
        ST32(gs + kGsAimAssist, c.aim_assist && pad);
        ST8(gs + kGsAutoLeveling, c.auto_leveling && pad);
        ST8(gs + kGsLeftHanded, 0);  // touch layout
        ST8(gs + kGsInvertY, 0);     // the host inverts the view itself ([controls] invert_y)
        ST8(gs + kGsGyroscope, 0);
    }
    if (g_dirty && rt::os::monotonic_ns() - g_changed_at > 500'000'000) {
        g_dirty = false;
        input::save_config();
    }
    g_wheel_steps = input::take_menu_wheel();
    if (!g_app) g_app = call(cpu, fns().get_instance, {});
#ifdef NOVA3_CONSOLE
    update_peers_row(cpu);
#endif
}

bool hide_text() { return g_title_menu && g_title_render; }

uint32_t string(int id) {
    switch (id) {
    case kStrAimAcceleration: return guest_text("Aim Sensitivity");
    case kStrLeftHanded: return guest_text("AIM ASSIST");
    case kStrGyroscope: return guest_text("AUTO-LEVELING");
    case kStrControlsTitle:
        if (!g_render_bindings) return 0;
        return guest_text(g_page9_graphics ? "Graphics" : g_pad_page ? "Controller" : "Keyboard & Mouse");
    case kStrAdvanced:  // the title Options screen's button (the menus' own table: +0x18754 current, +0x18758 MAIN)
        return g_title_screen == kTitleOptionsScreen && g_app && LD32(g_app + 0x18754) == LD32(g_app + 0x18758)
                   ? guest_text("Graphics") : 0;
    case kStrPeersLabel: {
        std::string peers = input::config().peers;
        if (peers.size() > 28) peers = peers.substr(0, 25) + "...";  // (the row's width)
        return guest_text(peers.empty() ? "Player IP: none (broadcast)" : "Player IP: " + peers);
    }
    case kStrPeersValue: return guest_text("EDIT");
    default: return 0;
    }
}

}  // namespace controls_menu

// Menu_GoToMenu(screen) (Lua): the title menu's Controls and Advanced screens open the in-game
// menu's Controls and Graphics pages.
extern "C" void pc_hook_menu_goto(Cpu *cpu, uint32_t entry, GuestFn original) {
    static const uint32_t to_integer = rt::export_address("lua_tointeger");
    const int32_t screen = int32_t(rt::call_nested(cpu, to_integer, {cpu->r[0], 1}));
    rt::log("menu: screen %d", screen);
    if (screen == kTitleControlsScreen || screen == kTitleAdvancedScreen) {
        const uint32_t lr = cpu->r[14];
        open_from_title(cpu, screen == kTitleAdvancedScreen);
        cpu->r[0] = 0;  // no Lua results
        cpu->ret_pc = lr;
        return;
    }
    g_title_screen = screen;
    original(cpu, entry);
}

// CUpgradesManager::GetUpgradesCount(): GS_InGameMenu::Create lists the upgrade shop, which
// needs a level (GetUpgradeCost reads the player's data). Opened from the title menu, the
// list stays empty.
extern "C" void pc_hook_upgrades_count(Cpu *cpu, uint32_t entry, GuestFn original) {
    if (g_creating_for_title) {
        cpu->r[0] = 0;
        cpu->ret_pc = cpu->r[14];
        return;
    }
    original(cpu, entry);
}

// GS_InGameMenu::CreateObjectivesPage(): the level's objectives (CObjectivesDisplay reads the
// level). For the title menu an empty panel stands in for the page (+0x74) and its tab
// control (+0xb4), which UpdatePageUIs shows and hides; the page cannot be reached from there.
extern "C" void pc_hook_igm_objectives_page(Cpu *cpu, uint32_t entry, GuestFn original) {
    if (!g_creating_for_title) {
        original(cpu, entry);
        return;
    }
    const uint32_t menu = cpu->r[0], lr = cpu->r[14];
    const uint32_t panel = call(cpu, fns().alloc, {0xa4});
    call(cpu, fns().panel_ctor, {panel, menu + kPagesRect});
    ST32(menu + 0x74, panel);
    ST32(menu + 0xb4, panel);
    cpu->ret_pc = lr;
}

// GS_InGameMenu::CreateControlsPage(): the game's page, then the re-purposed texts, the added
// rows, the binding page and the Graphics page.
extern "C" void pc_hook_igm_create_controls(Cpu *cpu, uint32_t entry, GuestFn original) {
    const uint32_t menu = cpu->r[0];
    g_creating_controls = true;
    original(cpu, entry);
    g_creating_controls = false;
    if (uint32_t b = LD32(menu + kKeyboardButton)) call(cpu, fns().spr_text, {b, guest_text("Keyboard & Mouse")});
    if (uint32_t b = LD32(menu + kPadButton)) call(cpu, fns().spr_text, {b, guest_text("Controller")});
    build_binding_page(cpu, menu);
    build_graphics_page(cpu, menu);
    build_controls_extra(cpu, menu);
    sync_controls_page(cpu, menu);
#ifdef __3DS__
    // No keyboard or mouse on a 3DS: the upper button opens the controller's bindings, the
    // lower one is hidden and disabled (CButton::EnableDraw: +0x62) and left out of the column's
    // up / down links (+0x1c, +0x20)
    const uint32_t kb = LD32(menu + kKeyboardButton), pad = LD32(menu + kPadButton);
    if (kb && pad) {
        static const uint32_t set_enabled = rt::export_address("_ZN7CButton10SetEnabledEb");
        call(cpu, fns().spr_text, {kb, guest_text("Controller")});
        call(cpu, fns().spr_text, {pad, guest_text("")});  // (its text is drawn apart from its frame)
        ST8(pad + 0x62, 0);
        if (set_enabled) call(cpu, set_enabled, {pad, 0});
        const uint32_t below = LD32(pad + 0x20);
        ST32(kb + 0x20, below);
        if (below && LD32(below + 0x1c) == pad) ST32(below + 0x1c, kb);
    }
#endif
}

// HasGyroscope(): yes while the Controls page is built, so its last row (auto-leveling) exists.
extern "C" void pc_hook_has_gyroscope(Cpu *cpu, uint32_t entry, GuestFn original) {
    if (g_creating_controls) {
        cpu->r[0] = 1;
        cpu->ret_pc = cpu->r[14];
        return;
    }
    original(cpu, entry);
}

// GS_InGameMenu::UpdateControlsPage(), replaced.
extern "C" void pc_hook_igm_update_controls(Cpu *cpu, uint32_t entry, GuestFn original) {
    (void)entry, (void)original;
    const uint32_t menu = cpu->r[0], lr = cpu->r[14];
    const Fns &f = fns();
    settings::Settings &c = input::config();
    wheel_scroll(menu);
    virt(cpu, LD32(menu + kControlsPanel), 0xa0);  // the panel's own update (touch, navigation)
    if (uint32_t s = LD32(menu + kSensSlider); s && std::fabs(sensitivity_of(s) - c.sensitivity) > 0.004f) {
        c.sensitivity = sensitivity_of(s);
        changed();
    }
    if (uint32_t s = LD32(menu + kAimSlider); s && std::fabs(sensitivity_of(s) - c.aim_sensitivity) > 0.004f) {
        c.aim_sensitivity = sensitivity_of(s);
        changed();
    }
    struct Toggle {
        uint32_t field;
        bool *value;
    } toggles[] = {{kAssistSwitch, &c.aim_assist}, {kInvertSwitch, &c.invert_y}, {kLevelSwitch, &c.auto_leveling}};
    for (const Toggle &t : toggles) {
        const uint32_t b = LD32(menu + t.field);
        if (!clicked(cpu, b) && !left_right(b)) continue;  // (left / right switch it too)
        *t.value = !*t.value;
        set_switch(cpu, b, *t.value);
        call(cpu, f.feedback, {menu});
        changed();
    }
    if (uint32_t o = LD32(menu + kVehicleOption); clicked(cpu, o)) {
        const uint32_t gs = f.game_settings ? LD32(f.game_settings) : 0;
        if (gs) ST32(gs + kGsVehicleControls, LD32(o + 0xa4));  // as the game does
    }
    update_controls_extra(cpu, menu);
    for (uint32_t field : {kKeyboardButton, kPadButton}) {
        if (!clicked(cpu, LD32(menu + field))) continue;
#ifdef __3DS__
        g_pad_page = true;  // (the one device: see pc_hook_igm_create_controls)
#else
        g_pad_page = field == kPadButton;
#endif
        g_page9_graphics = false;
        call(cpu, f.feedback, {menu});
        call(cpu, f.push_page, {menu, kPageBindings});
        break;
    }
    cpu->ret_pc = lr;
}

// GS_InGameMenu::UpdateControlSchemePage(), replaced by the binding page or the Graphics page.
extern "C" void pc_hook_igm_update_bindings(Cpu *cpu, uint32_t entry, GuestFn original) {
    (void)entry, (void)original;
    const uint32_t menu = cpu->r[0], lr = cpu->r[14];
    auto it = g_pages.find(menu);
    if (g_page9_graphics) {
        auto g = g_graphics.find(menu);
        if (g != g_graphics.end() && g->second.panel) {
            wheel_scroll(menu);
            update_graphics_page(cpu, menu, g->second);
        }
    } else if (it != g_pages.end() && it->second.panel) {
        BindingPage &page = it->second;
        wheel_scroll(menu);
        call(cpu, fns().panel_update, {page.panel});
        if (g_capture_row >= 0) {
            if (const int code = input::take_pressed()) {
                // Escape, Start / Back, or the other device (a click on the controller page) cancel
                const bool cancel = is_pad(code) != g_pad_page || code == SDL_SCANCODE_ESCAPE ||
                                    code == settings::kPadBase + settings::PadStart ||
                                    code == settings::kPadBase + settings::PadBack;
                if (!cancel) {
                    assign(kRows[g_capture_row].action, code);
                    changed();
                }
                g_capture_row = -1;
                input::end_capture();
                refresh_rows(cpu, page);
            }
        } else {
            for (int i = 0; i < kRowCount; i++) {
                if (!clicked(cpu, page.buttons[i]) || (g_pad_page && movement(kRows[i].action))) continue;
                g_capture_row = i;
                set_row(cpu, page.buttons[i], "Press...", true);
                call(cpu, fns().feedback, {menu});
                input::begin_capture();
                break;
            }
        }
    }
    cpu->ret_pc = lr;
}

// GS_InGameMenu::RenderControlSchemePage(), replaced: the Controls page's renderer (frame,
// title, scrolling panel) with the binding or Graphics panel in place.
extern "C" void pc_hook_igm_render_bindings(Cpu *cpu, uint32_t entry, GuestFn original) {
    (void)entry, (void)original;
    const uint32_t menu = cpu->r[0], lr = cpu->r[14];
    uint32_t panel = 0;
    if (g_page9_graphics) {
        if (auto g = g_graphics.find(menu); g != g_graphics.end()) panel = g->second.panel;
    } else if (auto it = g_pages.find(menu); it != g_pages.end()) {
        panel = it->second.panel;
    }
    if (panel) {
        const uint32_t controls = LD32(menu + kControlsPanel);
        ST32(menu + kControlsPanel, panel);
        g_render_bindings = true;
        call(cpu, fns().render_controls, {menu});
        g_render_bindings = false;
        ST32(menu + kControlsPanel, controls);
    }
    cpu->ret_pc = lr;
}

// GS_InGameMenu::PushPage(page): Advanced (the Options page's Graphics button) is page 9 with
// the Graphics panel.
extern "C" void pc_hook_igm_push_page(Cpu *cpu, uint32_t entry, GuestFn original) {
    const uint32_t menu = cpu->r[0];
    cpu->r[1] = offline::in_game_page(menu, cpu->r[1]);  // (no credits store)
    if (!cpu->r[1]) {
        cpu->ret_pc = cpu->r[14];
        return;
    }
    if (cpu->r[1] == kPageAdvanced && g_graphics.count(menu)) {
        g_page9_graphics = true;
        cpu->r[1] = kPageBindings;
    } else if (cpu->r[1] == kPageBindings) {
        g_page9_graphics = false;
    }
    original(cpu, entry);
    entered_page(cpu, menu);
}

// GS_InGameMenu::PopPage(): Back on the Controls or Graphics page opened from the title closes
// the menu; the Graphics page goes back to Options (as Advanced, page 8, does).
extern "C" void pc_hook_igm_pop_page(Cpu *cpu, uint32_t entry, GuestFn original) {
    const uint32_t menu = cpu->r[0], lr = cpu->r[14];
    const uint32_t page = LD32(menu + kPage);
    if (offline::close_shop(cpu, menu)) {
        cpu->ret_pc = lr;
        return;
    }
    const bool graphics = page == kPageBindings && g_page9_graphics;
    if (menu == g_title_menu && (page == kPageControls || graphics)) {
        call(cpu, fns().pop_state, {state_stack(cpu), 1});  // releases the menu (pc_hook_igm_release)
        cpu->ret_pc = lr;
        return;
    }
    if (graphics) {
        ST32(menu + kPage, kPageAdvanced);
        g_page9_graphics = false;
    }
    original(cpu, entry);
    entered_page(cpu, menu);
}

// GS_InGameMenu::CreateOptionsPage(): without Customer Care (an online help page); Sound,
// Controls and Advanced move up a place, and Advanced (its settings are on the Controls page or
// do nothing here) becomes Graphics: it opens the Graphics page (pc_hook_igm_push_page).
extern "C" void pc_hook_igm_create_options(Cpu *cpu, uint32_t entry, GuestFn original) {
    const uint32_t menu = cpu->r[0];
    original(cpu, entry);
    const Fns &f = fns();
    const uint32_t panel = LD32(menu + kOptionsPanel), care = LD32(menu + kCustomerCare);
    const uint32_t sound = LD32(menu + kSoundButton), controls = LD32(menu + kControlsButton);
    const uint32_t advanced = LD32(menu + kAdvancedButton);
    if (!panel || !care || !sound || !controls || !advanced) return;
    // CPanel controls: vector of CButton* at +0x8c .. +0x90
    std::vector<uint32_t> kept;
    for (uint32_t it = LD32(panel + 0x8c); it < LD32(panel + 0x90); it += 4)
        if (LD32(it) != care) kept.push_back(LD32(it));
    for (size_t i = 0; i < kept.size(); i++) ST32(LD32(panel + 0x8c) + 4 * uint32_t(i), kept[i]);
    ST32(panel + 0x90, LD32(panel + 0x8c) + 4 * uint32_t(kept.size()));
    const int y_care = pos_y(care), y_sound = pos_y(sound), y_controls = pos_y(controls);
    call(cpu, f.set_pos_y, {sound, uint32_t(y_care)});
    call(cpu, f.set_pos_y, {controls, uint32_t(y_sound)});
    call(cpu, f.set_pos_y, {advanced, uint32_t(y_controls)});
    call(cpu, f.spr_text, {advanced, guest_text("Graphics")});
    call(cpu, f.navig_up, {sound, 0});
    call(cpu, f.navig_down, {advanced, 0});
    link(cpu, {sound, controls, advanced});
}

// CMenuManager::onEvent / CMenuScreen::ProcessAccessoryInput: the title menu under the page
// ignores input.
extern "C" void pc_hook_title_menu_event(Cpu *cpu, uint32_t entry, GuestFn original) {
    if (title_input_blocked()) {
        cpu->r[0] = 0;
        cpu->ret_pc = cpu->r[14];
        return;
    }
    original(cpu, entry);
}

// CMenuManager::Render(): the title menu, under the page opened from it. Its text is drawn
// with the in-game fonts then (load_fonts), twice as large, and showed through the page: it is
// left out (hide_text, the font wraps in host/offline.cpp); the background and the dimmed
// buttons stay.
extern "C" void pc_hook_title_render(Cpu *cpu, uint32_t entry, GuestFn original) {
    g_title_render = true;
    original(cpu, entry);
    g_title_render = false;
}

// GS_InGameMenu::Release(): the menu closes, however (Resume pops the whole state without
// PopPage). A key capture still waiting and the title menu's input block end with it;
// otherwise the game would stay deaf to input.
extern "C" void pc_hook_igm_release(Cpu *cpu, uint32_t entry, GuestFn original) {
    const uint32_t menu = cpu->r[0];
    original(cpu, entry);
    if (g_capture_row >= 0) {
        g_capture_row = -1;
        input::end_capture();
    }
    if (menu == g_title_menu) {
        g_title_menu = 0;
        g_title_input_after = rt::os::monotonic_ns() + 300'000'000;  // the click that closed it
        load_fonts(cpu, false);  // the title menu's own sizes again
        move_title_screens(cpu, false);
    }
    g_pages.erase(menu);  // its widgets are gone with it
    g_graphics.erase(menu);
    g_extra.erase(menu);
    g_page9_graphics = false;
}
