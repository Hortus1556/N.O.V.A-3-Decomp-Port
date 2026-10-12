# Gameplay fixes

Problems that appeared once the game ran on a PC, mostly because touch was gone or because
the game now runs at 240 fps instead of a phone's 30. Each section gives the cause, the fix
and how it was checked. Hooks are in `host/hooks.cpp` unless noted.

## Loading screen: no "Touch the screen to continue"

A mission's loading screen (`GS_Loading`) waited for a touch once the level was loaded.
`GS_Loading::onEvent` sets the "touched" flag (`+0x44c`) on a touch, and `GS_Loading::Update`
calls `EndTouch` when that flag, or the section-load flag `+0x44d`, is set after loading. The
host sets `+0x44c` every frame while `GS_Loading` is the top state
(`hooks::auto_continue_loading`), so the level starts as soon as it is ready. Section loads
(`+0x44d`) never waited for a touch in the first place.

## Campaign tutorial and quick-time events

The first level teaches the controls with tutorials and slow-motion shots. On PC it hung at
the first shot ("Tap ⊙ to shoot!") and showed touch-button markers. Five fixes:

1. **Fire through the real fire button** (see [Input](input.md#fire)): writing
   `PlayerComponent`'s fire flags directly did nothing in the campaign.
2. **Fire QTEs**: each slow-motion shot is a `CQTEFire` (started from Lua; slow time through
   `CQTEManager::AddSlowTime`). It completes when the fire button is active
   (`CButton::IsActive`: `+0x25` set, `+0x29` clear) or when **accessory slot 10** is down
   (`CAccessory::m_inputStatus + 0x1e`). The fire key sets slot 10, so holding fire plays the
   whole sequence (four fire QTEs in the first level). A **repel QTE** (`CQTERepel`)
   completes with slot 3, the power key.
3. **Move QTEs** (`CQTEMove`) only check the touch move stick. While a move key is held, the
   host sets the QTE's done flag (`+5`). **Untested**: no move QTE was found in the first level.
4. **No touch prompts**: `CQTEManager::Draw` (which inlines `CQTE::Draw`, so wrapping
   `CQTE::Draw` had no effect) is skipped in gameplay mode; every QTE prompt points at a touch
   control.
5. **Tutorial highlights**: tutorials (`CLevelTutorial*::Draw`) draw their dialog (sprite
   `+0x34`) and animated highlights over the touch control to use: sprites `+0x40` / `+0x44`
   (button marker, move-stick ring, swipe finger), `+0x3c` in the special-power and rush
   tutorials. The highlight pointers are cleared for the duration of the call, so the dialog
   stays. The steps complete with the PC controls: look around (mouse;
   `CLevelTutorialRotate` checks the view direction), abilities (F), jump (Space), move (WASD).
   Shield, reload, change-weapon and grenade tutorials draw no highlight.

Tested by playing the first level's tutorial chain in a scripted run: shots, ability, look,
jump and move each complete, through to the next cutscene. The orange touch markers and the
fingerprint no longer appear.

## Slow motion at high frame rates

`Application::UpdateApp` computes the frame time in **whole milliseconds**:

- game time `g = int((now - last) * speed)` (`App+0x18808`; `speed` is
  `CGameSettings::Singleton+0x1c`, normally 1)
- during slow time, `dt = int(g * factor)` (`App+0x18804`; factor `App+0x18810`, remaining
  slow time `App+0x18814`)

The fraction is dropped every frame. At 240 fps a frame is 4 ms, so a factor below 0.25 makes
**every frame 0 ms and the world freezes**. Measured over the tutorial's slow motion: 99 ms of
game time credited where 861 ms was due. The slow-down ramp (`CQTEManager::Update`) runs on
`dt` too, so it also stalled, at factor 0.109 instead of reaching its target. On a phone at
30 fps (33 ms frames) the same code still advances. This was the "tutorial hangs" bug.

The fix needs the value computed in the middle of `UpdateApp`, which hooks cannot patch.
Right after storing `dt`, `UpdateApp` calls `VoxSoundManager::SetGlobalPitch` and then reads
`dt` back. A wrap on that call, recognised by its return address (`UpdateApp + 0x4cc`),
rewrites `dt` as the slowed time plus the fraction carried over from earlier frames (the
carry stays in [0, 1) ms). Measured afterwards: 401 ms credited for 397 ms due, and the
slow-down reaches its target (a full stop that waits for the shot, which is the intended
design).

The game clock itself loses nothing: `OS_GetTime` is in whole milliseconds and the last time
is stored as the same integer, so the per-frame deltas add up exactly. Time is lost where a
frame's milliseconds are scaled and truncated. `tools/truncscan.py` lists the places that add
`int(dt * factor)` to a stored counter. Besides slow motion, one matters in play:

- **Cinematics** advance their clock (`CCinematicComponent+0x30`) by
  `int(dt * GetCinematicSpeed())`. A slowed cinematic loses the fraction every frame, and below
  speed 0.25 stops at 4 ms frames. `GetCinematicSpeed` is wrapped at that call
  (`CCinematicComponent::Update + 0x4c`): the speed returned makes the truncated step equal the
  exact one, with the fraction carried per component (`pc_hook_cinematic_speed`).

The others: the bike's engine RPM (`CPlayerBikeComponent::ComputeRPMAndGear`, a later level,
untested), touch-widget animations (unused) and one-off values.

## Cutscene skip with Space

`CLevel::Update` shows the skip button (`CLevel+0x2e8`) when any accessory button is clicked
during a skippable cinematic. While the button is shown, a click of **slot 2** skips: it sets
`CLevel+0x2c0`, after which `UpdateApp` runs 200 ms frames until the cinematic ends.

When jump is pressed while `PlayerComponent::IsWatchingCinematic` (player state `+0x130 == 3`),
the host:
- shows the button right away (`CLevel::ToggleCutsceneSkip`, as the first tap does on touch),
  because the button hides itself again after a moment;
- holds slot 2, so releasing the key skips.

One press of Space or pad A skips. Tested on the opening cutscene: one press jumps to the
first gameplay scene. If Space is still held when the cinematic ends, releasing it may click
slot 2 (the weapon widget) once.

## Enemies that fall out of the level

An enemy that falls out of the level is never killed: nothing catches it, and the script
waiting for the room's enemies to die waits forever. Seen in Lost Ark's cryo room (after
checkpoint 16830): a spider's jump attack (`CBigSpiderAiComponent::JumpInit`, "jump dynamic")
missed the player and flew on through an opening into a room without active collision; it was
at z -87,000 a minute later, still counted. A spider's jump ends only when it reaches the
player or hits collision.

**Fix** (`hooks::kill_fallen_enemies`, every port): once a second in single player, an enemy
the scripts count (`hooks::for_each_enemy`: the `CAIController` set that `GetNoEnemies` counts)
that is more than 100 m below the bottom of its zone's box takes fatal damage the way the level
scripts deal it: `GiveDamage(id, 1000000, false)` (`CLevel::FindObject`,
`CGameObject::ReceiveDamage`; with fewer arguments the damage is scaled by
`Application+0x18804`, which can be 0). The game's own death, `OnDead` scripts and counting
run. It compares with the enemy's own zone, not the level's lowest point: the outdoor zones'
boxes reach thousands of metres down. Damage only takes effect in an active zone; an enemy
that fell from near the player keeps the player's zone.

Tested by dropping the first wave's enemy (16316, after `Enable(16316)`) 800 m below its room:
killed within a second, the count went to 0.

## Falling through the world after a section load

Reported as "the second stage spawns me outside the map". Some levels are split into
sections. At the end of the first level's parking-lot basement, the level script saves a
checkpoint and calls the Lua function `LoadLevelSections(2, …)`, which reloads the level with
section 2 behind a loading screen and puts the player back where they stood:

- `LoadLevelSections` creates a `GS_Loading` in section mode (`+0x44d = 1`) and stores the
  player's position (`+0x450`), rotation (`+0x45c`), weapon (`+0x46c`) and the section mask
  (`+0x470`).
- `GS_Loading::FinishLoadingStep2` copies them into the new level (`CLevel+0x660…`) and sets
  `CLevel+0x680`.
- After `CLevel::Init`, `CLevel::UpdateMCSavedData` moves the player there with a plain
  `SetPosition`.

The player is never spawned on this path. The checkpoint restores the player's zone by the ID
saved in the old section (`CGameObject::SaveLoad` → `CZonesManager::FindZone`), and the new
section doesn't have it: the basement is zone 15389 in section 1 and zone 12809 in section 2.
So the player has **no zone** (`CGameObject+0xe8` = 0), and the view zone (`CZonesManager+0`)
becomes the first zone whose box contains the camera (`GetZoneFromPos`), which is not the
basement.

Collision follows zone activity. A zone is active while it is visible (the view zone and the
zones seen through its portals, `CZone::SetVisible` → `RegisterActive`) or while it is the
player's own zone (`CGameObject::SetZone`). With no zone of their own, the player stood on
floor that existed only while the camera happened to see it: the player fell forever (z went
-60, -188, -561, … past -24,000), or fell as soon as they turned around or walked on.

How it was pinned down (see [Debugging](debugging.md)):
- The event log showed the transition and the fall; the `event: zone` lines showed
  `player 0` after the load.
- `--place` / `--lua` replayed the transition deterministically, `--look` turned the camera
  around, which reproduced the fall on demand.
- `--zones` listed both sections' zones: 12809 has the same bounds as section 1's basement.
- The "No triangles in collision object …" warnings in the log are harmless: those objects
  keep their real collision in a second group.

**Fix** (`pc_hook_section_restore`, `hooks::watch_section_fall`): after such a restore, the
player is held at the restored position while the zones whose bounding box contains it
(`CZone+0x12c` min, `+0x138` max) are entered one after another (`TeleportInZone`). A zone's
collision appears a few frames after entering it. Each frame, a short physics ray tests for
ground at foot level (`getClosestRayResult` with a `TTarget`: the object hit is at `+0`, the
point at `+8`). It uses the collision filter of `PlayerComponent::CheckInteract` (mask
`0xffff7ef7`, group 6) so it does not hit the player's own body. The zone of the object hit
(its `+0xe8`) owns the ground; if that is a neighbour seen through a portal, it is entered
instead. The owning zone becomes the view zone and the player's zone (`SetZone`), as
`PlayerComponent::SpawnPlayer` does for a spawn point. At most 30 frames are spent per zone.
Only if no zone has ground is the player respawned at the checkpoint the script saved
(`SpawnPlayer`, as "Restart checkpoint" does).

When the game did give the player a zone (the script teleported them, as Lost Ark's (chapter 3)
`Obj17269_OnEnter` does with `TeleportPlayerToZone(17268)`), that zone gets the first 30
frames to show ground below them, and no other zone is entered. Entering a zone makes it
active, and its triggers run: Lost Ark's section 2 starts with the player far below trigger
17649 (zone 15213, the upper deck), whose `WhileOut` handler wakes three enemies up there.
Probing made 15213 the view zone for a few frames, the handler ran at once, and the generator
room's waves, which wait until no enemy is left (`GetNoEnemies`), could not end when those
enemies stayed upstairs (reported on the Switch as "the enemies don't spawn" and as the
generator's dialogue looping). The original game enables 17649 only from `Obj15088_OnEnter`,
upstairs. Chapter 0's transitions resolve in the same zones as before (12112, 28598, 28694).

Earlier versions:
1. Respawning at the checkpoint whenever the player fell stopped the fall, but the
   checkpoint's spawn point is further into section 2, so it skipped the first part of the
   section (a checkpoint restart does the same).
2. Keeping the first zone with ground under the player (12474) without giving the player a
   zone: the ground was zone 12809's, seen through 12474's portal, so it disappeared when the
   camera turned away. Reported as "still go through the ground".

Tested by replaying the transition at (-831.2, -713.1, -60.1): ground seen from zone 12474,
owned by zone 12809; the player gets zone 12809 and stays at z = -60 while turning around,
looking up and down and walking to the objective. Walking straight ahead stops at the plaza's
wall. Still open: whether the original game assigns the zone some other way on this path (it
can't be compared with the Android original here; the same fall was seen on BlueStacks but not
on a phone).

## No aiming on the jeep (Price of Loyalty)

Reported as "in the last section, shooting from the top of the moving car, the camera can't be
moved". Chapter 2 (Price of Loyalty, `San_Francisco_2_FINAL`) ends with a ride on the back of a
jeep (object 92183, boarded with interact at the end of section 1, driven along paths by the
script through sections 2 and 3); chapter 6 ends with another. There the player is a
`CPlayerJeepComponent` in the gunner's seat. Its view is its own: yaw `+0x164` and pitch `+0x16c`
(radians), which `CPlayerJeepComponent::UpdateFromPhysics` turns by the touch-look delta its
embedded `CPlayerControl` (`+0x10`) receives, wraps, and holds within the script's limits
(`Vehicle_SetYawLimits`, the pitch limits) before placing the camera. The mouse and stick look
went to the floats behind the on-foot `CPlayerControl` after its `UpdateFromInput`, which does
not run on the jeep: the camera did not move. Fire worked (the shoot button feeds the jeep's
control too); the jeep's rocket, the scheme's second-shoot button, had no key.

Fix (`pc_hook_jeep_physics`): the look of the frame is added to `+0x164` / `+0x16c` before
`UpdateFromPhysics`, which applies its own wrap and limits (`GetYawDiff` turns the view by
-yaw, so a turn to the right adds; pitch as on foot). The aim key also holds the second-shoot
control while it is up, as the controller's aim trigger did (accessory slot 9): the jeep's and
the mech's rockets.

Checked by boarding the jeep at the end of section 1 (`--tour 1 --tour-stop 96263`, then
interact at the jeep) and riding into section 2: `--look` turned the view to the yaw limits
(-1.222 to -1.396 rad, the script's -70 to -80 degrees) and the pitch to +0.343 / -0.347 (20
degrees up and down, the limits); fire counted 8 shots (`CWeaponComponent::DoFire`), aim fired
one rocket.

## The jeep backs up and wedges itself (Price of Loyalty)

Reported as "in this section the car keeps getting stuck on things": in chapter 2's last
section, after checkpoint 96589, the jeep stopped in the gully at about (-876, 906, -90) and
never moved again. A restart from the checkpoint stopped it in the same place.

The scripted cars (`Vehicle_MoveOnPath`: the player's jeep 92183, the other jeeps and trucks
of the ride) follow their path in `CVehicleCar::updateMoveOnPath`, which first asks
`CVehicleCar::checkMoveOnPathCollisions(dt)` whether the car is blocked. A frame counts as
blocked when the car is slow (speed, physics `+0x70`, below 4) or pushed (the rigid body's
vector at `+0x238` longer than 1; only that during the grace time `+0x324`, 3000 ms set after a
manoeuvre). Each blocked frame adds one to a float (`+0x314`), any other frame clears it, and
above 3 the car is reported blocked: it switches to state 8 and backs up for up to 5000 ms
(`+0x328`). The timers are in milliseconds, but the counter counts **frames**. That is
133 ms of being blocked at a phone's 30 fps and 17 ms at 240 fps, so every bump, climb or
nudge from another car made the jeep reverse. In the log of a test run, the jeep drove to
(-875, 900), backed up 45 m, came forward again, climbed onto the rock on the gully's right
and stuck there.

Fix (`pc_hook_vehicle_path_blocked`, a replacement of `checkMoveOnPathCollisions`): the same
conditions, with the count advancing by the frame's time in 30 fps frames (`dt * 30 / 1000`)
and at most one per frame. At 30 fps and below it counts exactly as before.

Checked with a copy of the player's save (`Continue` on Price of Loyalty from checkpoint 96589):
at 240 fps the jeep stopped for good at (-876.0, 905.9, -89.6) before the fix; at 30 fps
(`max_fps = 30`) it drove through. With the fix at 240 fps it drove at a steady pace
through that place and the next two checkpoints (-1000, 1305) and (-1027, 1947), onto the
bridge (where the test, which does not shoot back, was killed).

The hook is shared by every port. The Switch (60 fps: blocked after 67 ms before) gets the same
time-based count. On the New 3DS (30 fps and below) the count stays one per frame, as before; the
replacement goes through the native hook table (`tools/hooktable.py`). Checked on both with
chapter 2's tour, which plays the jeep ride's checkpoints with the path cars driving: 32
checkpoints and no crash on the Switch build in Ryujinx and on the 3DS build in Azahar.

## Jetpack and jump next to objects

Reported as "the jetpack generally doesn't work". The jetpack is the special power `jetpack`
(chapter 9, `13_volteron_1`: `UnlockSpecialPower("jetpack")`; in multiplayer the armory's
jetpack): holding jump starts it after the power's delay (`CLevelInterfaceNormal::Update`
counts the hold in `+0xa4`, then `PlayerComponent::SetState(2)`), thrust lasts while jump is
held and burns the fuel (`PlayerComponent+0x398`, refilled on the ground). Holding Space did that
(measured: a full tank lifts the player about 14 units, the same at 30 and 240 fps), but not next
to an interactive object: there the game's controller layer stops mapping jump, and the player's
update interacts on the release of slot 0 instead (on the Android controller A did both). The
jetpack sections are full of consoles, and next to one Space used the console (or boarded the
jeep, in chapter 2) instead of jumping. On foot, next to an object, the host now presses the jump
control itself (see [Input](input.md#jump-next-to-objects)); the grenade key likewise throws a
grenade instead of picking a second option the object does not have.

Checked: in chapter 9 at the sphere puzzle's console with the jetpack unlocked, holding Space
raised the player from z 19.3 to 38 (state 2, fuel 100 to 17) without using the console; in a
local match with the armory's jetpack from -32 to -25.4 (a ceiling); next to the jeep, Space
jumped instead of boarding it.

## Other controls the keys did not reach

An audit of every mode with its own controls (vehicles, turrets, the mech, the hanging
sequence, puzzles, scopes, mines, co-op, multiplayer items) found more gaps, fixed the same way
(details in [Input](input.md#other-control-modes)):

- the hanging / zero-gravity sequence of chapter 3 keeps its own view (`pc_hook_limited_control`);
- the sphere puzzle's console in chapter 9 has four buttons of its own, which the controller
  layer gives to the D-pad; the keys had no way to press them (the puzzle blocks the way on);
  now the arrow keys and the D-pad do;
- a sniper scope's zoom is a touch slider: the wheel and next power / next grenade zoom now,
  and the look slows down with the zoom as touch did;
- mines are detonated with their own button: new `detonate` action (X, PadBack);
- multiplayer vehicles: entering takes the object's own seat buttons (interact = driver,
  grenade = gunner); with the pedal controls W / S and fire / aim work the pedals;
- the co-op heal button, and the multiplayer item sweep on next power.

## Lua script errors crashed the game

A crash about 14 minutes into the campaign was caught Lua errors returning through `longjmp`
into the wrong stack slots. See [Runtime](runtime.md#setjmp-longjmp-and-c-exceptions).

## Age gate

`SocialMgr::ShowInputAge` pushed an age prompt (`GS_InputAge`) whenever the age was unknown.
The hook answers it with `SocialMgr::ReceiveAge(18)`, what the prompt's Accept does. The value
was only used for ad targeting and is saved with the game settings.

## Progress lost on every restart

Reported as "it says progress saved, but when I restart the game it makes me redo the
campaign". Every save file (`Application::EncryptAndSave`) starts with the key and the
player's anonymous Gaia account ID, and `DecryptAndLoad` refuses a file whose ID differs from
the current one (`Gaia::GetCredentialDetails`). The ID (`android_v2_GloftNAHM_<time>_…`) is
generated once and kept in Gameloft's DataSharing store, a Java class the port answered with
"nothing stored" and forgot every write. So a new ID was generated from the clock at every
request, and no save could ever be read back:

- the title menu found no level to continue (`Application::GetSavedLevelName` → empty,
  `CMenuManager::CanContinueLevel` false);
- confirming the level therefore started a **new game**, which deletes the checkpoint
  (`Application::DeleteSaveGame`) and the campaign save.

DataSharing now keeps its values (`save\shared_prefs\data_sharing.txt`, see
[Runtime](runtime.md#fake-java)). Tested over two launches: the second reads the campaign
save, Continue is offered and the player starts at the checkpoint.

Saves written before the fix carry IDs that no longer exist and cannot be read; the first
launch with the fix starts the campaign over once. (Quitting a mission to the main menu keeps
the checkpoint, as on a phone; it only drops the upgrades in progress, `a6.dat`.)

## Easier campaign on the 3DS

The 3DS is played with the circle pad, slower than a mouse or a stick with the game's touch-era
tuning, so the campaign is made easier there (`host/hooks.cpp`; multiplayer, where the other side
are players, is left alone):

- **Enemy accuracy** x 0.4 (`kEnemyAccuracy`). `CNPCWeaponComponent::Fire` spreads a shot by
  `(1 - accuracy) x` the weapon's spread (accuracy at `+0xd0`, 0..1, set from the AI's
  `CNPCComponent::UpdateWeaponsAccuracy` and the NPC's data); the wrap fires with the scaled
  value and puts the original back.
- **Damage taken** x 0.5 (`kDamageTaken`). `PlayerComponent::ReceiveDamage` (already wrapped
  for the rumble) runs with the `CDamage`'s amount (`+0`: `CHealthComponent::ReceiveDamage`
  takes it from health) scaled, then restored.
- **Aim assist**: the window in which raising the sights turns the view onto a target is
  widened x 1.75 (`kAimSnap`; 40 x 30 degrees -> 70 x 52.5 in chapter 0). It is `+48` / `+52` of
  the weapon's auto-aim entry (`CWeaponManager::GetAutoAimCoefs`, 56 bytes from the level's data:
  `0.6 0.4 5 5 0 0 3 0 0.002 0 0.01 0.1 40 30`), which `CWeaponManager::SwitchAim` turns into
  radians. The other fields are not known well enough to change. The game's aim assist itself is
  on (`[controls] aim_assist`, the 3DS counts as a controller).

## Menus at 400x240 and other small windows

Windows (and the 3DS's screen) of the game's 480x320 class other than exactly 480x320, such as
400x240 or 480x288, showed the menus at twice their size and cropped: the menu camera was set
up for a 480x320 menu layer while the layer kept its 960x640 size. Those sizes now use the
960x640 class, like an 800x480 phone, fonts included. Cause, fix and checks:
[3DS port](3ds.md#menu-layout-at-400x240).

