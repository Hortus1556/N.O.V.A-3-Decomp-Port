# Recompiler

The game ships as one ARM library, `lib/armeabi-v7a/libGloftNAHM.so`, inside the APK.
`recomp/arm2c.py` translates every function in it into C ahead of time. The C compiles
(clang-cl) into a static library, `nova3_core.lib`, that runs natively on x86-64. There is no
emulator or JIT at run time.

## The binary

- 20.6 MB of `.text`, built with GCC 4.6 / 4.9, almost entirely **ARM mode** (only 72 Thumb
  functions), **not stripped**: 41k dynamic symbols, about 35k of them mangled C++ names.
  Every function quoted in these docs has its real name.
- Engine: Gameloft **Glitch** (an Irrlicht fork), **GLES 2.0 only**. Also linked in: Bullet
  physics, Lua 5.1, OpenSSL, libcurl, libpng / libjpeg, stb_vorbis, the **Vox** audio engine,
  rapidjson, and the glwebtools / GLSocialLib online stack.
- 426 imports: libc / libm / pthread, GLES2 / EGL, OpenSL ES, Android sensors and looper,
  sockets.
- About 80 JNI entry points. On Android, Java drives the main loop through
  `GL2JNILib_{init, initGL, resize, step, touchEvent, OnKeyDown, OnKeyUp, ...}`.
- Remnants of controller support: `NativeBridgeHIDControllers_*`, `Controller`, `JoystickBase`,
  and the `CAccessory` input layer (used for the PC controls, see [Input](input.md)).
- Other hook points spotted for PC features: `CLevelInterfaceNormal::UpdateCameraSensitivity`,
  `CWeaponComponent::GetAimSensitivityFactor`, `Menu_SetAimAssist`, `Menu_SetResolutionType`,
  `Application::GetMaxUsedResolution`, `glf::App::GetScreenSize`.
- No relocations touch `.text`, so literal-pool constants are folded at translation time.
  45k `R_ARM_RELATIVE` pointers into `.text` (vtables, callbacks) become dispatch entries.

## Game data

- **OBB container** (`CCustomPakReader`): big-endian u32 header `file_count, data_start,
  table_end, entry_count`, then `entry_count × (offset, size, name_offset, flags)`. Names in
  `[table_end, data_start)` are encrypted with `DECODE_XOR32(seed = 0x3857A)`, an LCG keystream
  (`s = s * 0x19660D + 0x3C6EF35F`, `rand(n) = (s >> 16) % n`): each 32-bit word is rotated by a
  random amount in a random direction and XORed with a random key; the 1–3 byte tail uses the
  same scheme at reduced width. `flags & 1` would encrypt the body the same way (no shipped
  file uses it). `patch.obb` overrides `main.obb`. `tools/obb.py` lists and extracts them.
- **Assets**: `.gla` files are zip archives of `.etc` (ETC1 textures), `.bdae` (Gameloft
  binary COLLADA), `.col`, `.irr`, `.glsl`, `.bsprite`, `.bin`, `.xml`, `.ttf`, Lua-carrying
  level files (`.lvc`). `*_stream.gla` and `sounds_*.gla` use their own formats (not
  reversed; not needed).
- **Menus** are a level of their own, `levels.gla/menus.lvc`: menu objects with embedded Lua
  handlers (see [Offline game](offline.md)).

## Translation

- Coverage: 40,083 ARM functions, 188 Thumb functions and about 3,700 mid-function entry
  points. The output compiles with zero errors. About 2,200 reachable instructions are
  unsupported: mostly NEON, and data words decoded as code after calls that never return.
- **Function discovery**: symbols, the unwind table, data pointers (even values), `bl`
  targets, and `blx` targets found in reachable ARM code. Unwind-table starts that begin with
  a Thumb `push` and no unconditional ARM instruction are classified as Thumb.
