# Debugging

## Event log

`host/eventlog.cpp` writes the game's level flow to `nova3.log` (next to `nova3.exe`). It is always on: about 1,000
lines per 1.5 minutes of campaign.

| Prefix | Content |
|---|---|
| `game:` | The game's own debug console (`glf::Console::PrintRaw`), silent on a release Android build: saves (`GAME SAVED SPAWNPOINT ID 15015`), `StartCinematic` / `EndCinematic`, damage (`HURT`), missing zone ground layers, asserts, script errors. Loading chatter (file probes, texture and shader setup, ~80% of the lines) is filtered out, and repeats are counted. |
| `script:` | Lua handlers the level starts: `CLuaScriptManager::StartFunction` / `StartFunctionOnThread` (numeric references named through `GetFuncRef`) and `CallFunction`. Trigger callbacks such as `Obj150034_OnEnter`, menu handlers such as `Obj3063_OnClick`. One line per name every 2 s at most. |
| `event: zone` | Every change of the player's zone (`CGameObject+0xe8`) or the view zone (`CZonesManager+0`), with the position. `player 0` in gameplay means the player has no zone (see [Gameplay fixes](gameplay-fixes.md#falling-through-the-world-after-a-section-load)). |
| `event:` | Checkpoint saved / loaded, campaign saved / loaded, level section saved, `LoadLevelSections` (with its arguments), level respawn, player spawned / respawned / checkpoint at a spawn point, teleports (zone, waypoint, `TeleportPlayerToZone`), cinematic start / end, each with the player position (`CLevel::GetPlayer`, `CGameObject+0x54`); the position every 5 s of gameplay. Before the player is placed it reads (0, 0, 1). |

Other log lines: `input: gameplay / menu mode`, `audio:` (format, underruns), `section load:`
(the zone search after a section load, and its result), `offline:` (refused store screens), crash reports.

The log is rewritten at every start, so copy it after reproducing a problem.

## Scripted test runs

Every fix here was checked by running the game with scripted input and screenshots. A run with
any scripted option never takes focus, ignores real input, uses the silent audio driver,
skips the launcher and is windowed at `--size` (default 1280x720).

**Use `-silent` and at least one scripted option** (e.g. `--screenshot`). A run with only
`--frames` counts as a normal start and opens the launcher on the desktop.

Use a copy of the player's folder (`--user build\…`) so tests never touch real saves.
`--user` and `--config` are made absolute when parsed: the game changes the working directory
to the game folder at start, and relative paths used to land next to it.

```bat
nova3.exe -silent --user build\test --tap 450:780,260 --tap 900:640,323 ^
          --hold 15000:Mouse1,300 --screenshot 15400:shot.png --frames 15410
```

