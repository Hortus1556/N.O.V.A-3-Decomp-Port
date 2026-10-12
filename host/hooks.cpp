// Host hooks into recompiled game functions (see OVERRIDES in recomp/arm2c.py) and the
// keyboard/mouse gameplay controls built on them.
//
// Findings (from tracing EventManager::raiseSync while driving the touch HUD):
//  * the walk joystick raises EvStickMove {vtable, type 13, float x, float y} and
//    EvStickReleased {type 14} on GlobalEventManager::Singleton;
//  * firing: the touch fire button (CButtonShoot, the control scheme's GetShootControl)
//    raises EvGeneric type 29 on press and 30 on release; CPlayerControl::onEvent turns them
//    into the fire flags of PlayerComponent (+0x30 active, +0x31 held, +0x32 released),
//    which CPlayerControl drops again while the button is not Pressed(), and
//    PlayerComponent::UpdateFire calls CWeaponManager::FireButtonPress every update while
//    held. The fire key holds that button (PressButton / ReleaseButton, like the gamepad
//    layer); writing the flags directly fails in the slow-motion shoot tutorial;
//  * the other actions are touch HUD buttons, handled by CLevelInterfaceNormal::Update
//    (CLevel+0x188): buttons of its ControlScheme (+0xc: jump, iron sights, rush, powers) and
//    its own members (interact +0x60, grenade +0x70, melee +0x74). Most are driven through
//    the game's gamepad layer (CAccessory, below); weapon switching is a swipe on the weapon
//    widget, which SweepChangeWeaponButton(dir) synthesises;
//  * PlayerComponent builds CPlayerControl with pointers to two view-angle floats of the
//    player object (stored at CPlayerControl+0xf0 / +0xf4); both are in degrees
//    (yaw, pitch) and touch look moves them.
#include "hooks.h"
#include "eventlog.h"

#include "os.h"
#include "runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <cstdlib>
#include <unordered_map>

