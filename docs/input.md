# Input

`host/input.cpp` turns SDL events into what the game expects; `host/hooks.cpp` applies the
gameplay controls to the game each frame. The touch controls are gone in gameplay: everything
goes through the game's own functions, so its rules (cooldowns, availability, multiplayer
messages) still apply.

## Two modes

The mode follows whether `GS_InGame::Update` ran in the last frame **while `GS_InGame` was the
top of the state stack** (`Application+0x1873c`, a vector of states; the last one is current).
The game keeps updating `GS_InGame` underneath the pause menu, which at first left the mouse
captured and movement active there.

On the New 3DS the menus are drawn on the touch screen itself, which then points at them 1:1;
play is on the top screen (see [New 3DS](3ds.md), Platform: Screens).

- **Menus**: the left mouse button is touch pointer 0 (`GL2JNILib.touchEvent`); keys go to
  `OnKeyDown` / `OnKeyUp` as Android keycodes (Esc = Back); `SUtils.ShowKeyboard` starts
  text entry from the physical keyboard (sent through `SUtils.nativeSendKeyboardData`, Enter
  finishes).
- **Gameplay**: no touch input at all. The cursor is hidden and confined, raw mouse motion
  turns the view, and held keys form a control state applied before each frame. Only Esc
  reaches the game as a key (Back on key-up opens the pause menu). The touch controls
  (every `ControlScheme*::Draw`), the tutorials' touch hints and the QTE prompts are not drawn;
  radar, ammo, health and context prompts stay. The HUD's pause button (top left; `CLevel`
  +0x2cc, a `CButtonHud`) is never shown: `CHud::SetButtonState` hides its two scene nodes
  instead (`CHud::HideButton`). It stays enabled, because `CLevel::Update` opens the pause menu
  on the Back key (Esc, Start) only while it is.

## Controls

Keys are physical keys (SDL scancodes), so WASD keeps its place on AZERTY and other layouts.
Every action is rebindable in `nova3.ini` `[keys]`.