| Option | Effect |
|---|---|
| `--frames N` | run N frames, then exit |
| `--screenshot FRAME:file.png` | PNG of that frame |
| `--tap FRAME:X,Y`, `--drag FRAME:X1,Y1,X2,Y2,N` | touch input (menus) |
| `--hold FRAME:KEY,N` | hold a binding name (`W`, `Space`, `Mouse1`, `Escape`, …) for N frames |
| `--look FRAME:DX,DY` | raw mouse motion |
| `--wheel FRAME:STEPS` | mouse wheel notches (+ = down) |
| `--type FRAME:text` | text entry |
| `--headless 1` | no window, null renderer |
| `--trace-events 1` | log every event raised on the game's event manager |
| `--dump-hud FRAME` | log the in-game interface buttons |
| `--peek FRAME:LINKADDR` | log a word of game memory |
| `--probe addr,...` | call functions with sentinel registers and report ABI violations |
| `--launcher-shot file.png` | render the launcher and exit |
| `--lua FRAME:code` | run a Lua chunk with the level's script manager (e.g. `LoadLevelSections(2, false)`) |
| `--place FRAME:x,y,z` | move the player |
| `--restart FRAME` | restart from the checkpoint (`CLevel::RestartLevel(true)`, as the pause menu does) |
| `--ray FRAME:x,y` | vertical physics ray from z = +200 to -400, logs the hit |
| `--zones FRAME` | log the level's zones (ID, bounds, portal count), the player's zone and the view zone |
| `--speed N` | game time runs N times faster (0.1–16): the frame time `UpdateApp` hands the game is scaled (`pc_hook_set_global_pitch`); cutscenes, scripts and waits too |
| `--tour CHAPTER` | play a campaign chapter through, checkpoint by checkpoint, then exit (0 done, 2 stuck); `--tour-shots DIR` a screenshot per checkpoint, `--tour-triggers object:handler:section,...` the section triggers (`tools/tourplan.py`) |
| `--tour-stop SPAWN` | with `--tour`: stop at the checkpoint of that spawn point and leave the game to the scripted options, whose frame numbers then count from the stop (`--frames` too); `--speed` ends there. It visits no other checkpoint: only the section triggers until the stop's room is loaded, then straight there (the checkpoints on the way run their scripts, and their list is not the story's order). A spawn a trigger saves (17268 in Lost Ark, `--tour 2`) is no stop: it comes without its section |
| `--tour-lua CODE` | with `--tour-stop`: Lua run just before the stop's checkpoint is saved, so restarting it keeps it; what the skipped part of the level would have done, e.g. `PlayerEnableRush(true) EnableRushControl(true)` (Lost Ark unlocks running in `Obj142242_WhileIn`) |
| `--play 1` | with `--tour-stop`: a person plays on from the stop: a normal window that takes the focus, the settings' display mode and size, real sound and input, 60 fps when the settings have no limit. With `--config <the player's nova3.ini>` their controls too |
| `--log-enemies EVERY` | from the stop: the enemies the scripts count (`GetNoEnemies`), each with its ID, position, zone, health and dead / targetable, every EVERY frames and whenever the count changes |
| `--log-player FRAME[,EVERY,COUNT]` | log the player: state (`PlayerComponent+0x130`: 1 in the air, 2 jetpack), jetpack fuel, view angles, the interactive object in reach, shots fired, the current weapon and special power, the HUD buttons up (leave, detonate, heal, zoom, seats, pedals) and, on a vehicle, its view, fire flags, seat and control type |

Useful sequences at 1280x720 (frame numbers at ~240 fps):

- Local Wi-Fi match: `--tap 800:780,525 --tap 1300:785,517 --tap 2000:640,243
  --tap 2600:895,575`. Gameplay from about frame 3400.
- Campaign, first level: `--tap 450:780,260 --tap 900:640,323`. The intro cinematics run until
  about frame 13,000. The first slow-motion shot comes at about 14,000, the ability tutorial at
  about 19,500, the look tutorial at about 25,500, the jump tutorial at about 30,000 and the
  move tutorial at about 36,000.
- Replay the section-1 → 2 transition: add `--place 13000:-822.7,-713.1,-60.1
  --lua "13010:LoadLevelSections(2, false)"`.
- Play a place in a chapter: `--user build/zk_tour --config "<Saved Games>/N.O.V.A. 3/nova3.ini"
  --tour 2 --tour-triggers 17269:OnEnter:2,142294:OnEnter:3 --tour-stop 10140 --tour-lua
  "PlayerEnableRush(true) EnableRushControl(true)" --play 1 --log-enemies 600` (chapter 3,
  Lost Ark: the generator room).
- A place in a chapter: `--speed 4 --tour 1 --tour-stop 96263` (chapter 2, at the jeep), then
  e.g. `--lua "60:SwitchToPlayerOnFoot() local x,y,z = Object_GetPosition(92183)
  PlayerSetPosition(x+3,y,z+1)" --hold 300:E,30 --log-player 1400,50,20 --look 1600:-1500,0`
  (boards the jeep and turns its view). `Object_GetPosition` of an object outside the loaded
  rooms crashes the game (a null object in the game's own function).

Environment variables: `NOVA3_REAL_AUDIO=1` (real audio device in tests),
`NOVA3_AUDIO_DUMP=file.pcm` (record the mixed output), `NOVA3_DISCORD_TEST=1` (connect to
Discord in test runs; they only log the activity otherwise).

## Performance in the log

Every log (PC `nova3.log`, Switch `sdmc:/switch/nova3/nova3.log`) reports performance as it
plays, for diagnosing a machine from its log alone:

- `perf: frame N: …` (every 300 frames on the PC, 600 on the Switch): frame rate; average and
  worst frame time; late frames (over 20 / 34 ms); the game thread's **step** split into `GL`
  (time inside GL calls: the driver), `file I/O`, `waits` (locks, condition variables, sleeps,
  socket polls), `alloc` (malloc / new / free) and `game code` (the rest: the translated game
  and the runtime); the **swap** (presenting the frame: waiting for the GPU or VSync); the
  game thread's CPU load (near 100% = its core is saturated, CPU-bound) and, on the PC, the
  whole process's.
- `gl per frame: …`: draw calls (and how many draw from client memory), program, texture and
  enable/disable calls with the share that repeats the current state, uniform calls and the
  other state calls with the share skipped because they would not change anything
  (docs/runtime.md, Graphics).
- `slow frame: …`: each frame over 50 ms (at most 10 per window) with its own split: a
  loading hitch shows as file I/O, a driver stall as GL, a burst of game work as game code.
- `profile: …` (PC, every 30 s): the game thread sampled ~500 times a second; time by module
  (game code, runtime, ANGLE, driver) and the hottest functions (`f_<address>` = a game
  function: `python tools/disasm.py 0x<address> 1` names it). `NOVA3_PROFILE=0` turns it off.
- `gl resources in N frames: …`: texture and buffer uploads (with their size) and deletions.
  Uploads happen while loading; in play the game streams nothing.
- `memory: …`: Java objects alive in the fake JNI, guest threads running and ended but not
  yet joined; on the Switch also the heap in use (`mallinfo`: Mesa's GPU buffers and the
  thread stacks live there). `guest memory: … committed` (Switch) as it grows.
- `NOVA3_ALLOC_LOG=1` (Switch; `env NOVA3_ALLOC_LOG 1` in `test.txt`): every heap block of
  1 MB or more, allocated and freed, with its caller (`alloc: aligned_alloc … from
  elf+<offset>`) for `aarch64-none-elf-addr2line -e build/switch/nova3.elf`. It found the
  thread-stack leak (docs/switch.md, Memory).
- `audio: underrun N (X ms in all)`: times the output device ran out of sound.
- New 3DS: `host heap: <what> at <call>: block ..., called from ...` stops the game when a host
  free, realloc or allocation meets a damaged or foreign block (with the chunk and the one
  before it in hex: `arm-none-eabi-addr2line -e nova3.elf` on the caller); `env NOVA3_HEAP_CHECK
  1` walks newlib's whole heap after every boot and load frame and at each thread's start and
  end ([New 3DS: Memory layout](3ds.md)). `memory: ... host heap: N allocations and frees`
  counts them per 300 frames.
- `thread: a lookup of X, which holds Y: not its handle`: a guest mutex / cond / sem word that
  looks like a handle but was not installed there (uninitialised or copied memory); the first
  eight are logged ([Runtime](runtime.md)).

### Memory profiles (heap and working set)

PC (Windows) only, for sizing ports with little memory ([GameCube](gamecube.md)); each slows
the game, so they are for measurement runs, not play.

- `NOVA3_HEAP_PROFILE=N` (`runtime/heap.cpp`): every N frames, the guest heap's live bytes by
  the guest call stack that allocated them (`heap profile: frame F, X MB live in N blocks`, then
  one line per stack with at least 64 KB: MB, blocks, its innermost six guest functions). The
  host stack of each allocation is captured (`os::capture_stack`) and its recompiled frames
  named as guest symbols (`os::host_symbol`, `rt::guest_symbol`). The innermost name is often
  the exported symbol nearest the game's allocator rather than the allocator itself.
- `NOVA3_WORKING_SET=N` (`runtime/os_win.cpp`): every frame all committed guest pages lose
  their access (`PAGE_NOACCESS`); a touch faults, gets its access back and records the frame.
  Every N frames: `working set: frame F <region> last 1/10/60/N frames: ... MB (touched ever
  ... MB)` for the library's image, the heap and the rest, and the faults a frame. While it is
  on, host file reads and writes go through a host buffer (the kernel cannot fault a page in
  for them).
- `NOVA3_HEAP_DUMP=F` (`runtime/heap.cpp`): the heap's bytes at frame F into `heap_F.bin` in
  the working directory (compressibility of the heap's pages).

On the GameCube, the host heap by allocation site: `-DNOVA3_GC_MPROF=ON`
(`runtime/gc_mprof.c`, see [GameCube](gamecube.md#building-and-testing)).

## Analysis tools

- `tools/disasm.py`, `tools/xrefs.py`, `build/analysis/gotrefs.py` (see [Recompiler](recompiler.md#tools)).
- `tools/obb.py extract` to look inside the game's data (e.g. the menu level's Lua).
- `tools/tour.py [chapters]` runs `--tour` on every chapter (with its section triggers from
  `tools/tourplan.py`) and prints a report: checkpoints, sections, deaths, crashes, hangs,
  section-load fallbacks; `tools/tour.py --mp [maps]` hosts a local match on each multiplayer
  map; `NOVA3_TOUR_ARGS` adds arguments to every run (e.g. `--obb DIR`). Results in
  `build/tours/`. One game at a time by default: each takes several GB. How the tour moves on: the checkpoint chain with the game's own
  `CLevel::SkipToNextCheckpoint` (the cheats page), then unvisited spawn points of the loaded
  rooms (`FindSpawnPointInRooms`), then the next section's trigger volume: the player is put
  there, the trigger enabled and its handler started as a script coroutine
  (`CLuaScriptManager::GetFuncRef` / `StartFunction`), so the game loads the section itself.
  Loading a section by hand (`LoadLevelSections`) with the player elsewhere unloads the zone the
  camera is in and crashes the renderer (`CZone::SetVisible`); that never happens in play.
  After a trigger the tour waits for the section load it starts: the handler's
  `LoadLevelSections` call (counted from before the handler starts: it may load at once) and a
  frame out of gameplay or 3 s since, at most 20 s. Taking the next frame for the new section
  ended chapter 0 at its section 4 ("done" after 29 of 56 checkpoints: no trigger 91880 in the
  section 3 rooms).
  The results screen (`GS_EndLevel`) counts as the chapter's end. Test profile `build/zk_tour`
  (HD textures and VSync off), 960x540, `--speed 4`; a chapter stuck at that speed is run again
  at 1 (the last chapter has a timed sequence the idle player fails when faster).

  **Result (2026-10-09):** every chapter played through, no crash, no hang:

  | Chapter | Level | Checkpoints | Sections | Notes |
  |---|---|---|---|---|
  | 1 | San_Francisco_1_FINAL | 56 | 5 | |
  | 2 | San_Francisco_2_FINAL | 32 | 3 | |
  | 3 | Ship_01 | 56 | 3 | |
  | 4 | therrius_2_prototype_Ciprian | 29 | 3 | ends on the results screen |
  | 5 | Boreas_1 | 11 | 1 | killed once by enemies while idle (at 4x), respawned |
  | 6 | Boreas_2 | 18 | 1 | |
  | 7 | 08_volteron2 | 33 | 2 | ends on the results screen |
  | 8 | 09_volteron_3 | 31 | 3 | |
  | 9 | 13_volteron_1 | 16 | 1 | |
  | 10 | 14_volteron_1 | 17 | 1 | at normal speed (fails its timed sequence at 4x) |

  `tools/tourplan.py` missed the triggers written `LoadLevelSections (2)` (with a space), so
  chapters 6 and 9 were toured in their first section only; it finds them since 2026-10-10.

  The section-load fix fell back to the checkpoint 6 times (no ground under the restored
  position: the tour drops the player at a trigger's origin), always recovering. All 7
  multiplayer maps (`GetMPLevelsCount`) loaded and played in a hosted local match; indices past
  the list crash in `CLevel::LoadStep`, but no menu offers them. Screenshots of every checkpoint
  were checked for blank or uniform frames: the 8 darkest are dark places (garage, tunnels).
- `NOVA3_TRACE_FILES=1` logs every file the game opens or looks for (`file: fopen <guest path>
  -> <host path>`), e.g. where it expects a missing file.
- `NOVA3_TRACE_READS=1` logs every read of a file (`read: <host path> <offset> <bytes>`);
  `tools/3ds_slimdata.py measure` sums them per archive member and kind (docs/3ds.md, Slim data).
- `NOVA3_TRACE_NET=1` logs every datagram sent and received (`net: sendto fd 10 -> 45,
  192.168.0.2:7892`: size or -1, address); on the consoles `env NOVA3_TRACE_NET 1` in
  `test.txt`. Without it the log still shows each socket call's first failure per error
  (`net: sendto 255.255.255.255 failed: host error 13`, host errno values).
- `tools/truncscan.py` lists the integer counters fed by a truncated float product
  (`counter += int(dt * rate)`), the time loss at high frame rates.
- Crash reports name the game function and instruction (see [Runtime](runtime.md#crash-reports)).
- `llvm-objdump` on `build\cmake_full\CMakeFiles\nova3_core.dir\build\gen\code_XXXX.c.obj`
  shows the x86 code of a translated function, used to find the setjmp frame bug.

## How the harder bugs were found

- **Lua crash** (14 minutes in): the crash report said "last import called: longjmp", at a
  return address inside Lua's protected-call code. A probe that called `lua_pcall` on a failing
  chunk crashed every time. Disassembling the compiled function showed the frame pointer
  restored by `__builtin_longjmp` at the wrong offset.
- **Tutorial hang**: holding fire did nothing (ammo stayed 30/180) while a touch tap worked.
  `--trace-events` showed the touch button's events. Logging the fire flags showed them
  cleared every frame. The QTE state (`CQTEManager::Singleton+4`) showed which QTE waited for
  what. Logging the frame time during slow motion revealed the 0 ms frames.
- **Section fall**: the event log showed the transition, then the position sinking. `--place`
  and `--lua` replayed it on demand. Rays per zone showed where the floor lives. The zone log
  showed the player had no zone. The zone search's rays first hit the held player's own body;
  the game's interact filter excludes it. The first zone search still let the player fall
  later: `--look` (turning around after the load) reproduced it, and the ray's hit object
  named the zone that really owns the floor.
- **Menu cleanup**: handler names from the event log and the menu level's Lua mapped icons to
  object ids. A temporary dump of a screen's children (ids, classes, flags) and of the modules
  its layout sprite paints (module, x, y) located the currency bars.