- **Thumb** code (Boost.Thread, libgcc `__gnu_thumb1_case_*` switch helpers) is plain Thumb-1.
  Each instruction is rewritten into its A32 equivalent and goes through the ARM generator;
  Thumb functions are dispatched at their odd addresses. Their `blx` calls into ARM code can
  land on the linker's long-branch veneers after an ARM function's last instruction
  (`ldr ip, [pc]; add pc, pc, ip`): Thumb code is lowered before the mid-entry fixed point,
  so those become entries of that function like any other (they once ran the function from
  its start, so two Boost static constructors called `jinit_2pass_quantizer` instead).
- **Model**: registers live in C locals; the `Cpu` struct holds the architectural state at
  function boundaries. Flags are locals, so unused flag math is dead-code eliminated. Memory is
  a flat 4 GiB reservation; each function keeps its base in a local (`gm`, set by `PROLOGUE`):
  the global would be reloaded after every guest store. `ldrex` / `strex` become host
  compare-and-swap, `dmb` a sequentially consistent fence.
- **Call-boundary analysis** (`Flow` in arm2c.py): instead of storing and reloading all 15
  registers and 4 flags around every call and return, an interprocedural fixed point over the
  call graph computes, per entry point, the registers it may change (push / pop and stack
  slot tracking: a function that saves and restores r4-r11 leaves them unchanged), the
  registers it reads at entry (pushes count as reads, so C++ exception unwinding and
  `longjmp`, which restore saved registers from the stack, see correct values), the flags its
  callers read after it (only ~50 libgcc-style helpers return flags), and whether it may
  return elsewhere than its `lr` (code that pops its caller's frame: shared epilogues, cold
  blocks, libgcc). A call then stores only what the callee reads and the caller changed since
  the struct last held it, reloads only what the callee may change; prologues load only what
  is read before written; returns store only what is out of date (the `_X` macros in
  recomp.h). Measured: 6.0 stores / 8.6 loads per direct call, 5.5 stores per return, 6.4
  loads per prologue (all were 19). Imports, host hooks, `setjmp`, landing pads and Thumb
  code stay conservative (store / reload everything); indirect calls store everything and
  reload what AAPCS lets a callee change (see below). Stack values are only
  trusted while nothing could have written them (calls and stores through unplaceable
  pointers invalidate all but push-saved slots: assuming otherwise once let a list splice
  into itself). `NOVA3_X_ONLY=lo-hi` limits the optimized emission to address ranges, for
  bisecting a mistranslation.
- **Inlined helpers** (`INLINE_HELPERS`): calls to the ARM EABI division routines
  (`__aeabi_uidiv`, `__aeabi_idivmod`, `__aeabi_uldivmod`, …; ARMv7 phones had no divide
  instruction) become C division at the call site.
- **Inlined imports** (`INLINE_IMPORTS`): calls to side-effect-free libc imports (`memcpy`,
  `memmove`, `memset`, `memcmp`, `strlen`, `strcmp`, `strncmp`, `strcpy`, `strchr`, …) and the
  float / double math functions (`sinf`, `cosf`, `atan2f`, `powf`, `sin`, `pow`, …) become the
  same host calls as the runtime's implementations, at the call site (recomp.h `GR_` / `GW_`
  / `IM_` helpers): no import table, and only r0 (r0:r1 for doubles) changes, which AAPCS
  allows. Constant arguments are hidden from the compiler (`IM_OPAQUE`), so a call is never
  folded at build time with rounding different from the host library's. `strstr` and
  `isalnum` are inlined too. So are the game's most frequent thread calls, about 1,000 per
  busy frame: `pthread_getspecific`, `pthread_self` and mutex lock / unlock. They become direct
  calls to the runtime (`rt_im_*`, runtime/thread.cpp), which need no guest registers. A lock
  that has to wait is still timed for the log's "waits".
- **Plain imports** (`plain_import`): GL, the allocator, condition variables and `clock` are
  host code that never runs guest code. Around them only the arguments (r0-r3, sp, lr) are
  stored and r0-r3 / r12 reloaded: 6 stores and 5 loads for a `glUniform4fv` instead of 15 and
  15. Other imports (callbacks into the game, exceptions, `setjmp`) still sync every register.
