# Open issues

## Known problems

- **Truncated timers at high frame rates**: slow motion and cinematics are fixed (see
  [Gameplay fixes](gameplay-fixes.md#slow-motion-at-high-frame-rates)). The bike's engine RPM
  (`CPlayerBikeComponent::ComputeRPMAndGear`) has the same pattern and is untested. Code that
  counts frames instead of milliseconds does not show up in that scan; one such counter, the
  blocked check of path-driven cars, was found by play and fixed (see
  [Gameplay fixes](gameplay-fixes.md#the-jeep-backs-up-and-wedges-itself-price-of-loyalty)). A `max_fps` around
  240 is the tested range; uncapped rates (~1000 fps in menus) are untested in gameplay.
- **Saves from before the DataSharing fix** cannot be read (their account IDs were random);
  the campaign starts over once.
- **Section transitions**: the player is kept where they stood by searching the zones for
  ground and giving the player the zone that owns it (see
  [Gameplay fixes](gameplay-fixes.md#falling-through-the-world-after-a-section-load)). Only
  the first level's basement transition has been tested, by replay. Other levels'
  transitions may need the checkpoint fallback.
- **Space held through the end of a cutscene** may click the weapon widget once on release.
- **Rebinding**: only the first control per device is shown; the Controller page and the rate
  popup removal are untested with a real controller / at a real level end.

- **New 3DS console** ([3DS](3ds.md#console-status-2026-10-12-new-3ds-with-luma3ds-the-sd-card-image)):
  the game runs and draws right. Built and checked in Azahar but not yet on the console: the
  floors' and hands' depth fighting (float24 precision), the HOME Menu crash, the easier
  campaign and aim snap, the Controls page, the shorter sound queue. Open: the pause when
  changing weapons (needs a console log), the CPU cost of the per-draw index rewrite (chapter 3
  in Azahar 31.8 -> about 26-28 fps).
- **Builds not run since the last changes**: the Switch NSP (the pause menu's Military Shop
  button, the online services removed, the counters, the section-load and fallen-enemy fixes)
  was built, not played in Ryujinx or on a console.

## Untested

- Gameplay with a physical gamepad.
- Grenade, melee and interact keys (context buttons), next power and next grenade type (need
  two or more unlocked).
- Move QTEs (`CQTEMove`), the jeep, mech, iron-sight and detonate tutorials.
- End-of-match and end-of-mission screens after the offline cleanup.
- Linux after the first WSL test.
- The migration of an existing `nova3.ini` and saves into Saved Games on a real install (tested
  in a test folder).

## Nintendo Switch

Tested in Ryujinx (1.2.86, the player's keys and firmware 19.0.0) and Citron: the campaign's
ten chapters played through with the tour (the PC's checkpoint counts), the seven
multiplayer maps hosted locally, menus driven by scripted controller buttons, 60 fps in the
emulator, no audio drops. Open:

- **Real hardware** (handheld, from players' logs): 52-56 fps in play, the game thread's
  core 88-97% busy (step 18-19 ms: 13-15 of game code, 4 of GL), so the 60 fps cap is not
  held yet; loading is dominated by file reads and shader compiles. About 11 minutes of play
  (34,000 frames) ran the heap out: the thread-stack leak (docs/switch.md, Memory; fixed).
- **The physics thread race** (fixed): the game steps Bullet's world on a thread of its own
  (`CPhysicsManager::EnablePhysicsThread`, from the device profile) while the game thread
  ray tests and adds / removes collision objects in the same world; on truly parallel cores
  that hit shapes being torn down in a few percent of multiplayer loads. The physics now
  runs on the game thread (`pc_hook_use_physics_thread`, `NOVA3_PHYSICS_THREAD=1` restores
  the thread).
- **JNI local references** are never freed (`DeleteLocalRef` is a no-op): the web services
  leave a few small strings per request (about a megabyte over hours of play).
- **Controllers** were driven through the pad state by the test script; the libnx reading
  itself (`poll_switch` in `host/input.cpp`), the touchscreen and the system keyboard have
  not been exercised.
- **Local multiplayer** between consoles is untested (one emulator instance at a time).

## Performance

Not measured yet. Gameplay holds 236–240 fps with VSync at 240 Hz (1280x720 and 3840x2160);
memory was 0.5–1 GB in the task manager. The costs to expect:
- explicit flag math and bookkeeping at every call and return in the translated code;
- the C library `setjmp` on entry to functions that catch exceptions;
- ANGLE's GLES → Direct3D 11 translation.

A useful benchmark: uncapped fps in a menu, a match and a campaign scene, plus a CPU profile
split between game code, runtime and ANGLE / driver.

## Ideas

- **More decompilation**: replace translated functions with readable C++, starting with the
  platform layer, input, camera, timing and renderer (`OVERRIDES` is the mechanism).
- **GameCube**: in progress (the title menu runs in Dolphin); the design, its measurements and
  the status are in [GameCube](gamecube.md).
- **PS Vita**: the Vita's CPU is ARMv7 like the original library, so a port there would load
  the Android `.so` directly ("so-loader", as other Android ports to the Vita do) rather than
  use this recompiler. The 4 GiB guest window cannot exist on a 32-bit system. The findings
  (boot sequence, hooks by symbol and offset, controller slots, offline cleanup, fixes) carry
  over. The main risk is memory: 512 MB of RAM. Measuring the game's peak heap here would tell
  whether it fits.
