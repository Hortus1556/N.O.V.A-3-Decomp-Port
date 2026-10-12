# Nintendo Switch port

The same recompiled game, runtime and game-side host modules as the PC build, compiled for
the Switch (AArch64) with devkitPro and run as a homebrew title. Only the platform layer is
new: `runtime/os_switch.cpp` (memory, threads, clocks, crash handler, heap) and
`host/switch/main.cpp` (SD card paths, SDL2 window and GLES context, the frame loop, test
scripts); `host/input.cpp` reads the controllers and the touchscreen through libnx and
`host/audio.cpp` plays through SDL2's audio queue.

## Building

devkitPro (MSYS2 on Windows, `C:\msys64\opt\devkitpro`) with `switch-dev`, `switch-sdl2`,
`switch-mesa`, `switch-glad`, `switch-libdrm_nouveau`, `switch-zlib`, `switch-libpng` and
`switch-cmake`. SDL3 is not available for the Switch outside Nintendo's SDK, so the port uses
SDL2 (`host/sdl.h` picks the header; the shared modules only use calls both have).

```
/opt/devkitpro/portlibs/switch/bin/aarch64-none-elf-cmake -S . -B build/switch -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build/switch nova3_nro nova3_exefs
```

The generated code compiles in a few minutes per file and job (GCC 16, 1-2 GB per job: a full
rebuild at `-j3` took 16 minutes and 5.9 GB on 2026-10-10); the link takes about a minute.
`nova3_core` is compiled `-fPIE`, not with the `-fPIC` devkitPro's platform file gives every
static library (`CMAKE_POSITION_INDEPENDENT_CODE`): it is linked into the PIE executable, and
under `-fPIC` GCC treats each global function as interposable, so none of the translated
functions could be inlined or optimised across calls, and every global was read through the
GOT. Outputs:

- `build/switch/nova3.nro`: a homebrew-menu program;
- `build/switch/.dkp-generated/nova3_exefs/{main, main.npdm}`: the exefs for an installable
  title; the NPDM comes from `host/switch/nova3.json` (title `01004E4F56410000`).

## The .nsp

`tools/switch_nsp.py --keys <prod.keys>` packs an installable title with hacBrewPack
(`build/tools/hacbrewpack.exe`, version 3.05; the original repository is gone, a copy ships
with Switch Army Knife):

- exefs: `main` + `main.npdm`;
- control: `nova3.nacp` and the game's icon from the APK (256x256 JPEG);
- RomFS: the game files from `dist/nova3_data` (libGloftNAHM.so and the OBBs' contents,
  1.6 GB) without the PC's `hd_textures.pak`.

The keys are the player's own, dumped from their console (Lockpick_RCM). Installing a
self-made title needs sigpatches (Atmosphère), as for any homebrew .nsp. The .nsp is for the
player's own copy, like the rest of the build output.

## Files

- game files: `sdmc:/switch/nova3/nova3_data/` when present (the .nro), otherwise `romfs:/`
  (the .nsp);
- `sdmc:/switch/nova3/`: `save/` (the save games), `nova3.ini` (the PC's settings file; the
  launcher-only parts are ignored), `nova3.log`, `scratch/` (the guest's /sdcard, cache, /tmp).

## Memory

- Guest address space: 4 GB at the top of the process's alias region (39-bit address space,
  `address_space_type` 3), mapped with `svcMapPhysicalMemory` as the runtime commits pages.
  That call needs the NPDM's `system_resource_size` (the kernel's page tables for it): 128 MB.
- libnx would take all of the application's memory as its heap at start; the port's
  `__libnx_initheap` leaves 768 MB to the guest and gives the rest to the heap (libstdc++,
  SDL, Mesa's GPU buffers), counting the application's memory as at most a retail console's
  3285 MB (emulators may offer more): about 2.3 GB of heap. Measured over the campaign in
  Ryujinx: the guest committed 200 MB at most, the heap held 1.1 GB at most (chapter 2).
  `nova3.log` prints the sizes at start, the guest's committed total every 64 MB and the
  heap in use with the frame counter.
- Thread stacks are heap blocks (16 MB each for the guest's threads, committed: no demand
  paging), and libnx frees one only in `pthread_join`: its `pthread_detach` does nothing
  (ENOSYS). The game's web services (glwebtools' thread pool) start threads every few
  seconds and leave one of every three unjoined; on real hardware the heap grew about 16 MB
  every 10 seconds of play until, at a section load, textures no longer fitted
  (`glCompressedTexImage2D … error 0505`, GL_OUT_OF_MEMORY) and the screen turned green.
  Now every host thread is detached in the runtime's sense: it queues itself when it ends
  and the next thread start joins it (`runtime/os_switch.cpp`), and a guest thread's own
  stack and registers are freed when it ends, joined or not (`runtime/thread.cpp`).