- **Indirect calls** go through a cache per call site (recomp.h `RT_ICALL`): the last target's
  guest address and table index in one 64-bit word, read with one atomic load; a hit calls the
  translated function directly, a miss looks the target up (`rt_dispatch_ic`) and refills it.
  After the call only r0-r3, r12, lr and the flags are reloaded: the union of what the
  pointer-reachable entries may change, with r4-r11 / sp kept (AAPCS, which the value
  analysis already assumes there). The few pointer-reachable entries whose summary does not
  show that (Thumb code, which is not analysed, and functions whose stack accesses defeat the
  analysis; 5 in this binary) sit in the dispatch table behind `rt_aapcs_checked`, which
  traps if one does change r4-r11 or sp.
- **Native hot paths** (`host/native_hot.cpp`, `OVERRIDES`): the hottest game functions
  rewritten in C++ with SIMD (SSE2 / NEON): skinning (`CSoftwareSkinTechnique::skin`, whose
  NEON branch the wrapper enables, with native kernels for the `neonSkin*` routines), the
  particle update and its color / size / direction animations, the TTF glyph map lookups.
  Each is bit-exact against the recompiled original: `NOVA3_NATIVE_CHECK=all` runs both on the
  same inputs in game and logs differences (millions of calls, none);
  `NOVA3_NATIVE_OFF=…` falls back to the originals. They use the generated code's float rules
  (no contraction).
- Switch idioms (`addls pc, pc, rX, lsl #2`, `ldrls pc, [pc, rX, lsl #2]`) become C `switch`.
- **NEON** appears only in Vox audio assembly and Glitch `neonSkin*`. Both are chosen at run
  time through Android `cpu-features` reading `/proc/cpuinfo`; the runtime presents a CPU
  without NEON, so the game uses its C fallbacks (except skinning: see native hot paths).