namespace hooks {

bool g_trace_events = false;
float g_pending_yaw, g_pending_pitch;  // view change (degrees) for the next CPlayerControl update
float g_move_x, g_move_y;                // current walk input (for the sprint-strafe fix)
bool g_sprinting;                        // sprint held while moving forward
float g_fov_scale = 1;                   // [video] fov relative to the game's own
float g_time_scale = 1;                  // --speed: game time per real time (tests)
int g_shots;                             // CWeaponComponent::DoFire calls (--log-player)
#ifdef __3DS__
int g_font_width = 0;                    // the width font sizes are picked for (0: the screen's)
#endif

namespace {

bool g_stick_active, g_firing;
bool g_skip_held;  // jump held since it was pressed in a cinematic (accessory slot 2)
uint32_t g_fire_button;  // the touch fire button held for the fire key

struct GameSymbols {
    uint32_t raise_sync, manager_singleton, ev_stick_move, ev_stick_released;
    uint32_t get_level, get_player_component, interface_vtable;
    uint32_t press_button, release_button, sweep_change_power, sweep_change_item, sweep_change_weapon, reload_weapon, switch_aim, accessory_update;
    bool ok;
};
const GameSymbols &symbols() {
    static GameSymbols s = [] {
        GameSymbols g{};
        g.raise_sync = rt::export_address("_ZN12EventManager9raiseSyncERK6IEvent");
        g.manager_singleton = rt::export_address("_ZN18GlobalEventManager9SingletonE");
        g.ev_stick_move = rt::export_address("_ZTV11EvStickMove");
        g.ev_stick_released = rt::export_address("_ZTV15EvStickReleased");
        g.get_level = rt::export_address("_ZN6CLevel8GetLevelEv");
        g.get_player_component = rt::export_address("_ZN6CLevel18GetPlayerComponentEv");
        g.interface_vtable = rt::export_address("_ZTV21CLevelInterfaceNormal") + 8;
        g.press_button = rt::export_address("_ZN7CButton11PressButtonEv");
        g.release_button = rt::export_address("_ZN7CButton13ReleaseButtonEv");
        g.sweep_change_power = rt::export_address("_ZN21CLevelInterfaceNormal22SweepChangePowerButtonEv");
        g.sweep_change_item = rt::export_address("_ZN21CLevelInterfaceNormal21SweepChangeItemButtonEv");
        g.sweep_change_weapon = rt::export_address("_ZN21CLevelInterfaceNormal23SweepChangeWeaponButtonEi");
        g.reload_weapon = rt::export_address("_ZN14CWeaponManager12ReloadWeaponEv");
        g.switch_aim = rt::export_address("_ZN14CWeaponManager9SwitchAimEv");
        g.accessory_update = rt::export_address("_ZN10CAccessory6UpdateERK14AccessoryInput");
        g.ok = g.raise_sync && g.manager_singleton && g.ev_stick_move && g.ev_stick_released && g.get_level &&
               g.get_player_component && g.interface_vtable > 8 && g.press_button && g.release_button && g.sweep_change_power && g.sweep_change_item &&
               g.sweep_change_weapon && g.reload_weapon && g.switch_aim && g.accessory_update;
        if (!g.ok) rt::log("hooks: game symbols for controls not found");
        return g;
    }();
    return s;
}

// Raise an event {vtable, type, payload...} on the global event manager.
void raise(Cpu *cpu, uint32_t ztv, uint32_t type, float a, float b) {
    const GameSymbols &s = symbols();
    uint32_t mgr = LD32(s.manager_singleton);
    if (!s.ok || !mgr) return;
    static uint32_t ev = rt::gcalloc(1, 64);
    ST32(ev, ztv + 8);  // vtable pointer = typeinfo-table address + 8
    ST32(ev + 4, type);
    uint32_t wa, wb;
    std::memcpy(&wa, &a, 4);
    std::memcpy(&wb, &b, 4);
    ST32(ev + 8, wa);
    ST32(ev + 12, wb);
    rt::call(cpu, s.raise_sync, {mgr, ev});
}

// The live game objects the controls act on (all 0 outside a level).
struct Targets {
    uint32_t player = 0;  // PlayerComponent
    uint32_t iface = 0;   // CLevelInterfaceNormal (other interfaces, e.g. the bike, are skipped)
};
Targets targets(Cpu *cpu) {
    const GameSymbols &s = symbols();
    Targets t;
    uint32_t level = s.ok ? rt::call(cpu, s.get_level, {}) : 0;
    if (!level) return t;
    t.player = rt::call(cpu, s.get_player_component, {level});
    uint32_t iface = LD32(level + 0x188);
    if (iface && LD32(iface) == s.interface_vtable) t.iface = iface;
    return t;
}

// Gamepad ("accessory") input. The game's controller layer survives in this build:
// CAccessory::Update(const AccessoryInput &) takes 13 button states into
// CAccessory::m_inputStatus, and CLevel::Update -> CLevel::HandleAccessoryInput maps them to
// the touch HUD buttons, pressing/releasing them with the reserved touch id 777 so the
// game's own button logic (cooldowns, availability, MP messages) still applies. Slots, from
// HandleAccessoryInput:
enum AccessorySlot {
    kAccJump = 0,     // ControlScheme jump button
    kAccGrenade = 1,  // item button (interface +0x70)
    kAccPowers = 3,   // ControlScheme powers button + power sweep (+0x5c)
    kAccSkip = 2,     // weapon widget; in a skippable cinematic its click presses the skip button
    kAccUp = 4,       // click: detonate (interface +0x68); held: zoom in (the scope's slider, +0x78)
    kAccDown = 5,     // held: zoom out
    kAccLeft = 6, kAccRight = 7,  // the four-button console: with up / down, its buttons (below)
    kAccAim = 9,      // second shoot + iron sights; the brake pedal of a vehicle driven with pedals
    kAccFire = 10,    // not mapped by HandleAccessoryInput; completes fire QTEs (CQTEFire::Update); the gas pedal
    kAccSprint = 11,  // ControlScheme rush button
    kAccessorySlots = 13,
    // menus (CMenuScreen::ProcessBasicNavigation)
    kAccMenuConfirm = 0, kAccMenuBack = 1, kAccMenuUp = 4, kAccMenuDown = 5, kAccMenuLeft = 6, kAccMenuRight = 7,
    // 2: weapon widget, 6/7: previous/next weapon on release (left alone: the weapon keys sweep
    // the widget themselves), 12: item sweep (single player)
};

// Buttons the accessory layer does not map: pressed directly with the same touch id.
// CLevelInterfaceNormal::Update acts on them while touched (CButton +0x25) or on the click
// (touch end) that ReleaseButton produces.
struct DirectButton {
    bool input::Controls::*key;
    uint32_t iface_offset;  // CLevelInterfaceNormal member
};
constexpr DirectButton kDirectButtons[] = {
    {&input::Controls::melee, 0x74},     // -> PlayerComponent::UseMeleePower
};
bool g_direct_held[std::size(kDirectButtons)];

// Buttons held directly for a key while it is down (CButton::PressButton, then ReleaseButton):
// the interface's interact / heal buttons, the scheme's jump and second-shoot controls and the
// item button where the accessory layer would not press them (apply_controls).
struct HeldButton {
    uint32_t button = 0;  // the button pressed, 0 while released
    void hold(Cpu *cpu, uint32_t target, bool down);
};
HeldButton g_interact, g_jump, g_grenade, g_second_shoot;

bool g_aim_held, g_reload_held, g_next_held, g_prev_held, g_next_power_held, g_next_grenade_held;
uint8_t g_accessory[kAccessorySlots];
int g_weapon_steps;

// CWeaponManager -> current CWeaponComponent (weapons +0x148, 24 bytes each, index +0x160).
uint32_t current_weapon(uint32_t weapons) {
    uint32_t list = LD32(weapons + 0x148);
    return list ? LD32(list + LD32(weapons + 0x160) * 24 + 0xc) : 0;
}

// Virtual CButton methods (vtable offsets).
bool button_enabled(Cpu *cpu, uint32_t b) { return b && (rt::call(cpu, LD32(LD32(b) + 0xbc), {b}) & 0xff); }
bool button_active(Cpu *cpu, uint32_t b) { return b && (rt::call(cpu, LD32(LD32(b) + 0x30), {b}) & 0xff); }

void HeldButton::hold(Cpu *cpu, uint32_t target, bool down) {
    const GameSymbols &s = symbols();
    if (button && !down) {
        // still the button of this HUD (not one of an unloaded level) and still held
        if (button == target && button_active(cpu, button)) rt::call(cpu, s.release_button, {button});
        button = 0;
    }
    if (down && !button && target) {
        rt::call(cpu, s.press_button, {target});
        button = target;
    }
}

// What the HUD offers around the player (CLevelInterfaceNormal::Update and the player's update
// act on these).
struct HudState {
    uint32_t near = 0;   // PlayerComponent +0x1a0: the interactive object in reach (id at +0x50)
    int near_id = 0;
    int options = 0;     // its own interact buttons (CInteractiveComponent +0x12c, template +0x18:
                         // a vector of 12-byte entries at +0x44..+0x48; touch taps them, MultiActivate)
    bool leave = false;  // the leave button (interface +0x10 -> PlayerComponent::UnInteract): at a turret
    bool zoom = false;   // the scope's zoom slider (interface +0x78, a CSlider): a sniper rifle aimed
    bool board = false;  // a multiplayer vehicle's change-seat buttons (interface +0x20 / +0x24)
    uint32_t touched = 0;  // its CInteractiveComponent when touch uses it by a tap on the object itself
};
HudState hud_state(Cpu *cpu, uint32_t player, uint32_t iface) {
    HudState h;
    if (!player || !iface) return h;
    if ((h.near = LD32(player + 0x1a0))) {
        h.near_id = int32_t(LD32(h.near + 0x50));
        const uint32_t interactive = LD32(h.near + 0x12c), tpl = interactive ? LD32(interactive + 0x18) : 0;
        if (tpl) h.options = int((LD32(tpl + 0x48) - LD32(tpl + 0x44)) / 12);
        // CInteractiveComponent::ProcessScreenTouch: an object with a touch node (+0x74, not
        // hidden: +0x7c's flags +0xf4) is used by a tap on it in the world
        if (interactive && LD32(interactive + 0x74) && LD32(interactive + 0x7c) &&
            (LD32(LD32(interactive + 0x7c) + 0xf4) & 0x18) != 0x18)
            h.touched = interactive;
        h.leave = button_enabled(cpu, LD32(iface + 0x10));
        h.board = button_enabled(cpu, LD32(iface + 0x20)) || button_enabled(cpu, LD32(iface + 0x24));
    }
    h.zoom = button_enabled(cpu, LD32(iface + 0x78));
    return h;
}

}  // namespace

// GS_FaneMinigame: the laser-mirror puzzle an object can start when used (CInteractiveComponent::
// Activate, template +0xd: chapter 3's crystal pods). It takes touch only (onEvent): a tap on a
// deflector selects its cell (+0xdc column, +0xe0 row) and, released, turns it a quarter
// (MapElementCommitRotate with +0xd8 = 1), after which the laser is traced again; the puzzle
// ends itself (+0x120) when the beam reaches the arriving pad. Its 8x6 board (+0x128 wide,
// +0x12c high) sits centred vertically against the right edge, as far from it as from the top
// (the inverse of onEvent's arithmetic on g_ScreenW / g_ScreenH). Keys and pad move a cursor
// over the board (arrows, D-pad, left stick, the move keys), drawn as a frame around its cell
// (pc_hook_fane_render), and confirm, interact or jump turns the cell under it (aim the other
// way, as a drag to the left does: +0xd8 = -1); pause leaves the
// puzzle and reload opens its help page (which confirm closes), as its two touch buttons do.
namespace {
uint32_t g_fane;        // the puzzle in progress (its Update ran this frame or the last)
int g_fane_idle = 99;   // frames since its last Update
int g_fane_col = -1, g_fane_row = 0;
bool g_fane_cursor;     // the cursor is shown: keys or pad moved or turned since the last mouse click
uint8_t g_fane_held;    // up, down, left, right, turn, turn back: the last frame's state (presses act once)

struct FaneBoard { int left, top, cw, ch; };
FaneBoard fane_board(uint32_t fane) {
    static const uint32_t sw = rt::export_address("g_ScreenW"), sh = rt::export_address("g_ScreenH");
    const int w = int32_t(LD32(fane + 0x128)), h = int32_t(LD32(fane + 0x12c));
    const int margin = (int32_t(LD32(sh)) - h) / 2;
    return {int32_t(LD32(sw)) - w - margin, margin, w / 8, h / 6};
}
bool fane_rotatable(Cpu *cpu, uint32_t fane, int col, int row) {
    static const uint32_t rotatable = rt::export_address("_ZN15GS_FaneMinigame22MapElementIsRotateableEii");
    return rotatable && (rt::call(cpu, rotatable, {fane, uint32_t(row), uint32_t(col)}) & 0xff);
}

void fane_controls(Cpu *cpu, const input::Controls &c, const input::MenuNav &nav) {
    static HeldButton exit_button, help_button, close_help;
    if (++g_fane_idle > 1) g_fane = 0;
    if (!g_fane) {
        exit_button.button = help_button.button = close_help.button = 0;
        return;
    }
    if (g_fane_col < 0)  // a new puzzle: the cursor on its first deflector
        for (int i = 0; i < 48 && g_fane_col < 0; i++)
            if (fane_rotatable(cpu, g_fane, i % 8, i / 8)) g_fane_col = i % 8, g_fane_row = i / 8;
    if (g_fane_col < 0) g_fane_col = 0;
    using settings::Action;
    const bool up = nav.up || c.move_y > 0.5f || input::action_held(Action::MoveForward);
    const bool down = nav.down || c.move_y < -0.5f || input::action_held(Action::MoveBack);
    const bool left = nav.left || c.move_x < -0.5f || input::action_held(Action::MoveLeft);
    const bool right = nav.right || c.move_x > 0.5f || input::action_held(Action::MoveRight);
    const bool turn = nav.confirm || input::action_held(Action::Interact) || input::action_held(Action::Jump);
    const bool turn_back = input::action_held(Action::Aim);  // the other way (a drag to the left)
    // its buttons on the left: leave (+0x34: the puzzle ends unsolved), help (+0x38), and the
    // help page's close button (+0x3c, while the page is up: +0x120)
    const bool help_up = LD8(g_fane + 0x120);
    exit_button.hold(cpu, LD32(g_fane + 0x34), !help_up && input::action_held(Action::Pause));
    help_button.hold(cpu, LD32(g_fane + 0x38), !help_up && input::action_held(Action::Reload));
    close_help.hold(cpu, LD32(g_fane + 0x3c), help_up && (turn || input::action_held(Action::Reload)));
    const uint8_t held = up | down << 1 | left << 2 | right << 3 | turn << 4 | turn_back << 5, pressed = held & ~g_fane_held;
    g_fane_held = held;
    if (nav.pointer) g_fane_cursor = false;  // the mouse taps the board itself
    if (!pressed || help_up) return;
    g_fane_cursor = true;
    g_fane_row = std::clamp(g_fane_row - (pressed & 1) + (pressed >> 1 & 1), 0, 5);
    g_fane_col = std::clamp(g_fane_col - (pressed >> 2 & 1) + (pressed >> 3 & 1), 0, 7);
    if (pressed & 48 && fane_rotatable(cpu, g_fane, g_fane_col, g_fane_row)) {
        static const uint32_t commit = rt::export_address("_ZN15GS_FaneMinigame22MapElementCommitRotateEv");
        ST32(g_fane + 0xdc, uint32_t(g_fane_col));
        ST32(g_fane + 0xe0, uint32_t(g_fane_row));
        ST32(g_fane + 0xd8, pressed & 16 ? 1u : 0xffffffffu);
        rt::call(cpu, commit, {g_fane});
        ST32(g_fane + 0xdc, 0xffffffffu);
        ST32(g_fane + 0xe0, 0xffffffffu);
    }
}
}  // namespace

void apply_controls(Cpu *cpu, const input::Controls &c, const input::MenuNav &nav) {
    fane_controls(cpu, c, nav);
    g_pending_yaw = c.look_yaw;
    g_pending_pitch = c.look_pitch;
    g_move_x = c.move_x;
    g_move_y = c.move_y;
    g_sprinting = c.sprint && c.move_y > 0;
    const GameSymbols &s = symbols();
    if (!s.ok) return;
    const Targets t = input::gameplay() ? targets(cpu) : Targets{};
    const HudState h = hud_state(cpu, t.player, t.iface);
    {  // aiming down the sights: the current weapon's aimed flag (+0x61) selects the aim sensitivity
        uint32_t weapons = t.player ? LD32(t.player + 0x170) : 0;
        uint32_t weapon = weapons ? current_weapon(weapons) : 0;
        input::set_aiming(weapon && LD8(weapon + 0x61));
    }
    if (c.move_x != 0 || c.move_y != 0) {
        // stick y grows towards the bottom of the screen (backwards)
        if (!g_stick_active && g_trace_events) rt::log("controls: move %g,%g", c.move_x, c.move_y);
        raise(cpu, s.ev_stick_move, 13, c.move_x, -c.move_y);
        g_stick_active = true;
    } else if (g_stick_active) {
        raise(cpu, s.ev_stick_released, 14, 0, 0);
        g_stick_active = false;
    }

    // The game's controller layer gives jump and grenade other meanings while an interactive
    // object is in reach (PlayerComponent +0x1a0): HandleAccessoryInput stops mapping slots 0 and
    // 1 to the jump and item buttons, and the player's update interacts on a click of slot 0 and
    // picks the object's second option on slot 1 (MultiActivate(1)). On the controller of the time
    // A was both jump and interact; here interact has its own key, and Space next to a console
    // used it instead of jumping (or starting the jetpack: its sections are full of consoles).
    // There, on foot, the keys press the jump control and the item button themselves. Jump still
    // goes through slot 0 off foot (a turret's leave button, interface +0x10, a vehicle's, the
    // third chapter's limited-control minigame StartEngine: they act on its click) and at a
    // turret; grenade through slot 1 at an object with options of its own (the second one: a
    // multiplayer vehicle's gunner seat) and in a vehicle with change-seat buttons.
    const uint32_t scheme = t.iface ? LD32(t.iface + 0xc) : 0;
    auto scheme_control = [&](uint32_t vtable_offset) {  // CBaseControlScheme::Get*Control()
        return scheme ? rt::call(cpu, LD32(LD32(scheme) + vtable_offset), {scheme}) : 0u;
    };
    const bool on_foot = t.player && LD8(t.player + 0xd);  // PlayerComponent enabled
    const bool jump_direct = on_foot && h.near && !h.leave;
    const bool grenade_direct = on_foot && h.near && !h.options && !h.board;
    g_jump.hold(cpu, scheme_control(0x3c), c.jump && (g_jump.button || jump_direct));
    g_grenade.hold(cpu, t.iface ? LD32(t.iface + 0x70) : 0, c.grenade && (g_grenade.button || grenade_direct));

    // An object with options of its own (a multiplayer vehicle's driver and gunner seats, the
    // sphere puzzle's console) hides the interact button (interface +0x60) and shows its own;
    // a click of slot 0 interacts with it (PlayerComponent::Interact: the first option), as the
    // controller's A did. Interact does that there (jump is pressed directly near objects on foot).
    const uint32_t interact_button = t.iface ? LD32(t.iface + 0x60) : 0;
    const bool interact_slot = h.options && !button_enabled(cpu, interact_button) && !g_interact.button;
    // Objects used by a tap on the object itself (chapter 3's crystal pods and the engine-room
    // console): no interact button, and the controller's A did nothing there. A tap ends in
    // CInteractiveComponent::ProcessScreenTouch, which by the object's template starts the
    // player's crystal animation (+0x38: PlayerComponent::PlayerStartCrystalInteract, state 6, at
    // whose end the crystal goes in), an interact transition (+0x39:
    // PlayerStartInteractTransition) or uses the object (CInteractiveComponent::Activate: the
    // console's OnEnter, the engine start). Interact does the same on its press.
    const bool tap_object = h.touched && !h.options && !button_enabled(cpu, interact_button);
    static bool tap_held;
    if (tap_object && c.interact && !tap_held && on_foot) {
        static const uint32_t start_crystal = rt::export_address("_ZN15PlayerComponent26PlayerStartCrystalInteractEv");
        static const uint32_t start_transition = rt::export_address("_ZN15PlayerComponent29PlayerStartInteractTransitionEv");
        static const uint32_t activate = rt::export_address("_ZN21CInteractiveComponent8ActivateEv");
        const uint32_t tpl = LD32(h.touched + 0x18);
        if (tpl && LD8(tpl + 0x38)) rt::call(cpu, start_crystal, {t.player});
        else if (tpl && LD8(tpl + 0x39)) rt::call(cpu, start_transition, {t.player});
        else rt::call(cpu, activate, {h.touched});
    }
    tap_held = c.interact;

    // A limited-control minigame (CPlayerLimitedControlComponent::IsStateMinigame: chapter 3's
    // jetpack and engine start, "touch your arm panel") waits for a tap on the HUD's marker on
    // the arm's console (interface +0x64) or a click of slot 0: jump, and interact too.
    bool minigame = false;
    {
        static const uint32_t get_limited = rt::export_address("_ZN6CLevel32GetPlayerLimitedControlComponentEv");
        static const uint32_t is_minigame = rt::export_address("_ZN30CPlayerLimitedControlComponent15IsStateMinigameEv");
        const uint32_t level = t.iface ? LD32(t.iface + 4) : 0;  // (ILevelInterface +4: its CLevel)
        const uint32_t limited = level && get_limited ? rt::call(cpu, get_limited, {level}) : 0;
        minigame = limited && LD8(limited + 0xd) && is_minigame && (rt::call(cpu, is_minigame, {limited}) & 0xff);
    }

    uint8_t acc[kAccessorySlots] = {};
    // (held through a move away from the object, the slot keeps the button pressed: the
    // accessory layer releases a pressed button whose slot is idle)
    acc[kAccJump] = (c.jump && !jump_direct) || (c.interact && (interact_slot || minigame));
    acc[kAccGrenade] = c.grenade && !grenade_direct;
    acc[kAccPowers] = c.power;
    acc[kAccSprint] = c.sprint && c.move_y > 0;  // the game's rush only runs forward
    acc[kAccFire] = c.fire;
    {  // Driving a vehicle with the pedal controls (the Vehicle controls option, multiplayer):
       // CLevelInterfaceNormal::UpdateJeepPedals gives gas while its pedal (+0x28) or slot 10 (the
       // controller's fire trigger) is held, brake and reverse with the other pedal (+0x2c) or
       // slot 9 (aim); the stick only steers there. Forward and back press them too.
        static const uint32_t get_jeep = rt::export_address("_ZN6CLevel22GetPlayerJeepComponentEv");
        static const uint32_t control_type = rt::export_address("_ZNK20CPlayerJeepComponent14GetControlTypeEv");
        const uint32_t level = t.iface ? LD32(t.iface + 4) : 0;  // (ILevelInterface +4: its CLevel)
        const uint32_t jeep = level && get_jeep ? rt::call(cpu, get_jeep, {level}) : 0;
        // driving (+0x1ac; +0x1ad: the gunner's seat) a car (+0x17c), with pedals (control type 1)
        if (jeep && LD32(jeep + 0x17c) && LD8(jeep + 0x1ac) && control_type && rt::call(cpu, control_type, {jeep}) == 1) {
            acc[kAccFire] = c.fire || c.move_y > 0.5f;
            acc[kAccAim] = c.aim || c.move_y < -0.5f;
        }
    }
    // Slots 4-7 (the controller's D-pad): HandleAccessoryInput gives the four buttons of the
    // sphere puzzle's console (object 123239, chapter 9) to slots 6 / 7 / 4 / 5 (left, right, up,
    // down: MultiActivate(1 / 0 / 2 / 3)); touch taps them. There the arrow keys and the D-pad
    // press them and do nothing else (input::set_dpad_buttons). While a sniper scope's zoom
    // slider is up, slots 4 / 5 held zoom in / out (CSlider::ScrollUp / ScrollDown): next power
    // and next grenade do that then, and the mouse wheel zooms instead of switching weapons.
    // Otherwise a click of slot 4 detonates the mines (CLevelInterfaceNormal::Update, while the
    // detonate button, interface +0x68, is up).
    const bool console = h.near_id == 0x1e167;
    input::set_dpad_buttons(console);
    int weapon_steps = c.weapon_steps;
    bool next_power_key = c.next_power, next_grenade_key = c.next_grenade;
    if (console) {
        acc[kAccUp] = c.dpad_up;
        acc[kAccDown] = c.dpad_down;
        acc[kAccLeft] = c.dpad_left;
        acc[kAccRight] = c.dpad_right;
    } else if (h.zoom) {
        acc[kAccUp] = c.next_power;
        acc[kAccDown] = c.next_grenade;
        next_power_key = next_grenade_key = false;
        if (weapon_steps) {  // a wheel notch moves the slider an eighth of its travel (+ = down)
            static const uint32_t from_position = rt::export_address("_ZN7CSlider26CalculateValueFromPositionEv");
            const uint32_t slider = LD32(t.iface + 0x78);
            const int lo = int16_t(LD16(slider + 0xb2)), hi = int16_t(LD16(slider + 0xb6));
            const int pos = int32_t(LD32(slider + 0xa0)) + weapon_steps * std::max(1, (hi - lo) / 8);
            ST32(slider + 0xa0, uint32_t(std::clamp(pos, lo, hi)));
            if (from_position) rt::call(cpu, from_position, {slider});
            weapon_steps = 0;
        }
    } else {
        acc[kAccUp] = c.detonate;
    }
    {  // a move QTE (CQTEMove) waits for the touch move stick: complete it (+5) on movement
        static const uint32_t qte_manager = rt::export_address("_ZN11CQTEManager9SingletonE");
        static const uint32_t move_vtable = rt::export_address("_ZTV8CQTEMove") + 8;
        uint32_t manager = qte_manager ? LD32(qte_manager) : 0, qte = manager ? LD32(manager + 4) : 0;
        if (qte && LD32(qte) == move_vtable && LD8(qte + 4) && (c.move_x != 0 || c.move_y != 0)) ST8(qte + 5, 1);
    }
    // Cutscenes: CLevel::Update shows the skip button (CLevel+0x2e8) on an accessory click
    // and, while it is shown, skips on a click of slot 2; the button hides itself again after
    // a while (about 0.8 s at 240 fps). Jump (Space / pad A) pressed while the player watches
    // a cinematic shows it right away (CLevel::ToggleCutsceneSkip, as a tap on touch does) and
    // drives slot 2, so releasing the key skips.
    if (c.jump && !g_skip_held) {
        static const uint32_t watching = rt::export_address("_ZN15PlayerComponent19IsWatchingCinematicEv");
        static const uint32_t toggle_skip = rt::export_address("_ZN6CLevel18ToggleCutsceneSkipEv");
        uint32_t level = rt::call(cpu, s.get_level, {});
        uint32_t player = level ? rt::call(cpu, s.get_player_component, {level}) : 0;
        g_skip_held = player && watching && rt::call(cpu, watching, {player});
        uint32_t skip_button = g_skip_held ? LD32(level + 0x2e8) : 0;
        if (skip_button && toggle_skip && !LD8(skip_button + 0x28)) rt::call(cpu, toggle_skip, {level});  // not shown yet
    } else if (!c.jump) {
        g_skip_held = false;
    }
    acc[kAccSkip] = g_skip_held;
    // Menus: every CMenuScreen runs ProcessAccessoryInput on the same status each frame:
    // slot 0 confirms, slot 1 goes back, slots 4-7 move the selection up/down/left/right
    // (on release; holding scrolls).
    if (!input::gameplay()) {
        // The game's controller mode (s_bAccessoryOn, set by Controller::HIDConnectedCallback
        // on Android) shows the menu selection highlight and lets Navig_* move it: on while
        // navigating with keys / pad, off again after a mouse click.
        static const uint32_t accessory_on = rt::export_address("s_bAccessoryOn");
        if (accessory_on) {
            bool any = nav.up || nav.down || nav.left || nav.right || nav.confirm || nav.back;
            if (any && !LD8(accessory_on)) ST8(accessory_on, 1);
            else if (nav.pointer && LD8(accessory_on)) ST8(accessory_on, 0);
        }
        acc[kAccMenuConfirm] = nav.confirm;
        acc[kAccMenuBack] = nav.back;
        acc[kAccMenuUp] = nav.up;
        acc[kAccMenuDown] = nav.down;
        acc[kAccMenuLeft] = nav.left;
        acc[kAccMenuRight] = nav.right;
    }
    if (std::memcmp(acc, g_accessory, sizeof acc)) {
        static uint32_t input = rt::gcalloc(1, kAccessorySlots);
        std::memcpy(HOST(input), acc, sizeof acc);
        std::memcpy(g_accessory, acc, sizeof acc);
        rt::call(cpu, s.accessory_update, {input});
    }
    if (!t.iface) {
        g_interact.button = g_second_shoot.button = 0;
    } else {
        // Interact: the heal button instead while it is up (cooperative play: a fallen team mate
        // in reach, interface +0x6c -> PlayerComponent::DoHeal), which no controller key reached.
        const uint32_t heal = LD32(t.iface + 0x6c);
        const uint32_t use = g_interact.button ? g_interact.button
                             : button_enabled(cpu, heal) ? heal : interact_button;
        g_interact.hold(cpu, use, c.interact && (g_interact.button || (!interact_slot && !tap_object)));
        // Aim also holds the second-shoot control while it is up: the rockets of the mech and of
        // the jeep (their tutorials light it) and a weapon's secondary fire (CLevel::
        // NotifyWeaponChanged enables it for weapons with two fire modes). The controller's aim
        // trigger pressed it together with the iron sights (slot 9).
        const uint32_t second = scheme_control(0x38);
        g_second_shoot.hold(cpu, second, c.aim && (g_second_shoot.button || button_enabled(cpu, second)));
    }

    bool direct_changed = c.aim != g_aim_held;
    for (size_t i = 0; i < std::size(kDirectButtons); i++) direct_changed |= c.*kDirectButtons[i].key != g_direct_held[i];
    g_weapon_steps = std::clamp(g_weapon_steps + weapon_steps + (c.next_weapon && !g_next_held) -
                                    (c.prev_weapon && !g_prev_held), -3, 3);
    g_next_held = c.next_weapon;
    g_prev_held = c.prev_weapon;
    const bool reload = c.reload && !g_reload_held;
    g_reload_held = c.reload;
    const bool next_power = next_power_key && !g_next_power_held;
    g_next_power_held = c.next_power;
    const bool next_grenade = next_grenade_key && !g_next_grenade_held;
    g_next_grenade_held = c.next_grenade;
    if (!c.fire && !g_firing && !direct_changed && !g_weapon_steps && !reload && !next_power && !next_grenade) return;

    if (t.player) {
        uint32_t weapons = LD32(t.player + 0x170);  // CWeaponManager
        if (reload && weapons) rt::call(cpu, s.reload_weapon, {weapons});
    }
    // virtual CBaseControlScheme::GetShootControl() (vtable +0x34)
    const uint32_t shoot = scheme_control(0x34);
    if (c.fire && !g_firing) {  // hold the touch fire button
        g_fire_button = shoot;
        if (shoot) rt::call(cpu, s.press_button, {shoot});
    } else if (!c.fire && g_firing && g_fire_button) {
        if (shoot == g_fire_button) rt::call(cpu, s.release_button, {shoot});  // still the same level
        g_fire_button = 0;
    }
    if (t.player && !g_fire_button) {  // no fire button (other interfaces): the fire flags directly
        if (c.fire) ST8(t.player + 0x31, 1);
        else if (g_firing) ST8(t.player + 0x32, 1);
    }
    g_firing = c.fire;
    if (!t.iface) {
        std::fill(std::begin(g_direct_held), std::end(g_direct_held), false);
        g_aim_held = false;
        g_weapon_steps = 0;
        return;
    }
    for (size_t i = 0; i < std::size(kDirectButtons); i++) {
        bool down = c.*kDirectButtons[i].key;
        if (down == g_direct_held[i]) continue;
        g_direct_held[i] = down;
        if (uint32_t button = LD32(t.iface + kDirectButtons[i].iface_offset))
            rt::call(cpu, down ? s.press_button : s.release_button, {button});
    }
    if (c.aim != g_aim_held) {
        // hold to aim: SwitchAim is what the iron-sight button calls (it checks CanStartAim
        // and handles MP); only toggle when the weapon's aimed flag (+0x61) disagrees
        g_aim_held = c.aim;
        uint32_t weapons = t.player ? LD32(t.player + 0x170) : 0;
        uint32_t weapon = weapons ? current_weapon(weapons) : 0;
        if (weapon && (LD8(weapon + 0x61) != 0) != c.aim) rt::call(cpu, s.switch_aim, {weapons});
    }
    // next power: a one-step swipe on the powers button (what accessory slot 5 does while the
    // zoom slider is hidden; does nothing until two or more powers are unlocked), and on the
    // consumables button (multiplayer items, interface +0x5c), which that click sweeps too
    if (next_power) {
        static const uint32_t sweep_consumable =
            rt::export_address("_ZN21CLevelInterfaceNormal27SweepChangeConsumableButtonEv");
        rt::call(cpu, s.sweep_change_power, {t.iface});
        if (sweep_consumable) rt::call(cpu, sweep_consumable, {t.iface});
    }
    // next grenade type: a swipe on the item button; single player only, like the gamepad
    // layer (accessory slot 12 in CLevel::HandleAccessoryInput)
    if (next_grenade && !multiplayer()) rt::call(cpu, s.sweep_change_item, {t.iface});
    if (g_weapon_steps) {  // one weapon change per frame, like a swipe on the weapon widget
        int dir = g_weapon_steps > 0 ? 1 : -1;
        rt::call(cpu, s.sweep_change_weapon, {t.iface, uint32_t(dir)});
        g_weapon_steps -= dir;
    }
}

// The mission loading screen (GS_Loading) waits for "Touch the screen to continue" once the
// level is loaded: GS_Loading::onEvent sets +0x44c on a touch, and Update calls EndTouch when
// the flag is set and loading has finished (multiplayer sets the flag itself). Setting it
// while GS_Loading is the top state starts the level as soon as it is ready.
// The top game state (Application's state stack, +0x1873c: a vector of gxGameState pointers).
uint32_t top_state(Cpu *cpu) {
    static const uint32_t get_app = rt::export_address("_ZN11Application11GetInstanceEv");
    const uint32_t app = get_app ? rt::call(cpu, get_app, {}) : 0;
    if (!app) return 0;
    const uint32_t begin = LD32(app + 0x1873c), end = LD32(app + 0x1873c + 4);
    return end == begin ? 0 : LD32(end - 4);
}

void auto_continue_loading(Cpu *cpu) {
    static const uint32_t loading_vtable = rt::export_address("_ZTV10GS_Loading") + 8;
    if (loading_vtable == 8) return;
    const uint32_t top = top_state(cpu);
    if (top && LD32(top) == loading_vtable && !LD8(top + 0x44c)) ST8(top + 0x44c, 1);
}

bool loading(Cpu *cpu) {
    static const uint32_t level_vtable = rt::export_address("_ZTV10GS_Loading") + 8;
    static const uint32_t menu_vtable = rt::export_address("_ZTV11GS_LoadMenu") + 8;
    const uint32_t top = top_state(cpu);
    const uint32_t vtable = top ? LD32(top) : 0;
    return vtable && (vtable == level_vtable || vtable == menu_vtable);
}

// Debug (--dump-hud): list the in-game interface's button-like members and the control
// scheme's controls with their class and screen rect, to map HUD buttons to actions.
void dump_hud(Cpu *cpu) {
    const GameSymbols &s = symbols();
    uint32_t level = s.ok ? rt::call(cpu, s.get_level, {}) : 0;
    uint32_t iface = level ? LD32(level + 0x188) : 0;
    if (!iface) { rt::log("dump-hud: no level interface"); return; }
    auto readable = [](uint32_t a) { return rt::guest_mapped(a) && rt::guest_mapped(a + 3); };
    auto class_of = [&](uint32_t obj) -> std::string {
        if (obj < 0x10000 || (obj & 3) || !readable(obj)) return {};
        uint32_t vt = LD32(obj);
        if (vt < g_load_base + 8 || !readable(vt)) return {};
        std::string n = rt::guest_symbol(vt - 8 - g_load_base);
        return n.rfind("_ZTV", 0) == 0 ? n.substr(0, n.find('+')) : std::string();
    };
    auto show = [&](const char *where, uint32_t off, uint32_t obj) {
        std::string c = class_of(obj);
        if (c.find("Button") == std::string::npos && c.find("Control") == std::string::npos) return;
        auto h = [&](uint32_t o) { return int(int16_t(LD16(obj + o))); };
        rt::log("dump-hud %s+%03x %08x %-34s rect %d,%d-%d,%d active %d/%d enabled %d", where, off, obj, c.c_str(), h(8),
                h(0xa), h(0xc), h(0xe), LD8(obj + 0x25), LD8(obj + 0x29), LD8(obj + 0x18));
    };
    rt::log("dump-hud: interface %08x %s", iface, class_of(iface).c_str());
    for (uint32_t off = 4; off < 0x200; off += 4) show("iface", off, LD32(iface + off));
    uint32_t scheme = LD32(iface + 0xc);
    rt::log("dump-hud: scheme %08x %s", scheme, class_of(scheme).c_str());
    static const char *getters[] = {"move", "rotate", "shoot", "second-shoot", "jump", "ironsight", "rush", "powers"};
    for (uint32_t k = 0; k < 8 && scheme; k++) {
        uint32_t obj = rt::call(cpu, LD32(LD32(scheme) + 0x2c + 4 * k), {scheme});
        rt::log("dump-hud: scheme %-12s %08x %s", getters[k], obj, class_of(obj).c_str());
        show(getters[k], 0, obj);
    }
}

}  // namespace hooks