- The .nro gets an application's memory only when started over a game (hold R); from the
  album applet it has far too little. It also needs that game's NPDM to grant a system
  resource (`svcMapPhysicalMemory` refuses otherwise, and `svcMapMemory` only maps into the
  small stack region, too small for the 4 GB guest space): without one it stops with a
  message saying so. Emulators start an .nro without one, so there the .nsp is the way to
  test. The installed .nsp is the main build.
- A fatal error shows its message in the system's error dialog before the game closes
  (details in `nova3.log`).

## Platform differences handled

- Pipes: newlib on the Switch has none, and boost::asio's reactor (the analytics tracker's
  io_service) needs one to wake itself; `runtime/libc_io.cpp` emulates pipes in-process.
  Without them the tracker's constructor failed and the game called `terminate()` at start.
- Exit (HOME, close, a guest `exit()`, a fatal error): the guest threads are still going, so
  the installed title ends with `svcExitProcess` after flushing the log: neither static
  destructors (a guest thread crashed on freed host state) nor libnx's service teardown (the
  SD card closed under the audio thread's log write crashed newlib) run under them. The .nro
  leaves through libnx (`_Exit`), the way back to the homebrew menu. The main thread only
  waits for the game thread: SDL's event pumping polled the touchscreen after an exit from
  the game thread had closed HID.
- Audio: libnx's AudioOut (48 kHz stereo) fed by a thread of `host/audio.cpp` that keeps
  three 21 ms buffers queued and mixes the open tracks into each released one (converted to
  48 kHz stereo on the way in, the volume applied in the mix; silence when nothing is
  queued). SDL2's Switch audio thread stalled now and then and the queue stopped draining
  for good: the game went silent. A write waits while the queue is full, as Android's
  blocking `AudioTrack.write` does, so an output clock slower than the game's paces the
  mixer instead of losing chunks.
- Cores and priorities: libnx starts every thread on the process's default core (0), and
  Horizon schedules by strict priority without time slices: a busy thread starves every
  thread of its priority on its core. The game thread (the first one the runtime starts)
  has core 1 to itself, so no loader or worker can hold it up; the guest's other threads
  alternate between cores 2 and 0 (core 0 also has the main thread, which only waits, the
  audio feeder, the log writer and the watchdog). Vox's mixer runs just above the game's
  threads (0x2b vs 0x2c), never on the game thread's core; it sleeps between chunks. Yields
  (`sched_yield`, spin waits) let lower-priority threads run too (`YieldToAnyThread`), so a
  raised thread waiting on a lock never starves its holder.
- Logging is asynchronous (a background thread writes the lines; an SD card write per line
  stalled the game thread and Vox's mixer).
- VSync is adaptive where the driver has it: a frame that misses the refresh tears instead
  of waiting a whole refresh (which dropped the rate to 30 fps).
- The log's frame lines give the average and worst frame time, the time in the game's step
  (CPU) and in the swap (GPU / VSync), and the GL calls per frame: what to look at when
  the game is slow.
- Input: buttons by label (A, B, X, Y; L/R are the bumpers, ZL/ZR the triggers, + Start,
  − Back), both sticks, the touchscreen in menus, the system keyboard for text fields.
- Display: docked 1920x1080, handheld 1280x720, switching while playing. The framebuffer
  is always 1920x1080; the game renders at the mode's size into its lower-left corner (told
  through GL2JNILib.resize, as a phone rotating) and the window's crop shows just that
  region, scaled to the screen: no new surface on a mode change. Touch input follows.
- Frame cap and VSync: 60 fps by default (30 can be chosen), with adaptive VSync (a late
  frame shows at once instead of waiting a whole refresh). The in-game Graphics page sets
  them, the field of view and anisotropic filtering (`display::apply`, saved to nova3.ini).
- Gyro aiming ([gamepad] gyro = off / aim / always, gyro_sensitivity): the six-axis sensor of
  the console (handheld), a Pro Controller or the right Joy-Con. Yaw is the angular velocity
  around gravity (from the accelerometer), so it works held upright or flat; pitch is the
  rotation around the controller's left-right axis. Added to the frame's look like the mouse;
  gyro_invert_x / gyro_invert_y turn either axis the other way (the stick's Invert Y does not
  apply to it).
