# Switch port

Reference: [Nintendo Switch](../switch.md).

## Goal

Build the same translated code and runtime for the Nintendo Switch as an installable title
(`.nsp`) with the game files inside, no HD texture pack, and test it unattended in emulators
with the player's own keys.

## What was done

**Build and package**
- devkitPro (libnx, SDL2, Mesa nouveau GLES2) CMake block: the host sources, the runtime, the
  generated code, `--whole-archive`, `nx_create_nro` and `nx_create_exefs` with an NPDM
  (`host/switch/nova3.json`: program id `0x01004E4F56410000`, 39-bit address space,
  128 MB system resource for `svcMapPhysicalMemory`).
- `tools/switch_nsp.py` packs the exefs, the control data (icon from the APK) and a RomFS of
  `dist/nova3_data` minus `hd_textures.pak`, with hacBrewPack and the player's `prod.keys`.
  The hacBrewPack repository was gone, so the binary from the player's SAK folder is used.

**Memory**
- The guest's 4 GB lives in the alias region, mapped page by page with
  `svcMapPhysicalMemory` (that call needs the NPDM's system resource; without it the game
  stopped at the first commit).
- `__libnx_initheap` leaves 768 MB to the guest and gives the rest (about 2.3 GB of a retail
  console's 3285 MB) to the heap, which also holds Mesa's GPU buffers and thread stacks.

**Platform differences**
- In-process pipes for boost::asio (newlib has none; the analytics tracker's constructor
  failed and the game called `terminate()`).
- Socket readiness through a non-blocking `recv(MSG_PEEK)`: emulators answer a zero-timeout
  poll only after about 50 ms, and multiplayer ran at 10 fps.
- Exit through `svcExitProcess` after flushing the log (`fsync`), without static destructors
  or libnx teardown: the guest threads are still running, and every other exit path crashed.
  The main thread only waits; it no longer pumps SDL events after the game closed HID.
- Fatal errors show the system's error dialog (`errorApplication`).
- `sdmc:/switch` is created before the log is opened.

**Threads and cores**
- Horizon schedules by strict priority without time slices, and every thread started on
  core 0: audio stalled behind busy threads. The game thread now has core 1 to itself, the
  guest's other threads alternate between cores 2 and 0, Vox's mixer runs at priority 0x2b,
  and `yield` is `svcSleepThread(YieldToAnyThread)`.
- The asynchronous log writer and other service threads use libnx threads directly:
  `std::thread` / `call_once` and a condition variable `wait_for` hung at startup.

**Display**
- Docked and handheld: the framebuffer is 1920x1080, and the game renders at the mode's size
  inside it (`nwindowSetCrop`), so a mode change needs no new surface. Switching while running
  works.
- 60 fps cap by default (30 can be chosen), adaptive VSync.

**Emulators**
- Ryujinx (the player's normal install, with keys and firmware) and Citron. Ryujinx's PPTC
  cache had to be cleared once (a stale cache reported an unknown `MRS`), and its `glReadPixels`
  screenshots came back at a quarter width, so tests use window captures instead.
- `cntvct_el0` traps in Citron, so `cntpct_el0` is used for timing.

## Results

The campaign's ten chapters play through with the PC's checkpoint counts on Ryujinx and
Citron, the seven multiplayer maps host and play, and the build runs on real hardware
(players' logs: 52-56 fps handheld before the later performance work).