// EventManager::raiseSync(const IEvent &): every gameplay input passes through here.
// Event layout: +0 vtable, +4 type, +8.. payload.
extern "C" void pc_hook_raise_sync(Cpu *cpu, uint32_t entry, GuestFn original) {
    if (hooks::g_trace_events) {
        uint32_t ev = cpu->r[1];
        uint32_t vtable = LD32(ev);
        uint32_t w[4];
        float f[4];
        for (int i = 0; i < 4; i++) {
            w[i] = LD32(ev + 8 + 4 * i);
            std::memcpy(&f[i], &w[i], 4);
        }
        rt::log("event type %3u %-28s %08x %08x %08x %08x | %g %g %g %g", LD32(ev + 4),
                rt::guest_symbol(vtable - 8 - g_load_base).c_str(), w[0], w[1], w[2], w[3], f[0], f[1], f[2], f[3]);
    }
    original(cpu, entry);
}

static bool g_trace_events_flag() { return hooks::g_trace_events; }

// GS_InGame::Update(): runs only while the gameplay state is on top of the state stack
// (pause menu, death screen, loading/briefing and end-of-level states are separate states).
// It also keeps running underneath the pause menu (GS_InGameMenu is pushed on top), so a
// frame only counts as gameplay when GS_InGame itself is the top of the state stack
// (Application + 0x1873c: a vector of gxGameState*, the last one current).
extern "C" void pc_hook_ingame_update(Cpu *cpu, uint32_t entry, GuestFn original) {
    static const uint32_t get_app = rt::export_address("_ZN11Application11GetInstanceEv");
    const uint32_t self = cpu->r[0], lr = cpu->r[14];
    uint32_t app = get_app ? rt::call(cpu, get_app, {}) : 0;
    cpu->r[0] = self;
    cpu->r[14] = lr;
    uint32_t top = 0;
    if (app) {
        uint32_t begin = LD32(app + 0x1873c), end = LD32(app + 0x1873c + 4);
        if (end != begin) top = LD32(end - 4);
    }
    if (!app || top == self) input::note_ingame_frame();
    original(cpu, entry);
}

