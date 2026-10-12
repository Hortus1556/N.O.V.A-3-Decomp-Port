# Nintendo GameCube port

Status: design and groundwork (started 2026-10-12). The target is a stock GameCube (or a Wii in
GameCube mode) booting a disc image from an optical drive emulator (GC Loader, FlippyDrive)
or Dolphin. The aim is the PC's **highest** detail profile wherever the hardware allows it,
with the game's data streamed through the memory tiers below. This document records the
measurements that decided the design, the hardware facts it relies on, and the design itself.

## Why it is hard

| | GameCube | What the game needs (chapter 3, measured on the PC) |
|---|---|---|
| CPU | Gekko: PowerPC 750 class, 486 MHz, one core, 2-issue, 32 KB I / 32 KB D L1, 256 KB L2 | ARM code translated to C: 0.87 ms/frame (3DS profile) and 1.06 ms/frame (highest profile) on a Ryzen 7 9800X3D; 13-15 ms on a Switch A57 at 1 GHz |
| Main memory | 24 MB 1T-SRAM (MEM1), the only memory the CPU and GPU can address | heap 54 MB live (3DS profile), 60 MB (highest); 76-83 MB peak during loads |
| Other memory | 16 MB ARAM, reachable only by DMA (60-70 MB/s) | |
| Code | | 20.6 MB of ARM code, ~76 MB translated to PowerPC at `-O2` (~56 MB with cold code at `-Os`) |
| GPU | Flipper: fixed function (TEV, 16 stages, 8 textures, indirect textures), 1 MB texture cache, 2 MB embedded frame buffer, 640x528 at most | GLES2 shaders; textures 11.4 MB (3DS profile) / 59.2 MB (highest) in 4-8 bit formats; buffers 13-21 MB |
| Storage | disc: 1.46 GB, 2-3 MB/s, over 100 ms per random seek | 980 MB of game files after the 3DS's slimming |

Nothing fits as it is. The port works only because the game touches a small part of its
memory and code at a time, as measured below.

## Measurements that decided the design