- Buttons: by their labels by default (A is the game's confirm and jump, B back and interact,
  as Nintendo's layout has it); [gamepad] swap_ab_xy (Controls: SWAP A/B, X/Y) takes them by
  position instead, the Xbox layout (the bottom button, B, confirms and jumps). Prompts and the
  binding page name the Switch's buttons: A / B / X / Y as swapped, L, R, ZL, ZR, +, -,
  Left / Right Stick. (The game's own menu icons, such as the Back button's (B), stay.)
- Mesa's glthread (`[video] glthread`, off by default): it moves the driver's work to another
  core, but this Mesa (devkitPro's 20.1) cannot queue a draw that reads vertices or indices
  from client memory. Before each such draw it waits for the queue to drain and draws on the
  game thread, and in a busy frame 40-50% of the game's draws are like that (the log's
  `gl per frame` line counts them). Its library has no glthread upload code, which newer
  Mesa uses for these draws. The test builds' `threaded_gl` setting is ignored.
- Loading screens (`GS_Loading`, `GS_LoadMenu`; `hooks::loading`) run the CPU in boost mode,
  1785 MHz instead of 1020 (`appletSetCpuBoostMode`, as retail games do while loading; the GPU
  drops to its minimum, which a loading screen does not need). The log says `cpu: boost on` /
  `off`. Shader compiles, decompression and the game's parsing are all CPU work.
- Player IP: match setup > Advanced has a fifth row, Player IP ([multiplayer] peers: players'
  addresses that also get the broadcasts, for networks that drop them); EDIT opens the system
  keyboard. It is the game's Private Match switch (150360), online only and hidden in local
  play, relabelled (host/controls_menu.cpp). On the PC the launcher has the field.
- Rumble ([gamepad] rumble, rumble_strength; all platforms): the game had none, so hooks add
  it on the player's shots (CWeaponComponent::DoFire), hits taken
  (PlayerComponent::ReceiveDamage) and camera / HUD shakes (CGameCamera::ShakeCamera,
  CHud::StartShake: strength from the intensity, as long as the shake). libnx HD rumble on
  the Switch, SDL gamepad rumble on the PC.
- Toggle run ([controls] toggle_sprint) applies to every device: a press of sprint runs, the
  next one (or stopping, or aiming) walks.
- Sockets: readiness uses `POLLIN` / `POLLOUT` (the Windows build needs WSAPoll's
  `POLLRDNORM` / `POLLWRNORM`, which the Switch's BSD service in Ryujinx rejects), and
  read-readiness is a non-blocking receive: Ryujinx answers a zero-timeout poll only after
  ~50 ms, and the game selects on its sockets every frame (a local match ran at 10 fps; 60
  without the poll). A stream socket is peeked (1 byte, `MSG_PEEK`); a UDP socket's next
  datagram is received ahead and handed to the game's next read, because Citron ignores
  `MSG_PEEK` (the peek ate the datagram). No `MSG_NOSIGNAL` (no signals here; emulators assert
  on the flag).
- Socket addresses: libnx's are BSD's (a length byte, an 8-bit family), the game's bionic's; they
  are translated both ways. Untranslated, the console's service got family 0 for the join
  search's broadcast, and Join left the search at once while Create worked
  ([Runtime](runtime.md#network), Joining from a console). The Wi-Fi address and subnet come
  from nifm (`net: broadcasts go to ...`; Citron with no network interface selected has none).
- Local Wi-Fi tested in Citron (1.0, on the PC's own network): Join's list stays open; with a PC
  hosting map 0 the Switch listed it, joined and played the match.

## Testing on a PC

Emulators run the .nsp directly (`Ryujinx.exe nova3.nsp`, `citron.exe -g nova3.nsp`) with the
player's keys and firmware. `sdmc:/switch/nova3/test.txt` scripts a run (see
`host/switch/main.cpp`; Windows line endings are fine): `shot`, `tap`, `hold` (pad buttons go through the pad state, as a
controller's), `lua`, `tour CHAPTER TRIGGERS`, `tour_shots`, `tour_stop SPAWN` (as `--tour-stop`:
the other commands' frames then count from the stop), `log_player FRAME`, `bench CHAPTER FRAMES [walk]` (as
the PC's `--bench`: the perf lines, then `bench: chapter N: ... fps`), `speed`, `env NAME VALUE`
(`NOVA3_TOUR_SETTLE_MS`, `NOVA3_SAMPLE`: the watchdog logs the game thread's guest callers
every second, a poor man's profiler), `quit`, `trace_files`. `build/swtour.py` (a scratch tool, not part of the project) plays campaign
chapters through with the tour and captures the emulator's window.

Emulator quirks seen:

- Ryujinx's `glReadPixels` returns only the left quarter of the frame (the display is
  correct): the test script's `shot` is unreliable there; window captures are used instead.
- Ryujinx's translation cache (PPTC) may hold GCC 16's unwinder, whose guarded-control-stack
  check reads a system register Ryujinx does not know (`Unknown MRS 0xD53B2521`) while
  translating ahead; deleting the title's `games/01004e4f56410000/cache/cpu` fixes it. A real
  Switch never executes that read (it sits behind a feature check that is a no-op there).
- The port never throws host C++ exceptions in normal runs; `runtime/os_switch.cpp` logs any
  (`--wrap=__cxa_throw`).