// <ControlScheme>::Draw(int): the touch controls (move/look sticks, fire, jump, iron sights,
// rush, powers; each scheme, picked in the game's control options, has its own layout). Not drawn while playing with keyboard and mouse; radar, ammo, health and
// context prompts are drawn by the level interface and stay.
extern "C" void pc_hook_scheme_draw(Cpu *cpu, uint32_t entry, GuestFn original) {
    if (input::gameplay()) {
        cpu->ret_pc = cpu->r[14];  // return to the caller without drawing
        return;
    }
    original(cpu, entry);
}

// CHud::SetButtonState(int index, bool pressed): shows a HUD button's normal or pressed scene
// node (CHud +0xd0: 16-byte entries, the two nodes at +0 / +4); CButtonHud::Update calls it
// every frame while the button is enabled. The pause button (CLevel +0x2cc, a CButtonHud:
// its CHud at +0x88, its index at +0x8c) is hidden instead: Escape / Start pause. It stays
// enabled, since CLevel::Update opens the menu on the Back key only while it is.
extern "C" void pc_hook_hud_button_state(Cpu *cpu, uint32_t entry, GuestFn original) {
    static const uint32_t get_level = rt::export_address("_ZN6CLevel8GetLevelEv");
    static const uint32_t hide = rt::export_address("_ZN4CHud10HideButtonEi");
    const uint32_t hud = cpu->r[0], index = cpu->r[1], lr = cpu->r[14];
    const uint32_t level = rt::call_nested(cpu, get_level, {});
    const uint32_t pause = level ? LD32(level + 0x2cc) : 0;
    if (pause && hud == LD32(pause + 0x88) && index == LD32(pause + 0x8c)) {
        rt::call_nested(cpu, hide, {hud, index});
        cpu->ret_pc = lr;
        return;
    }
    original(cpu, entry);
}

// CQTEManager::Draw(): draws the running quick-time event's prompt over the touch control to
// use (fire QTE: the fire button, repel QTE: the powers button, move QTE: the move stick).
// While playing with keyboard and mouse no prompt is drawn; the keys complete the QTEs (fire:
// accessory slot 10, repel: slot 3, move: apply_controls).
extern "C" void pc_hook_qte_draw(Cpu *cpu, uint32_t entry, GuestFn original) {
    if (input::gameplay()) {
        cpu->ret_pc = cpu->r[14];
        return;
    }
    original(cpu, entry);
}