All on the PC build in chapter 3 (`--bench 3:3000:walk`), with the profiling options
described in [Debugging](debugging.md#memory-profiles-heap-and-working-set).

- **Code that runs** (the PGO profile of boot + chapters 0, 3 and 6): 8,113 of 40,218
  functions, 6.7 MB of the 20.6 MB of ARM code. Called at least once every three frames:
  2,163 functions, 1.46 MB of ARM code (about 5.4 MB of PowerPC code). The PowerPC code is
  3.7x the ARM bytes at `-O2` and 2.7x at `-Os` (one generated file compiled both ways).
- **Heap working set** (`NOVA3_WORKING_SET=300`): pages touched in play

  | Profile | in 1 frame | in 60 frames | in 300 frames | ever |
  |---|---|---|---|---|
  | 3DS | 4.6-6.5 MB | 5.0-7.3 MB | 5.2-9.3 MB | 70 MB |
  | highest | 5.3-7.7 MB | 5.8-8.5 MB | 6.0-10.1 MB | 81 MB |

  The library's own data: 0.3 MB a frame, 0.6 MB ever.
- **What fills the heap** (`NOVA3_HEAP_PROFILE`): about 8 MB are thread stacks and buffers of
  the dead online services (analytics workers 4 MB, voice chat 2 MB, DNS 1 MB); physics 9.5 MB
  (collision objects 5.9, BVH trees 3.0); Collada resource files 5 MB; scene nodes 3.5 MB;
  the irradiance volume 2.5 MB; particle buffers 1.6 MB; the rest small.
- **Compressibility** (the heap at frame 2500, `NOVA3_HEAP_DUMP`): of an 81 MB extent, 20 MB
  are zero pages; the other 59-63 MB compress to 21.9 MB (zlib level 1, 4 KB pages, 2.7x),
  18.8 MB (zlib 9, 16 KB blocks, 3.3x), 15.5 MB (LZMA, 16 KB blocks, 4x). The extent includes
  freed blocks, which the pager can drop.
- **CPU**: the highest profile costs 22% more game-thread time than the 3DS profile. Two
  estimates for the Gekko (from this PC's 1 ms at ~60x, and from the 3DS's 24 ms of native
  ARM11 code adjusted for clock, issue width and the translation's overhead) both land around
  40-60 ms of game code a frame before optimisation: 15-25 fps.

## Hardware facts used (sources at the end)

- **Gekko**: 486 MHz; two integer units, one FPU with paired singles (two single-precision
  operations per cycle, 3-cycle latency), one load/store unit, branch unit; 6-entry
  completion queue, two instructions retired per clock. L1 caches 32 KB 8-way, 32-byte lines;
  the data cache can be split into 16 KB cache + 16 KB locked scratchpad with a DMA engine
  (15-entry queue, up to 4 KB per command). 256 KB 2-way L2. Write-gather pipe: 128-byte FIFO
  bursting non-cacheable stores to the GPU's command FIFO.
- **Loads and stores**: `lwbrx`, `lhbrx`, `stwbrx`, `sthbrx` (byte-reversed) take the same 2:1
  cycles as `lwz`, but only in the indexed form (register + register). Floating-point loads
  have no byte-reversed form. A load does not bypass a pending store to the same address: a
  value stored and reloaded at once waits for the store.
- **Little-endian mode** (`MSR[LE]`) is not a way out: it swaps address bits rather than bytes
  in memory (other bus masters see a different layout), the quantised paired-single loads are
  undefined in it, and Dolphin does not emulate it.
- **MMU**: 4 IBATs + 4 DBATs, 16 segment registers, a hashed page table searched in hardware,
  128-entry 2-way TLBs for instructions and data, 4 KB pages. A missing page raises a DSI
  (data) or ISI (instruction) exception that software can resolve and return from. VBA-GX,
  Not64 and Nintendo's own SDK libraries page memory this way on a GameCube.
- **Memory**: 1T-SRAM, 2.6 GB/s peak, no bank conflicts; the CPU bus is 162 MHz x 64 bit.
  ARAM DMA 60-70 MB/s (a 4 KB page in about 60 us); the audio DSP's ARAM accesses take
  priority.
- **GPU (Flipper, 162 MHz)**: hardware transform and lighting (8 lights per vertex, 10+
  position matrices addressable per vertex, texture coordinate generation from positions or
  normals through matrices), 4 pixels a clock, TEV with 16 stages and 8 textures, indirect
  texturing (bump and distortion), fog, alpha compare, copies of the frame buffer into
  textures (with 2x2 box downsampling and format conversion). Vertex attributes can be
  indexed (8 or 16-bit indices into arrays in main memory) and quantised (8/16-bit with a
  fixed-point shift). Display lists replay recorded command streams without CPU work.
  Texture formats: I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, CI4/8/14, CMPR (DXT1, 4 bpp).
- **Disc**: 2-3 MB/s, over 100 ms per random seek: at most ~10 non-sequential reads a second
  on a real drive. Optical drive emulators read from SD cards with no seek.

## Design

### Address space

Guest addresses are host addresses (`g_mem` = 0, as on the 3DS), translated by the MMU's
page table: the library's image at 0x10000000, the heap from 0x20000000, guest stacks and
mappings below 0x80000000. The host (libogc, the runtime, its heap, the GPU's buffers) stays
in MEM1 through libogc's BAT mappings at 0x80000000 (cached) and 0xC0000000 (uncached).

### Memory tiers (the pager)

A page fault handler in the style of VBA-GX's (`vm.c`, tueidj / emu_kidid; DSI for data, ISI
for code) keeps three tiers:

1. **MEM1 frames**: the resident working set. Second-chance (clock) eviction from the page
   table's referenced and changed bits.
2. **ARAM**: dirty pages compressed (LZ77 in LZ4's token format, 4 KB at a time, into 256-byte
   sectors: 1 to 16 a page, found first fit in a bitmap; a page that does not save a sector is
   stored as it is). A changed page found to be all zeros at eviction becomes a zero page again
   and takes no ARAM. Freed heap pages are dropped (the guest allocator tells the pager), and
   pages filled straight from a game file are kept as clean and re-read from the disc instead
   (the private file mapping trick). The GX textures' copies share the same sectors
   (`pager_aram_alloc`: from the top of ARAM down, while pages fill from a roving start).
   Out of ARAM, the pager stops with a report (`=== the pager is out of ARAM ===`).
3. **Disc** (and an SD card when one is there): the translated code, pre-converted textures
   and buffers, and the clean file-backed pages.

Measured MEM1 use in the title menu (2026-10-12): the DOL 2.4 MB + 0.7 MB bss; the host heap
about 6 MB of its 6.5 MB reserve (two XFBs 1.4 MB, thread stacks 1 MB, the GX FIFO 256 KB, the
read caches and disc blocks ~0.9 MB, the indirect-call index 80 KB, the runtime's tables); the
texture pool 5.5 MB (4 MB in use); pager frames 7.8 MB (2,000 frames). ARAM: guest pages
about 5 MB (compressed to 37-41%) and the textures' copies.

Planned MEM1 budget (before the measurements above):

| Use | MB |
|---|---|
| program (runtime, host, libogc, newlib) and its heap | 4 |
| GPU command FIFO, two XFBs, frame-buffer copy targets | 2 |
| translated code frames (the hot set, function order from the profile) | 4 |
| guest data frames (the 300-frame working set) | 8.5 |
| texture cache (CMPR / I4, mip levels chosen for 640x480) | 3.5 |
| vertex and index cache (quantised formats) | 2 |
| total | 24 |

### Code

The recompiler's C output is compiled for the Gekko and linked at a virtual address range of
its own, outside the guest's. The DOL holds only the runtime, the host and the pager; the
translated code is a file on the disc paged in through ISI faults. Functions are ordered by
the PGO profile (hot code together, so it fills few pages), hot code built `-O2` and cold
code `-Os`. Per-target translation changes:

- **Byte order**: guest memory stays little-endian, so `LD16/32/64` and `ST16/32/64` swap
  (`lwbrx` and friends). Host code that reads guest memory through raw pointers must go
  through the same accessors: an audit of the runtime and host for big-endian hosts.
- **VFP registers as locals**: the translator keeps the VFP register file in the `Cpu`
  struct, so every float operation stores and reloads memory. That is cheap on x86-64 and
  AArch64 (store forwarding) and costly on the Gekko (a reload waits for its store). Keeping
  the registers a function uses in locals lets GCC put them in FPRs.
- **The VFP register union** (`S(2n)` is the low half of `D(n)`) depends on byte order: on a
  big-endian host single registers are indexed `n ^ 1`.
- **Native hot paths** (`host/native_hot.cpp`) gain paired-single versions (skinning,
  matrices, culling).

### Graphics (`gles.cpp` over a GX backend)

What is built (`runtime/gles_gx.cpp`, the programs and their uniforms shared with the 3DS's
backend in `runtime/gl_program.cpp`; the plan's tricks below are not all there yet):

- **Transform**: the game's MVP split into GX's projection (perspective or orthographic) and
  position matrix (rows R0, R1, -R3; the depth row turns GL's [-w, w] into GX's [-w, 0]).
  Render targets are drawn with y mirrored, so that a texture's first row is GL's.
- **Vertices**: direct mode through the write-gather pipe, converted from the game's
  little-endian arrays per vertex; buffer objects' contents live in the guest heap (paged).
  The lit materials (irradiance, two point lights) are lit on the CPU as the PICA's vertex
  shaders do it (`runtime/pica/lit.v.pica`, `point2.v.pica`).
- **Materials**: a TEV setup per recipe (texture x vertex colour, lightmaps, vertex colour);
  the split alpha textures' red becomes the alpha through a TEV swap table. The other recipes
  are drawn as texture x vertex colour for now.
- **Textures**: ETC1 to CMPR (both 4 bits a texel), RGBA to RGBA8, RGB to RGB565, one channel
  to I8, converted 4x4 blocks at a time from the upload (no full-image copies: a 960x640 image
  as RGBA8 is 2.4 MB of MEM1). Sides that are not powers of two are rounded up to whole tiles
  (GL ES 2 clamps those), not to powers of two. Largest side: 1024 for the interface's images
  (screens at their own sizes; the 2048 sprite atlases halved), 512 for the others (the world's
  and the models': what a 640x480 screen shows). Every texture has a copy in ARAM; the MEM1
  pool (5.5 MB) keeps what recent frames drew with, least recently used out (never one the
  current frame drew with), and a draw brings a texture back by DMA.
- **Render targets**: drawn in the EFB at most 640x528 (the menus' 1024x1024 interface layer at
  0.52: 528x528) with an RGBA6 pixel format, and copied into their texture (RGBA8, stored at
  the drawn size: 1.1 MB instead of 4 MB) when the game binds another framebuffer. The EFB is
  the screen's too: a clear of the screen waits for the frame's first draw to it (the game
  clears the screen, then draws its interface layer into a render target, then the screen).
- **Clears**: a quad at the clear depth, just inside GX's far clip plane (at it, GX clips the
  quad and nothing is cleared).

The design it follows: the same structure as the 3DS's PICA200 backend: `runtime/gles.cpp` keeps the game's GL
calls, a GX driver below it picks a material recipe for each linked program and sets the TEV,
the transform unit and the textures per draw. Where the GameCube does more than the PICA200,
the recipes use it: one-pass terrain (four textures), normal-mapped specular through indirect
textures, the hardware's per-vertex lights for point lights, bloom from downsampled
frame-buffer copies. The tricks:

- Static vertex and index buffers converted once to quantised GX formats and drawn through
  display lists (no CPU work per vertex).
- Client-memory draws (the game batches small meshes every frame) written straight into the
  command FIFO through the write-gather pipe, byte-swapped on the way.
- Texture streaming with the mip level chosen for a 640x480 screen; textures pre-converted
  offline into a pack on the disc (ETC1 to CMPR, split alpha to I4), as the 3DS's pack.
- The frame buffer at 640x480 (480p where the cable allows), a large command FIFO so the CPU
  and GPU overlap.

### Storage

A GameCube disc image built by a tool in this repository: our DOL, the translated code file,
the texture and buffer packs, and the game's files (the slim set, 980 MB, under the 1.46 GB
limit). Dolphin boots it directly; on a console it runs from an optical drive emulator.
Dolphin does not emulate a GameCube SD adapter (checked: no such device in the March 2025
build), so the SD tier is optional and only for real hardware.

## Building and testing

- **devkitPPC** from devkitPro (`pacman -S gamecube-dev ppc-libmad` in devkitPro's MSYS2).
- **libogc2** (extremscorner's libogc, which the GameCube homebrew projects build on: Swiss,
  Wii64 / Cube64, VBA-GX), not devkitPro's libogc 3.x, whose new kernel has other exception
  interfaces. `tools/gc/build_libogc2.sh` builds a pinned commit into
  `/opt/devkitpro/libogc2/gamecube` (beside devkitPro's libogc, no package repository added)
  with `tools/gc/libogc2.patch`: since "Fix EXI_Sync on Wii U" (Nov 2025) `EXI_Sync` polls an
  alias of the channel's control register 0x80 bytes higher, which Dolphin 2503 does not map,
  so every program hung in `__exi_init` (found with `tools/gc_gdb.py`); the patch polls the
  register itself, as libogc always did (a GameCube answers on both). And larger LWP object
  pools (`lwp_config.h`): 48 threads instead of 16, 2048 mutexes and 1024 condition variables
  instead of 64 (every `std::mutex` and newlib lock is an LWP mutex, made at its first use),
  from libogc2's 1 MB kernel workspace (the top of MEM1).
- **libdvm** (extremscorner's FAT library for libogc2, for the SD card): `tools/gc/build_libdvm.sh`
  builds a pinned commit's `libfat.a` (FatFs behind newlib devices, `fatMountSimple`) into the
  same prefix.
- **Tests**: `sh tests/gc/build.sh hello pager_test` builds `build/gc/*.dol`.
- **The game**: the translated code for the GameCube is generated into its own directory (the
  translator's output differs: single-precision VFP registers are indexed through `VS()`), then
  built with the toolchain file, from devkitPro's MSYS2 shell:

  ```
  python recomp/arm2c.py gen build/gen_gc
  cmake -S . -B build/gc/cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=tools/gc/gamecube.cmake \
        -DCMAKE_BUILD_TYPE=Release -DGEN_DIR=/e/nova3/build/gen_gc
  ninja -C build/gc/cmake
  python tools/gc_dol.py build/gc/cmake/nova3.elf build/gc/out          # nova3.dol + gcode.bin
  python tools/gc_disc.py build/gc/nova3.iso --dol build/gc/out/nova3.dol \
        --file gcode.bin=build/gc/out/gcode.bin --dir nova3_data=<game files> --skip-suffix .z
  ```

  `tools/gc/ld/ogc.ld` (found first through `-B`, in place of devkitPPC's) links the
  translated objects (`code_*`) at 0x40000000 and everything else at 0x80003100;
  `tools/gc_dol.py` turns the first into `gcode.bin` and the rest into the DOL.
- **Dolphin harness**: `python tools/gc_run.py build/gc/pager_test.dol` runs a DOL, ELF or
  disc image in Dolphin (`E:\Emulators\Dolphin`) with a user folder of its own
  (`build/gc/dolphin_user`: the player's settings stay as they are), MMU emulation on, serial
  port 1 set to the dummy device (libogc2 probes it at start), the OSReport channel logged to a
  file. It prints the log until a line matches `gc: done` / `gc: FAIL` or the time runs out.
  Options: `--mem1 MB` (Dolphin's MEM1 size override, for bring-up only), `--no-mmu`,
  `--fast-disc`, `--log CHANNEL`, `--verbosity 5`, `--shot SECONDS:PNG` (a screenshot of the
  Dolphin window, repeatable), `--until REGEX`. Dolphin is set to write EFB copies to memory
  (its default keeps them in its own texture cache, where the CPU reads zeros) and to emulate
  the EFB's format changes, as a console does. For the game, `--until` should match every
  report the port writes when it stops (`=== `, `unresolved`, `Exception`), or Dolphin sits on
  libogc's exception screen until the timeout.
- **Host heap by allocation site**: configure with `-DNOVA3_GC_MPROF=ON` (links malloc, calloc,
  realloc, memalign and free through `runtime/gc_mprof.c`: a 16-byte header per block naming
  its three return addresses); `gc_mprof_dump()` (called on a fatal exit) lists the sites
  holding the most bytes for `powerpc-eabi-addr2line`. A bring-up tool: a run with it on hung
  after the first frames (not looked into).
- **Draw trace** (`gles_gx.cpp`, `g_trace`): every 1000th frame, three times, the log gets each
  draw (material, textures, the first vertex in GL's clip space, the fixed-function state),
  every vertex of small draws, framebuffer binds, viewports and clears, and a count of the
  drawn texels in each render target's copy.
- **Log output**: `runtime/gc_report.c` makes stdout / stderr write to the IPL device's UART,
  which Dolphin shows as OSReport (libogc2's own report stream writes there only on development
  consoles). On a retail console the writes go nowhere.
- **Where is it stuck**: `python tools/gc_gdb.py build/gc/x.dol --after 5 --samples 3` runs
  the program with Dolphin's GDB stub, interrupts it and prints the registers and the stack's
  return addresses with their symbols (from the ELF beside the DOL). It speaks the GDB remote
  protocol itself; Dolphin sends an extra reply after a stop, which it drains.

## Status

- **The pager** (`runtime/gc_pager.c`, `gc_pager_stub.S`) works in Dolphin
  (`tests/gc/pager_test.c`): 8 MB of anonymous memory over 2 MB of frames written and checked
  twice (8,193 data faults, 6,145 reloads from ARAM, 4,096 write-backs), 16 code pages filled
  from a source and run through instruction faults, a discarded page reading zeros again.
  86 us a fault in Dolphin's timing; with compression (2026-10-12) also 6 MB of random,
  repetitive, mixed and zero pages over 2 MB of frames, every word checked twice. Not yet: the
  disc tier, a frame's pinning for DMA.
- **The game** (2026-10-12, Dolphin): boots from the disc image into the title menu and runs
  there (17,000 frames in one run, about 22 ms a frame in Dolphin's timing, 26 draws and ~8,000
  vertices a frame): the 3D background and planet, the interface layer drawn into its render
  target and composited in perspective, the Campaign / Multiplayer panel with its artwork and
  text. The image starts about 36 pixels from the left in Dolphin's PAL output (the video
  interface's origin; not checked on NTSC). Next: the remaining materials (some interface
  parts are drawn as texture x colour), the controller in the menus, a level, the first console
  test (the SD card build, see Running on a console).

## Running on a console

Swiss starts the program; the game's files come from an SD card (Swiss's disc emulation cannot
be assumed to serve a program that drives the DVD interface itself, as libogc2 does).

- `python tools/gc_sd.py OUT` lays out the card (OUT is the card's root or a folder to copy
  there; only changed files are copied): `nova3.dol`, `nova3/gcode.bin` (the translated code,
  paged from the card), `nova3/nova3_data/` (the game files: the slim data without the 3DS's
  packing, about 1.2 GB with the code). FAT32 (or exFAT) card.
- At start (`host/gc/main.cpp`, `mount_sd`) the program probes an SD2SP2 in serial port 2, then
  SD adapters in memory card slots A and B (libogc2's `__io_gcsd2`, `__io_gcsda`,
  `__io_gcsdb`) on a thread of its own at the lowest priority for at most 5 s (libogc2's
  driver busy-waits forever on Dolphin's serial port 2 device). A card with `nova3/gcode.bin`
  is mounted as `sd:` and holds the game files, the player's files (`nova3/user/`) and the log
  (`nova3/log.txt`, flushed every second and on a fatal stop, also from the page fault
  handler: the UART goes nowhere on a retail console). Without one, the disc as in Dolphin.
- Paged code and the library's read-only segment are then read from the card from inside page
  faults (interrupts on, as for the disc). That is safe as long as no thread touches a source
  page while it holds the card's locks: reads into guest memory hold them while they copy, but
  only into the heap (zero-backed pages), never into source pages.
- Not yet checked on a console: everything. Known differences from Dolphin to expect: timing
  (Dolphin's CPU and MMU timing is approximate), the video mode (the console's own preferred
  mode; no 480p yet), the SD card's speed inside page faults.

## The platform layer

- `runtime/os_gc.cpp`: the OS layer. `commit` maps zero pages in the pager; threads are LWP
  threads with MEM1 stacks (256 KB for the game thread, 64 KB for the others: the deepest use
  measured in the menus was 71 KB and 4 KB; painted so the watchdog can log the deepest use
  every 20 s along with the pager's counters and the host heap); ended
  threads are joined and freed by the next start or the watchdog; clocks from the time base.
- `runtime/gc_atomic.c`: GCC's out-of-line 64-bit atomics with interrupts off (one core, no
  libatomic in devkitPPC). The indirect-call cache of the translated code avoids them: on a
  32-bit host it holds the target's `g_funcs` entry, one word (`RtIc`, `recomp.h`).
- `runtime/net_none.cpp` in place of `net.cpp`: no sockets (socket() fails as on a device with
  no network); `select` and `poll` still wait on pipes and files.
- `runtime/gc_dvdfs.c`: the disc as `dvd:` (the FST read at mount, 64 KB bounce block, which
  serves later reads from the same block: the game files' stdio streams have no buffer of their
  own on the GameCube, `libc_io.cpp`);
  `runtime/gc_ramfs.cpp`: `ram:`, files in memory for the player's files while there is no SD
  card. `libc_io.cpp` uses the consoles' in-process pipes.
- `host/gc/main.cpp`: MEM1 split between the host heap (`kHostReserve`), the texture pool
  (`kTexturePool`) and the pager's frames, ARAM to the pager (shared with the textures' copies),
  the translated code mapped from `gcode.bin` (64 KB blocks, four cached, read with interrupts
  on from inside the fault), the JNI boot and frame loop of the other ports, GX
  (`rt::install_gx_gl`, `gx::swap` after each step). The log gets the host heap whenever it
  grew by 256 KB and the GX counters every 300 frames.
- **The device the game sees**: the 3DS's (`rt::low_end_device()`): the game's MEM_256 profile
  (its texture streaming and smaller pools) and GPU_0 (the low shaders, which the GX materials
  know). With the 1 GB phone's profile the guest's written memory outgrew MEM1 + ARAM in the
  menus. Raising the GPU profile is for later, once the materials cover its shaders.
- `host/input.cpp` (`poll_gc`): the controller in port 1. L aims and R fires, Z is the right
  bumper, the D-pad sprints / melees / left bumper / changes power in play and navigates in
  menus; rumble through the controller's motor. No system keyboard (text fields keep their
  text).
- `host/audio.cpp`: the audio interface's DMA, 32 kHz stereo, three 16 ms buffers; its
  interrupt queues the next buffer and wakes the feeder thread that mixes.

## Things the GameCube does differently (found in bring-up)

- **The library image**: `load_library` reads segments straight into guest memory and the
  tables (dynamic section, symbols, relocations) back from there, instead of reading the whole
  25 MB file into a host buffer first; a read-only segment is mapped from the file where the
  system can page it in (`os::map_file`: on the GameCube a source range of the pager), so only
  the pages the game touches are ever read. The writable segment (412 KB) is copied.
- **Exported names** are looked up through the image's own ELF hash table on every platform
  (`lib_export`); the map of 40,850 names cost ~5 MB of host heap, which ended the first
  GameCube run in an out-of-memory abort.
- **Freed heap memory**: `heap.cpp` tells the OS layer (`os::discard`) about the whole pages of a
  freed large block; the pager drops them instead of keeping them in ARAM.
- **Thread-local variables**: GCC's TLS uses r2 as the thread pointer, which is the EABI
  small-data base libogc2 and the prebuilt libraries use, and this devkitPPC has no emulated
  TLS: `thread_local` read garbage. The runtime's twelve thread-local variables go through
  `runtime/tls.h` (`RT_THREAD_LOCAL`, `RT_TLS`): plain `thread_local` elsewhere, an array per
  LWP thread slot on the GameCube, reset when a thread starts in that slot.
- **Logging**: newlib's `stdout` / `stderr` are per thread, so the UART writer is installed as
  newlib's device for descriptors 1 and 2 (`gc_report.c`), not as one thread's stream.
- **Service threads** (the log writer, the watchdog, the audio feeder) run above the game's
  threads: LWP runs only the highest ready priority (threads of equal priority share by time
  slices), so a busy game thread starved them.
- **MEM1 taken by the program**: unwind tables and small-data constants of the translated code
  were 3.7 MB of the DOL; built with `-G 0 -fno-asynchronous-unwind-tables -fno-unwind-tables`
  they go away or into the paged code. The guest sync table is 64 K entries (256 KB) instead
  of 1 M. The DOL is now 2.3 MB plus 0.7 MB of bss.
- **Indirect calls**: the call-site cache holds a pointer to the target's `g_funcs` entry (one
  32-bit word, no 64-bit atomics on the Gekko). A miss looks the target up in an index of the
  sorted `g_funcs` by 1 KB of guest code (80 KB) instead of the other ports' hash (1 MB).
- **libogc2's allocator** is its own dlmalloc (`malloc.o`), not newlib's `_malloc_r`: wrapping
  the allocator means wrapping `malloc` and its siblings.

## Plan

1. Toolchain and a test harness: devkitPPC + libogc, a DOL that boots in Dolphin (own user
   directory, MMU emulation on) and logs; Dolphin's MEM1 size override for early bring-up.
2. The pager on its own: page table, DSI / ISI handlers, MEM1 frames, ARAM with compression,
   tested in Dolphin.
3. Big-endian runtime: the accessors, the VFP union, the host-code audit.
4. Translated code for the Gekko: the paged code file, function ordering, VFP locals.
5. Disc image tool and the file layer on DVD reads.
6. GX backend.
7. Platform: video, controllers, audio, threads, the frame loop.
8. Bring-up in Dolphin, then measurement and optimisation on a console.

## References

- IBM, *Gekko RISC Microprocessor User's Manual* 1.2 (2000)
- Nintendo, *Nintendo GameCube Architecture Guide* (2004)
- [GameCube architecture (copetti.org)](https://www.copetti.org/writings/consoles/gamecube/)
- [VBA-GX](https://github.com/dborth/vbagx): `source/drivers/ogc/gamecube/vm/` (the pager)
- [libogc2](https://github.com/extremscorner/libogc2)
- [WiiBrew: Paired single](http://www.wiibrew.org/wiki/Paired_single)