| Action | Default (`[keys]` name) | How it reaches the game |
|---|---|---|
| Move | W A S D, arrows (`move_*`) | `EvStickMove` / `EvStickReleased` on the global event manager (what the walk stick raises); keyboard diagonals normalised |
| Look | mouse, right stick (`[controls] sensitivity`, `aim_sensitivity` while the weapon is aimed, `+0x61`; the stick turns at `look_speed` times that) | the yaw / pitch floats (degrees) behind `CPlayerControl+0xf0` / `+0xf4`, after `CPlayerControl::UpdateFromInput`; pitch clamped like touch look; slower in a zoomed sniper scope by the game's own factor (`CPlayerControl+0x1c`); on a vehicle and while hanging the view of that mode (see [Other control modes](#other-control-modes)) |
| Fire | Mouse1, PadRT (`fire`) | holds the touch fire button (the scheme's virtual `GetShootControl`, `CButton::PressButton` / `ReleaseButton`) and sets accessory slot 10 (fire QTEs) |
| Aim (hold) | Mouse2, PadLT (`aim`) | `CWeaponManager::SwitchAim` whenever the weapon's aimed flag (`+0x61`) disagrees; also holds the scheme's second-shoot control while it is up (the rockets of the mech and the jeep), as the controller's aim trigger did (slot 9) |
| Jump | Space, PadA (`jump`) | accessory slot 0; next to an interactive object on foot the jump control itself (see [Jump next to objects](#jump-next-to-objects)); in a cinematic also the skip (see [Gameplay fixes](gameplay-fixes.md)); leaves a turret or a vehicle |
| Sprint | Shift, PadL3 (`sprint`) | accessory slot 11, only while moving forward |
| Reload | R, PadX (`reload`) | `CWeaponManager::ReloadWeapon` |
| Grenade | G, PadLB (`grenade`) | accessory slot 1; next to an object without options of its own, the item button itself; at a multiplayer vehicle the gunner's seat |
| Power | F, PadRB (`power`) | accessory slot 3 |
| Melee | V, Mouse3, PadR3 (`melee`) | `CButton::PressButton` on interface button `+0x74` |
| Interact | E, PadB (`interact`) | `CButton::PressButton` on interface button `+0x60`; the heal button `+0x6c` while it is up (co-op); at an object with buttons of its own (a multiplayer vehicle, the sphere puzzle) a click of slot 0, the first option (the driver's seat) |
| Next / previous weapon | wheel, Q, Mouse5, PadY, PadRight / Mouse4, PadLeft (`next_weapon`, `prev_weapon`) | `CLevelInterfaceNormal::SweepChangeWeaponButton(±1)`, one step per frame; in a sniper scope the wheel zooms instead (the slider, interface `+0x78`) |
| Next power | C, PadUp (`next_power`) | `CLevelInterfaceNormal::SweepChangePowerButton()`, a one-step swipe on the powers button, and `SweepChangeConsumableButton()` (multiplayer items), as slot 5's click does; in a sniper scope zooms in (slot 4 held) |
| Next grenade type | H, PadDown (`next_grenade`) | `CLevelInterfaceNormal::SweepChangeItemButton()`, single player only (as the game's controller support); in a sniper scope zooms out (slot 5 held) |
| Detonate mines | X, PadBack (`detonate`) | accessory slot 4 (its click detonates while the detonate button, interface `+0x68`, is up) |
| Pause | Escape, PadStart (`pause`) | Android Back |

Verified in a local Wi-Fi match: move, look, fire, hold-to-aim, jump, sprint, reload, weapon
switching, Esc → pause → Resume, the jetpack, a vehicle (driver: in, steer, gas, brake and
reverse, out; gunner's seat). Verified in the campaign: fire (including the slow-motion
QTEs), power, jump, move, look, cutscene skip, interact (boarding the jeep), grenade, next
power (with Slow and Paralyze unlocked it steps through both and none), the jeep ride, the mech, a turret, the sniper
scope's zoom, the jetpack, the sphere puzzle's console, the hanging sequence. **Untested**:
melee, next grenade type and detonate in play (the mines are only found further into the
chapters), the co-op heal button (needs a second player), and gamepads with a physical
controller.

**Crouch** does not exist for the player in this game: only enemies crouch (`CNPCComponent::
EnterCrouch`, the `NpcCmd_Crouch` script command). The player component has no crouch state,
animation or camera height, so there is no key for it.

### Sprint and strafe

The game's rush is a forward-only run that ignores sideways input (measured: W+D while rushing
gave a purely forward velocity; Shift+D ran forward). Sprint is therefore only pressed while
moving forward. While rushing (`PlayerComponent+0x324` > 0) with a strafe key held, a hook after
`PlayerComponent::ComputeControlVelocity` turns the world-space control velocity
(`PlayerComponent+0x260`) towards the walk direction at the same speed (right vector = forward
turned a quarter in the ground plane).

**Toggle sprint** (`[controls] toggle_sprint`, launcher: Controls → Toggle sprint, in game:
Options → Controls): a press of sprint (key, mouse button or controller button) starts
running and the next press stops it; so do stopping (no forward movement) and aiming down the
sights, like the game's own touch button.

**Rumble** (`[gamepad] rumble`, `rumble_strength`): the phone game had none, so hooks add it on
the player's shots, hits taken and camera / HUD shakes (strength from the shake's intensity);
SDL gamepad rumble on the PC, HD rumble on the Switch. **Gyro aiming** (Switch controllers
only, `[gamepad] gyro`): see [Nintendo Switch](switch.md).

### Fire

Firing first wrote the fire flags of `PlayerComponent` directly (`+0x31` held, `+0x32`
released, read by `UpdateFire`). That worked in matches but not in the campaign: the touch fire
button (`CButtonShoot`) raises `EvGeneric` 29 / 30, `CPlayerControl::onEvent` turns them into
the flags `+0x30` / `+0x31` / `+0x32`, and **drops them again while the button is not
`Pressed()`**. The fire key now holds the button itself. The direct flags remain only as a
fallback for interfaces without a fire button. `GetShootControl` must be called through the
vtable: `CBaseControlScheme::GetShootControl` is a base version that returns 0.

## The game's controller support

The game has controller support whose Java input source is gone:

- `CAccessory::Update(const AccessoryInput &)` takes 13 button states into
  `CAccessory::m_inputStatus`, 3 bytes per slot: down, released, clicked.
- `Application::_Update` advances them each frame (`UpdateAccesoryInputStatus`).
- `CLevel::Update → HandleAccessoryInput` presses and releases HUD buttons with the reserved
  touch id 777. An idle slot releases any id-777 press of its button, which is why mapped
  buttons go through `CAccessory::Update` rather than `PressButton`.

Slots:

| Slot | Gameplay | Menus |
|---|---|---|
| 0 | jump; next to an interactive object (`PlayerComponent+0x1a0`) instead its click interacts (`PlayerComponent::Interact`), and leaves a turret or a vehicle | confirm (`Navig_Confirm`) |
| 1 | grenade; next to an interactive object instead the object's second option (`MultiActivate(1)`); a multiplayer vehicle's other seat | back (`PushBackButton`) |
| 2 | weapon widget; skip button in cinematics | |
| 3 | powers (and the repel QTE) | |
| 4, 5 | click of 4: detonate; held: zoom a sniper scope in / out; 5's click changes power and consumable while no scope is up; at the sphere puzzle's console (object 123239) its up / down buttons | up, down |
| 6, 7 | previous / next weapon; at the sphere puzzle's console its left / right buttons | left, right (`Navig_*` on release, `Scroll_*` while held) |
| 9 | second fire + iron sights; the brake pedal of a vehicle driven with pedals | |
| 10 | not used by `HandleAccessoryInput`; completes fire QTEs (`CQTEFire::Update`); the gas pedal of a vehicle driven with pedals | |
| 11 | rush | |
| 12 | item sweep (single player) | |

`glf::AndroidOnKeyDown` is an empty stub and `AndroidOnKeyUp` handles only Back and Menu, so
keys could not simply be passed through.

The slots are read by `HandleAccessoryInput`, but also directly: `CLevelInterfaceNormal::Update`
(slot 0's click leaves a turret, slot 1's picks a vehicle seat, slot 4's detonates, slot 12
held melees), `UpdateJeepPedals` (slots 9 / 10 held), the player's update (slots 0 / 1 next to
an object), `CPlayerLimitedControlComponent::Update` (slot 0's click in a minigame), the fire
and repel QTEs. The prompts of the original controller (string table lines 0x139..0x148: "Press
D-Pad UP to detonate mines!") match these slots.

## Other control modes

Where the player is not on foot, another player component takes the controls
(`CLevel::GetPlayer*Component`), and some HUD buttons are not the control scheme's. Each was
reached with the tour (`--tour-stop`, see [Debugging](debugging.md#scripted-test-runs)) and
played with scripted input.

| Mode | Where | How touch drives it | With keys / pad |
|---|---|---|---|
| Jeep, gunner (`CPlayerJeepComponent`) | chapter 2's ride (sections 2-3), chapter 6's last section (path-driven rides) | look: the delta its own `CPlayerControl` (`+0x10`) gets from the rotate joystick, turning the jeep's view (`+0x164` yaw / `+0x16c` pitch, radians) in `UpdateFromPhysics`, within `Vehicle_SetYawLimits` and the pitch limits; fire: the shoot button; rocket: the second-shoot button | look: added to `+0x164` / `+0x16c` before `UpdateFromPhysics` (it was not reached: the player's `UpdateFromInput` does not run there); fire key; aim key (second shoot) |
| Jeep, driver | multiplayer maps with vehicles (Badlands) | the move stick steers; with the pedal controls (the Vehicle controls option, control type 1) the gas and brake pedals (interface `+0x28` / `+0x2c`) | move keys / left stick; with pedals also W / fire = gas, S / aim = brake and reverse (slots 10 / 9) |
| Entering / leaving a vehicle | multiplayer vehicles: the object's seat buttons (driver, gunner); campaign jeep: its interact button | taps; leave: interface `+0x10` (`PlayerComponent::UnInteract`) | interact = driver (slot 0's click), grenade = gunner (slot 1's click); jump or interact leaves |
| Mech (`CPlayerMechComponent`) | chapter 2 (section 1), chapter 6, multiplayer | its `CPlayerControl::UpdateFromInput` turns the view; shoot / second-shoot buttons | as on foot (the look hook acts on any `CPlayerControl`); aim fires the rockets |
| Turret (`CPlayerMachineGunComponent`, `SwitchToMachineGun`) | chapters 2, 4, 7, 9, multiplayer | as the mech; leave button `+0x10` | as on foot; jump leaves (slot 0's click) |
| Hanging / zero gravity (`CPlayerLimitedControlComponent`) | chapter 3 (the decompression, `PlayerLimitedControlStartHang`) | the rate its `CPlayerControl` keeps from the rotate joystick (`+0x3c` / `+0x38`) turns its view (`+0x120` yaw / `+0x124` pitch, degrees) within the script's limits | look added to `+0x120` / `+0x124` before its update (not reached before) |
| Limited-control minigame (`StartEngine_Therrius`) | chapter 3 ("Touch your arm panel to activate the jetpack") | a tap on the marker (interface `+0x64`) on the arm's console (`Dummy_consola`) | jump or interact (slot 0's click, while `CPlayerLimitedControlComponent::IsStateMinigame`); the marker is not drawn |
| Engine-room console (object 50205) | chapter 3, "Use the console to start the engines" | no interact button: a tap on the console (`ProcessScreenTouch` → `CInteractiveComponent::Activate`, after the interact transition, player state 7) runs its `OnEnter` (the engine start, `StartEngine_Therrius`, about 25 s, then play goes on) | interact does what the tap does (any object with a touch node and no interact button: crystal animation, interact transition or `Activate`, by its template) |
| Laser puzzle (`GS_FaneMinigame`) | chapter 3: pods whose template has the minigame flag (`CInteractiveComponent::Activate`, template `+0xd`; their scripts have `OnMinigameWin`) | an 8x6 board of deflectors: a tap turns one a quarter, a drag left or right turns it either way, and the impulse ball has to reach the central unit; a "?" button opens the help page, a return button gives up | a cursor (arrows, D-pad, left stick, move keys; drawn as a yellow frame) over the board: confirm / interact / jump turn the deflector under it (`MapElementCommitRotate`), aim turns it the other way; reload opens the help page (confirm closes it), pause gives up. Mouse clicks still tap the board itself |
| Multiplayer scoreboard | local matches | the score button on the HUD (`CLevel+0x2dc`) | pause twice in quick succession (the game's own "Press Twice for Scoreboard", `CLevel::Update`) |
| Multiplayer chat | local matches | the chat button on the HUD (`CLevel+0x2e0` → `CLevel::OpenIngameChat`) | **not mapped** |
| Energy crystal pods | chapter 3 (`CInteractiveComponent::NeedCrystal`: the doors and engines that take a crystal) | no interact button: the marker (interface `+0x64`) on the object, a tap on it (`CInteractiveComponent::ProcessScreenTouch`) starts the crystal animation (`PlayerComponent::PlayerStartCrystalInteract`, state 6), at whose end the pod's script runs | interact calls `PlayerStartCrystalInteract` on its press (the interact button and slot 0 did nothing there); the marker is not drawn |
| Jetpack | chapter 9 (sections with the `jetpack` special power), multiplayer (the armory's jetpack) | hold jump: after the special power's delay (`CLevelInterfaceNormal::Update`, `+0xa4`) `PlayerComponent::SetState(2)`; thrust while held, fuel `PlayerComponent+0x398` | hold jump (see [Jump next to objects](#jump-next-to-objects)) |
| Sphere puzzle's console (object 123239) | chapter 9, the lava chamber | its four buttons (`CInteractiveComponent::MultiProcessScreenTouch`) | arrow keys / D-pad (slots 6 / 7 / 4 / 5, as `HandleAccessoryInput` maps them for this object); they do not move or switch weapons there (`input::set_dpad_buttons`) |
| Sniper scope zoom | sniper rifle aimed | the zoom slider (interface `+0x78`, `CSlider`) | wheel (an eighth of its travel per notch), next power / next grenade held (slots 4 / 5) |
| Mines | grenade type "mine" | the detonate button (interface `+0x68`) | `detonate` (slot 4's click) |
| Heal a team mate | co-op | the heal button (interface `+0x6c` → `PlayerComponent::DoHeal`) | interact, while the button is up |
| QTEs | chapter 1 and others: `CQTEFire`, `CQTERepel`, `CQTEMove` (the only kinds) | the fire button / powers button / move stick | fire (slot 10), power (slot 3), any move ([Gameplay fixes](gameplay-fixes.md)) |
| Multiplayer items | multiplayer (consumables) | the item sweep button (interface `+0x5c`) | power uses, next power changes |

Not in the game: ladders, ziplines, climbing, cutscene choices (no symbols or script commands);
`CPlayerBikeComponent`, `CPlayerCarComponent` and `CPlayerControlledFlyComponent` exist but no
level's script switches to them. The vehicle HUD's touch buttons (leave, seat, pedals) are drawn
by the level interface, not the control scheme, and still show.

### Jump next to objects

Next to an interactive object (`PlayerComponent+0x1a0`: a console, a door panel, a turret, a
vehicle) the game's controller layer gives slots 0 and 1 other meanings: `HandleAccessoryInput`
stops pressing the jump and item buttons, and the player's update interacts on a click of slot 0
and picks the object's second option on slot 1. On the Android controller A was jump and
interact at once. Here interact has its own key, but Space next to an object interacted on its
release instead of jumping: the jeep of chapter 2 was boarded by holding Space beside it, and
in chapter 9 the jetpack (a long press of jump) did not start next to the jetpack sections'
consoles. On foot there the host presses the jump control and the item button itself
(`CButton::PressButton` / `ReleaseButton`); a press kept while walking away continues through
the slot. Off foot (turrets, vehicles, the minigame) slot 0 keeps its meaning, so jump still
leaves a turret.

## The Controls page

The title menu (Lua screens from `menus.lvc`) and the in-game menu (C++ pages of `CPanel`,
`CSlider` and `CButtonSpr` widgets, `GS_InGameMenu`) had different Controls screens, both made
for touch. There is now one, built from the game's own widgets (`host/controls_menu.cpp`):
the in-game menu's page 7, which the title menu's Controls button (screen 150509, through
`Menu_GoToMenu`) opens too. Its widgets are re-purposed:

| Row | Was | Now |
|---|---|---|
| Slider | Sensitivity (touch) | Sensitivity, `[controls] sensitivity` (0.25–4, logarithmic, 1 in the middle) |
| Slider | Aim Acceleration | Aim Sensitivity, `[controls] aim_sensitivity` |
| Button | Control Scheme | Keyboard & Mouse: the binding page (on the 3DS: Controller, the controller's binding page) |
| Button | Customize Interface (touch layout) | Controller: the binding page for the gamepad (on the 3DS hidden: `CButton::SetEnabled(false)`, frame and text not drawn, left out of the up / down links; no keyboard or mouse there) |
| Option | Vehicle controls | unchanged |
| Switch | Left-handed (touch layout) | Aim assist (controller only) |
| Switch | Invert Y-axis | Invert Y, mouse and stick |
| Switch | Gyroscope (only on devices with one) | Auto-leveling (controller only) |
| Switch | (added) | Toggle run, `[controls] toggle_sprint` (any device) |
| Switch | (added) | Rumble, `[gamepad] rumble` |
| Slider | (added) | Rumble Strength, `[gamepad] rumble_strength` (0–1; a short rumble while it moves) |
| Switch | (added, Switch only) | Gyro Aiming: OFF / AIM (while aiming) / ALWAYS, `[gamepad] gyro` (click or left / right) |
| Slider | (added, Switch only) | Gyro Sensitivity, `[gamepad] gyro_sensitivity` (0.25–4, logarithmic) |
| Switch | (added, Switch only) | Gyro Invert X-Axis / Y-Axis, `[gamepad] gyro_invert_x` / `gyro_invert_y` (the gyro's own, not the stick's Invert Y) |
| Switch | (added, Switch only) | Swap A/B, X/Y, `[gamepad] swap_ab_xy` |

The added rows are made like the game's (`CButtonText` labels, `CButtonSpr` switches with the
switch sprite frames 0x7b / 0x7c, `CSlider` with the sensitivity sliders' arguments, the wide
`CButtonSpr` of the Keyboard & Mouse button), placed below the last row at the same spacing,
linked into the arrow / D-pad navigation, and the panel's scroll range is recomputed
(`CPanel::UpdateScrollBarData`). Left / right on a selected switch also switches it: the
host reads the left / right slots of `CAccessory::m_inputStatus` (6 and 7, their "clicked"
byte, +0x14 / +0x17; `CPanel::Update` moves a selected slider while they are down, +0x12 /
+0x15) for the selected button (`CButton` +0x24).

The **binding page** is the in-game menu's page 9 (Control Scheme), replaced: a scrolling
panel with one row per action (label and button, like the switch rows), drawn by the Controls
page's renderer with that panel in place. Clicking a row (mouse, Enter or pad A) waits for
the next key or mouse button (on the Controller page: pad button). Escape, Start / Back, or
input from the other device cancel. A control taken from another action gives that action
the old one. The page shows each action's first control on that device; rebinding replaces
all of them (more can be listed in `nova3.ini`). Movement on the Controller page is the left
stick and cannot be changed. Pause stays Escape / Start. While a row waits, input is held back
from the game (`input::begin_capture`). Arrows, D-pad and left stick move from row to row
(the rows are linked like the game's own: `CButton::SetNavig_Up / Down`; `CPanel::NavigDown`
scrolls the selection into view), and the first row starts selected.

Both pages scroll with the **mouse wheel**, a row per notch: the wheel moves the menu's scroll
bar (`GS_InGameMenu` +0x5c, position +0xa8 within +0xa0 .. +0xa4), from which
`CPanel::Update` takes the panel's offset, as a drag on the bar does. In menus the wheel's
notches are taken once a frame (`input::take_menu_wheel`); those turned elsewhere are dropped.

Opened from the title menu, the in-game menu needs a few things a level would provide: the
upgrade shop list stays empty, and the objectives page is an empty panel. The title menu
underneath ignores touches and controller navigation while the page is open (and for 0.3 s
after, so the click that closes it does not reach it). Back on the Controls page closes it, and
so does Resume. Whichever way it closes, `GS_InGameMenu::Release` ends a key capture still
waiting and the title menu's input block (closing with Resume used to leave the game deaf).
Fonts: the title menu loads them at menu sizes (`Application::LoadFonts(false)`) and draws
them scaled; levels load them at in-game sizes (`LoadFonts(true)`), which the in-game menu's
pages are made for. The page opened from the title loads the in-game fonts first and the menu
fonts again when it closes; at 4K its text was otherwise half the size it has in a level.
With the in-game fonts loaded, the title menu's own text under the page would be drawn twice
as large, so it is left out while the page is open (`CMenuManager::Render` is wrapped; the
font `DrawString` wraps in `host/offline.cpp` skip it); the background and dimmed buttons stay.

**Animation.** The in-game menu changes pages at once, with the button sound
(`PlayFeedbackSound`, `sfx_i_button.wav`), and the Controls and binding pages do the same.
The title menu is a stack of screens in depth (`CMenuManager` +0x30 .. +0x34): opening a
screen moves every screen one level back (`CMenuScreen::PushScreenForward`, target depth
+0x60), and the menu's update moves them there over a quarter second (current depth +0x5c,
timer +0x64), shrinking and dimming them; going back reverses it. The Controls page opened
from the title does the same: Options moves back as it opens, as it does for Sound, and comes
forward again when it closes.

**Aim assist and auto-leveling** are the game's own (`CGameSettings` +0x10 and +0x18). They
are applied only while a controller was used last (`input::using_gamepad`): aim assist pulls
the aim towards targets, auto-leveling tilts the view back to the horizon while moving
(`PlayerComponent::UpdateAutoLeveling`); with a mouse both would fight the player. The game's
own left-handed, invert and gyroscope settings are kept off: the host inverts the view itself.

The pause menu's Options lose Customer Care (Sound, Controls and Advanced move up); Advanced,
in both the pause menu and the title menu, is now **Graphics** and opens the Graphics page
(see [Application and settings](app-and-settings.md#the-graphics-page)). Advanced held aim assist (now
on the Controls page), auto-leveling (likewise), high resolution and push notifications
(nothing to do on a PC) and **Metric**: whether the distance to the objective marker reads in
metres or feet (`CLevel::RenderObjective`, `CGameSettings` +0x5c). It keeps its current value
(metres unless changed before).

## Tutorial prompts

The first level's tutorial lines name touch controls ("Tap ^29^ to shoot!", ^29^ being an
icon of the touch button), or, with the game's joystick flag, buttons of the Android pads of
the time ("Press the Right Bumper to shoot!"), which match this port's mapping only in part.
Chapter 3 (Lost Ark) has one more: the objective "Touch your arm panel to activate the jetpack."
(line 0x12 of the level table with 108 strings), replaced the same way with the interact binding.
`Application::GetString` is wrapped (`host/prompts.cpp`): such a line is replaced by one that
names the player's binding for the action on the device used last, in all ten languages
("Press Left Mouse Button to shoot!", "Drücke RT, um zu schießen!", "マウスの左ボタンを押して撃て！").

The lines are found by string ID, the same in every language: `GetString(id)` indexes the
current string table (`CStrings`: `Application` +0x18754; +0x18758 is the menus' MAIN table,
+0x1875c the level's), and with the joystick flag `MapJoystickTutorialString` maps a touch
line's ID to its controller line's (0x27 → 0x140, ...), so the wrap sees the touch ID. The
level table is taken for the first level's when line 0x27 holds the fire icon (`^29^`) and
line 0x2a the jump icon (`^28^`), which every language keeps. "Press X to skip" is MAIN 0x787.
The sentences follow the game's own controller lines in each language, with the binding in
place of the button; mouse buttons are named in the language ("die linke Maustaste"), keys and
pad buttons as in the settings ("Space", "RT"). The language is `Application` +0x188c0 (0 EN,
1 FR, 2 DE, 3 SP, 4 IT, 5 JP, 6 KR, 7 CH, 8 BR, 9 RU; `ADSGetLanguage` gives the codes). The
mine-detonation line keeps the game's text (that control is not mapped). Checked in English,
German and Japanese.

## Menus without the mouse

Arrows, Enter, the D-pad / left stick, pad A and pad B drive the game's own controller
navigation: every `CMenuScreen` runs `ProcessAccessoryInput` on the accessory status each frame
(`CMenuManager::Update`). The selection highlight belongs to the game's controller mode
(`s_bAccessoryOn`), switched on when navigating with keys or pad and off after a mouse click.
Works in the main menu and the pause menu.

## Gamepads

SDL's gamepad API: Xbox (XInput), PlayStation, Switch and generic controllers through SDL's
database. `PadA` / `B` / `X` / `Y` are positions (bottom / right / left / top). The left stick
walks (analog, through the same `EvStickMove`), the right stick looks (`look_speed` times the sensitivity, squared
response, `deadzone`), buttons follow `[keys]`. Start (the pause binding) is Back everywhere;
in menus B is Back through slot 1. The pad is ignored while the window is in the background.