// CLevelTutorial<X>::Draw(): a tutorial draws its dialog ("Tap <icon> to jump!", sprite +0x34)
// and animated highlights over the touch control to use: sprites +0x40 and +0x44 (button
// marker, move-stick ring, swipe finger for looking around), or +0x3c for the special-power
// and rush tutorials. While playing with keyboard and mouse the highlights are left out for
// the duration of the call.
static void draw_without(Cpu *cpu, uint32_t entry, GuestFn original, uint32_t first, int count) {
    const uint32_t self = cpu->r[0];
    uint32_t highlight[2] = {};
    if (input::gameplay())
        for (int i = 0; i < count; i++) {
            highlight[i] = LD32(self + first + 4 * i);
            ST32(self + first + 4 * i, 0);
        }
    original(cpu, entry);
    for (int i = 0; i < count; i++)
        if (highlight[i]) ST32(self + first + 4 * i, highlight[i]);
}
extern "C" void pc_hook_tutorial_draw(Cpu *cpu, uint32_t entry, GuestFn original) {
    draw_without(cpu, entry, original, 0x40, 2);
}
extern "C" void pc_hook_tutorial_draw_3c(Cpu *cpu, uint32_t entry, GuestFn original) {
    draw_without(cpu, entry, original, 0x3c, 1);
}

// CLevelInterfaceNormal::Draw(): draws the touch marker (button +0x64) while +0x4a is set, which
// CLevelInterfaceNormal::Update decides each frame: on an object that takes an energy crystal
// (chapter 3's pods) and on the arm's console in a limited-control minigame (chapter 3's
// jetpack and engine start). While playing with keys or a pad it is left out; Interact uses the
// crystal and starts the minigame (apply_controls).
extern "C" void pc_hook_iface_draw(Cpu *cpu, uint32_t entry, GuestFn original) {
    const uint32_t self = cpu->r[0];
    const uint8_t marker = LD8(self + 0x4a);
    if (input::gameplay()) ST8(self + 0x4a, 0);
    original(cpu, entry);
    ST8(self + 0x4a, marker);
}

// GS_FaneMinigame::Update(): the laser puzzle is running (see fane_controls).
extern "C" void pc_hook_fane_update(Cpu *cpu, uint32_t entry, GuestFn original) {
    if (cpu->r[0] != hooks::g_fane) hooks::g_fane_col = -1, hooks::g_fane_held = 0xff;  // a new puzzle (keys still held from using the pod act later)
    hooks::g_fane = cpu->r[0];
    hooks::g_fane_idle = 0;
    original(cpu, entry);
}

// GS_FaneMinigame::RenderBall(), the last of the board's layers in Render (the map, the beams,
// the ball): then the keys' and pad's cursor, a frame around its cell (CSprite::DrawRectangle,
// a filled rectangle in screen pixels; after Render returns the frame's 2D batch is gone).
extern "C" void pc_hook_fane_render(Cpu *cpu, uint32_t entry, GuestFn original) {
    const uint32_t fane = cpu->r[0];
    original(cpu, entry);
    if (fane != hooks::g_fane || !hooks::g_fane_cursor || hooks::g_fane_col < 0 || LD8(fane + 0x120)) return;
    static const uint32_t draw_rect = rt::export_address("_ZN7CSprite13DrawRectangleEN6glitch5video6SColorERKNS0_4core4rectIiEE");
    static const uint32_t rect = rt::gcalloc(1, 16);
    const hooks::FaneBoard b = hooks::fane_board(fane);
    const int x = b.left + hooks::g_fane_col * b.cw, y = b.top + hooks::g_fane_row * b.ch, t = std::max(2, b.cw / 24);
    const int edges[4][4] = {{x, y, x + b.cw, y + t}, {x, y + b.ch - t, x + b.cw, y + b.ch},
                             {x, y, x + t, y + b.ch}, {x + b.cw - t, y, x + b.cw, y + b.ch}};
    for (const auto &e : edges) {
        for (int k = 0; k < 4; k++) ST32(rect + 4 * k, uint32_t(e[k]));
        rt::call_nested(cpu, draw_rect, {0xff40e0ffu, rect});  // (ABGR) opaque yellow
    }
}

// Slow motion at high frame rates. Application::UpdateApp computes the frame time in whole
// milliseconds: game time g = int((now - last) * speed) (App+0x18808) and, while a slow
// motion runs (QTE slow time: factor App+0x18810, remaining App+0x18814), the slowed frame
// time dt = int(g * factor) (App+0x18804), which the level then runs on. The fraction is
// dropped every frame: at 240 fps (g = 4 ms) the tutorial's slow motion, which settles at
// factor 0.109, credited 0 ms per frame and froze the world (99 ms of 861 over the whole
// sequence). Right after storing dt, UpdateApp calls VoxSoundManager::SetGlobalPitch and
// then reads dt back; this hook replaces dt there with the slowed time including the
// fraction carried over from earlier frames.
extern "C" void pc_hook_set_global_pitch(Cpu *cpu, uint32_t entry, GuestFn original) {
    static const uint32_t update_app = rt::export_address("_ZN11Application9UpdateAppEv");
    static double carry;  // slowed milliseconds owed to the next frame, [0, 1)
    if (update_app && cpu->r[14] == update_app + 0x4cc) {  // the call after the slowed dt (0x10220c0)
        const uint32_t app = cpu->r[4];                       // UpdateApp: r4 = this + 0x18000
        const uint32_t factor_bits = LD32(app + 0x810);
        float factor;
        std::memcpy(&factor, &factor_bits, 4);
        int g = int32_t(LD32(app + 0x808));
        if (hooks::g_time_scale != 1 && g > 0) {  // --speed (tests): everything runs faster
            static double g_carry;
            const double want = g * double(hooks::g_time_scale) + g_carry;
            g = int(want);
            g_carry = want - g;
            ST32(app + 0x808, uint32_t(g));
            ST32(app + 0x804, uint32_t(g));
        }
        if (factor > 0 && factor < 1 && g > 0) {
            double want = g * double(factor) + carry;
            int dt = int(want);
            carry = want - dt;
            ST32(app + 0x804, uint32_t(dt));
        } else {
            carry = 0;
        }
    }
    original(cpu, entry);
}

// Cinematic clock at high frame rates. CCinematicComponent::Update advances the cinematic's
// time (+0x30, ms) by int(dt * GetCinematicSpeed()): at 4 ms frames a speed below 0.25 adds
// nothing and the cinematic stops, and any speed other than 1 loses the fraction every frame.
// At that call (Update + 0x4c; dt is the int at [sp + 0xc], the component in r10) the speed
// returned is adjusted so the truncated step equals the exact one, with the fraction carried
// over per component.
extern "C" void pc_hook_cinematic_speed(Cpu *cpu, uint32_t entry, GuestFn original) {
    static const uint32_t update = rt::export_address("_ZN19CCinematicComponent6UpdateEi");
    static std::unordered_map<uint32_t, double> carry;  // component -> ms owed, [0, 1)
    const uint32_t lr = cpu->r[14], sp = cpu->r[13], component = cpu->r[10];
    original(cpu, entry);
    if (!update || lr != update + 0x4c) return;
    float speed;
    std::memcpy(&speed, &cpu->r[0], 4);
    const int dt = int32_t(LD32(sp + 0xc));
    if (speed == 1 || speed <= 0 || dt <= 0) {
        carry.erase(component);
        return;
    }
    static float logged = 1;
    if (speed != logged) rt::log("cinematic speed %.3f: carrying the fraction of each step", double(logged = speed));
    double &owed = carry[component];
    const double want = dt * double(speed) + owed;
    const int step = int(want);
    owed = want - step;
    const float adjusted = (step + 0.5f) / dt;  // int(dt * adjusted) == step
    std::memcpy(&cpu->r[0], &adjusted, 4);
}

// Path-driven cars at high frame rates. CVehicleCar::checkMoveOnPathCollisions(int dt) is how a
// car on a script path (Vehicle_MoveOnPath: the jeep rides of chapters 2 and 6) notices it is
// blocked: each frame it is slow (physics +0x70 below 4) or pushed (the rigid body's vector at
// +0x238 longer than 1; only that during the grace time +0x324, ms, set after a manoeuvre) adds
// one to a float counter (+0x314), any other frame clears it, and above 3 it reports a
// collision: the car backs up (state 8). Counted in frames, that is 133 ms at 30 fps but 17 ms
// at 240: every bump or climb made the jeep reverse, until it wedged itself against a rock
// (chapter 2, the last section, before the base gate). Replaced: the count advances by the
// frame's time in 30 fps frames, at most one per frame, so 30 fps and below are unchanged.
extern "C" void pc_hook_vehicle_path_blocked(Cpu *cpu) {
    const uint32_t car = cpu->r[0], physics = LD32(car + 0x58), body = LD32(physics + 0x74);
    const int dt = int32_t(cpu->r[1]);
    float push[3], speed, count;
    LDFV(body + 0x238, push, 3);
    speed = LDF(physics + 0x70);
    count = LDF(car + 0x314);
    const bool pushed = push[0] * push[0] + push[1] * push[1] + push[2] * push[2] > 1;
    int grace = int32_t(LD32(car + 0x324));
    if (grace > 0) ST32(car + 0x324, uint32_t(grace -= dt));
    const bool blocked = grace > 0 ? pushed : pushed || speed < 4;
    count = blocked ? count + std::min(1.0f, dt * (30 / 1000.0f)) : 0;
    const bool collision = count > 3;
    if (collision) count = 0;
    STF(car + 0x314, count);
    cpu->r[0] = collision;
}