- **Return tracking**: every translated return records its real target in `cpu->ret_pc`, and
  call sites check it, so a callee that pops its caller's frame (libgcc soft-float does this)
  still unwinds correctly. Only where it can happen: a direct call to an analysed entry that
  always returns to its `lr` (the analysis's "may return elsewhere" is false) has no check
  (`CALLN_X`): 271,754 of 272,674 direct call sites. Indirect calls, hooks, Thumb code and the
  920 calls to entries that may return elsewhere keep it.
- `setjmp` is expanded inline in the caller's own C frame (see [Runtime](runtime.md)).
- Output files cover fixed 512 KiB address windows (`build/gen/code_XXXX.c`) and are only
  rewritten when they change, so most fixes rebuild in seconds. A full regeneration takes
  about 5 minutes (the analysis) and 4.7 GB; a change to `runtime/recomp.h` recompiles
  everything (PC ~3 minutes at 5 jobs; the Switch ~45 minutes at 2 jobs: GCC takes ~3 GB per
  generated file).

## Profile-guided optimisation

The generated code and the runtime (`nova3_core`) can be built with a profile of real runs:
`-DNOVA3_PGO=generate` builds an instrumented game, `-DNOVA3_PGO=use` optimises with the
merged profile. An instrumented game writes its profile when the process ends
(`runtime/pgo.cpp`, called by `os::exit_process`, which skips atexit handlers). Profile the
build that will be optimised: the same generated code, and on the Switch the same build
directory (GCC names each profile after the object's path relative to it).

PC (clang-cl):

```
cmake -S . -B build/cmake_pgo -G Ninja ... -DNOVA3_PGO=generate -DNOVA3_PGO_DIR=E:/nova3/build/pgo
ninja -C build/cmake_pgo nova3            # runs write build/pgo/nova3-<pid>.profraw
(play: tools/tour.py chapters, bench.sh runs, with this exe)
"C:/Program Files/LLVM/bin/llvm-profdata" merge -o build/pgo/nova3.profdata build/pgo/*.profraw
cmake -B build/cmake_pgo -DNOVA3_PGO=use  # same NOVA3_PGO_DIR
ninja -C build/cmake_pgo nova3
```

Switch (devkitA64 GCC):

```
aarch64-none-elf-cmake -S . -B build/switch_pgo ... -DNOVA3_PGO=generate
ninja -C build/switch_pgo nova3_nro       # runs write sdmc:/switch/nova3/pgo/run-<n>/*.gcda
(play in Citron / on the console: build/swtour.py chapters)
aarch64-none-elf-gcov-tool merge run-0 run-1 -o merged   # again for more runs
cmake -B build/switch_pgo -DNOVA3_PGO=use -DNOVA3_PGO_DIR=<dir with the merged .gcda files>
ninja -C build/switch_pgo nova3_nro nova3_exefs
```

`-fprofile-info-section` makes the instrumented objects list their counters in a section that
`runtime/pgo.cpp` writes out itself (`__gcov_info_to_gcda`), one file per object, under a new
`run-<n>` directory per run; `NOVA3_PGO_OUT` (default `sdmc:/switch/nova3/pgo`) changes the
place. Functions a run never reached keep their normal optimisation
(`-fprofile-partial-training`; clang leaves functions without a profile alone).

Measured on the PC (bench, 6000 frames, profile from 3000-frame benches of chapters 0, 3 and
6): game code per frame 0.617 -> 0.549 ms on chapter 6 (-11%), 0.886 -> 0.790 ms on chapter 8,
which was not in the profile (-11%); the inlined imports and cached indirect calls alone
give -3% / -6%.

The shipped PC build (`build/cmake_full`, which `tools/package.py` packs) is a `use` build:
configure it with `-DNOVA3_PGO=use -DNOVA3_PGO_DIR=E:/nova3/build/pgo_pc2`, the profile of an
instrumented build of the same generated code (`build/cmake_pgo`, `-DNOVA3_PGO=generate
-DNOVA3_PGO_DIR=E:/nova3/build/pgo_pc2`, then `--bench 0:3000:walk`, `3:3000:walk` and
`6:3000:walk` with it, `llvm-profdata merge`). Regenerate the profile after the generated code
changes: clang drops the profile of every function whose code changed. (Until 2026-10-10 the
package was built without PGO.)

## Verification

Differential tests against Unicorn run each instruction from random register, flag, VFP and
memory states (`tests/difftest.py`):

- ARM: 122,894 of 122,896 cases match across 14,002 distinct instructions. The two misses are
  UNPREDICTABLE `*mull` encodings with RdHi == RdLo.
- Thumb: 7,471 of 7,471 cases across 921 distinct instructions.
- VFP, including fixed-point `vcvt` and fused `vfma`: all match. Generated code is built with
  `-ffp-contract=off` so `vmla` stays unfused, as on ARM.
- `tests/xor32_test.cpp`: the game's own `DECODE_XOR32`, translated, decodes the OBB names.

## Hooks: `OVERRIDES`

`OVERRIDES` in `recomp/arm2c.py` changes how a game function is translated:

- `'symbol': 'host_fn'` replaces the function with `void host_fn(Cpu *)`. Arguments are in
  `cpu->r[0..3]` (softfp ABI: floats and doubles travel in core registers), the result goes
  in `cpu->r[0]`; the generated wrapper returns to the caller.
- `'symbol': ('wrap', 'host_fn')` keeps the original and calls
  `void host_fn(Cpu *, uint32_t entry, GuestFn original)` instead; the host function decides
  whether and when to call `original(cpu, entry)`. To return without calling it, set
  `cpu->ret_pc = cpu->r[14]`.
- Lists such as `SOCIAL_DISABLED` and `OFFLINE_STUBS` map many functions to one do-nothing stub
  (`pc_disabled_service`, which returns 0; for a Lua C function that means "no results").

Rules learned the hard way:

- `rt::call` (calling back into the game) clobbers `r0`–`r3` and `r14`. Before the original
  runs, restore them, or use `rt::call_nested`, which saves and restores the whole CPU state.
- Some calls are devirtualised or inlined by the original compiler. `CQTE::Draw` is inlined
  into `CQTEManager::Draw`, so the outer function must be wrapped.
- Function-level hooks cannot patch the middle of a function. When a value computed inside
  one must change, look for the next call it makes and hook that, checking the caller by
  return address (`cpu->r[14] == function + offset`). The slow-motion fix does this through
  `VoxSoundManager::SetGlobalPitch` (see [Gameplay fixes](gameplay-fixes.md)).

After changing `OVERRIDES`: `python recomp/arm2c.py gen build/gen`, then
`ninja.cmd build\cmake_full nova3`.

The current overrides, by area:

| Area | Functions | Host side |
|---|---|---|
| C++ exceptions | `restore_core_regs` | `runtime/eh.cpp` |
| Controls | `EventManager::raiseSync` (trace), `CPlayerControl::UpdateFromInput`, `CPlayerControl::EnablePlayerControl`, `GS_InGame::Update`, `PlayerComponent::ComputeControlVelocity`, `CPlayerJeepComponent::UpdateFromPhysics` (look on a vehicle), `CPlayerLimitedControlComponent::Update` (look while hanging) | `host/hooks.cpp`, [Input](input.md) |
| Laser puzzle | `GS_FaneMinigame::Update` (cursor and keys), `GS_FaneMinigame::RenderBall` (the cursor's frame) | `host/hooks.cpp`, [Input](input.md#other-control-modes) |
| Touch UI hidden | every `ControlScheme*::Draw`, `CQTEManager::Draw`, tutorial `Draw`s, `CHud::SetButtonState` (pause button), `CLevelInterfaceNormal::Draw` (the touch marker on crystal pods and the arm's console) | `host/hooks.cpp` |
| Fixes | `VoxSoundManager::SetGlobalPitch` (slow motion), `CCinematicComponent::GetCinematicSpeed` (cinematic clock), `CVehicleCar::checkMoveOnPathCollisions` (path cars backing up), `CLevel::UpdateMCSavedData` (section fall), `CCameraSceneNode::setFOV` / `getFOV`, `SocialMgr::ShowInputAge` | `host/hooks.cpp`, [Gameplay fixes](gameplay-fixes.md) |
| Controls page | `Menu_GoToMenu`, `GS_InGameMenu::{PushPage, PopPage, CreateOptionsPage, CreateControlsPage, UpdateControlsPage, UpdateControlSchemePage, RenderControlSchemePage, CreateObjectivesPage}`, `CUpgradesManager::GetUpgradesCount`, `HasGyroscope`, `CMenuManager::{onEvent, Render}`, `CMenuScreen::ProcessAccessoryInput`, `GS_InGameMenu::Release` | `host/controls_menu.cpp`, [Input](input.md#the-controls-page) |
| Menu layout | `Is480x320Res`, `Is960x640Res`, `Application::GetFontSize`, `CTTFFontSpr::{GetOffsetForOutlinedGlyph, GetBorderSizeForOutlinedGlyph}` | `host/hooks.cpp`, [3DS port](3ds.md#menu-layout-at-400x240) |
| Prompts | `Application::GetString` | `host/prompts.cpp`, [Input](input.md#tutorial-prompts) |
| Event log | console, Lua script manager, checkpoints, spawns, teleports, cinematics | `host/eventlog.cpp`, [Debugging](debugging.md) |
| Offline | `CMenuManager::{Load, ShowMenuObjectById, EnableMenuObjectById}`, `Menu_PushState`, pause page, `CSprite::PaintModule`, font `DrawString`s, `CMenuSprite::Render`, `XPlayerManager::GetUsername` (nickname), `CMenuButtons::Pressed` and `GS_IGMRateGame::Render` (rate popup), `GameMpManager::{LoadUnlockTableData, LoadPurchaseData}` (armory), plus `SOCIAL_DISABLED` / `OFFLINE_STUBS` | `host/offline.cpp`, [Offline game](offline.md) |

## Tools

- `tools/disasm.py <name-substring | 0xaddr> [max]`: symbol-aware disassembly with branch
  targets and literal-pool values resolved; `--callers <name>` lists functions that `bl` to it.
- `tools/xrefs.py build`, then `xrefs.py grep <regex>`: which functions reference which strings
  (PIC string references resolved).
- `build/analysis/gotrefs.py <symbol>`: functions that reach a data symbol through the GOT.
- `tools/obb.py list | extract`, `tools/elf.py`, `tools/survey.py`, `tools/insnmix.py`.