// Section transitions. LoadLevelSections (Lua) reloads the level with other sections and keeps
// the player where they stood: GS_Loading carries the position, rotation and weapon over, and
// after CLevel::Init the level puts them back (CLevel::UpdateMCSavedData, when CLevel+0x680 is
// set; position at +0x660). The player is not spawned on this path, and the checkpoint restores
// the player's zone by the id saved in the old section (CGameObject::SaveLoad, FindZone), which
// the new section does not have: the player has no zone (CGameObject+0xe8 = 0) and the view
// zone (CZonesManager+0) is whatever box contains the camera first (GetZoneFromPos).
//
// Collision follows zone activity: visible zones (the view zone and those seen through its
// portals) and the player's own zone are active. With no zone of their own, the player stood
// on ground that only existed while the camera saw it: turning around dropped them through
// the world. Seen at the end of the first level's parking-lot basement (section 1 -> 2): the
// basement is zone 15389 in section 1 and 12809 in section 2, GetZoneFromPos picks another.
//
// After such a restore the player is held in place. When the game did give them a zone (the
// script teleported them, as Obj17269_OnEnter's TeleportPlayerToZone(17268) in Lost Ark), that
// zone gets the first frames to show ground below them, and no other zone is entered: entering
// one makes it active, and its triggers run. Lost Ark's section 2 starts with the player far
// below trigger 17649 (zone 15213, upstairs), whose WhileOut wakes three enemies upstairs; the
// probe made zone 15213 the view zone for a few frames, the trigger fired at once, and the
// generator room's enemy waves (which wait until no enemy is left) could not end.
// Otherwise the zones whose box contains the
// position are entered one after another (CZonesManager::TeleportInZone); a zone's collision
// appears a few frames after entering it. A physics ray tests for ground just below the player
// and names the zone of the object hit; when that is a neighbour seen through a portal, that
// zone is entered instead. The zone with the ground becomes the view zone and the player's
// zone (CGameObject::SetZone), as PlayerComponent::SpawnPlayer does for a spawn point. If no
// zone has ground, the player is respawned at the checkpoint the level script just saved
// (PlayerComponent::SpawnPlayer), as the pause menu's "Restart checkpoint" does.
namespace {
constexpr int kZoneSettleFrames = 30;  // frames to wait for a zone's ground after entering it
struct SectionRestore {
    bool active = false;
    float pos[3] = {};            // the restored position
    std::vector<uint32_t> zones;  // candidate zones, in the zone manager's order
    size_t next = 0;              // next candidate to enter
    int wait = 0;                 // frames left for the current candidate's ground to appear
    int own_wait = kZoneSettleFrames;  // frames left for the player's own zone to show ground
    uint32_t owner = 0;           // the zone owning the ground found, once entered
} g_restore;

uint32_t level_of(Cpu *cpu) {
    static const uint32_t get_level = rt::export_address("_ZN6CLevel8GetLevelEv");
    return get_level ? rt::call_nested(cpu, get_level, {}) : 0;
}

// Physics ray straight down from just above p. If it hits at foot level, between `depth` below
// p and just above it (higher hits are the held player's own body), returns true and the zone
// of the object hit (its CGameObject+0xe8; 0 if unknown) in *owner.
bool ground_below(Cpu *cpu, const float p[3], float depth, uint32_t *owner) {
    static const uint32_t singleton = rt::export_address("_ZN15CPhysicsManager9SingletonE");
    static const uint32_t ray = rt::export_address(
        "_ZN15CPhysicsManager19getClosestRayResultERKN6glitch4core8vector3dIfEES5_PS3_S6_P7TTargetii");
    // from, to, TTarget (with a target, the hit goes there: game object at +0, point at +8)
    static const uint32_t buf = rt::gcalloc(1, 64);
    uint32_t physics = singleton ? LD32(singleton) : 0;
    if (!physics || !ray) return false;
    const float v[16] = {p[0], p[1], p[2] + 2, p[0], p[1], p[2] - depth};
    STFV(buf, v, 16);
    // collision filter as PlayerComponent::CheckInteract uses it (the player's own body excluded)
    if (!(rt::call_nested(cpu, ray, {physics, buf, buf + 12, 0, 0, buf + 24, 0xffff7ef7u, 6}) & 0xff)) return false;
    const float hit_z = LDF(buf + 24 + 8 + 8);
    if (hit_z < p[2] - depth || hit_z > p[2] + 0.5f) return false;
    const uint32_t object = LD32(buf + 24);
    *owner = object ? LD32(object + 0xe8) : 0;
    return true;
}

void hold_player(Cpu *cpu, uint32_t level, const float p[3]) {
    static const uint32_t get_player = rt::export_address("_ZN6CLevel9GetPlayerEv");
    static const uint32_t set_position = rt::export_address("_ZN11CGameObject11SetPositionERKN6glitch4core8vector3dIfEE");
    static const uint32_t pos = rt::gcalloc(1, 12);
    uint32_t player = rt::call_nested(cpu, get_player, {level});
    if (!player) return;
    STFV(pos, p, 3);
    rt::call_nested(cpu, set_position, {player, pos});
}
}  // namespace

extern "C" void pc_hook_section_restore(Cpu *cpu, uint32_t entry, GuestFn original) {
    const uint32_t level = cpu->r[0];
    const bool restore = LD8(level + 0x680) != 0;
    original(cpu, entry);
    if (!restore) return;
    g_restore = SectionRestore{};
    for (int i = 0; i < 3; i++) {
        uint32_t bits = LD32(level + 0x660 + 4 * i);
        std::memcpy(&g_restore.pos[i], &bits, 4);
    }
    const uint32_t zones = LD32(level + 0x2bc);  // CZonesManager: zone list at +0x18..+0x1c
    for (uint32_t it = zones ? LD32(zones + 0x18) : 0; zones && it < LD32(zones + 0x1c); it += 4) {
        const uint32_t zone = LD32(it);
        float box[6];  // CZone bounding box: min at +0x12c, max at +0x138
        for (int i = 0; i < 6; i++) {
            uint32_t bits = LD32(zone + 0x12c + 4 * i);
            std::memcpy(&box[i], &bits, 4);
        }
        const float *p = g_restore.pos;
        if (p[0] >= box[0] && p[0] <= box[3] && p[1] >= box[1] && p[1] <= box[4] && p[2] >= box[2] - 2 &&
            p[2] <= box[5] + 2)
            g_restore.zones.push_back(zone);
    }
    g_restore.active = true;
    rt::log("section load: restoring the player at (%.1f, %.1f, %.1f), %zu candidate zones", g_restore.pos[0],
            g_restore.pos[1], g_restore.pos[2], g_restore.zones.size());
}

void hooks::watch_section_fall(Cpu *cpu) {
    if (!g_restore.active) return;
    static const uint32_t teleport = rt::export_address("_ZN13CZonesManager14TeleportInZoneEP5CZone");
    static const uint32_t get_player_component = rt::export_address("_ZN6CLevel18GetPlayerComponentEv");
    static const uint32_t spawn = rt::export_address("_ZN15PlayerComponent11SpawnPlayerEv");
    const uint32_t level = level_of(cpu);
    const uint32_t zones = level ? LD32(level + 0x2bc) : 0;
    if (!zones) {
        g_restore.active = false;
        return;
    }
    hold_player(cpu, level, g_restore.pos);
    uint32_t owner = 0;
    static const uint32_t get_player = rt::export_address("_ZN6CLevel9GetPlayerEv");
    if (g_restore.own_wait > 0) {  // the zone the game gave the player, before any other
        const uint32_t player = rt::call_nested(cpu, get_player, {level});
        const uint32_t own = player ? LD32(player + 0xe8) : 0;
        if (!own) {
            g_restore.own_wait = 0;
        } else if (ground_below(cpu, g_restore.pos, 3, &owner) && (!owner || owner == own)) {
            rt::log("section load: ground found in the player's own zone %u", LD32(own + 0xb0));
            g_restore.active = false;
            return;
        } else if (--g_restore.own_wait > 0) {
            return;
        }
        owner = 0;
    }
    if ((g_restore.next > 0 || g_restore.owner) && ground_below(cpu, g_restore.pos, 3, &owner)) {
        const uint32_t zone = LD32(zones);
        // The ground can belong to a neighbouring zone, seen through a portal of the one entered;
        // its collision would go away as soon as the camera turns. Enter the zone that owns it.
        if (owner && owner != zone && !g_restore.owner) {
            rt::log("section load: ground of zone %u seen from zone %u", LD32(owner + 0xb0), LD32(zone + 0xb0));
            g_restore.owner = owner;
            rt::call_nested(cpu, teleport, {zones, owner});
            g_restore.wait = kZoneSettleFrames;
            return;
        }
        // The player object joins the zone too, as SpawnPlayer does: the player's own zone
        // (CGameObject+0xe8) stays active whatever the camera sees, and the player's portal
        // crossings are only checked with one.
        static const uint32_t set_zone = rt::export_address("_ZN11CGameObject7SetZoneEP5CZone");
        if (uint32_t player = rt::call_nested(cpu, get_player, {level})) rt::call_nested(cpu, set_zone, {player, zone});
        rt::log("section load: ground found in zone %u", LD32(zone + 0xb0));
        g_restore.active = false;
        return;
    }
    if (g_restore.owner) {  // the owner zone was entered but its ground has not appeared (yet)
        if (--g_restore.wait > 0) return;
        rt::log("section load: no ground in zone %u itself", LD32(g_restore.owner + 0xb0));
        g_restore.owner = 0;  // go on with the candidates
    }
    if (g_restore.wait > 0 && --g_restore.wait > 0) return;
    if (g_restore.next < g_restore.zones.size()) {
        rt::call_nested(cpu, teleport, {zones, g_restore.zones[g_restore.next++]});
        g_restore.wait = kZoneSettleFrames;
        return;
    }
    rt::log("section load: no ground at the restored position: respawning at the checkpoint");
    g_restore.active = false;
    uint32_t player = rt::call_nested(cpu, get_player_component, {level});
    if (player && spawn) rt::call_nested(cpu, spawn, {player});
}

// MpManager::IsMultiplayerGame() is a method: it reads +4 of its object, the GameMpManager
// instance (GameMpManager::Singleton, as the game's callers pass it). Called without one it read
// whatever r0 held: the campaign's pause page took itself for the multiplayer one and left its
// upgrade panel empty (pc_offline_pause_page hides that half of the screen in multiplayer).
bool hooks::multiplayer() {
    static const uint32_t singleton = rt::export_address("_ZN13GameMpManager9SingletonE");
    const uint32_t mp = singleton ? LD32(singleton) : 0;
    return mp && LD32(mp + 4) != 0;
}

// The enemies the scripts wait for: GetNoEnemies() counts CAIController::GetNoOpponentsByTeams(~2),
// the registered enemies (a std::map whose node keeps the CGameObject at +0x14) whose team
// (+0x8e & 0x7f) is not 1.
void hooks::for_each_enemy(const std::function<void(uint32_t)> &fn) {
    static const uint32_t singleton = rt::export_address("_ZN13CAIController9SingletonE");
    const uint32_t ai = singleton ? LD32(singleton) : 0;
    if (!ai) return;
    const uint32_t header = ai + 8;
    auto next = [](uint32_t n) {  // std::_Rb_tree_increment: node colour +0, parent +4, left +8, right +0xc
        if (LD32(n + 0xc)) {
            n = LD32(n + 0xc);
            while (LD32(n + 8)) n = LD32(n + 8);
            return n;
        }
        uint32_t p = LD32(n + 4);
        while (n == LD32(p + 0xc)) n = p, p = LD32(p + 4);
        return LD32(n + 0xc) != p ? p : n;
    };
    for (uint32_t n = LD32(header + 8); n && n != header; n = next(n))
        if (const uint32_t o = LD32(n + 0x14); o && (LD8(o + 0x8e) & 0x7f) != 1) fn(o);
}

// An enemy that falls out of the level is never killed: nothing catches it, it keeps falling
// (z -87,000 a minute later), and the script waiting for the room's enemies to die waits
// forever. Seen in Lost Ark's (chapter 3) cryo room: a spider's jump attack (CBigSpiderAiComponent, "jump
// dynamic") missed the player, flew on through an opening into a room without active collision
// and fell, still in its zone (15213, whose box ends at z -9.5). Once a second (single player),
// an enemy more than 100 m below the bottom of its zone's box takes fatal damage the way the
// level scripts deal it (GiveDamage: CLevel::FindObject, CGameObject::ReceiveDamage; its third
// argument false, or the damage is scaled by Application+0x18804, which can be 0), so the game's
// own death and counting run. (Not the level's lowest point: the outdoor zones' boxes
// reach thousands of metres down.)
void hooks::kill_fallen_enemies(Cpu *cpu) {
    static int frame = 0;
    if (++frame % 60) return;
    static const uint32_t get_level = rt::export_address("_ZN6CLevel8GetLevelEv");
    static const uint32_t is_dead = rt::export_address("_ZNK11CGameObject6IsDeadEv");
    if (!get_level || !is_dead || !rt::call_nested(cpu, get_level, {}) || multiplayer()) return;
    auto f = [](uint32_t a) {
        float v;
        const uint32_t bits = LD32(a);
        std::memcpy(&v, &bits, 4);
        return v;
    };
    std::vector<std::pair<int32_t, float>> fallen;
    for_each_enemy([&](uint32_t o) {
        const uint32_t zone = LD32(o + 0xe8);  // CZone: bounding box min at +0x12c (its z at +0x134)
        if (!zone) return;
        const float bottom = f(zone + 0x134), z = f(o + 0x5c);
        if (bottom > -1e30f && bottom < 1e30f && z < bottom - 100 && !(rt::call_nested(cpu, is_dead, {o}) & 0xff))
            fallen.push_back({int32_t(LD32(o + 0x50)), bottom});
    });
    for (const auto &[id, bottom] : fallen) {
        rt::log("enemy %d fell out of the level (more than 100 m below its zone, which ends at z %.0f): killed", id, bottom);
        char code[48];
        std::snprintf(code, sizeof code, "GiveDamage(%d, 1000000, false)", id);  // (false: not scaled)
        eventlog::run_lua(cpu, code);
    }
}

// Replacement for methods of dead online services (SOCIAL_DISABLED in recomp/arm2c.py):
// does nothing and returns 0 (false / null); the generated wrapper returns to the caller.
extern "C" void pc_disabled_service(Cpu *cpu) { cpu->r[0] = 0; }

// SocialMgr::ShowInputAge(): pushes the GS_InputAge age gate whenever the age is unknown
// (SocialMgr+0x78 == -1). The age only lives in memory, so the gate came back on every
// launch, and its only use is ad targeting. Answer it directly instead: ReceiveAge is what
// the dialog's Accept button calls.
extern "C" void pc_hook_show_input_age(Cpu *cpu, uint32_t entry, GuestFn original) {
    (void)entry, (void)original;
    const uint32_t lr = cpu->r[14];
    static const uint32_t get_instance = rt::export_address("_ZN9SocialMgr11GetInstanceEv");
    static const uint32_t receive_age = rt::export_address("_ZN9SocialMgr10ReceiveAgeEi");
    if (get_instance && receive_age) {
        uint32_t mgr = rt::call(cpu, get_instance, {});
        if (mgr && LD32(mgr + 0x78) == 0xffffffffu) rt::call(cpu, receive_age, {mgr, 18});
    }
    cpu->r[14] = lr;
    cpu->ret_pc = lr;
}

// PlayerComponent::ComputeControlVelocity(int): the game's rush (sprint) runs straight ahead
// and ignores sideways input. While rushing (PlayerComponent +0x324 counts up) with a strafe
// key held, turn the sprint velocity (PlayerComponent +0x260, world space; x/y horizontal)
// towards the walk direction, keeping the sprint speed. The right vector is the forward
// direction turned a quarter in the ground plane: (f.y, -f.x).
extern "C" void pc_hook_control_velocity(Cpu *cpu, uint32_t entry, GuestFn original) {
    const uint32_t player = cpu->r[0];
    original(cpu, entry);
    if (!hooks::g_sprinting || hooks::g_move_x == 0 || !input::gameplay()) return;
    if (int32_t(LD32(player + 0x324)) <= 0) return;  // not rushing
    float v[2];
    LDFV(player + 0x260, v, 2);
    float speed = std::hypot(v[0], v[1]);
    if (speed < 1e-6f) return;
    const float fx = v[0] / speed, fy = v[1] / speed;  // facing (the rush direction)
    float dx = fx * hooks::g_move_y + fy * hooks::g_move_x;
    float dy = fy * hooks::g_move_y - fx * hooks::g_move_x;
    float len = std::hypot(dx, dy);
    if (len < 1e-6f) return;
    v[0] = speed * dx / len;
    v[1] = speed * dy / len;
    STFV(player + 0x260, v, 2);
}

// [video] fov on the level's view camera (the scene manager's active camera,
// g_sceneManager +0xdc, inside a level; menus keep their projection).
//
// The game animates the FOV incrementally: every frame CLevel::Render sets
// camera->setFOV(camera->getFOV() + step) (the step, CLevel+0x2f0, eases towards the target
// set by CLevel::SetTargetFov: wider while running, narrower while aiming). So the scale is
// applied in setFOV and hidden again in getFOV: the game reads back its own value, and only
// the projection uses the scaled one. (Scaling in setFOV alone fed the scaled value back into
// the next frame's addition: the FOV ran away to 1.49 rad instead of 0.69 and jumped when a
// run started or ended.)
//
// CNovaSceneManager::drawWeapons draws the gun with the same camera at getFOV() + a weapon
// offset and then restores getFOV(): with getFOV unscaled, the gun pass gets the game's own
// FOV + offset (the model keeps its shape) and is passed through; the restore, the game's
// value again, is scaled like any other set.
namespace {
float g_fov_game = -1;    // the view camera's FOV as the game set it
float g_fov_scaled = -1;  // what was given to the camera instead

bool is_level_view_camera(Cpu *cpu, uint32_t camera) {
    static const uint32_t scene_manager = rt::export_address("g_sceneManager");
    static const uint32_t get_level = rt::export_address("_ZN6CLevel8GetLevelEv");
    uint32_t sm = scene_manager ? LD32(scene_manager) : 0;
    if (!sm || LD32(sm + 0xdc) != camera || !get_level) return false;
    return rt::call_nested(cpu, get_level, {}) != 0;
}
}  // namespace

// glitch::scene::CCameraSceneNode::setFOV(float fovy)
extern "C" void pc_hook_set_fov(Cpu *cpu, uint32_t entry, GuestFn original) {
    if (hooks::g_fov_scale != 1 && is_level_view_camera(cpu, cpu->r[0])) {
        float fov;
        std::memcpy(&fov, &cpu->r[1], 4);
        const uint32_t lr = cpu->r[14];
        const bool gun_pass = rt::guest_symbol(lr - g_load_base).find("drawWeapons") != std::string::npos &&
                              std::fabs(fov - g_fov_game) > 1e-5f;
        if (!gun_pass) {
            g_fov_game = fov;
            fov = g_fov_scaled = std::min(fov * hooks::g_fov_scale, 2.6f);  // < 150 degrees vertical
            std::memcpy(&cpu->r[1], &fov, 4);
        }
    }
    original(cpu, entry);
}

// glitch::scene::CCameraSceneNode::getFOV(): the game's own value for the view camera.
extern "C" void pc_hook_get_fov(Cpu *cpu, uint32_t entry, GuestFn original) {
    const uint32_t camera = cpu->r[0];
    original(cpu, entry);
    // (also at scale 1: the Graphics page may just have set the default back, with the camera
    // still holding the last scaled value)
    if (g_fov_scaled < 0) return;
    float fov;
    std::memcpy(&fov, &cpu->r[0], 4);
    if (std::fabs(fov - g_fov_scaled) > 1e-6f) return;  // not the value this hook set
    const uint32_t result = cpu->r[0];
    if (!is_level_view_camera(cpu, camera)) return;
    cpu->r[0] = result;
    std::memcpy(&cpu->r[0], &g_fov_game, 4);
}

// CPlayerControl::EnablePlayerControl(bool): cutscenes, briefings and scripted sequences
// take control away from the player; KB/M gameplay mode follows it.
extern "C" void pc_hook_enable_control(Cpu *cpu, uint32_t entry, GuestFn original) {
    bool on = (cpu->r[1] & 0xff) != 0;
    if (on != input::player_control() && g_trace_events_flag()) rt::log("player control: %d", on);
    input::set_player_control(on);
    original(cpu, entry);
}

// CPlayerControl::UpdateFromInput(int dt): after the game's own update, turn the view by
// the mouse / right-stick motion collected this frame.
extern "C" void pc_hook_player_update(Cpu *cpu, uint32_t entry, GuestFn original) {
    uint32_t self = cpu->r[0];
    original(cpu, entry);
    if (!input::gameplay() || (!hooks::g_pending_yaw && !hooks::g_pending_pitch)) return;
    uint32_t pa = LD32(self + 0xf0), pb = LD32(self + 0xf4);
    if (!pa || !pb) return;
    float a, b;
    uint32_t wa = LD32(pa), wb = LD32(pb);
    std::memcpy(&a, &wa, 4);
    std::memcpy(&b, &wb, 4);
    // degrees; like the touch path, yaw is left unwrapped and pitch is clamped to
    // +-(int) tuning[+4], the tuning block being CPlayerControl+0xec
    // CPlayerControl +0x1c: the game's own factor for touch look, below 1 while a sniper scope
    // zooms in (CLevelInterfaceNormal::UpdateCameraSensitivity -> CWeaponComponent::
    // GetSniperSensitivity); the mouse and sticks turn slower in the scope the same way
    float zoom;
    const uint32_t zoom_bits = LD32(self + 0x1c);
    std::memcpy(&zoom, &zoom_bits, 4);
    if (!(zoom > 0 && zoom < 1)) zoom = 1;
    a -= hooks::g_pending_yaw * zoom;
    b -= hooks::g_pending_pitch * zoom;
    if (uint32_t tuning = LD32(self + 0xec)) {
        float limit = float(int32_t(LD32(tuning + 4)));
        if (limit > 0) b = std::clamp(b, -limit, limit);
    }
    hooks::g_pending_yaw = hooks::g_pending_pitch = 0;
    std::memcpy(&wa, &a, 4);
    std::memcpy(&wb, &b, 4);
    ST32(pa, wa);
    ST32(pb, wb);
}

// CPlayerJeepComponent::UpdateFromPhysics(int dt): on a vehicle (the jeep of the second chapter's
// ride, the drivable jeeps) the view is the jeep component's own: yaw +0x164 and pitch +0x16c,
// in radians, which this update turns by its CPlayerControl's touch-look deltas (+0x14 / +0x18,
// the control being embedded at +0x10), wraps (yaw) and clamps to the vehicle's limits (pitch;
// the yaw limits of Vehicle_SetYawLimits follow), then places the camera. The player's
// CPlayerControl::UpdateFromInput does not run there, so mouse / stick look reached nothing.
// The look collected this frame is added to the jeep's angles before the update, which then
// applies its own wrap and limits. GetYawDiff turns the view by -yaw: a turn to the right
// (positive yaw, which the on-foot view subtracts) adds.
extern "C" void pc_hook_jeep_physics(Cpu *cpu, uint32_t entry, GuestFn original) {
    const uint32_t jeep = cpu->r[0];
    if (input::gameplay() && (hooks::g_pending_yaw || hooks::g_pending_pitch)) {
        constexpr float kRadians = 3.14159265f / 180;
        float yaw = LDF(jeep + 0x164), pitch = LDF(jeep + 0x16c);
        if (g_trace_events_flag())
            rt::log("jeep look: yaw %.3f pitch %.3f + %.2f / %.2f deg", double(yaw), double(pitch),
                    double(hooks::g_pending_yaw), double(hooks::g_pending_pitch));
        yaw += hooks::g_pending_yaw * kRadians;
        pitch -= hooks::g_pending_pitch * kRadians;
        hooks::g_pending_yaw = hooks::g_pending_pitch = 0;
        STF(jeep + 0x164, yaw);
        STF(jeep + 0x16c, pitch);
    }
    original(cpu, entry);
}

// CPlayerLimitedControlComponent::Update(int dt): the scripted hang and zero-gravity moments of
// the third chapter (PlayerLimitedControlStartHang, SwitchToPlayerLimitedControl) keep their own
// view, yaw +0x120 and pitch +0x124 in degrees, turned by the rate the touch look stick leaves in
// its CPlayerControl (embedded at +0x10: +0x3c / +0x38) and clamped to the script's limits
// (SetCameraYaw / SetCameraPitch, PlayerLimitedControlSetLimitYaw / Pitch). The look collected
// this frame is added before the update, as on foot (a turn to the right lowers the yaw), and
// taken, so the control's UpdateFromInput does not also turn the player object.
extern "C" void pc_hook_limited_control(Cpu *cpu, uint32_t entry, GuestFn original) {
    const uint32_t self = cpu->r[0];
    if (input::gameplay() && (hooks::g_pending_yaw || hooks::g_pending_pitch)) {
        float yaw = LDF(self + 0x120), pitch = LDF(self + 0x124);
        yaw -= hooks::g_pending_yaw;
        pitch -= hooks::g_pending_pitch;
        hooks::g_pending_yaw = hooks::g_pending_pitch = 0;
        STF(self + 0x120, yaw);
        STF(self + 0x124, pitch);
    }
    original(cpu, entry);
}

// ---- controller rumble ([gamepad] rumble): the game had none (phones); these moments get it.
namespace {
float float_arg(uint32_t bits) {  // softfp: a float argument arrives in a core register
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
}  // namespace

// CWeaponComponent::DoFire(bool): each shot of the player's weapon (NPCs and remote players
// have their own weapon components): a short, light kick.
extern "C" void pc_hook_weapon_fire(Cpu *cpu, uint32_t entry, GuestFn original) {
    original(cpu, entry);
    hooks::g_shots++;
    input::rumble(0.15f, 0.35f, 45);
}

// PlayerComponent::ReceiveDamage(const CDamage &): the player is hit.
// Floats in guest memory (the game's objects).
float guest_float(uint32_t at) {
    const uint32_t bits = LD32(at);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
void set_guest_float(uint32_t at, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    ST32(at, bits);
}

#ifdef __3DS__
// The campaign made easier on the 3DS (docs/gameplay-fixes.md, Easier campaign on the 3DS): the
// enemies' shots spread more and hurt less. The game's own values stay in its objects; each call
// sees the scaled one and the original is put back. Multiplayer (players, not NPCs) is left alone.
constexpr float kEnemyAccuracy = 0.4f;  // x CNPCWeaponComponent's accuracy: spread = (1 - accuracy) x the weapon's
constexpr float kDamageTaken = 0.5f;    // x CDamage's amount (+0) the player receives
bool multiplayer(Cpu *cpu) {
    static const uint32_t is_mp = rt::export_address("_ZN9MpManager17IsMultiplayerGameEv");
    return is_mp && rt::call(cpu, is_mp, {}) != 0;
}
#endif

// PlayerComponent::ReceiveDamage(const CDamage &): the rumble of a hit (on the 3DS, less damage).
extern "C" void pc_hook_player_damage(Cpu *cpu, uint32_t entry, GuestFn original) {
#ifdef __3DS__
    const uint32_t damage = cpu->r[1];
    const float amount = damage ? guest_float(damage) : 0;
    const bool scaled = damage && amount > 0 && !multiplayer(cpu);
    if (scaled) set_guest_float(damage, amount * kDamageTaken);
    original(cpu, entry);
    if (scaled) set_guest_float(damage, amount);
#else
    original(cpu, entry);
#endif
    input::rumble(0.55f, 0.45f, 180);
}

// CNPCWeaponComponent::Fire(...): an enemy's shot. Its accuracy (+0xd0, 0..1) sets the spread,
// (1 - accuracy) x the weapon's; on the 3DS the shot is fired with less of it.
extern "C" void pc_hook_npc_fire(Cpu *cpu, uint32_t entry, GuestFn original) {
#ifdef __3DS__
    const uint32_t at = cpu->r[0] + 0xd0;
    const float accuracy = guest_float(at);
    set_guest_float(at, accuracy * kEnemyAccuracy);
    original(cpu, entry);
    set_guest_float(at, accuracy);
#else
    original(cpu, entry);
#endif
}

// CWeaponManager::GetAutoAimCoefs(): the current weapon's auto-aim entry (56 bytes, from the
// level's data; chapter 0's: 0.6 0.4 5 5 0 0 3 0 0.002 0 0.01 0.1 40 30). +48 and +52 are angles
// in degrees (CWeaponManager::SwitchAim multiplies them by pi / 180): the window in which raising
// the sights turns the view onto a target. On the 3DS they are widened (kAimSnap), once per entry
// (again when a level's data is read in its place); the other fields stay the game's.
extern "C" void pc_hook_auto_aim_coefs(Cpu *cpu, uint32_t entry, GuestFn original) {
    original(cpu, entry);
#ifdef __3DS__
    constexpr float kAimSnap = 1.75f;
    const uint32_t coefs = cpu->r[0];
    if (!coefs) return;
    static std::unordered_map<uint32_t, float> widened;  // entry -> its +48 as widened
    auto it = widened.find(coefs);
    if (it != widened.end() && it->second == guest_float(coefs + 48)) return;
    const float h = guest_float(coefs + 48), v = guest_float(coefs + 52);
    if (!(h > 0 && h < 90 && v > 0 && v < 90)) return;  // (not angles: left alone)
    set_guest_float(coefs + 48, std::min(h * kAimSnap, 85.0f));
    set_guest_float(coefs + 52, std::min(v * kAimSnap, 85.0f));
    widened[coefs] = guest_float(coefs + 48);
    rt::log("aim: snap window %g x %g degrees -> %g x %g", h, v, guest_float(coefs + 48), guest_float(coefs + 52));
#else
    (void)cpu;
#endif
}

// CGameCamera::ShakeCamera(int ms, float intensity): explosions, impacts, scripted shakes
// (the vec3 variant and the Lua CameraShake command end up here); CHud::StartShake(int, float)
// the same for the HUD. Strength follows the intensity, the rumble lasts as long (at most 1 s).
extern "C" void pc_hook_camera_shake(Cpu *cpu, uint32_t entry, GuestFn original) {
    const int ms = int32_t(cpu->r[1]);
    const float intensity = float_arg(cpu->r[2]);
    original(cpu, entry);
    if (ms <= 0 || !(intensity > 0)) return;
    const float s = std::clamp(intensity / 2.0f, 0.15f, 1.0f);
    input::rumble(s, s * 0.7f, std::clamp(ms, 60, 1000));
}

// DeviceOptions::SetUsePhysicsThread(bool): physics stays on the game thread (synchronous
// stepping, as on single-core devices). The device profile turns on a physics thread
// (CPhysicsManager::EnablePhysicsThread) that steps Bullet's world while the game thread ray
// tests (coronas, character shadows) and adds / removes collision objects in the same world:
// on truly parallel cores that hit shapes being torn down (pure virtual calls, vtables read from
// freed memory) in a few percent of multiplayer loads, and waiting for the step before those
// calls did not cover every case. Stepping on the game thread cost nothing measurable (game
// thread CPU 0.82 ms/frame against 0.85-0.91 with the thread: its synchronization cost as much).
// NOVA3_PHYSICS_THREAD=1 restores the thread (tests).
extern "C" void pc_hook_use_physics_thread(Cpu *cpu, uint32_t entry, GuestFn original) {
    static const char *env = std::getenv("NOVA3_PHYSICS_THREAD");
    if (!(env && env[0] == '1')) cpu->r[1] = 0;
    rt::log("physics thread: %s", cpu->r[1] & 0xff ? "on" : "off");
    original(cpu, entry);
}

// Menu layout on small screens (docs/3ds.md "Menu layout at 400x240"). The game sorts screens
// into classes by width / height: Is480x320Res() (width <= 480, 1.5 to 1.7), Is960x640Res()
// (width > 480, 1.5 to under 1.7), Is1136x640Res() (1.7 and over). The menus are a 2D layer laid
// out on a quad of CMenuScreen::QUAD_W x QUAD_H, seen by CMenuCamera, whose field of view
// CMenuCamera::Load takes from the class: for the 480x320 class it fits a 480x320 quad to the
// screen. But CMenuManager::Load makes the quad 480x320 only on a screen of exactly 480x320;
// every other screen keeps the 960x640 quad of menu_elements_nova3 (sprites are scaled for the
// 960x640 layout in both classes, CSpriteManager::GetSpriteFilename). On the 3DS's 400x240 (and
// 480x288) the menus showed at twice their size, cropped. Those screens are put in the 960x640
// class instead, where an 800x480 phone (twice 400x240) is: the 480x320 class is left with the
// one screen its layout is made for. Nothing changes at other sizes.
namespace {
struct ScreenClass {
    float aspect;
    bool exactly_480x320;
    bool moved;  // a screen of the game's 480x320 class put in the 960x640 class
};
uint32_t screen_w_address() {
    static const uint32_t a = rt::export_address("g_ScreenW");
    return a;
}
ScreenClass screen_class() {
    static const uint32_t screen_h = rt::export_address("g_ScreenH");
    const int w = int32_t(LD32(screen_w_address())), h = int32_t(LD32(screen_h));
    const float aspect = float(w) / float(h);
    const bool exact = w == 480 && h == 320;
    return {aspect, exact, w <= 480 && aspect >= 1.5f && aspect <= 1.7f && !exact};
}

// Calls a game function that picks small or large sizes by g_ScreenW > 480 with the width of
// a 960x640-class screen (twice the screen's) on a moved screen.
void call_as_960x640_class(Cpu *cpu, uint32_t entry, GuestFn original) {
    const uint32_t w = LD32(screen_w_address());
#ifdef __3DS__
    if (hooks::g_font_width) {  // the in-game fonts, loaded on the bottom screen for the top one
        ST32(screen_w_address(), uint32_t(hooks::g_font_width));
        original(cpu, entry);
        ST32(screen_w_address(), w);
        return;
    }
#endif
    if (!screen_class().moved) return original(cpu, entry);
    ST32(screen_w_address(), w * 2);
    original(cpu, entry);
    ST32(screen_w_address(), w);
}
}  // namespace

// Is480x320Res()
extern "C" void pc_hook_is_480x320_res(Cpu *cpu) { cpu->r[0] = screen_class().exactly_480x320; }

// Is960x640Res(): also the rest of the 480x320 class's ratios (1.5 to under 1.7, as the game's).
extern "C" void pc_hook_is_960x640_res(Cpu *cpu) {
    const ScreenClass s = screen_class();
    cpu->r[0] = s.aspect >= 1.5f && s.aspect < 1.7f && !s.exactly_480x320;
}

// Application::GetFontSize(int font, int size, bool) and CTTFFontSpr::GetOffsetForOutlinedGlyph /
// GetBorderSizeForOutlinedGlyph (the outline around each glyph): the fonts of the menus' 960x640
// layout on a moved screen.
extern "C" void pc_hook_font_size(Cpu *cpu, uint32_t entry, GuestFn original) {
    call_as_960x640_class(cpu, entry, original);
}
