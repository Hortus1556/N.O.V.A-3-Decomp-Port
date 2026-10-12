# New Nintendo 3DS port

Status: in progress (started 2026-10-10). Target: **New 3DS / New 3DS XL** only (the old 3DS has
too little memory and CPU), capped at **30 fps** by default (60 selectable on the Graphics page);
the performance goal is 24-30 fps in play, with dips no lower than 24 fps.

## Why this is possible, and how it differs from the PC and Switch builds

The PC and Switch builds run the game's ARM code translated to C. The 3DS is itself ARM: its
ARM11 MPCore (ARMv6K, VFPv2, 4 cores at 804 MHz on the New 3DS) runs the game's ARMv7 code
**natively**, except for about 3% of the reachable instructions. So the 3DS build does not use
the recompiler's output at all: it loads `libGloftNAHM.so` like Android does (a "so-loader"),
patches those instructions, and runs it. This is faster than translated code and saves the
~70 MB the translated code would take.

| | PC / Switch | New 3DS |
|---|---|---|
| Game code | translated to C (`recomp/arm2c.py`) | the original ARM code, patched (`tools/v6patch.py`) |
| Guest memory | a 4 GiB window at `g_mem` | the process's own memory: `g_mem` = 0, guest address = host address |
| Imports, hooks | called by the generated code with a `Cpu *` | the same `Cpu *` functions, reached through assembly gates |
| Host -> game calls | `rt::call` runs the translated function | `rt::call` jumps into the game's code (assembly trampoline) |
| C++ exceptions | `restore_core_regs` replaced (`runtime/eh.cpp`) | the game's own unwinder, unchanged |
| GPU | GLES2 (ANGLE / Mesa) | PICA200 through citro3d: no fragment shaders (see Graphics) |

### Measurements that decided it

- **Instructions** (`tools/v6census.py`): of 4.95 M reachable instructions, ARMv6K + VFPv2 lack
  `dmb` (93 k), `movw`/`movt` (33 k), `ubfx` (12 k), VFPv3 `vmov` immediates (4 k), `bfi`, `mls`,
  `sbfx`, `bfc`, fixed-point `vcvt` (about 1.3 k together). NEON (813) and d16-d31 (118) sit in
  12 functions on paths taken only when `/proc/cpuinfo` reports NEON; the runtime never does.
- **Memory** (PC run with `NOVA3_DEVICE=3ds`, chapters 1, 3 and 8, the `memory:` log line): the
  game then picks its own low-end profiles from `GameOptions.json` (GPU_0: low shaders, no post
  effects, two mip levels skipped, LOD 2; MEM_256: texture streaming, 64 MB pool). Guest heap
  live at most 76 MB; textures at 3DS formats (ETC1 4 bpp, ETC1A4 8 bpp) 10-25 MB plus render
  targets; vertex buffers at most 13 MB. With the 25 MB library and 2.5 MB of stubs, about
  150 MB of the New 3DS's 178 MB application memory.

## The ARMv6K patch set (`tools/v6patch.py`, done)

Every reachable ARMv7 / VFPv3 instruction is replaced in place by a branch with the same
condition to a stub that does the same thing with ARMv6K / VFPv2 instructions and branches
back. Nothing moves, so code addresses, vtables, unwind tables and landing pads stay valid.
Stubs: 146,173 sites, 2.5 MB at link address `0x18f1000` (just past the image).

- **Which code is reachable** (`reachable_words`, 2026-10-12): one walk over the whole of
  `.text` from every known entry (function starts, pointer and mid-function entries, landing
  pads), with the Thumb functions and the literals marked visited beforehand; then a walk from
  each unexplored stretch's first decodable word and from every stub-patchable word left, until
  nothing grows. Walking each function alone stopped at branches out of its range, so code only
  such a branch reaches kept its ARMv7 instructions: the console stopped at a `ubfx` in zlib's
  `inflate` (0x3f6a20) when the game first read its `.obb`, which Azahar runs anyway.
- **Never a literal.** The literals are the words the code loads PC-relative (`ldr`, `ldrb`,
  `ldrh`, `ldrsb`, `ldrsh`, `ldrd` and `vldr` reading two); they and the code are iterated to
  a fixed point. A walk that runs on into a pool (after a call that does not return) takes its
  PIC offsets for instructions: the patch set of 2026-10-11 rewrote 17 such offsets (0x6a9994
  decodes as `mlseq`) into branches to stubs. The first pass's pool walks also yield garbage
  loads pointing into real code, and those targets would stay barriers (a `ubfx` in
  `btQuantizedBvh::walkStacklessQuantizedTree`, `dmb`s in `GS_OnlineMenu::Update`): a literal
  no reached code reads, but which reached code falls through or branches into, goes back to
  code for good. Result: 5,037,727 reachable words in 6 passes, no word both code and literal;
  every stub-patchable word left unpatched outside Thumb code (396) is a literal the code reads.

- `python tools/v6patch.py` writes `build/3ds/v6patch.bin`: `'N3V6'`, stub base, stub size,
  patch count (u32 each), the stub bytes, then (link address, new word) pairs. The loader copies
  the stubs to `load base + stub base` and writes each word at `load base + address`.
- The stubs are assembled by devkitARM with `.arch armv6k` / `.fpu vfpv2`, so they cannot
  contain an instruction the ARM11 lacks.
- **Barriers** (`dmb`, 93,392 of the sites): ARMv6's barrier is `mcr p15, 0, rX, c7, c10, 5`
  with rX holding 0; the stub saves r0 on the stack, zeroes it, restores it. Chapter 3's walk
  runs ~20,600 barriers a frame (counted in the PC's translation under `NOVA3_DEVICE=3ds`), the
  game's atomics (`dmb; ldrex ...; strex; cmp; bne; dmb`, mostly reference counts): some 0.15
  ms a frame of stub overhead in Azahar. Tried and dropped (2026-10-10): the barrier ending such
  a loop as the `mcr` in place with the `strex` status register (0 when the `bne` falls
  through; 11,391 sites), and one before an `ldrex rT` zeroing rT in its stub without the stack
  (38,645 sites). Unicorn found every site equivalent, but in Azahar the boot then ran away in
  Boost's exception destructors during the static constructors (a recursion down through the
  heap), and booted again with the stubs as they were. Explained later the same day (below):
  three sites were inside Thumb code, where the stub addresses become Thumb instructions.
- **Never inside Thumb code.** An ARM function's range ends at the next ARM start, so it can
  span Thumb functions (0x12d72f4 spans `boost::exception`'s destructor at 0x12d73f0), and its
  exploration read their halfword pairs as ARM words. Three pairs (`ldr r3, [r3]; adds r3, #8`,
  the destructor setting its vtable) decoded as `movw` and got an ARM branch written over them,
  which then ran as Thumb instructions ARMv6 lacks (`cbnz`): Azahar executed them (harmlessly,
  by luck of the stub addresses, until those changed), the console's ARM11 did not, and the
  first starts on a New 3DS stopped in the static constructors (Boost destroys exception
  objects there) with a data abort whose registers did not match its code. `reachable_words`
  now leaves out every address inside a Thumb function: 143,856 sites.
- **Thumb-2 long calls** (the cause of the console's start-up crash, 2026-10-11). The game's
  Thumb code (Boost.Thread, libgcc) calls far ARM functions with Thumb-2's 32-bit BL / BLX,
  whose J1 / J2 bits extend the range to +-16 MB. ARMv6 has only Thumb-1: there BL is a prefix
  (LR = PC + offset << 12) plus a suffix, +-4 MB, and a Thumb-2 suffix with a J bit clear runs as
  an ordinary B or Bcc. Azahar decodes the Thumb-2 forms, the console's ARM11 does not: Boost's
  `generic_category()` (static constructor 18) jumped into the middle of other code instead of
  calling `__cxa_guard_acquire`. 481 such calls sit in 127 Thumb functions (24 KB), none with
  free space within 4 MB, so `thumb_island` in `v6patch.py` moves those functions whole to
  right after the ARM stubs (at the same address modulo 4, so their PC-relative literal loads
  keep their offsets): every call out of a copy becomes a Thumb-1 BLX to a position-independent
  ARM veneer next to it (`ldr ip, [pc, #4]; add ip, pc, ip; bx ip` + the distance), the PIC
  literals their `add rX, pc` use are corrected by the distance moved, and each original entry
  becomes `bx pc` + an ARM `b` (+-32 MB) to a veneer entering the copy (8 bytes, enough for the
  smallest functions). Anything it cannot move safely (a branch or literal outside the function,
  an `add pc` without its literal, a PC read) stops the build. `thumb_island.json` lists the moved
  ranges for `hooktable.py`, which repeats the kernel-helper literal fixups in the copies (56
  instead of 53). Checked on Unicorn's ARM11 MPCore model at the real load base: from the
  original bytes `generic_category()` faults at its long BLX; patched, it reaches
  `__cxa_guard_acquire` through the copy. A first version put absolute link addresses in the
  veneers (the loader moves everything by the load base): Azahar ran into undefined
  instructions at 0x01b5e650 and hung. Azahar then ran chapter 3 at 27.8 fps through the copies.
- **Landing pads** (C++ exception cleanups; no branch reaches them, the unwinder enters them from
  the function's LSDA). The first island explored only from function entries and missed 22
  Thumb-2 long calls in Boost's pads (`blx _Unwind_Resume` at a cleanup's end), in functions it
  then left in place; its copies had no unwind table entries either, so an exception through a
  copy would have found none and ended in `std::terminate`. Now each Thumb function is explored
  from its entry and its landing pads (`ThumbFuncGen.explore(extra)`, the pads from
  `.ARM.extab`), so those calls are converted and their functions move (144 in all, 44 KB), and
  the island ends with an unwind table of its own: each copy's entry is the original's with the
  copy's address (inline unwind data and EXIDX_CANTUNWIND as they are, an `.ARM.extab` reference
  re-made relative to the new place; the LSDA stays where it is, and its landing pads are
  offsets from the function's start, so they land in the copy), sorted, with a CANTUNWIND end.
  `v6patch.bin` ends with the island's range and the table's address and entry count; the
  loader keeps them in `LoadedLib` (`moved*`), and `__gnu_Unwind_Find_exidx` (the game's
  unwinder asks it for every frame) returns that table for an address in the island. Checked:
  no Thumb-2 long call left in any Thumb function or copy (capstone over every range, literal
  pools excluded), the table sorted and each entry the original's for its copy (136 exact, 8
  inline / CANTUNWIND entries that cover more than one function).
- `python tools/v6patch.py --verify` runs every site in Unicorn: the original instruction on a
  Cortex-A15 model, the patched site on an ARM11 MPCore model, from random register and VFP
  states. Result: 143,856 sites, 0 mismatches. (Two `mls` with `sp` operands, data
  decoded as code, are left alone.)

## Memory layout (`runtime/os_3ds.cpp`, `runtime/native_3ds.cpp`)

- Application memory: the New 3DS 178 MB mode (`SystemModeExt: 178MB` in the exheader,
  `host/3ds/nova3.rsf`; a `.cxi` / `.cia` build, not a `.3dsx`).
- `g_mem` = 0, so `LD32(a)` reads host address `a`, `ptr(a)` is `a`; the runtime's guest/host
  conversions all become identities. `g_load_base` is a variable on the 3DS (`recomp.h`).
- `__system_allocateHeaps` (libctru's hook, `os_3ds.cpp`) lays the memory out: the regular heap
  96 MB, the most a console's kernel gives (0x08000000 up to `OS_HEAP_AREA_END` = 0x0E000000;
  Azahar allowed one up to the main thread's stack, 128 MB, and the first start on a console
  stopped right there, in `svcBreak` after the refused `svcControlMemory`), the linear heap the
  rest (~79 MB of the 175 MB free). The first 28 MB of the regular heap (`kNativeImageReserve`)
  hold the game: the library at `kNativeLoadBase` = 0x08000000, its ARMv6K stubs at +0x18f1000
  (2.4 MB), the hook trampolines at +0x1b5d000. The other 68 MB are shared by two heaps growing
  towards each other: newlib keeps 30 MB above the image (`kHostBudget`; peak ~27 MB), the
  guest takes the rest (~38 MB) and then up to the linear heap's size minus 40 MB for the GPU
  (`kGpuReserve`; the GPU's peak ~34 MB), in 1 MB segments. Chapter 3 (the guest's peak, 65 MB)
  ran with 24.7 MB of linear memory still free at its lowest. A guest allocation no heap can
  serve stops with "guest heap: out of memory" and the game's callers (instead of the game
  writing through a null pointer).
- **The guest heap is apart from the host's** (`runtime/guest_heap_3ds.c`): dlmalloc 2.8.6
  (`extern/dlmalloc/malloc.c`, MIT-0, with the `dl` prefix) serves the game's `malloc` / `free` /
  `calloc` / `realloc` (bound directly, no gate) and `gmalloc`; newlib's heap is the host's
  (libctru, thread stacks, the runtime's containers). newlib's grows up from the image (sbrk,
  limited by `fake_heap_end`), the guest's down from the top in 1 MB segments (dlmalloc's MMAP
  hook; adjacent segments merge): each new segment lowers `fake_heap_end`, under newlib's
  malloc lock, keeping 2 MB for the host. So a game overrun can only damage the game's own
  blocks (before, a guest block could sit next to a thread stack or libctru state), and a bad
  `free` or a damaged chunk header stops with `guest heap: ...` and the game's callers.
  dlmalloc's locks are libctru `LightLock`s (blocking; a spin lock could spin forever under the
  kernel's strict priorities). Nothing goes back to the system: each heap keeps its peak.
  `heap_usage` reports the guest heap (in use, footprint, peak footprint); the status line and
  the `memory: host heap in use` line newlib's.
- GPU memory (textures, vertex buffers, command lists, framebuffers) comes from the linear heap
  and VRAM (6 MB).
- Code pages ([base, base + 0x1855000) and the stubs + trampolines) become read + execute with
  `svcControlProcessMemory(MEMOP_PROT)` on a duplicated process handle (the pseudo-handle is
  refused), after `svcFlushProcessDataCache`; `svcBackdoor` then cleans the data cache and
  invalidates the instruction cache and branch predictor (`native_3ds_gates.S`), on each of
  cores 0, 1 and 2 (a short thread per core: every ARM11 core has its own instruction cache).
  Skipped only in Citra-family emulators (`native_emulator`: system info 0x20000 answers 1).
  The first check took a successful call for an emulator; Luma3DS answers it too, so the first
  console start skipped the invalidate and ran stale instructions left in the instruction cache
  by the program that had the pages before (FBI): a data abort at a `strex` whose registers did
  not match its code, in the static constructors. Azahar does not answer 1 either, so it now takes
  the console's path (which boots there).
- Guest threads use their host thread's stack (the size the game asks for, + 64 KB for the
  runtime's frames under the imports); `new_cpu` allocates no guest stack.
- Threads (`os_3ds.cpp`) are joinable: an ended one notes itself, and the next thread start
  (or the watchdog, every second) joins and frees it. A libctru thread created detached frees
  its memory, stack included, inside `threadExit` while it still runs on that stack (the end of
  `free` and of the lock release after it read return addresses from a block another core may
  already have been given); four of the game's threads end together at frame 1-2 of the boot.
- **newlib's heap is checked** (`guest_heap_3ds.c`, linked with `--wrap` of `_malloc_r`,
  `_free_r`, `_realloc_r`, `_memalign_r`): every free and realloc checks its block (inside the
  heap, its header whole, still in use: the next chunk's PREV_INUSE bit), every allocation that
  its block lies inside the heap. These read the block's two headers and the heap's bounds, no
  lock: 0.15 us for a free and an allocation in Azahar (`host heap: the checks ... take` at
  boot); the boot makes ~380,000 host allocations and frees, chapter 3's load ~840,000, play
  120-160 a frame (`memory: ... host heap: N allocations and frees` every 300 frames), so about
  60 ms of a 28 s load and 0.01 ms a frame. A bad call stops at once, with its caller and the
  damaged chunk and the one before it dumped (`host heap: ...`), instead of a fault later
  somewhere inside malloc. `env NOVA3_HEAP_CHECK 1` also walks the whole heap and its free lists
  after every frame of the boot and of loads, at each thread's start and end, and every second
  while a step lasts (the watchdog).

## Native execution (`runtime/native_3ds.cpp`, `runtime/native_3ds_gates.S`, `tools/hooktable.py`)

- **Loading** (`rt::load_library`): the PT_LOAD segments are read straight into place; the
  dynamic section, symbols and relocations are read from the loaded image; relocations as on
  Android (`R_ARM_RELATIVE` + base, `ABS32` / `GLOB_DAT` / `JUMP_SLOT` to symbols). Then the
  patch set (`romfs:/v6patch.bin`), the trampolines, the hook patches and the kernel helper
  fixups are written, and the code made executable.
- **Imports**: each function import points at a gate (2048 gates, one per import slot,
  `mov r12, #hi; orr r12, r12, #lo; b rt_gate_common`; `rt::import_function_address` returns
  the gate on the 3DS, so JNI tables and `dlsym` hand out gates too). The common gate stores
  r0-r12, sp (at the call), lr into the thread's `Cpu` (`ret_pc` = lr), calls
  `rt_call_import(cpu, slot)`, reloads r0-r3 from the `Cpu` and returns to the lr it kept on
  its own stack (a nested guest call reuses the same `Cpu`). Host C code preserves r4-r11 and
  d8-d15 by AAPCS, so nothing else is restored. Data imports get `data_import_address` as on
  the PC.
- **Direct imports** (no gate): integer-only C functions with the same meaning in bionic and
  newlib are bound to newlib itself: `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`,
  `strlen`, `strcmp`, `strncmp`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strchr`, `strrchr`,
  `strstr`; the guest heap (`malloc`, `free`, `calloc`, `realloc`: dlmalloc); bionic's C-locale
  ctype and case folding (`isalnum` ... `isxdigit`, `tolower`, `toupper`, `strcasecmp`,
  `strncasecmp`: small native functions); the math functions (`sinf`, `atan2`, `pow`, `frexp`,
  ... 35 in all) through `pcs("aapcs")` wrappers, which take and return floats in core
  registers as the game's softfp code passes them; the game's most frequent thread calls
  (`pthread_mutex_lock` / `unlock`, `pthread_getspecific`, `pthread_self`: thread.cpp's
  `rt_im_*` entries, the ones the PC's recompiled code calls directly too); and `setjmp` /
  `longjmp` (native assembly on the game's `jmp_buf`: r4-r11, sp, lr, d8-d15). The
  core-register calling convention is the same for the game's softfp code and the host's
  hard-float code when no float is passed. In the title menu this took the gated imports from
  about 1,000 a frame to 300 (nearly all GL); during the boot load, 6,700 mutex calls a frame
  (the game's allocator locks one per allocation) no longer pass a gate.
- **GL calls without a gate**: 300 (menus) to 3,400 (chapter 3's busiest frames) a frame. Every
  typed import whose arguments and result are all words (`rt::direct_entry`, `runtime.h`: no
  float, no 64-bit value, no `Cpu *`) registers its function too, and the game's GOT slots of
  such GL imports point straight at `runtime/gles.cpp`'s wrappers (`w_gl...`), except in the 1
  frame of 8 whose GL time the perf report measures: `runtime.cpp` calls `g_gl_timing_hook` at
  those frames' ends and `native_3ds.cpp` writes the gate addresses into the slots for that
  frame (the report's "GL" therefore includes the gates' cost; "game code" in the other frames
  does not). `NOVA3_IMPORT_PROFILE=1` keeps every gate (it counts the calls there). GL calls
  with float arguments (`glUniform1f`, `glClearColor`, ...) keep their gates.
- **Hooks** (`OVERRIDES` in `recomp/arm2c.py`, minus the recompiler-only ones:
  `restore_core_regs`, the `host/native_hot.cpp` functions): `python tools/hooktable.py` writes
  `build/3ds/native_table.inc` (hook table, trampoline code, kernel helper fixups, the PLT
  import names). A hooked function's first 8 bytes become `ldr pc, [pc, #-4]` + the address of
  hook gate k (256 gates), which saves the registers like an import gate and calls the host
  function with the function's link address as `entry`. A wrap's `original(cpu, entry)` runs
  the trampoline (assembled by devkitARM at its link address after the stubs, so its branches
  reach the game): the two displaced instructions, then `b function + 8`, with r0-r3 from the
  `Cpu` and the 16 words at the caller's sp copied as stack arguments. The generator checks
  every hook: an ARM function at least 8 bytes long, no branch to its second word, no loop back
  to its first; a displaced instruction the patch set replaced is emitted as its stub body, a
  `b` / `bl` is re-targeted, a literal load (`ldr rX, [pc, #imm]`) gets a copy of its literal,
  `ldr rX, [pc, rY]` is rebuilt from a PC-relative offset; any other PC-relative instruction is
  rejected. Result: 137 hooks, 94 trampolines (1.2 KB), none rejected.
- **Host -> game**: `rt::call` / `call_nested` (`rt_native_enter`) load r0-r3 from the `Cpu`,
  copy any stack arguments onto the real stack and `blx` the target; r0-r3 come back in the
  `Cpu`. As on the PC, `rt::call` keeps the `Cpu`'s r4-r13 (the gates of nested calls overwrite
  them) and leaves r14 = `kReturnSentinel`. The game runs on the calling host thread's stack;
  the thread's first call notes that stack's top for crash reports (`cpu->r[13]` is otherwise
  only what the last gate saw). The gates find the `Cpu` through a thread-local set by
  `rt::call` (host threads calling into the game bring their own).
- **Linux kernel helpers**: 53 literal-pool words hold `0xffff0fc0` (`__kuser_cmpxchg`, 48) or
  `0xffff0fa0` (`__kuser_memory_barrier`, 5), loaded by `ldr rX, [pc, #imm]` before `blx rX` /
  `bx rX` in the Thumb-1 Boost.Thread code (`thread_detail::commit_once_region`, ...) and in
  libgcc's ARM atomics (`__sync_*`). The loader writes the address of an ARMv6K implementation
  (`ldrex` / `strex` with `mcr p15` barriers) into each; `0xffff0fe0` (TLS) and `0xffff0ffc`
  (version) have implementations too but no references.
- **Device profile**: `rt::low_end_device()` is always true: the game is told it has 256 MB,
  one 804 MHz core and an Adreno 200, and picks the profiles above.

## Graphics (`runtime/gles_pica.cpp`, `runtime/pica/`)

The PICA200 has programmable vertex shaders but a fixed-function fragment stage (six texture
combiner stages, three 2D texture units, fog, alpha test). The game's GLES2 calls are kept:
`runtime/gles.cpp` does what it does on every platform (guest pointers, skipping repeated
state, the uniform cache, the program pool), and `runtime/gles_pica.cpp` is its driver on
citro3d. `rt::install_pica_gl()` starts citro3d (1 MB command buffer, the screen as a 240x400
RGBA8 + D24S8 target) and hands `gl_load()` the backend's 100 functions; `pica::swap()` after
each `step()` submits the frame (it begins lazily at the first draw); `pica::set_texture_pack()`
names the pack of large images (below). The game is told it has a 400x240 screen in play and a
320x240 one in the menus; `pica::set_screen()` moves the target's output between the top and
the bottom screen (Platform: Screens).

### What the console's GPU needs that Azahar does not check

Found on a console (Console bring-up, below); Azahar draws without any of them:

- **Caches.** The GPU reads memory, not the ARM11's data caches: the CPU's writes for it are
  flushed by whoever writes them (`gpu_flush`, which logs a refused flush: buffers and textures
  at their upload, the vertex ring before each submission, `ring_flush`, its overflow blocks
  too), and each command list by GSP (`GX_CMDLIST_FLUSH` on every split and at the frame's end,
  not citro3d's whole-linear-heap flush of `C3D_FrameEnd(0)`). A texture written in place gets
  every unit bound again at the next draw (`tex_written`), which clears the GPU's texture cache.
- **Shader outputs.** Every output register of the vertex shaders is written whole, exactly once
  (3dbrew GPU/Pitfalls: otherwise the PICA freezes); texture coordinates get z, w = 0, 1.
  `tools/3ds_shadercheck.py` checks the binary.
- **Vertex stride.** A vertex buffer's components (attributes and padding) add up to its stride
  (`buffers`: the rest of the vertex as padding); a layout that cannot be described is logged
  once (`a vertex of N bytes described by M`).
- **Index numbers.** The PICA keeps transformed vertices in a post-vertex cache by index number,
  and citro3d's clear after each draw (`GPUREG_VTX_FUNC`) did not clear it on the console: a
  draw got vertices of the draw before. Every indexed draw is given index numbers no draw used
  lately (`fresh_indices`: the indices rewritten into the ring as 16-bit `i - lo + k`, `k` from a
  counter that wraps at 65536, the vertex buffers' addresses moved back by `k` vertices); the
  GPU never reads the game's element buffers directly. Each pass of a draw (the terrain's second)
  takes its own. The cost: the indices scanned and rewritten on the CPU for every draw.
- **Precision**: below.

At the start the backend checks the console's behaviour and logs it: `pica: cache check:` (does
the GPU see the CPU's writes, after no flush, a CP15 clean of the L1, GSP's flush of the buffer
and of the whole linear heap: the L2 held none of them on the test console), `pica: draw test N`
(five quads drawn and read back: citro3d alone, fixed attributes, indexed, the backend's uniforms
and draw, pixel coordinates with blending; all right on the console), and `pica: startup check:`
(the first three frames' draws each submitted and waited for, the GPU's busy and command-list
registers logged if one does not finish). `read_screen` (screenshots, `glReadPixels`) has the GPU
copy the colour buffer from VRAM into linear memory first.

### Precision (the console's float24)

The PICA's vertex shaders compute in float24 (a 16-bit mantissa); Azahar computes in float32, so
precision problems show only on a console. The levels' static geometry is in world coordinates
(thousands of units), and a depth computed from them cancels large terms: nearly coplanar
surfaces fought in jagged patches. `position_origin` keeps a buffer object's float positions
relative to the centre of the vertex range a draw uses (a list of recentred ranges per buffer,
`Origin`: made at the range's first draw when its centre is 64 units or more from the origin,
overlapping ranges merged, the buffer copied first when this frame's earlier draws still read it;
cleared by `glBufferData`, applied to what `glBufferSubData` writes), and `setup` folds the centre
into every uniform that multiplies the position: the position matrix, the fog's `vz` and `wz`
rows, point2's `world` rows (each row's w gains row . centre) and lit's object-space light
position (minus the centre). Client-memory positions (copied for each draw: the hands and the
weapon, decals, particles) are recentred in the copy: the centre of the draw's positions, when 64
units or more from the origin, is subtracted as they are copied (`attrib_source`) and folded the
same way.

### What the game draws (traces)

`NOVA3_GLTRACE=dir` (PC, `runtime/gles.cpp`) records every GL call that reaches the driver,
with the data it references, into `dir/gltrace.bin`: object, upload, shader and query calls
from the start, state and draw calls only in the frames listed in `NOVA3_GLTRACE_FRAMES`
(`700,3200-3209`, the `--screenshot` numbers), which start with a snapshot of the state the
game had set. `tools/3ds_glreplay/gltrace.py programs|stats|draws|dump` reads a trace (`draws`:
each draw with its program, vertex sources, target, depth range, blending, polygon offset).
Traces were taken with `NOVA3_DEVICE=3ds --size 400x240`: the title menu (25 MB), the first
campaign level (55 MB: a cinematic, an office, a rooftop and ten consecutive frames), chapters
3, 5 and 7 (`--tour`: the ship, Boreas' surface with 310 draws, the volcanic caves), chapters 8
and 9 firing from frame 1100 (`build/3ds/trace_more.sh`; chapter 10's tour does not reach frame
1300) and a local multiplayer match, PC screenshots of the same frames beside them in
`build/3ds/traces/`.

- **ETC1**: with `GL_OES_compressed_ETC1_RGB8_texture` in the extension string the game
  uploads its `.etc` textures as they are (`glCompressedTexImage2D`) instead of decoding them
  to RGB: the menu's uploads fall from 120 MB to 20 MB. The game only takes an extension name
  that is followed by a space (ANGLE's string ends with one; a name at the very end is
  ignored). When tracing on the PC the name is added and the ETC1 data decoded for ANGLE.
  Every diffuse texture has its alpha in a second ETC1 texture (`SPLIT_ALPHA`, from
  `GLITCH_USE_HIGHP`).
- **Programs**: the menu links 56 programs and draws with 7, the first level links 150 and
  draws with 17 (`NV_Lightmap` low for the world: 552 of the ten frames' ~900 draws; lightmap
  with a diffuse mask; irradiance-lit characters; a two-point-light weapon shader; UV-animated
  and additive effects; the unlit texture x vertex colour UI programs); chapters 3, 5 and 7 link
  146-176 and draw with 10-16 (adding Boreas' terrain, three maps weighted by the vertex colour).
- **Depth range**: every frame the sky is drawn at depth 0.99999 and the first-person weapon in
  0..0.001 (`glDepthRangef`).
- **Skinning**: no recorded frame draws with a `SKINNED` program. GPU_0's `HardwareSkinning`
  is off, so Glitch skins on the CPU and draws characters with the ordinary programs from
  client memory (the multiplayer trace's first-person arms: 3,462 vertices copied every frame);
  the skinned variants are only linked.
- **Not used**: polygon offset is set once at start (`glPolygonOffset(0, 1)`, disabled) and
  never enabled, also in chapters 8 and 9 while firing (bullet impacts are particles);
  `glCopyTexSubImage2D` is never called.
- **Load**: 69-93 draws a frame in the level, 76 of 93 from client memory (Glitch batches small
  meshes into its own client-side buffers every frame); the menu draws its interface at 960x640
  into 1024x1024 RGBA textures (three, with one 1024x1024 depth renderbuffer) and composites it.

### Materials

At link time the backend parses the program's GLSL (a small preprocessor for Glitch's
`#define`s, then the `attribute` / `uniform` declarations: `glGetActiveUniform` and the
locations answer from these) and picks a recipe from the samplers, uniforms and defines
(`classify`); at each draw the recipe sets the vertex shader's uniforms, the texture units and
the combiners. The parsing, the programs' uniforms and `classify` are in
`runtime/gl_program.cpp`, shared with the GameCube's GX backend (`gles_gx.cpp`). Three vertex shaders (one DVLB): `generic.v.pica` (position, three texture
coordinates from three UV sets through 2x4 matrices, colour x scale + bias; the height fog's
factors behind a bool uniform), `lit.v.pica`
(the NV_NormalRim / NV_DiffuseSpec low lighting: second-order spherical harmonics on the world
normal x 2 plus one point light in object space; three coordinates from UV0),
`point2.v.pica` (two world-space point lights and an ambient term); the lit ones output their
colour halved (Lighting range, below). Combiner setups:

| Recipe | Programs | Combiners |
|---|---|---|
| texture x vertex colour | UI, unlit models, particles; split alpha from unit 2's red; `Ambient` / `FadeFactor` folded into the vertex colour | 1 stage |
| lightmap | NV_Lightmap (diffuse, lightmap, alpha texture) | diffuse x lightmap x 2 |
| lightmap TEX2 | NV_Lightmap TEX2 (vertex red, with `GLOW` vertex alpha, blends two diffuse maps; their alpha maps do not fit) | interpolate, then x lightmap x 2 |
| mask | `DiffuseMaskSampler` (with or without lightmap): alpha = mask green x vertex alpha | 1 stage |
| glow | two point lights: diffuse x light + diffuse x (1 - diffuse alpha) | 2 stages |
| terrain | red / green / blue maps weighted by the vertex colour, x lightmap x 2: four textures, so the blue map is a second pass over the same vertices (depth EQUAL, blending ONE + ONE, fog toward black) | 3 + 2 stages |
| two textures | Glitch's texture1 x texture2 x vertex colour, and mix(texture0, texture1, factor) | 1-2 stages |
| tinted | irradiance-lit models with `COLORMASK` (multiplayer team colours): texture x mix(1, `ColorMaskColor`, mask red) x lighting | 3 stages |
| vertex colour | colour-only programs (x `DiffuseColor`) | 1 stage |

Also covered: Glitch's interface programs whose sampler is renamed by a `#define`
(`texture_glitchprops_*`) and its fixed-function emulation (`Sampler0`, colour x
`DiffuseColor`, its per-vertex lights left out). Not drawn: skinned programs (never used, see
above), screen-space distortion and post effects (GPU_0 turns those off).

**Height fog** (`FOGZ`: Boreas' ship, the volcanic caves' lava glow): the game mixes toward
`FogZColor` by (1 - min(height term, the camera's)) x clamp(distance / `FogZ.z`) [x vertex
alpha], after its distance fog. The PICA's fog unit holds one fog and applies it after the
combiners, so for these programs both fogs go through the combiners: `generic.v.pica` (its
`fogzon` branch) computes both factors per vertex (GLSL's x / 0 clamps like a huge factor),
in the colour's green and blue for the world shaders (which use only its red and alpha), or as
texture coordinate 1 into an 8x8 ramp texture on the free unit 1 for the UV-animation effects
(ADDITIVE ones darken by the factor instead of mixing).

The menus' composite shader divides rgb by alpha before blending with `SRC_ALPHA`; the
combiners cannot divide, so that blend uses `ONE` (the same result). A texture that is not
bound reads as a constant (white, a lightmap as 0.5). A program without a recipe draws nothing
and is logged once (`pica: no material for program ...`).

**Coordinates and depth.** The GL matrices are turned into the PICA's on the CPU, in the
position matrix the shader gets: on the screen a quarter turn (the framebuffers are portrait), and z from [-w, w] to [0, -w]. Perspective draws use W-buffering with depth = view
distance / the farthest far plane of the previous frame (5000 in level 1): uniform precision
(24 bits over 5000 units) instead of a z-buffer's, and a depth that is the view distance for
every projection (the weapon's too). The game's fog is per vertex and linear in that distance
(`FogStartEnd`), so the PICA's fog unit, whose 128-entry table is indexed by depth, reproduces
it exactly and leaves the vertex colour free. W-buffered depth is `s z' + o w` with the PICA z'
affine in w for a given projection (`z' = alpha w + beta`), so `glDepthRangef(n, f)` maps
exactly too: `s = n / beta`, `o = (f - n) / far - s alpha`. `glPolygonOffset` (unused by the
game) adds a constant 2^-17 of the depth range per unit of factor + units to n the same way
(the PICA has no slope term).

**Textures.** Stored in the PICA's tiled order with GL's first row first; the vertex shader
flips t (and scales it for images padded to a power of two, whose last row and column are
repeated into the padding) instead of the data being flipped, which ETC1 blocks would not
allow without loss. ETC1 blocks are reordered into the PICA's 8x8 tiles and byte-swapped. RGB
uploads become RGB565, RGBA RGBA8 (BGRA swizzled), luminance / alpha L8 / A8 / LA8. Mip levels
under 8x8 are dropped. Render targets (`glTexImage2D` without pixels) live in VRAM at most
512x512 (the menus' 1024x1024 interface layers at half size: draws into them are scaled to
match) with one shared depth buffer per size. Images over 1024 (the menus' 2048x2048 sprite
atlases, 9 images) come from `nova3_data/pica_textures.pak`, made offline by `python
tools/3ds_glreplay/picapack.py build` (halved, encoded as ETC1 again, already in the PICA's
layout; 4.3 MB from the slim data, see Slim data; made from `dist/nova3_data` it also holds 10
mipmapped skyboxes and backgrounds that the 3DS's profile never uploads at that size: 19
images, 9.3 MB), keyed by the CRC-32 of the uploaded ETC1 bytes. The pack's encoder searches
every table, both partitions and modes and brightness shifts of each subblock's
average for the least RGB error (mean error 3.1 instead of the runtime encoder's 4.4 on a menu
atlas). Without the pack the backend converts them at load: about 3 s an atlas on the ARM11
(the title menu has six).

**Buffers and memory.** Vertex and index buffers, textures and a 4 MB per-frame ring for
client-memory vertex data are linear memory. A draw from client memory copies the vertices it
uses (the index range) once per interleaved stream into the ring and rebases its indices
(not when the range starts near 0: the indices are copied as they are); 32-bit indices are
converted to 16-bit. Attributes interleaved in the same vertices become one PICA vertex
buffer with a component list (padding for gaps), not one buffer each. Anything the GPU may
still read is freed after the next frame start has waited for the GPU; a buffer or texture changed after a draw in the same
frame gets new storage (copy on write). Level 1: textures 13.1 MB, buffers 13.1 MB, ring peak
0.8 MB; chapter 5: 9.6 and 8.5 MB; the title menu: textures 12.4 MB (the interface layers in
VRAM). The game needs a linear heap of at least 40 MB.

### Verified (`tools/3ds_glreplay`)

A replay app (`make` in `tools/3ds_glreplay`: `build/3ds/glreplay/glreplay.3dsx`) plays a trace
through the backend in Azahar, mapping the PC driver's names and locations to the backend's,
saves each recorded frame as a PNG (`--targets`: the render-target textures too) and logs its
timings; `python tools/3ds_glreplay/run.py TRACE [--repeat N]` runs it with `tools/3ds_run.py`
and collects the results in `build/3ds/replay/<trace>/` (several traces play in one emulator
start: `run.py A/gltrace.bin B/gltrace.bin ...`, the backend's objects deleted in between);
`gltrace.py frames` lists a trace's recorded frames (draws, client bytes, calls) and `gltrace.py
cut` keeps only some of them; `python tools/3ds_glreplay/compare.py
[TRACE ...]` compares them with the PC's screenshots (`cmp_<frame>.png`: PC left, 3DS right, the
difference x4 below). Mean absolute difference per channel (of 255), and the backend's CPU time
per frame (Azahar, `--repeat 2`), at the start of this work -> before the backend sent citro3d's
state itself (`send_state`, below) -> now:

| Trace | Frames | Difference | Draws | Backend CPU (Azahar) |
|---|---|---|---|---|
| title menu | 3D background, interface layer | 4.9 -> 4.8 | 25 | 1.0 -> 0.8 ms; wall 16.7 -> 6.0 ms (the VBlank wait, below) |
| chapter 1 | office (cinematic lights), rooftop | 0.8, 3.1 | 69, 93 | 7.8 -> 5.9 -> 5.5 ms (700 KB of client vertices copied), 4.4 -> 3.8 -> 3.1 ms |
| chapter 3 | ship tunnel (height fog) | 1.33 -> 1.04 | 50 | 3.5 -> 2.9 -> 2.6 ms |
| chapter 3, bench walk | the 3DS's slowest part (light shafts) | 3.6, 8.4 (the weapon's pose), 1.5 | 222, 209, 118 | 7.7, 7.3, 4.4 -> 5.9, 5.5, 3.6 ms |
| chapter 5 | Boreas surface (terrain) | 9.0 -> 3.1 | 310 (+3 terrain passes) | 10.8 -> 9.3 -> 7.1 ms |
| chapter 7 | volcanic cave | 2.5 -> 1.6-2.0 (weapon lighting) | 84 | 4.4 -> 3.4 -> 2.9 ms |
| chapter 8 | caves, firing | 1.7-1.9, 1.7 | 66, 72 | 2.6, 3.3 -> 2.2, 2.8 ms |
| chapter 9 | lava field, firing | 13.0 -> 10.2-10.5, 9.5 -> 6.3 | 60, 65 | 4.2, 3.6 -> 3.8, 3.1 ms |
| multiplayer | local match (team colours) | 1.2-1.8 -> 1.3-1.7 | 33-35 | 2.2 -> 1.9 -> 1.7 ms |
| multiplayer map 0 | the room with light lines | | 35 | 3.1 -> 1.9 ms |

The send_state change left every replayed frame as it was, pixel for pixel, but the first one of
some traces: the first recorded frame of a trace differs between emulator starts and with what
was replayed before it in the same start (chapter 7: 1.6 or 2.0; chapter 8's second frame after
another chapter 8 replay: 20 pixels), with the old path too. `run.py --debug 1,0` plays the
traces of one start with the old path (`pica::set_debug`) and the new one, for comparisons that
do not depend on it: identical images, chapter 3's bench walk 7.6, 7.2, 4.3 -> 5.9, 5.5, 3.6 ms.

The multiplayer match was started at 400x240 with `--lua Obj150346_OnClick()`: the match
setup screen's Continue button then lay outside a 400x240 screen (fixed since: see Menu layout
at 400x240).

**The frame start.** `C3D_FrameBegin(C3D_FRAME_SYNCDRAW)` also waits for the next VBlank
(`C3D_FrameSync`), and the frame begins inside the game's first draw or clear: in the game
that wait (8-9 ms a frame at the title menu, on top of the host's own frame cap) was counted as
GL time and cost frames (26 instead of 30 fps). The backend now begins with `C3D_FrameBegin(0)`,
which waits only for the previous frame's GPU work and its transfer to the screen (citro3d
still swaps at VBlank). In the game (chapter 0, Azahar): GL 11.2 -> 2.5 ms a frame, of which
1.9 ms in the backend (`NOVA3_GLPROFILE`); the rest is the imports' gates, the wrappers and
the per-import timer.

**CPU.** The replay times every backend call per function, less the timer's own syscalls and
the wait for the GPU; `make PROFILE=1` adds where each draw's time goes (`PROFILE_MARK`:
target, material, vertex data, textures and fragment state, the state sent, the draw
commands). `NOVA3_GLPROFILE=1` logs the same per-function split in the game ("gl profile:"
with the perf lines), less one timer read per call (a kernel call: before, the 679
`glVertexAttribPointer` calls of chapter 3's busy frames showed 0.9 ms, nearly all of it the
timer's; the raw total is logged beside). Azahar's clock does not model the New 3DS's 804 MHz
mode: a fixed loop takes 224.7 ms with and without `osSetSpeedupEnable`, about one instruction
per 268 MHz tick, so these figures are an upper bound; real hardware should need well under
half. citro3d sends again any state it is handed, so fragment state, vertex layout, fixed
attributes, combiners (built per recipe, textures present and the constants they use), texture
bindings and uniform registers are compared with what was last sent.

**The state a draw sends** (`send_state`). citro3d's setters only note the state in its
context and set a flag; at every draw `C3Di_UpdateContext` sends what is flagged, one function
call per register write, and also scans all 96 float uniforms of the vertex and of the geometry
shader for changes: 10 us of a draw's 35 in chapter 3's busy frames (profile build). The backend
still calls the setters (so citro3d's context stays right, and a return from the HOME menu
sends it all again; an APT hook then marks the backend's uniforms to be sent too), but writes
the state that changes from draw to draw itself, straight into the command buffer, with the
same register writes as citro3d (`C3Di_AttrInfoBind`, `C3Di_BufInfoBind`, `C3Di_EffectBind`,
`C3Di_SetTex` and the unit configuration, `C3Di_TexEnvBind`, the fog's mode, colour and table),
and clears those flags; the uniforms `set_fvec` changed go out as runs of registers from its own
dirty bits, not through citro3d's. Anything else pending (render target, viewport, scissor,
shader, fixed attributes, the bool uniform) still goes through `C3Di_UpdateContext`. The
offsets of citro3d's private context come from devkitPro's `libcitro3d.a` (its code read with
objdump); `c3d_check` verifies them at start with citro3d's own setters, and if they differ
every draw goes through citro3d as before. Chapter 3's busy frames (profile build): state 2.3 ->
0.6 ms, draw commands 1.3 -> 0.2 ms a frame. Per draw:

- A draw with the same material as the last one (same program, no change to a uniform the
  material reads, same textures and attribute formats: `MaterialKey`) only sets its matrices;
  uniforms the recipe does not read (the game sets `WorldViewT` for every object) do not
  count. Chapter 5: 235 of 310 draws.
- Interleaved attributes share one vertex buffer (above); client indices are copied without
  conversion when they need no rebasing; 16-bit index ranges are scanned two at a time.
- The draw's commands are written straight into the command buffer (`draw_elements`: the
  words `C3D_DrawElements` writes, without a function call per register), after `send_state`;
  the list is marked as used (`C3DiF_DrawUsed`) as citro3d does.
- Texture and framebuffer objects are cached pointers (no hash lookups per draw), uniforms go
  to the current program directly; the material's key is 132 bytes, built field by field.
- `glVertexAttribPointer` only stores its arguments; the layout is worked out at the draw.

Tried and dropped, measured A/B in one emulator start (each cost about what it saved, or more):
the combiner stages cached per recipe, each program's last material kept (its uniform registers
and stages written again when two materials take turns), the last four vertex layouts kept, and
batching (below).

Chapter 5's 310 draws (profile build, before the command change): citro3d's own state 2.7 ms
and draw commands 2.8 ms (now 1.9), the material 1.0, the vertex attributes 1.8, the rest
under 1 ms each. What is left is mostly citro3d's per-draw state (uniform upload, texture and
buffer configuration). Azahar does not model the PICA's speed either (`C3D_GetDrawingTime`
reads ~0.3 ms): GPU time needs real hardware.

**Chapter 3's busy part** (the 3DS's slowest bench: 21 fps, "GL" 20 ms, 198 draws a frame in
Azahar; `build/3ds/trace_ch3bench.sh` samples the PC's bench walk every 200 frames, and
`traces/ch3busy` keeps its frames 1000, 1200 and 2200): 222 draws, 126 of them one additive
UV-animation material (the light shafts), 74 program switches, 679 `glVertexAttribPointer`
calls, 166,000 indices from client memory (copied into the ring: 330 KB a frame). Before
`send_state` the replay's draws took 7.7 ms (35 us each; profile build, per draw: citro3d's state
10 us, the draw commands 6, index copies and attribute setup 10, the material 6); now 5.9 ms.
In the game (`NOVA3_GLPROFILE`) the driver took 10.6 ms a frame there, 7.4 of it in the 198
draws, and "GL" 19.6 ms on the timed frame (gates and wrappers for 2,000 calls); the game code
24.7 ms. Cached vertex-buffer and texture pointers and keeping the bindings of units the
combiners do not read had saved only ~0.2 ms (10.74 -> 10.58).

Batching consecutive draws of one material does not pay there. Not one of the 222 draws has the
complete state of the draw before it (program, uniforms, textures, fragment state, vertex
sources): the light shafts come in runs that take turns with an alpha-blended fog material and,
within a run, between six large world vertex buffers, each draw with its own client-memory
index list (30-900 indices spread over up to 1,200 vertices of its buffer); 70 draws differ
from the one before only in the vertex buffer. Merging those was tried both ways. Copying the
vertices both draws read into one block cost far more than a draw (2.7 MB a frame: the index
ranges are sparse). Without copies, with buffer objects placed at multiples of 384 bytes so that
buffers of the common vertex sizes lie whole vertices apart and one draw reads both from the
lower one, only 16-18 draws a frame were within 16-bit reach of the one before, and holding
back each draw and comparing the next cost more than the merges saved (6.0 -> 6.5 ms; chapter 5
7.5 -> 8.0 ms). Both were removed. Transforming the light shafts' vertices on the CPU would not
help either: they differ in vertex buffer, not in matrices.

In the game, chapter 3's bench walk (`bench 3 900 walk`, one run each, Azahar), the busiest perf
window, before -> after `send_state`: 24.3 -> 25.5 fps, frame 41.1 -> 39.3 ms (step 40.1 = GL
14.5 + game code 23.0 + file 2.3 -> 37.9 = 14.1 + 21.7 + 2.0); the whole walk 27.8 -> 28.3 fps.
Other runs of the old build gave 20.9-24.5 fps there (where the 300-frame windows fall). With
`NOVA3_GLPROFILE` (its timer adds ~5 ms a frame there): 20.9 -> 22.2 fps, 47.0 -> 45.1 ms; the
driver 6.4 ms a frame (9.0 with the timer, 10.6 before), its 200 draws 5.6 ms (5.8 with the
timer, 7.4 before), 28 us each in Azahar. What is left per draw: working out the material when
two take turns, the vertex layout and attribute sources, the client-memory index copies (330 KB
a frame), and outside the driver the imports' gates and `gles.cpp`'s wrappers (on the timed
frame, 1 in 8: "GL" 14 ms for a driver time of ~6).

**Lighting range.** The irradiance and point-light shaders' colour reaches 2-4 (spherical
harmonics x 2 plus a light of 2.1 in the volcanic caves); the PICA clamps vertex colours to
1, which made the weapon dark and green. `lit.v.pica` and `point2.v.pica` output it halved and
the combiners double it (`GPU_TEVSCALE_2`).

**Pitfalls found.** `C3D_TexBind(unit, NULL)` dereferences the texture (Azahar logs an
unmapped read at 0xC and goes on; hardware would fault): units without a texture get an 8x8
blank one. A shader bind writes its `.constf` constants behind citro3d's copy of the uniform
registers, so those registers are written again after a bind. `glDepthRangef(0.99999,
0.99999)` through the W-buffer formula cancels two terms of ~10 in the PICA's 24-bit floats:
a constant range uses the z-buffer path with scale 0 (exact). The depth scale (the farthest
far plane of the previous frame) carries over: replays in one launch reset it between traces
(`pica::reset_depth_scale`), or chapter 5 after chapter 3 got the wrong fog table. picasso makes one DVLE per
source file and ignores a second `.entry` in a file (the height fog is a `.bool` branch of
`generic.v.pica`). Once, a chapter 9 replay right after a rebuild showed only the clear and a
faint interface; two reruns of the same build were correct (not explained). The replay reads a
frame back after citro3d's queue is done (`C3Di_RenderQueueWaitDone`): `gspWaitForP3D` could
return at once on an interrupt nobody had waited for (the frame between two traces).

**Glitch's texture warm-up** (`defaulteffects.bdae`, Force Commit Texture 1D / 3D / Rect, at
boot): those programs declare `sampler1D`, `sampler3D` or `sampler2DRect` unless `GL_ES` is
defined, so the reflection has no `texture` uniform and Glitch logs "failed to find uniform
information" and falls back to its pink wireframe for them; no draw with either reaches the
backend (no skipped draw or primitive is logged), so nothing pink is drawn. Defining `GL_ES` as ANGLE does
made them link, but then chapter 3's `NormalRimDistort` material became invalid ("MaterialRenderer
is not valid, using pink wireframe", "Material renderer name exists already: <NULL>") and the game
hung in `CMaterialRendererManager::endMaterialRenderer` while loading the level; `GL_ES` stays
undefined. (The 2D variant links and is drawn as nothing, no longer logged as a missing
material; the "invalid bind symbol" lines of the NormalRim effects are the low shaders lacking
those uniforms, before as now.)

**Multiplayer map 0** (`build/3ds/trace_mp0.sh`: the lead's Lua sequence at 400x240) replays
as the PC draws it (a room with blue light lines); a flat blue in-game view there is the
spawn's view, not a rendering failure of that map.

**The menu's rifle.** The campaign panel's rifle is not lost: in that region the replay's
mean colour equals the PC's within 1-2 of 255, its detail is lower (standard deviation 13.8
against 15.0) because the 2048x2048 sprite atlas is halved and encoded again; the better
encoder of the pack (above) helps a little (menu 4.9 -> 4.8).

Open: chapter 9's distant lava (a band at the horizon stays dark) and the crystal's warm
tint; screen-space distortion is not drawn; render targets were only checked with the menus'
interface layers and the 3D scene the game draws into a texture before compositing it;
chapter 10's tour did not reach the traced frames.

## Menu layout at 400x240

The game sorts screens into classes by width / height: `Is480x320Res()` (width <= 480, 1.5 to
1.7), `Is960x640Res()` (width > 480, 1.5 to under 1.7), `Is1136x640Res()` (1.7 and over). The
menus are a 2D layer laid out on a quad of `CMenuScreen::QUAD_W` x `QUAD_H`, rendered to a
texture and seen by the menu camera, whose field of view `CMenuCamera::Load` takes from the
class: for the 480x320 class it fits a 480x320 quad to the screen (40 degrees; 72 for the
960x640 class's 960x640 quad). But `CMenuManager::Load` makes the quad 480x320 only when the
screen is exactly 480x320 (`g_ScreenW == 480 && g_ScreenH == 320`); every other screen keeps
the 960x640 size of `menu_elements_nova3`, and the sprites are scaled for the 960x640 layout in
both classes (`CSpriteManager::GetSpriteFilename`). So 400x240 (and 480x288) got the 960x640
quad behind the 480x320 view: the menus at twice their size, cropped (the title menu's
Multiplayer panel and left-hand buttons, the match setup's Continue). `--peek` of `QUAD_W`: 960
at 400x240, 480 at 480x320.

Fix (`host/hooks.cpp`, OVERRIDES): those screens are put in the 960x640 class, where an 800x480
phone (twice 400x240) is and the layout is right. `Is480x320Res()` is true only for exactly
480x320, `Is960x640Res()` covers the rest of its ratios. The fonts pick their sizes by
`g_ScreenW > 480` too (`Application::GetFontSize`; the glyph outlines,
`CTTFFontSpr::GetOffsetForOutlinedGlyph` / `GetBorderSizeForOutlinedGlyph`): these three run
with twice the width on a moved screen, so the text gets the 960x640 layout's sizes (the same
18 / 22 / 34 / 128 as at 800x480; with the small ones the labels came out far smaller).
Nothing changes at other sizes: both predicates return the game's own values there and the
font wraps call straight through.

**The bottom screen (320x240).** The menus are drawn there now (Platform: Screens). At 4:3 the
game takes its own 1024x768 (iPad) class, untouched by the hooks above, and the title menu and
its screens, the local Wi-Fi create / join / armory screens, campaign select, the loading
screens, the pause menu with its Options, Controls, Graphics pages and the exit dialog are
complete and well placed (PC `--size 320x240`, then Azahar). The fonts: a level's in-game fonts
are loaded by its loading screen (`Application::LoadFonts(true)`, GS_Loading's first step),
which is on the bottom screen, but are used on the top one: while GS_Loading is the top state the
font hooks work them out for the top screen's class (`hooks::g_font_width` = 800, as a moved
400x240 screen; 3DS only). The menu fonts are loaded at 320x240 (`LoadFonts(false)`, GS_LoadMenu).
The pause menu's pages use the in-game fonts and fit. The end-of-level screen (GS_EndLevel)
does not fit at 320x240 (its labels run into its values, with either font size), so it stays on
the top screen.

Checked on the PC (`NOVA3_DEVICE=3ds --size 400x240`, screenshots in `build/ui/`): title menu,
campaign select, a mission's loading screen, the campaign HUD (a chapter 0 tour), the results
screen (`--lua EndLevel()`), multiplayer, local Wi-Fi create / join / armory, match setup with
Continue, the armory, the match's HUD, pause menu and Options / Controls / Graphics pages (each
the 800x480 layout at half size); 480x320 and 1280x720 unchanged. Not reached: the end-of-match
screen (a 10-minute match). The only other exact-480x320 test, `CMenuAboutTextBox::Init` (halves
a size of the About box, +0x5c), now treats 400x240 as it treats 800x480. A local match by
script without taps: `Obj150022_OnClick()` (Multiplayer), `Obj150058_OnClick()` (Local Wi-Fi),
`Obj150482_OnClick()` (Create), `Menu_SetMPLevel(0)`, `Obj150346_OnClick()` (Continue), a second
or two apart. Join: `Obj150483_OnClick()` (Local Wi-Fi > Join, the search list, menu 150610),
then `Obj150613_OnClick()` (Join the first game listed).

**Local Wi-Fi join.** The search left at once: libctru's `SO_BROADCAST` is 0, which soc:U does
not know, and the game gives up when its `setsockopt` fails. It is accepted and ignored now
(`runtime/net.cpp`), the console's subnet comes from `SOCU_GetIPInfo`, and the discover packet
also goes to the subnet broadcast; Azahar refuses the limited broadcast itself (EACCES, logged
once), the subnet one goes out ([Runtime](runtime.md#network), Joining from a console). Tested
in Azahar: the list stays open; with the PC hosting map 0 on the same machine, the 3DS listed it,
joined (clock sync, `STARTGAME`) and played the match.

## Platform (`host/3ds/main.cpp`, `runtime/os_3ds.cpp`, packaging, tests)

- The game files are in the title's RomFS, `romfs:/nova3_data/` (the slim set's console files,
  see Slim data and The game files in the RomFS: one `.cia` to install);
  `sdmc:/3ds/nova3/nova3_data/` is read instead when it is there (a developer's override, as the
  Switch `.nro`'s; the full set works too). On the SD card: `sdmc:/3ds/nova3/` with `save/`,
  `nova3.ini`, `nova3.log` and `scratch/` (the guest's /sdcard, cache and /tmp); nothing is
  written to the RomFS.
- Frame cap 30 fps by default (`[video] max_fps = 30`); the Graphics page offers 30 / 60 fps
  and the field of view (no VSync or anisotropy rows: the screens are always synchronised,
  the PICA has no anisotropic filtering).
- **Screens** (`host/3ds/main.cpp` Screens, `pica::set_screen`): the menus are drawn on the
  bottom (touch) screen at 320x240, play on the top screen at 400x240. Before each step the top
  game state decides: GS_InGame and the states shown over it in play (GS_PlayerDead,
  GS_Respawn, GS_IGMTimeFactor, GS_InGameChat, GS_MPInGameScore, GS_FaneMinigame) and the end of
  a level or match (GS_EndLevel, GS_EndMultiplayer: see Menu layout at 400x240) go on top,
  everything else on the bottom: the boot's screens, the title menu and all its screens,
  multiplayer setup, the armory, loading screens, the pause menu and its pages. A change tells
  the game the size as Android tells it a new surface size (`GL2JNILib.resize`, which the game
  takes at the start of the next step: its `Resizing window (W H)` line; `g_ScreenW` /
  `g_ScreenH`, the fake Java's window size and the touch mapping at once) and relinks the PICA
  target. Three moments are timed so that what the game makes is made at the right size:
  - the game starts on the bottom screen (`resize(320, 240)` at boot), so the title menu, a
    level of the game's loaded at start, is built at 320x240;
  - a level's loading screen goes up when the level is loaded (GS_Loading's step 10, "touch the
    screen to continue", `+0x444`): the touch flag (`+0x44c`) is held back for that frame, so
    GS_Loading ends and GS_InGame's first update (`CLevel::Init`: the HUD, `CLevel::
    CreateInterface`) runs at 400x240. Checked: the HUD as on the PC at 400x240;
  - Back released in play (Start: the pause menu opens on Back's key-up) switches to the bottom
    screen first and reaches the game a frame later, so `GS_InGameMenu` (its constructor reads
    `g_ScreenW`) is made at 320x240. Resume (button, touch or Start) pops it and play goes back
    up: the HUD, which was made at 400x240, and the 3D view come back as they were (checked
    after pause / resume in chapter 0).
  The frame drawn right after a switch is not shown (`pica::set_screen` marks it; citro3d then
  transfers nothing): it may still hold a state made for the other size.
  One render target serves both screens, so the bottom screen costs no VRAM: its buffers are
  the top screen's 240x400 colour and depth, of which a 240x320 frame uses the first 320 rows
  (the PICA's buffers are rows of 8x8 tiles 240 wide: a prefix); its output (the display
  transfer) is linked to one screen at a time, so the other keeps its last image. While the
  game is on the bottom screen the top one shows the last frame of play at 3/8 brightness (both
  of its framebuffers dimmed once on the CPU, after the GPU is done; black before any play) and,
  during steps over a second, a `loading ... s, ... MB read` line; the bottom screen is BGR8 and
  double-buffered then. While play is on top the bottom screen is the libctru console again
  (RGB565, single-buffered): in test runs the status line, otherwise `N.O.V.A. 3` and `START:
  pause menu`. The game's stdout (its `printf` / `puts`: `WARNING: GAIA INIT ERROR` when its
  online services do not answer) goes to the log (`stdout: ...`), not to the console; the host
  writes the console through its own device (`console_printf`). The log has `screen: ...` and
  `pica: drawing on the ... screen` at each change.
- Input (`host/input.cpp` `poll_3ds`, read with libctru on the game thread; `NOVA3_CONSOLE`
  in `host/platform.h` marks what the Switch and the 3DS share):

  | 3DS | Gameplay (default bindings) | Menus |
  |---|---|---|
  | Circle Pad | move | navigate |
  | C-Stick (New 3DS) | look | |
  | R / L | fire / aim (they act as RT / LT) | |
  | ZR / ZL | power / grenade (RB / LB) | |
  | A / B / X / Y | jump / interact / reload / next weapon (by label; Swap A/B, X/Y as on the Switch) | confirm / back |
  | D-pad up / down | sprint (toggle by default: the thumb is on the Circle Pad) / melee | navigate |
  | D-pad left / right | previous / next weapon | navigate |
  | Select | next power | back |
  | Start | pause | |
  | Touch screen | | the menus are drawn on it: 1:1 (the end-of-level screen and the states over play, on the top screen: scaled 320x240 -> 400x240) |
  | Gyroscope | gyro aiming, as on the Switch (`[gamepad] gyro`), untested on hardware | |

  Rebinding takes a button's gameplay meaning (the D-pad is read as in gameplay while a binding
  waits). Next grenade type and detonate have no button. Prompts and the binding page name the 3DS
  buttons.
  No rumble (the Controls page leaves those rows out).
- Audio (`host/audio.cpp`): the console mixer the Switch uses (ring per track, a feeder thread
  mixing into the device's buffers) on one `ndsp` channel at the DSP's own 32728 Hz, which the
  game's Vox engine then mixes at, so nothing is resampled. `ndsp` needs the DSP firmware
  (`sdmc:/3ds/dspfirm.cdc`), which the game extracts from the HOME Menu at the first start when
  it is missing; without it the same mix goes out through CSND (DSP firmware, below). The log
  names the path taken.
- The game is told it has one 804 MHz core (`GetPhoneCPUCores`, `GetPhoneCPUFreq`,
  `sysconf`, `/proc/cpuinfo`, `/sys/devices/system/cpu`), so it picks the CPU_0 profile: no
  ragdolls, simpler 3D sound, "LowSounds".
- Boot (`host/3ds/main.cpp`, as `host/switch/main.cpp`): `osSetSpeedupEnable(true)` (804 MHz +
  L2), `APT_SetAppCpuTimeLimit(30)`, `gfxInitDefault`, the bottom screen as a libctru console
  (the game's stdout to the log; see Screens), the log, `romfsInit` (its reads made one at a time, see The game files in the
  RomFS), the test script, the DSP firmware's check (DSP firmware), the game files' folder,
  `socInit` (1 MB buffer), the settings; then the game thread (4 MB stack): the library,
  the static constructors, `rt::install_pica_gl()` + `pica::set_texture_pack`, the game on the
  bottom screen (Screens), the boot sequence of [Runtime](runtime.md#boot-sequence), and the
  frame loop (the screen for the top state, `aptMainLoop`, input, step, `pica::swap`), capped by
  sleeping to the next 33.3 / 16.7 ms. The main thread waits; while no frame completes for over
  a second (a load's steps take seconds each: the screens keep their last frame), it shows
  `loading | 12 s, 34.5 MB read` with a spinner on the screen the game is not on: the top one in
  the menus (a level's loading screen is on the bottom one), the console otherwise (the status
  line is not written meanwhile, so the two never overlap).
- Threads and cores (`os_3ds.cpp`): libctru threads with the stack size asked for. The game
  thread core 0 (priority 0x30); the guest's threads core 2 (0x30); the log writer (0x2e) and
  the watchdog (0x18) core 1. The kernel has no time slices within a priority: a thread that
  never waits starves the others of its priority on its core (see the asio spin below), and the
  log writer runs above the game threads (at 0x31 it was starved while holding the log).
- Crash reports: every thread started by the runtime gets a libctru exception handler
  (`threadOnException`, its own 16 KB stack) that writes `=== crash: ...` to `nova3.log`: the
  registers, pc / lr as game link addresses with the nearest symbol (or host addresses for
  `addr2line -e build/3ds/cmake/nova3.elf`), a scan of the stack for return addresses into the
  game, and the last import. The import gates check the stack: below 32 KB from the thread's
  stack bottom they stop with `stack overflow` and the game's callers (the game's code shares the
  host stack, and libctru keeps a thread's `_reent` at the bottom of its stack block: an overflow
  there showed up as a crash in `_svfprintf_r`). The watchdog (no frame for 15 s, then every
  15 s) prints the game thread's last import and stack scan, every thread in the game's code
  with its last gate and its count of imports (a spinning thread shows millions), and every
  guest thread (`runtime/thread.cpp` `report_threads`, the PC's watchdog prints it too): its
  start routine, the guest mutex it is blocked on and that mutex's holder (each guest mutex
  notes its holder and lock count: plain stores in the lock / unlock paths), and the return
  addresses on its stack. The same report is written when the guest has 40, 80, ... threads
  running at once (threads that never end).
  For hangs on a console, where no thread can read another's registers (2026-10-12):
  - The first report and the one at 120 s add the return addresses *below* the game thread's
    last gate (`scan_below`): into the game (`g`, link addresses) and into this program (`h`,
    for `arm-none-eabi-addr2line -e` the build's `nova3.elf`, kept as `build/3ds/nova3_probe*.elf`
    per console build). The calls it made since without a gate (GL, `malloc`, the mutexes,
    string and math functions are bound directly) leave their frames there; the live chain runs
    from the top down, deeper words are older calls' leftovers.
  - At 30 s the watchdog raises the game thread to 0x18 (`watchdog: the game thread's priority
    ... raised`); frames resuming (`frames again after the raise`) mean it was kept from running
    by a thread above it on core 0 (the main thread 0x2c, the sound feeder 0x18), otherwise it
    is stuck by itself. The main thread logs every 15 s of a stall (`main thread: running, ...;
    sound feeder woke N times since`, about 200 a second when the feeder only waits on the DSP).
  - At 120 s the hang probe makes the game's code non-executable (`native_stop_code`: .text,
    the stubs, the Thumb island, the trampolines): the game thread faults where it runs and the
    crash report gives the loop's address; the game's other threads that fault meanwhile are
    stopped in the handler (`hang probe: thread N stopped at ...`). No fault in 10 s: the game
    thread is outside the game's code; the process ends. Azahar does not enforce the permission.
  - None of it while the application is suspended (the HOME Menu, sleep): APT's hooks
    (`aptHook` in `start_watchdog`) mark the stay, the watchdog and the main thread's status
    line count again from the return (`app_suspended`, `app_resumes`). Before, a stay of 120 s in
    the HOME Menu ran the hang probe, which crashed the game on purpose.
- The wall clock (`os::realtime_ns`, behind `gettimeofday` / `clock_gettime` / `time`) reads
  libctru's (`osGetTime`, the shared page's time reference) once and then counts the system
  tick: the game asks every frame.
  On a console, also collect Luma3DS's dump: `sdmc:/luma/dumps/arm11/crash_dump_*.dmp`
  (Luma's exception handlers; `exception_dump_parser.py` from the Luma3DS repository reads it).
  Azahar does not run user exception handlers: it logs the exception in
  `user/log/azahar_log.txt` and stops the thread.
- Files (`runtime/libc_io.cpp`, the Switch shares these; docs/history/loading.md): the game
  probes for loose files before reading its archives and reopens the two OBBs for every mesh
  (chapter 0's load: 14,536 `fopen`s, 12,865 of them for missing files), and each probe or open
  is a request to the SD card's file service. Paths found missing are remembered and fail at
  once (any create, rename, remove or mkdir forgets them); a read-only game file the guest
  closes stays open with its 16 KB-block read cache (the 4 most recent), and the next `fopen`
  of it takes it back; read-only files get a 16 KB stdio buffer (newlib's 1 KB buffer made
  each cache block sixteen requests). Chapter 0's load in Azahar: 222 s before, 51 s after.
  On the 3DS also: the archives' mount index, the compressed library, and lookups without
  requests (see Boot and level loads), and the packed stream and sound archives (Slim data,
  Packed archives). All of it works the same on the RomFS (libctru's `romfs:` device: `stat`,
  `opendir`, `open` / `read` / `lseek`, stdio), where opens, stats and listings cost no request
  (libctru keeps the RomFS's tables in memory) and reads are one FS request each.
- DSP firmware (`host/3ds/dspfirm.cpp`): when `sdmc:/3ds/dspfirm.cdc` is missing at the start,
  it is extracted the way the DSP1 homebrew (github.com/zoogie/DSP1) does: the HOME Menu's ExeFS
  `.code` is opened through FS (the title-content archive 0x2345678A, the HOME Menu's title id
  for each region in turn, `0004003000008F02` USA ... `000400300000B102` TWN, on the NAND),
  unpacked (the ExeFS's backward LZSS, ctrtool's decoder as DSP1 uses it), the firmware found by
  its `DSP1` magic (0x100 into the file, after its signature) and the size noted there written
  from the signature on; unlike DSP1 a candidate is taken only when the SHA-256 of each of its
  segments (headers at 0x120) matches. The archive needs one of the exheader's FS access bits
  FS checks for it (mask 0x1005: `CategorySystemApplication`, `CategoryFileSystemTool`,
  `CategorySystemSettings`, all three in `host/3ds/nova3.rsf`; DSP1's own `.cia` has them too).
  Then `ndspInit`. Log: `audio: DSP firmware sdmc:/3ds/dspfirm.cdc (N bytes)` (there),
  `audio: DSP firmware extracted from the HOME Menu (USA) (...)` or `audio: no
  sdmc:/3ds/dspfirm.cdc and none extracted (USA: not readable (c8804478); ...)`. Without the
  firmware (`ndspInit` fails) the sound goes through CSND (`audio: ndsp unavailable ...: CSND
  instead`, `audio: CSND 32728 Hz stereo (channels 8 and 9)`), the older sound service, which
  needs none: two hardware channels (CSND plays mono), left and right, each looping over a ring
  of four 512-sample chunks in linear memory; nothing reports the play position, so the feeder
  (core 0, priority 0x18, every 4 ms) follows it by the system tick (CSND's timer at this rate is
  2048 ticks of 67 MHz a sample, 32728.5 Hz as the DSP's; the ARM11's tick is the same clock times
  four: 8192 ticks a sample, no drift) and mixes the chunks played since, up to three (47 ms, as
  ndsp's three buffers) ahead of the one playing, less 128 samples for the channels' start;
  `audio: CSND feeder late` counts chunks it missed. On HOME / sleep the channels stop and start
  again on return (an APT hook), and they are stopped before the process ends (otherwise they
  loop on). A test script's `env NOVA3_AUDIO csnd` takes CSND anyway, `env NOVA3_DSPFIRM_CODE
  sdmc:/...` takes the firmware from that `.code` file instead of the HOME Menu (Azahar has no
  HOME Menu).
- Testing: the Azahar emulator (New 3DS mode; its window shows both screens) with a scripted
  `sdmc:/3ds/nova3/test.txt` like the Switch build (a script also turns the status line on):
  `tap` (game coordinates: the bottom screen's 320x240 in the menus, 400x240 in play), `hold`, `lua`, `tour`, `bench CHAPTER FRAMES [walk]` (logs the
  level file, `bench: chapter N loaded, in play X s after the start`, and the result), `speed`,
  `quit`, `trace_files`, `env`, `headless` (the null renderer). Useful `env`s:
  `NOVA3_IMPORT_PROFILE 1` (with each perf line: the game thread's imports per frame, the
  busiest by name, and its hooks per frame), `NOVA3_GLPROFILE 1` (time inside the GL driver
  per function), `NOVA3_TRACE_THREADS 1`, `NOVA3_SAMPLE 1` (the game thread's callers every
  second), `NOVA3_LOAD_PROFILE 1` (what the game thread does in frames over 100 ms: see Boot
  and level loads). The perf report's GL time is measured in one frame of 8 on the 3DS (the clock is a
  kernel call; `slow frame` lines print `GL -` on the others, whose GL time is in "game code").
- **The process's end** (`rt::os::exit_process`: HOME -> Close, a guest `exit()`, a fatal error,
  a test's end). Not libctru's exit: `exit` and `_Exit` both run the static destructors and then
  unmap both heaps (`__libctru_exit`) while the guest threads, the log writer and the sound's
  feeder still run on them. Azahar only logged it (about 500,000 unmapped reads and 7,900
  exceptions within a second of the bench's end, threads at PC 0); a console takes the fault
  (a crash dump at every exit). Now: the log flushed, then libctru's `__appExit`, whose
  `userAppExit` hook (`host/3ds/main.cpp`) stops the sound (`ndspExit` or the CSND channels:
  either would play on), finishes the GPU's work and closes citro3d when no other thread can be
  drawing (`pica::finish`), and `gfxExit`; then fs, hid and APT (which hands the console back to
  the HOME Menu), and `svcExitProcess` ends every thread at once. Checked in Azahar: all steps
  run, no unmapped access or exception afterwards.
- **Checked against the console, not only Azahar** (where the emulator is lenient): the kernel
  clobbers r2/r3 (and the AAPCS r12/lr) in SVCs, inline `svc` lists them (the only one is
  `os::ticks`); the game's reachable ARM code has no `dsb` / `isb` (its `dmb`s are patched), and
  its NEON and d16-d31 code (glitch's `neonSkin*`, Vox's `FillBufferNeon*` / `DecodeBlockNeon*`,
  libgcc's VFP d16-d31 restore) is chosen through `android_getCpuFeatures`, which reads the
  runtime's `/proc/cpuinfo` (no `neon`, `vfpv3d16`): never run; the game never writes FPSCR,
  and libctru starts every thread in RunFast mode (flush-to-zero, default NaN), which VFP11
  needs (a denormal otherwise traps to support code the 3DS kernel lacks); the kernel helpers'
  cmpxchg and barrier are the Linux ARMv6K ones; Boost's landing pads have no Thumb-2 call left
  and the island's copies have unwind entries (see Landing pads). No C++ exception is thrown in
  the boot or play (the game's unwinder never asked for a table in a chapter 3 bench), so that
  path is checked statically only.

## Slim data (`tools/3ds_slimdata.py`)

The 3DS gets its own copy of the game files, made from the player's `dist/nova3_data` (which
stays as it is): `python tools/3ds_slimdata.py build` writes `build/3ds/slim_data`, which
`tools/3ds_package.py` and `tools/3ds_run.py --deploy` use. Changed files are written there,
the others are hard links. The first round (textures stored from the level the game uploads,
below): 1.72 GB -> 1.40 GB on the SD card, chapter 3's load in Azahar 44.9 -> 37.5 s, every
texture the game uploads the same, byte for byte. The second round (Second round, below:
textures and loading screens a level lower, `_low` twins, unreachable content, zopfli) brings
the set to 1.23 GB; `build --mips 0 --no-ui --no-twins --no-unreachable --no-recompress` makes
the first round's set again (the same content), `--mips 0 --no-ui` keeps the first round's
look with the second round's other savings. The build needs `pip install zopfli` (or
`--no-recompress`) and takes about 6 minutes (12 processes). It also
writes the two files the 3DS's boot reads instead of the game's (Boot and level loads):
`mount.idx` (the archives' local headers, 4.5 MB; `3ds_slimdata.py index [DIR]` alone) and
`libGloftNAHM.so.z` (the library's loaded parts in zlib pieces, 11.3 MB; `3ds_slimdata.py lib
[DIR]`). Both must be written again whenever an archive or the library changes (a stale one is
detected and not used, so the boot is only slower). Last, the stream and sound archives are
packed (Packed archives, below: `X.gla.z`; `--no-pack`: not): the console's set (what
`3ds_package.py` and `3ds_run.py --deploy` take: `X` left out where `X.z` is there) is 980 MB.

**What the 3DS's profile reads.** `NOVA3_TRACE_READS=1` (`runtime/libc_io.cpp`, any platform)
logs every read of a game file (host path, offset, bytes); `3ds_slimdata.py measure LOG...` lays
the reads over the archives' members (zip `.gla`, and the OBB-format `*_stream.gla` and
`sounds_*.gla`) and sums them per kind. PC, `NOVA3_DEVICE=3ds`, 400x240, the title menu and the
loads of chapters 0, 3 and 5 (`--bench N:900:walk`, the load and 15 s of play) and of a local
match on map 0; MB read (each run's boot and title menu: the first column):

| Kind | On disk | Boot + menu | Chapter 0 | Chapter 3 | Chapter 5 | Match |
|---|---|---|---|---|---|---|
| textures (`.etc` in `*4.gla`) | 384 | 9.5 | 44.5 | 24.0 | 23.5 | 14.3 |
| meshes (`.bdae` in zip `.gla`) | 406 | 0.1 | 13.6 | 10.4 | 3.7 | 4.5 |
| meshes, animations (`*_stream.gla`) | 357 | 2.5 | 13.0 | 8.1 | 8.1 | 5.4 |
| sounds (`sounds_hi*.gla`) | 379 | 1.1 | 0.6 | 2.9 | 2.4 | 2.3 |
| irradiance (`.irr` in `*6.gla`) | 126 | 0 | 1.0 | 2.3 | 0.5 | 9.0 |
| levels, scripts, text, other | 34 | 0.9 | 1.0 | 1.5 | 0.9 | 0.8 |
| archive mounting | 0.5 | 4.5 | 0.2 | 0.2 | 0.2 | 0.1 |
| **total** | 1,687 | 18.5 | 73.9 | 49.5 | 39.2 | 36.4 |
| **total, slim data** | 1,371 | 13.9 | 35.4 | 29.0 | 19.4 | 24.4 |

A load reads 2-5% of the data, half of it textures. Every `.gla` is opened at boot: the game
mounts a zip archive by reading 1 KB at each member's local header (about 4,400 reads; the
"archive mounting" row), so no archive goes unread; the files never read are a few small ones
(`abcde.dat`, `oconf.bar`, `options.gla`, `snsconfig.json`, 0.1 MB together).

**Textures.** GPU_0 ("Mipmaps to skip" 2) and MEM_256 ("Max texture size" 2) make the game drop
the largest levels of every mipmapped texture at load: it inflates the whole PVR from the zip
and uploads from a lower level on. `CustomTexturePolicy::getMipmapCountToSkip` (0xe6fce8)
returns 0 for a texture without mip levels or of 64 x 64 or less, otherwise 2, plus one while
the larger side, so reduced, is still 256 or more (512 for names containing `mc_` or
`earth_main`); the game drops one level less than that (`levels_skipped`; matched against the
uploads of all 517 textures that the five traces identify: 1024 -> 256, 512 -> 256, 256 -> 128,
128 -> 64, 2048 -> 256, `MC_*` 1024 -> 512, split alpha maps at their own half size alike).
The slim copy stores each mipmapped PVR from that level on (width, height, mip count and data
size in the header adjusted; the loader checks that the mip count is log2 of the larger side;
`SPLIT` `.etc` zips of `rgb.pvr` + `alpha.pvr` part by part), deflated again at level 9 in
the same member order, and its `GameOptions.json` has every profile's "Mipmaps to skip" and
"Max texture size" at 0, so the game takes the stored levels as they are: 666 MB of PVR data
-> 132 MB, the texture archives 384 -> 68 MB (the 2048x2048 sprite atlases and colour tables
have no mip levels and stay as they were). The set is for the 3DS's profiles only: under the
PC's own the textures would stay at the 3DS's size. `pica_textures.pak` is made again from the
slim data: the full data's pack held 10 mipmapped images over 1024 that are never uploaded at
that size; the 9 left are byte for byte the old pack's entries.

**Left as they are**, the risk outweighing the gain:

- Meshes beyond the `_low` twins (Second round). LOD levels inside a `.bdae` ("LOD start level" 2)
  or quantised vertex data would need the binary COLLADA format rewritten.
- Sounds' content (the user's choice: no lower rates or bitrates; they are packed losslessly,
  Packed archives, below). They are IMA / MS ADPCM (4 bits a sample at 32 kHz) and Musepack in
  the OBB-format `sounds_hi*.gla`, whose reader has no compression (an entry is a window of the
  file); what LowSounds and SoundLevel 2 leave unplayed depends on the gameplay, not on a file the
  traces can rule out.
- The fonts: `NanumGothic.ttf` and `WenQuanYiZenHei.ttf` (Korean, Chinese; 8.5 MB) are read even
  in English, `NewCezannePro-DB.ttf` (Japanese, 4 MB) is for that language: the language can be
  changed in the game.

**Verified, first round** (PC, `NOVA3_DEVICE=3ds`, full data against slim data,
`build/3ds/slim/scen.sh` and `run.sh`, the GL trace of each run):

- `3ds_slimdata.py uploads A/gltrace.bin B/gltrace.bin`: every texture upload
  (`glCompressedTexImage2D` / `glTexImage2D`: level, format, size, CRC-32 of the data), in order:
  identical in all five runs (title menu 353 uploads, chapter 0 2,284, chapter 3 1,553,
  chapter 5 1,610, the match 1,173).
- `3ds_slimdata.py shots A B`: the title menu's frames 700 and 990 pixel for pixel. In a level
  the runs drift apart by timing (a load of other length ends on another frame), as two runs of
  the full data do too: chapter 3's frames 1200-1800 differ by 2.4-11.6 of 255 between two full
  runs, 0.4-12.4 between full and slim. Chapter 0's cinematic, every frame from 1780 to 1820:
  26 of 41 frames identical, the others differ by about 1.1 (the camera's frame time); the
  match spawns at another point (its Lua steps come at fixed frames).
- `tools/tour.py 1 3` and `tools/tour.py --mp 0` with `NOVA3_TOUR_ARGS="--obb
  E:/nova3/build/3ds/slim_data"`: San Francisco 2 (32 checkpoints) and Therrius 2 (29, the results
  screen) played through, the match reached gameplay; no crash.
- PC loads (frames over 50 ms, chapters 0 / 3 / 5): 1.45 / 1.42 / 1.02 -> 1.26 / 1.29 / 0.88 s,
  the game code (mostly inflating) 0.60 / 0.59 / 0.35 -> 0.39 / 0.47 / 0.25 s; the rest is GL.
- Azahar (`bench 3 900 walk`, one run each, the same `nova3.cxi`): boot (frame 300) 52 -> 50 s;
  chapter 3's load 44.9 -> 37.5 s (the 300 frames with the load: file I/O 65.3 -> 52.1 ms, game
  code 85.9 -> 74.2 ms a frame); the walk 28.3 / 28.1 fps; the title menu 30 fps, step 7.25 ms;
  memory the same (GL textures 10.2 MB, guest heap peak 63 MB, linear free at least 16.9 MB).
  Captures of the menu and the level look alike (the menu's background is animated, the frames
  are not the same ones).

Measuring again (scratch scripts in `build/3ds/slim/`, run lock taken): `run.sh NAME DATA_DIR
ARGS` runs the PC build with the read, file and GL traces into `build/3ds/slim/NAME/`; `scen.sh
menu|ch0|ch3|ch5|mp SUFFIX DATA_DIR` one of the five scenarios; `tours.sh` the tours above
(results in `build/3ds/slim/tours/`).

### Second round (`build`'s options, all on by default)

MB on the SD card (the full data without `hd_textures.pak`; both slim sets with `mount.idx` and
`libGloftNAHM.so.z`; `build/3ds/slim2/sizes.py`):

| Kind | Full data | First round | Second round |
|---|---|---|---|
| textures (`*4.gla` but the sprites) | 365 | 48.2 | 13.0 |
| UI images and fonts (`sprites.gla`, `sprites4.gla`) | 32.6 | 32.6 | 21.1 |
| meshes (`.bdae` in zips) | 406 | 406 | 339 |
| meshes, animations (`*_stream.gla`) | 357 | 357 | 324 |
| irradiance (`.irr`) | 126 | 126 | 90.3 |
| sounds | 380 | 380 | 380 |
| levels, scripts, text, other | 21.8 | 21.8 | 21.4 |
| the library (`.so`, `.so.z`) | 25.9 | 37.2 | 37.2 |
| `mount.idx`, `pica_textures.pak` | 9.7 | 9.0 | 7.9 |
| **total** | 1,723 | 1,417 | 1,233 |

Each option's share (the set built with the options added in this order): `--mips 1` -34.7 MB,
`--ui` -12.1, `--twins` -67.8, `--unreachable` -63.6, `--recompress` -5.7.

- `--mips N` (1): every mipmapped texture is stored N levels below the one the game uploads
  (while its larger side stays at least 32), so the 3DS gets half the width and height: 1024 ->
  128 instead of 256, `MC_*` and `earth_main` 512 -> 256, split alpha maps alike. The game's
  sprite atlases, colour tables and HUD images have no mip levels and stay as they were. Textures
  48.2 -> 13.6 MB; in Azahar chapter 3's GL textures 10.2 -> 6.0 MB.
- `--ui`: the loading screens and splash screens as the game's own 480x320 sets (the comment
  above `UI_FAMILIES` in the tool): `CSpriteManager::GetSpriteFilename` picks a sprite's set by
  the screen from the files that exist, with the scale from the set's size to the layout's, so
  without the other sets it takes the 480x320 one and scales it up, as on a 480x320 phone; the
  3DS draws the 960x640 interface layer into a 512x512 target anyway. A loading screen at
  400x240: mean difference 3.7 of 255. Tried and left: halving an image in place (the game takes
  its sprites' texture coordinates from the texture's size, so the title menu came out cut
  wrong), the match setup's level image (`MP_*`, `multiplayer_locations_a`: empty with its
  480x320 set), `menu_level_images_nova3`, `flags` (named with their resolution by the menus:
  without them the menus do not load).
- `--twins`: with GPU_0's "Geometry level" 1, `CCustomFileSystem::createAndOpenFile` (0xe65a80,
  the virtual the engine opens every file through) asks for `X_low.bdae` whenever it is asked for
  `X.bdae` (`customCreateAndOpenFile` with the name changed) and opens `X.bdae` only when there is
  no `X_low.bdae`; `fastCreateAndOpenFile` (straight to `CGlfFileSystem`) serves only
  `DecryptAndLoad`, `LoadSingleData` and Vox's archives. So the build stores the `_low` model under
  both names in a zip .gla (156 models in 7 archives: the zip reader walks the local headers, so a
  copy is needed) and points `X.bdae`'s entry at `X_low.bdae`'s bytes in a `*_stream.gla` (16
  animations: the OBB reader opens an entry by its offset and size; `obb.encode_xor32` writes the
  names). A read of `X.bdae` would get a valid model. The read traces have no read of such an
  `X.bdae` (in `weapons_stream.gla` two 1 KB reads touch `hands_weapons_anim.bdae`'s first bytes:
  the read-ahead of the entry before it).
- `--unreachable`: the co-op and sandbox locations' levels (`CO_OP_1_Therrius`,
  `Ship_02_Physics`, ...) have no level file, so their archives are not needed, except what
  reachable data names: a scan of every other archive member and the library for
  `<name>.<bdae|etc|tga|png|pvr|irr|col|glsl>`, then of the members so kept, until nothing is
  added. 33 of their members stay (2.7 MB; chapter 3's teleporter models take `dm2_coop_lm` from
  `l_101_coop4.gla`, `SF3_Skybox.bdae` `sf2_skybox`, multiplayer map 2 `SF_concrete_redline_wall`,
  the library `SpotLightMap` from `l_999_sandbox4.gla`, `levels.gla/gol.bin` co-op animations).
  Names in both are read from the archive mounted first (chapter 3 reads `TS_prop_skybox.bdae`
  from `l_001_terraforming_ship_stream.gla`, not `l_101_coop_stream.gla`'s other copy). Irradiance
  files are named `<level>_<object id>.irr` by `CLevel::LoadNextObject`; those of levels without a
  level file go too (`l_101_coop6.gla`, `l_999_sandbox6.gla`, `San_Francisco_1_NEW` and
  `SF_Terraformed` in `l_002_SanFrancisco6.gla`).
- `--recompress`: every deflated zip member deflated again by zopfli (5 iterations; the smaller
  of it and the original kept). The archives were deflated well already: 1-2%. Inflating is as
  fast.

The zips are written by the tool (`write_zip`: members in their order, no data descriptors or
extra fields), the OBBs by `slim_obb`; both keep the formats the game's readers take (the zip
reader `glitch::io::CZipReader::scanLocalHeader` reads local headers only).

**Verified, second round** (PC, `NOVA3_DEVICE=3ds`, the final set; scripts and results in
`build/3ds/slim2/`):

- Tours (`tours2.sh`): `tour.py 1 3` (32 checkpoints; 29 and the results screen), `tour.py --mp
  0` (gameplay reached), `tour.py 0` at 400x240 on both rounds (56 checkpoints each); no crash.
- The five scenarios (`scen.sh`) ran; loads 3.4 / 2.6 / 2.4 -> 3.2 / 2.6 / 2.3 s (chapters 0 /
  3 / 5). The texture uploads differ by design (half the size).
- 400x240 comparisons, round 1 | round 2 side by side, in `build/3ds/slim2/compare/`
  (`overview.png`; `compare.py`, `stillcmp.py`, `tourcmp.py`): the title menu 35.1 / 37.8 dB
  PSNR (mean difference 2.0 / 1.5 of 255: the planet, the stars); the match setup screens
  27-38 dB; chapters 3 and 5 standing at the spawn point 2 and 4 s after the load 29.9 / 24.9 and
  32.5 / 32.4 dB; chapter 0's tour, the same view at each of its 57 checkpoints: median 25.5 dB,
  mean difference 7.8 (blurrier surfaces; the rest is animation at another phase). The bench
  presses Space every second while loading and in cutscenes, so with the faster load chapter
  0's intro is skipped at another moment and its frames do not line up (checked: the same set
  with the first round's texture archives plays it as before).
- Azahar (`bench 3 900 walk`, the `nova3.cxi` of Boot and level loads' "After"; `az/`): frames
  steady 15.9 -> 15.6 s, frame 300 26.5 -> 26.2 s, chapter 3's load 28.3 -> 27.5 s, the walk
  28.2 -> 28.3 fps (busiest window 25.5 -> 25.7); GL textures in the level 10.2 -> 6.0 MB, linear
  free 16.8 -> 21.0 MB, the title menu's GL textures 35.4 -> 34.4 MB (mostly the interface
  layers), the guest heap as before (peak 65 MB); no crash. Captures `az/end_0030.png` (the
  title menu), `az/end.png` (chapter 3).

### Packed archives (`build --pack`, on by default; `pack`, `verify`)

The OBB-format archives, `*_stream.gla` (animations, models: 324 MB) and `sounds_hi*.gla` (ADPCM,
Musepack: 380 MB), are stored uncompressed: the game's reader (`CCustomPakReader::openFile`:
`createLimitReadFile`) opens an entry as a window of the file and reads it in pieces. The console
gets them losslessly packed: `X.gla.z` in raw deflate blocks, which the 3DS's file layer
(`runtime/libc_io.cpp`, `Packed`) unpacks as the game reads `X.gla`: the same bytes, the same
size. `3ds_slimdata.py pack [DIR]` writes them (zopfli, ~5 minutes; `--no-zopfli` seconds; an
up-to-date container, by size, block size and CRC-32, is kept), `build` does it at its end, and
`3ds_slimdata.py verify [DIR]` unpacks every container and compares it with `X.gla` byte for byte
(and every block's CRC-32 and the whole's). `3ds_cia.py` (the RomFS), `3ds_package.py --sdmc`
and `3ds_run.py --deploy` leave `X` out where `X.z` is there (and `--deploy` removes it from the
emulated card).

- Format (`N3BZ`): the original's size, block size, block count, CRC-32; the blocks' offsets and
  CRC-32s; then the blocks, raw deflate or stored as they are where deflate does not make one
  smaller. An archive is packed only when that saves over 5% (`sounds_hi_p4.gla`, Musepack, and
  the empty `l_999_sandbox_stream.gla` stay as they are). Zip archives (the mount index's) are
  never packed.
- Block sizes, measured over the slim set and the PC read traces of the title menu, chapters 0, 3
  and 5 and a match (`3ds_slimdata.py`'s comment): the streams 64 KB (0.53 of their size with zlib
  9; 16 KB 0.57, 32 KB 0.56, 128 KB 0.52 but 1.4-2.7 times the bytes unpacked per load), the
  sounds 32 KB (0.80-0.92 at every size; the game streams them 1 KB at a time). Zopfli takes 2-5%
  off the streams and ~1% off the sounds.
- The file layer: the game files' folder listing turns `X.gla.z` without `X.gla` into a packed
  `X.gla`; its container is opened and its tables read at the first open and stay (no request for
  any later open, so these files are not parked); `fopen` gives a FILE whose `fread` / `fseek` /
  `ftell` go through the read cache straight to the unpacked blocks (its own 16 KB blocks are not
  used) and whose other stdio calls go through `fopencookie`; `open` / `read` / `lseek` / `pread` /
  `fstat` (a descriptor marked `kPackedFd`, also the FILE's), `stat`, `access`, `fdopen`, `dup2`
  and `opendir` (the name without `.z`) answer as for `X.gla`, with its size. The game itself
  only uses `fopen` / `fread` / `fseek` / `ftell` / `fstat` on these archives (the traces'
  lookups). Blocks are unpacked into a pool of 16 shared by all files (1 MB; the least recently
  read is reused: simulated on the traces it does as well as 4 per file), each checked against
  its CRC-32 (a damaged block is logged and its read comes back short); a read of a whole block
  is unpacked straight into the game's buffer. Unpacking runs under its file's lock, the pool's
  lock only for lookups and copies, so the sound thread and the game thread do not wait for each
  other's files. A container that cannot be opened or whose tables do not fit is logged and not
  used (the archive is then missing).
- Logs: `loading:` lines and the load profile add the MB unpacked and its time (`(unpacking)` for
  the game thread), and on the 3DS every perf report is followed by `files: X MB unpacked in Y ms,
  Z MB read in N requests` (any thread) when something was unpacked.

Sizes (MB; the console's set; ratios with zopfli):

| Archive | Plain | Packed | Ratio |
|---|---|---|---|
| `actors_stream.gla` | 64.5 | 38.5 | 0.596 |
| `l_001_terraforming_ship_stream.gla` | 40.3 | 20.4 | 0.507 |
| `l_002_SanFrancisco_stream.gla` | 90.6 | 46.3 | 0.512 |
| `l_003_Boreas_stream.gla` | 47.3 | 19.9 | 0.421 |
| `l_013_Volteron_stream.gla` | 58.8 | 27.9 | 0.475 |
| `l_100_multiplayer_stream.gla`, `l_101_coop_stream.gla` | 4.2 | 1.5 | 0.33-0.38 |
| `menus_stream.gla` | 6.2 | 3.4 | 0.542 |
| `weapons_stream.gla` | 12.0 | 5.9 | 0.494 |
| `sounds_hi.gla`, `_p1`, `_p2` | 281.6 | 222.2 | 0.783-0.795 |
| `sounds_hi_p3.gla` | 95.1 | 86.6 | 0.910 |
| **all packed** | 700.6 | 472.6 | 0.675 |
| **the console's game files** (the `.cia`'s RomFS) | 1,210 | 980 | |

**Verified** (scratch logs in `build/3ds/slim3/`): `3ds_slimdata.py verify`: all 13 containers
identical to their archives, byte for byte; in Azahar every block the game read passed its CRC-32
(no `damaged` line). Azahar, the same build, one run each with the plain archives (before) and the
packed ones (after); with a stub `sdmc:/3ds/dspfirm.cdc` (below) so that the game's sound runs:

| | Before | After |
|---|---|---|
| boot: frames steady; frame 300 | 15.8 s; 26.3 s | 16.0 s; 26.5 s |
| chapter 3's load (`bench 3 900 walk`) | 28.1 s | 28.5 s (7.2 MB unpacked in 0.82 s of the logged seconds) |
| the walk; its busiest window (step: GL + file I/O + game code) | 28.0 fps; 25.2 fps (13.3 + 1.7 + 23.6 ms) | 27.8 fps; 24.6 fps (13.3 + 2.1 + 24.1 ms) |
| frames over 34 ms in the busiest window; its worst | 199; 211 ms | 211; 263 ms |
| title menu (music); a local match on map 0 | 30.0 fps; 30.0 fps | 30.0 fps; 30.0 fps |
| unpacked in the title menu; in the match (per 300 frames) | - | 0.56 MB in 78 ms; 0.8-1.6 MB in 110-210 ms |
| audio underruns (`audio: underrun`) in all four runs | 0 | 0 |
| host heap in use (menu; level) | 22; 27 MB | 23; 27 MB |

Unpacking runs at ~8.5 MB/s in Azahar (a third of a console's speed). In the match, the game
thread's file I/O stays at 0-0.1 ms a frame while ~1 MB is unpacked every 10 s: the sounds are
streamed by the game's sound thread (core 2), about 2% of that core in Azahar. The busiest
window of the walk lost 0.9 ms a frame (0.4 of it file I/O; 300-frame windows of one build
varied by more than that in earlier runs: 20.9-25.5 fps).

## Boot and level loads

Azahar (New 3DS mode), `bench 3 900 walk`, the slim set, one run each (`build/3ds/fix4.log`):

| | Before | After |
|---|---|---|
| boot: the game initialised (JNI init done) | 8.2 s | 4.8 s |
| boot: frames steady (the title menu, `boot: frames steady`) | 35.4 s | 15.9 s |
| frame 300 | 47.2 s | 26.5 s |
| chapter 3's load (`bench: chapter 3 loaded, in play ... after the start`) | 37.5 s | 28.3 s |
| the walk; its busiest window | 27.8-28.3 fps; 25.4-25.5 | 28.2 fps; 25.5 |
| host heap in use (the menu; in the level) | 27 / 32 MB | 22 / 27 MB |

Azahar's clock is emulated: its SD card costs what its model says (a read 0.52 ms + 183 ns a
byte: 5.5 MB/s; an open or a stat measured 17-25 ms each), its CPU runs at about a third of the
New 3DS's. The times say where a console's go, not what they are.

**Where the time went.** The log has the boot's steps with times (`boot:` lines, `native: ...
loaded (2.22 s)`, `time: frame N at ... s`) and, while frames are over a second (`loading:`
lines), each second's file system requests: reads (MB, count, ms), opens, stats. `env
NOVA3_LOAD_PROFILE 1` (test script) adds a load profile: the 3DS cannot sample another thread's
registers, so the game thread's gates note the import or hook they are in (`g_sample_state`) and
libc_io the SD request it waits for; a thread on the system core reads that every millisecond and
keeps the samples of frames over 100 ms, logged once frames run again for 2 s, by import (with
`(SD read)`, `(SD open)`, `(SD stat)`), hook, and "game, last gate from F" (the game's own code
after its last gated call, made from F; the direct imports such as malloc and memcpy have no
gate). It keeps the GL gates (as `NOVA3_IMPORT_PROFILE`). Before:

- Boot (27 s of frames over 100 ms after 8.2 s): file system requests 20.4 s (reads 13.7 s: 3,534
  of them, 60 MB, 2,609 the 16 KB blocks of the archives' mount; opens 4.4 s: 256; stats 2.3 s:
  119), `sscanf` 8% (a 64 KB buffer allocated and cleared per call, 18,600 calls),
  `CMenuManager::Load` (the game's) 8%, ETC1 uploads 5%. Before the frames: the library's read
  4.8 s of 7.2 s.
- Chapter 3's load: about 60% of each second in requests: reads ~40% (28.6 MB), opens ~25%
  (~450 first probes of missing names, each an open). Then (with the fixes below) `fopen`'s own
  CPU 7.5% (17,000 probes of known misses, each mapped to a host path: ~45 string allocations,
  ~125 us in Azahar), `fstat` 2.5% (a size request per archive open).
- PC cross-check (`NOVA3_DEVICE=3ds`, `NOVA3_IMPORT_PROFILE=1`; `NOVA3_PROFILE_LOAD=1` profiles
  the bench's load with the Windows profiler): the load makes ~430,000 `malloc`s, 390,000 `free`s
  (dlmalloc, bound directly on the 3DS) and 17,000 `fopen`s; the boot 18,600 `sscanf`s. The
  game's hot code in the load is its own: zlib's `inflate_fast`, `pixel_format::convert`,
  `createQuantizedBuffer`, Bullet's `btQuantizedBvh::buildTree`, the archives' `findFile`.

**What changed** (3DS only unless noted):

- The archives' mount index (`nova3_data/mount.idx`, `runtime/libc_io.cpp`): the game mounts
  every zip `.gla` at boot by reading 1 KB at each member's local header (about 4,200 reads over
  1.3 GB). The index holds those bytes ([h, h + 1 KB) for every local header, merged; 4.5 MB for
  35 archives); an archive's part is read in one request when the game first opens it, checked
  (the archive's size, and the first read it covers, which still goes to the file, compared),
  then serves the small reads inside its ranges; freed at the end of the boot (`io_boot_done`).
  A stale or damaged index is not used for that archive. 4,211 reads from 34 archives.
- The library's compressed copy (`libGloftNAHM.so.z`, `runtime/native_3ds.cpp`): the ELF header
  and program headers, then the two PT_LOAD segments in raw deflate pieces of 256 KB (zlib from
  devkitPro's portlibs), 11.3 MB instead of 25.9; a reader thread fetches the next pieces while
  the game thread inflates (they hand slots over through polled counters, not the arbiter's
  waits, see the title-menu hang). Used when its noted size is the library's; otherwise, or on any
  failure, the plain library is read. 4.78 -> 2.22 s. The console package leaves the plain
  library out (25.9 MB): without it the compressed copy is used alone (checked in Azahar: loaded
  "from the compressed copy", frames steady at 15.6 s), and the start-up check accepts either.
- Lookups without requests: the game files' folder is listed once (lower-case names), which
  answers a missing name in it and anything "inside" one of its files; after a second miss in
  another folder one request finds out whether the folder exists, and a missing folder's names
  fail at once (`/sdcard/android/obb/<package>/...` in lower case: 150 names a load); known misses
  are kept by the guest's own path string too, before any host path is built; a write only
  forgets what it can make appear (the path, what is under it, the folders above it: the game
  writes its analytics files during loads); `fstat` of a game file is asked once. Chapter 3's
  load: ~470 lookups reaching the SD card -> ~35.
- Reads straight from the descriptor: newlib's `fseek` (it takes 1 KB as an SD file's block size)
  refills its 16 KB buffer at once from the 1 KB boundary below the position, so every read at
  an unaligned position was a 16 KB request more (not counted as a read); the read cache now
  reads with `lseek` + `read` and keeps the size for `SEEK_END` (newlib's is a size request and
  a read). The FILE's own buffer serves only other stdio calls, which seek first.
- `sscanf` (every platform): its string conversions' buffer is sized by the input, once per
  thread, instead of 64 KB allocated and cleared per call.
- Exports (`LoadedLib`, `lib_export`): looked up in the image's own symbol table through its ELF
  hash table instead of 40,850 names copied into a map (5 MB of the host heap).
- The texture pack: its CRC-32 eight bytes at a time and only for sizes the pack has, the pack
  file kept open; ETC1 blocks byte-swapped by words. `init_fs` keeps the generated /proc and /sys
  files of an earlier start (same size) instead of removing and writing them again (0.6 s).

**What is left.** Chapter 3's load (28.3 s): reads 5.5 s (28.6 MB, the data the game asks for,
throughput-bound in Azahar's model), opens 0.7 s, `glLinkProgram` 0.65 s, the wrappers ~1 s,
and the game's own work ~20 s (a third of that on a console). The boot (15.9 s): the library's
read 2.2 s, the patch set from the RomFS 0.85 s, the game's JNI init 1 s, then 11 s of the
game's boot frames: its menu building (`CMenuManager::Load`, 2.2 s), 58 archive opens (~1 s),
its analytics files (`gv3/Timer.bin`, `Token.bin`: about 70 opens and stats, read and written
again each cycle, ~1.4 s), reads 1.5 s, ETC1 uploads. Not done: serving the mount without
opening the archives (trusting the index without its check), read-ahead during loads (the next
read is rarely predictable: 1,100 of 2,000 reads jump), caching the analytics files.

## The game files in the RomFS

`tools/3ds_cia.py` puts the console's game files (`build/3ds/slim_data` without `X` where `X.z` is
there, without `hd_textures.pak`, `test.txt`, `nova3.ini`: 59 files, 0.98 GB) in the title's
RomFS next to `v6patch.bin`, as `romfs:/nova3_data/`: one file to install, like the Switch's
`.nsp`. makerom (Project_CTR 0.19) builds the RomFS (its `RomFs: RootPath`, a folder of hard
links in `build/3ds/cmake/romfs`) and both outputs in 11 s, 0.95 GB of memory at most;
`nova3.cxi` and `nova3.cia` are 992 MB each (a `.cia` must stay under 4 GB: FAT32, FBI; the
script refuses one that does not). `host/3ds/main.cpp` reads `sdmc:/3ds/nova3/nova3_data/` when
it holds the library (or its compressed copy), else `romfs:/nova3_data` (the log: `game files:
romfs:/nova3_data`).

Everything that reads game files (`runtime/libc_io.cpp`: `stat`, the folder listing for missing
names, `open` / `read` / `lseek`, stdio, the mount index, the packed archives;
`runtime/native_3ds.cpp`: `libGloftNAHM.so.z`; the texture pack) goes through libctru's `romfs:`
device unchanged, with the same caches (the kept files, the read cache, the known misses, the
block pool). The device's opens, stats and listings cost no request (its tables are read into
memory at `romfsInit`); each read is one `FSFILE_Read` on the RomFS's single file. Its reads are
made one at a time (a lock around the device's `read_r`, `host/3ds/main.cpp`): a console's FS
serves one session's requests one at a time anyway, but Azahar runs each RomFS read over 8 KB
on a host thread of its own, and two at once (the title menu's loading on the game thread while
the sound thread streamed its music) froze the emulation at the title menu's loading screen;
Azahar itself died of a stack overflow six minutes later. With the lock the run below went
through.

Azahar (New 3DS mode), the same build and `.cxi`, `bench 3 900 walk`, one run each, the game files
on the emulated SD card (`3ds_run.py --deploy`) and then only in the RomFS (`--no-sd-data`), with
the stub `dspfirm.cdc` (`build/3ds/dsptest/run_sdmc.log`, `run_romfs.log`):

| | SD card | RomFS |
|---|---|---|
| the library (compressed copy) read and loaded | 2.20 s | 1.79 s |
| boot: the game initialised; frames steady; frame 300 | 4.8; 15.9; 26.4 s | 4.3; 14.4; 25.3 s |
| the boot's slow frames: opens; reads | 69 in 1.27 s; 5.8 MB, 108 reads in 1.22 s | 69 in 0.91 s; 5.8 MB, 108 reads in 1.25 s |
| chapter 3's load; its slow frames' reads | 28.5 s; 15.4 MB, 287 reads in 3.1 s | 29.5 s; 15.8 MB, 297 reads in 3.8 s |
| the walk; its busiest window (step: GL + file I/O + game code) | 27.9 fps; 24.7 fps (13.4 + 2.2 + 23.7 ms) | 27.9 fps; 24.8 fps (13.3 + 2.4 + 23.5 ms) |
| audio underruns | 0 | 0 |

Azahar models its own costs: its SD card a request and 183 ns a byte, its RomFS reads other
delays, and neither is a console's. On a console a RomFS read of an installed title costs more
than an SD read of the same bytes: the title's content on the SD card is encrypted with the
console's SD key (AES-CTR, in hardware) and the RomFS's hash tree is checked as it is read
(SHA-256, in hardware), all of it in the ARM9's FS (Process9), which Azahar cannot show; opens
and stats cost nothing. To be measured on the console (the `loading:` lines and `files:` lines
give the bytes and the time of the reads).

The DSP firmware's paths in Azahar (`build/3ds/dsptest/`, the game files from the RomFS each time):
the file there (the stub): `ndsp`, as above. The file missing, no HOME Menu: `audio: no
sdmc:/3ds/dspfirm.cdc and none extracted (USA: not readable (c8804478); JPN: ... TWN: ...)`,
`ndsp unavailable ...: CSND instead`, `CSND 32728 Hz stereo (channels 8 and 9)`, the title
menu's music mixed (`first sound`), then `bench 3 300 walk` (chapter 3's load 29.3 s, 26.1 fps
over its first 300 frames) with no underrun and no late chunk (Azahar's CSND is a stub: silent).
The file missing, `env NOVA3_DSPFIRM_CODE` naming a `.code` packed with the ExeFS's LZSS
(`build/3ds/dsptest/blz.py`, whose decoder, DSP1's, unpacks makerom's own `.code` of `nova3.cxi`
to the ELF's text byte for byte) that holds a made-up firmware with two hashed segments, not
readable in the packed bytes: `DSP firmware extracted from ... (unpacked code, 1024 bytes,
segment hashes checked) to sdmc:/3ds/dspfirm.cdc in 0.10 s`, the file written equal to the
made-up firmware, then `ndsp 32728 Hz stereo` and the title menu's music. Not testable without
a console: FS opening the HOME Menu's code with the exheader's access bits (DSP1, a `.cia` with
the same bits, does exactly this on Luma3DS consoles), and the real firmware's layout (DSP1 finds
it the same way).

## Building and running

```
python tools/v6patch.py                 # build/3ds/v6patch.bin (needs Unicorn only for --verify)
python tools/hooktable.py               # build/3ds/native_table.inc (after OVERRIDES changes too)
C:/msys64/usr/bin/bash.exe -lc "cd /e/nova3 && cmake -S . -B build/3ds/cmake -G Ninja     -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/3DS.cmake -DCMAKE_BUILD_TYPE=Release"
python tools/memcap.py --limit 10G -- C:/msys64/usr/bin/bash.exe -lc "cd /e/nova3 && ninja -C build/3ds/cmake -j4 nova3"
python tools/3ds_shadercheck.py         # the vertex shaders' binary against the console GPU's rules (after shader changes)
python tools/3ds_slimdata.py build      # build/3ds/slim_data: the 3DS's game files (Slim data, above)
python tools/3ds_cia.py                 # build/3ds/cmake/nova3.cxi (Azahar) and nova3.cia (consoles), the game files in the RomFS
python tools/3ds_package.py             # build/3ds/package/nova3.cia
python tools/3ds_slimdata.py index; python tools/3ds_slimdata.py lib  # (after changing slim_data by hand)
python tools/3ds_slimdata.py pack; python tools/3ds_slimdata.py verify  # the packed archives (X.gla.z) alone, checked
python tools/3ds_run.py --no-sd-data    # boot nova3.cxi with the game files from its RomFS, print the log
python tools/3ds_run.py --deploy        # game files to Azahar's SD card (read instead of the RomFS's)
python tools/3ds_run.py --test t.txt --wait "bench: chapter" --timeout 600 --shot out.png
```

- `tools/3ds_cia.py`: makerom (`build/tools/makerom.exe`, Project_CTR 0.19) with
  `host/3ds/nova3.rsf` (178 MB mode, 804 MHz + L2, core 2, the SVCs incl. ControlProcessMemory
  and Backdoor, the FS access for the DSP firmware), the game's icon as SMDH, and a RomFS
  holding `v6patch.bin` and the game files (`--data DIR`, default the slim set; `--no-data`
  without them: 4.6 MB). Title id `000400000F4E3300`. No banner.
- `tools/3ds_run.py`: boots `build/3ds/cmake/nova3.cxi` by default; holds `build/3ds/run.lock`;
  sets Azahar (`E:/Emulators/Azahar`, 2126.2, portable `user/`) to New 3DS mode;
  `--no-sd-data` removes the emulated card's `nova3_data` (hard links) so that the RomFS is
  read; `--dspfirm stub` / `none` puts the stub firmware (below) on the card or removes it;
  `--deploy` hard-links the slim set `build/3ds/slim_data`
  (or with `--data DIR` another set, e.g. `dist/nova3_data`, whose `hd_textures.pak` is left out)
  to `user/sdmc/3ds/nova3/nova3_data`, as the console package does without `X` where the set has
  `X.z` (removed from the card too); boots the ROM, follows `sdmc:/3ds/nova3/nova3.log` until
  `--wait` / `--fail` match or `--timeout` (`--mark REGEX` prints matching lines with the
  seconds since the boot), captures the window (`--shot`, `--shot-every`), closes Azahar. Azahar
  opens as a normal window that does not take the focus (STARTUPINFO `SW_SHOWNOACTIVATE`; for its
  first 10 s a thread hands the foreground back to the window that had it when Qt activates its
  own; `pauseWhenInBackground=false`). `--visible` lets it take the focus; `--minimized` keeps it
  minimized for the whole run (a capture then restores it off-screen for the moment), which the
  user does not want by default. Prefer few long runs to many short ones: each start is a window
  on the user's desktop.
- Sound in Azahar: its HLE DSP needs `sdmc:/3ds/dspfirm.cdc` to exist but not the real firmware;
  a 768-byte stub (`DSP1` at 0x100, the size at 0x104; `3ds_run.py --dspfirm stub`) on the
  emulated card is enough for `ndspInit` (`audio: ndsp 32728 Hz stereo`), so `audio: underrun`
  lines can be checked there. Azahar has no HOME Menu (the extraction logs `not readable
  (c8804478)` for each region) and its CSND is a stub that plays nothing (the CSND path runs,
  silent; its pacing and underruns can still be checked).
- On a console: install the `.cia` (FBI). Nothing else to copy.
- `tests/3ds_hello.c` (target `nova3_hello`): the packaging check. In Azahar (New 3DS mode):
  application region 182,272 KB, regular heap 130,812 KB at 0x08000000 (up to the main
  thread's stack), linear 51,060 KB; `svcControlProcessMemory` is not implemented there
  ("unimplemented SVC function 70", the page stays RW) but Azahar executes RW pages anyway.

## Testing on a console

Quick rounds (while the console bring-up goes on): `python tools/3ds_package.py --sd` makes
`build/3ds/package_sd/nova3.cia` (4.9 MB: the program and `v6patch.bin`, no game files; the same
title, "N.O.V.A. 3 (SD test)" on the HOME Menu) and `build/3ds/package_sd/sdmc/3ds/nova3/
nova3_data.romfs` (the game files as one RomFS image, 0.98 GB, devkitPro's `mkromfs3ds`; made
again only when they changed). Copy it to the SD card once and install the small `.cia`; for each
new build only the small `.cia` is installed again (seconds); the package says when the image
changed. The game mounts the image (`mount_sd_image` in `host/3ds/main.cpp`: `sdgame:/nova3_data`)
and reads it through one file system handle, as the `.cia`'s RomFS. A folder of game files on the
SD card is read too (ahead of the RomFS, after the image; Azahar's `3ds_run.py --deploy`), but on
a console it ran out of open files (Console bring-up, 9). The game executable stays in the
`.cia`: a forwarder through the Homebrew Launcher would run without this title's exheader (178 MB
mode, svcControlProcessMemory, svcBackdoor). Back to the full `.cia` below: delete
`sdmc:/3ds/nova3/nova3_data.romfs` (and any `nova3_data/` folder) first. Azahar: the image
hard-linked to `user/sdmc/3ds/nova3/`, `python tools/3ds_run.py build/3ds/cmake/nova3_sd.cxi
--no-sd-data` (13:03 build: `game files: sdgame:/nova3_data`, ndsp, the title menu reached).

1. Build (above): `python tools/3ds_slimdata.py build`, `python tools/3ds_cia.py`, then
   `python tools/3ds_package.py`: `build/3ds/package/nova3.cia` (992 MB), the game files (the
   slim set's 59 console files, 0.98 GB: `mount.idx`, `libGloftNAHM.so.z` instead of the plain
   library, the stream and sound archives packed, see Slim data) in its RomFS. No `test.txt`, no
   `nova3.ini` (written with the defaults, 30 fps, at the first start). `--sdmc` also lays out
   the old SD card copy (`sdmc/3ds/nova3/nova3_data/`, read instead of the RomFS's when present;
   `--data dist/nova3_data` the full set, 1.72 GB, without `hd_textures.pak`).
2. A New 3DS / New 3DS XL with Luma3DS (custom firmware; it grants the exheader's SVCs and FS
   access). Copy `nova3.cia` to the SD card and install it with FBI. Nothing else: no game files
   to copy, no DSP1 (the first start extracts the DSP firmware from the HOME Menu if
   `sdmc:/3ds/dspfirm.cdc` is not there yet, see Platform; otherwise the sound goes through CSND).
   Delete an old `sdmc:/3ds/nova3/nova3_data/` from an earlier package, or it is read instead of
   the RomFS's.
3. Send back `sdmc:/3ds/nova3/nova3.log`: the start (`game files: romfs:/nova3_data`; the
   `audio:` lines: `DSP firmware ... (N bytes)`, `extracted from the HOME Menu (...)` or `no
   ... none extracted (...)`, then `ndsp 32728 Hz stereo` or `CSND ...`), the `perf:` line every
   300 frames (fps, step = GL + file I/O + waits + game code, swap), the `files:` lines (MB read
   in how many requests, unpacked), the `loading:` lines of each load (reads, opens, their
   time: what the RomFS costs on a console), the `memory:` lines (game heap, host heap, GL
   memory, free linear memory and its lowest), `slow frame` lines, `audio: underrun` / `CSND
   feeder late` lines, `=== crash` / `=== watchdog` reports with their `guest threads:` lists,
   and `sdmc:/3ds/dspfirm.cdc`'s size if it was extracted; after a crash also Luma's
   `sdmc:/luma/dumps/arm11/crash_dump_*.dmp`. Whether the sound plays (and through which path).
   A level load is a long pause with the last frame on the top screen and `loading ... s` on
   the bottom one. Also the folder `sdmc:/3ds/nova3/shots/` (`fNNNNN.png`: what the GPU drew at
   frames 60, 150, 300, 450, 600, then every 900, 12 at most; compared with Azahar's same
   frames) and the startup checks' lines (`pica: cache check`, `pica: draw test`, `pica: startup
   check`). The log survives a console that has to be switched off (it is flushed to the SD card
   at least once a second: `runtime/runtime.cpp`); the game overwrites it and the screenshots at
   each start.

## Console bring-up (2026-10-11 / 12, New 3DS with Luma3DS)

What each console start found, in order (logs in `dumps/3dsrealhw/`):

1. Static constructors: Thumb-2 long calls (see the patch set) and ARMv7 words inside Thumb code.
2. The first `.obb` read: a `ubfx` in zlib's `inflate`, outside every function's own walk: the
   patch set now walks the whole of `.text` (see Which code is reachable, Never a literal).
3. The loading screen, then no frame (build of 2026-10-12 00:56): the watchdog showed the game
   thread making no import call after `SocialMgr::Update`.
4. The hang probe (01:33): no fault, so the game thread was outside the game's code.
5. The stack scan (02:02): `GaiaMgr::Update` -> `OS_GetTime` -> `gettimeofday` -> `osGetTime`,
   the same in the 15 s and 120 s reports; the probe stopped the other threads (the console
   enforces the permission). Gaia is off since (offline game), the clock no longer calls
   `osGetTime` per frame.
6. The priority raise (09:48): the main thread kept running, the feeder woke ~205 times a
   second, and the raise changed nothing: not starved. The game thread waits in
   `C3D_FrameBegin` -> `C3Di_WaitAndClearQueue` -> `gxCmdQueueWait` -> `gspWaitForAnyEvent`
   (from `glClear`): the GPU never finished the frame before. A PC trace with the 3DS's settings
   (`NOVA3_DEVICE=3ds NOVA3_GLTRACE_FRAMES=0-12`) shows frames 0-2 without draws (clears and
   transfers, which the console finished) and frame 3, the loading screen, with three draws
   from client memory (6, 24 and 42-60 vertices, alpha blending, one with split alpha): the
   console's GPU stops on that frame; Azahar's does not. `draw_elements` writes the same
   commands as citro3d's `C3D_DrawElements`.
7. The startup GPU check (10:13, `check_draw` in `runtime/gles_pica.cpp`): for the first three
   frames that draw, each draw is submitted, citro3d's queue started (it only runs at the
   frame's end otherwise; its completion callback swaps the screens only after `C3D_FrameEnd`)
   and waited for, 3 s at most; one the GPU does not finish is described in the log (program,
   primitive, vertices, attribute and buffer configuration, textures with their physical
   addresses, blending, viewport, target) and the game stops. In Azahar all pass (`pica:
   startup check: frame 3 draw 0 done ...`). On the console: `did not finish frame 3 draw 0`
   (a 6-vertex indexed draw, position + colour from the ring, three fixed attributes, no texture).
   That result is not evidence: the check ran citro3d's queue after `C3D_FrameSplit(0)`, which
   passes no flush to GSP, without `ring_flush`, and left the queue running for the rest of the
   frame. On a console the GPU read the command list and the vertices from memory while they were
   still in the CPU's caches; Azahar keeps no caches.
8. The audit after it (12:44 build). The draw's commands match citro3d's `C3D_DrawElements` word
   for word, its layout is valid (buffer 0: position float x3 + colour ubyte x4, stride 16; all
   addresses in linear memory, 28-bit offsets), the shaders have no loops, and the GSP event
   thread (libctru: priority 0x1A, the default core) was not starved (the main thread at 0x2C on
   the same core kept running). The frames the console finished (0-2) had no command list, only
   fills and transfers; every stall was the first frame with one. Until then each frame's end was
   `C3D_FrameEnd(0)`, whose only flush of the command list is one `GSPGPU_FlushDataCache` of the
   whole linear heap (80 MB here, 32 MB in libctru's default layout), result unchecked. Now:
   - the backend flushes what it writes and nothing else: `GX_CMDLIST_FLUSH` on every submission
     (GSP flushes the command list), `submit_split` (the ring, then the split) for `glClear` and
     the check, the ring's overflow blocks too (they were never flushed), and a render target that
     falls back to linear memory once at its allocation (Graphics: Caches in `gles_pica.cpp`);
   - the check flushes before it runs the queue, stops the queue again afterwards, and on a
     timeout logs the GPU's busy bits (`GSPGPU_ReadHWRegs` 0x1EF00034: P3D bit 31, PPF bit 30) and
     the PICA's command list registers 0x238-0x23C next to the list it submitted (physical address,
     words, the last two: the finalize write). P3D busy: the GPU is stuck on the commands. P3D
     idle: it finished and the completion was not seen;
   - the cache check (`cache_check`, `pica: cache check:` lines at the start) tests the New 3DS's
     caches, the 2 MB L2 included (the exheader and `osSetSpeedupEnable` turn it on; CP15
     maintenance, as the port's code loader does through `svcBackdoor`, reaches only the L1): the
     CPU writes 64 KB (twice the L1), one method flushes it, the GPU's DMA (a TextureCopy, which
     flushes nothing) copies it, and the line gives the share of new words the GPU saw ("fresh")
     with the call's result and time. Methods: none, the L1 cleaned with CP15, GSP's flush of the
     buffer, GSP's flush of the whole linear heap (what `C3D_FrameEnd(0)` did every frame). On a
     console "none" and "L1 clean" near 0% fresh mean the L2 holds the CPU's writes; "whole
     linear heap" below 100% is the cause of the earlier stalls. Azahar: 100% for all (no caches).
   Azahar (build 12:44:53): the checks pass, the title menu draws.
9. The console with that build, the game files as a folder on the SD card (13:00):
   - cache check: none 81% fresh (18% stale), L1 clean (CP15) 100%, GSP's flush of the buffer
     100% (0.11 ms), GSP's flush of the whole linear heap 100% (0.11 ms). The L2 holds none of
     the CPU's writes (64 KB, twice the L1: what stays stale is what the L1 kept); a clean of the
     L1 is enough, and the whole-heap flush of `C3D_FrameEnd(0)` works and is cheap. So the
     stalls before the 10:13 check were not a missing flush: still open;
   - the folder broke the boot: opens of game files failed part way (`files: cannot open
     .../weapons_stream.gla.z`, `sounds_hi.gla.z`; `levels.gla`, `particlesystems4.gla` not
     added), and `ndspInit` could not open the DSP firmware found at the start (`ndsp
     unavailable`, CSND instead): the console's FS refuses more open files than the game holds
     (every archive it keeps open is a handle; the RomFS reads all through one). The SD card's
     game files are now one RomFS image (Testing on a console);
   - the log ends with the game's shader messages before the loading screen's draws: no startup
     check line, no watchdog, no main thread line afterwards.
10. The console with the SD card image (13:03 build; Luma: the New 3DS clock and L2, and
    "Redirect app. syscore threads to core2" on): the game files open (ndsp too), the cache
    check as before, and the same end of the log; the screen stopped at "loading 11 s", so the
    process ran on after the log's last line. Findings and changes (13:39 build):
    - the log lost its end: its size on the SD card changes only when FS flushes the file, and a
      console switched off after a freeze keeps nothing since. The 3DS's log writer now `fsync`s
      at most once a second and `log_flush` every time (`runtime/runtime.cpp`); the startup
      check logs and flushes each draw before the GPU runs it (`... to the GPU: program ...`);
    - the vertex shaders broke 3dbrew's GPU/Pitfalls rule "each configured output component has
      to be written exactly once or the PICA freezes": the texture coordinates (o2-o4) were
      written as `.xy` alone, z and w never. The console-tested shaders (citro2d, hbmenu) write
      whole registers. Every output register is now written whole, once (texture coordinates
      z, w = 0, 1); `tools/3ds_shadercheck.py` checks the binary (DVLB parsed as libctru does,
      every output component of every entry point written exactly once, none in a conditional
      block, no two MOVAs in a row): the old binary has 14 errors, the new none;
    - the same page: "the vertex stride ... must match the actual size of the vertex ... or the
      PICA will freeze". `buffers()` padded the gaps between a buffer's attributes but not the
      rest of the vertex after the last one (a 32-byte vertex of which the program reads 12
      bytes). It now pads to the stride; a layout padding cannot describe is logged once
      (`a vertex of N bytes described by M`) and counted (Stats::stride_mismatch): none in the
      title menu and chapter 3's bench in Azahar;
    - each thread's start logs its core (`on core N`): Luma's syscore redirection moves the
      log writer and the watchdog (core 1) to core 2;
    - Azahar: the startup checks pass, the title menu, chapter 3's bench 31.8 fps (27.4-28.3
      before), its play drawn as before; the `.cia` with the SD card image boots.
11. The console with the 13:39 build: no freeze. The startup draws finish (0.05-0.6 ms), the
    title menu and a campaign level run (2,700 frames, play at 26 fps: GPU 19 ms and game code
    19 ms a frame), exit through HOME; threads: the game thread on core 0, all others on core 2
    (Luma's redirection takes the core-1 ones there). But everything is drawn wrong, 2D and 3D:
    garbled textures, broken and missing geometry. The log has no flush failure, no stride
    warning, the game's own messages as in Azahar. Checked since: citro3d's starting state (its
    first context sends blending, depth, stencil, alpha test, combiners and texture units; only
    fragment lighting is never written, which the combiners do not read), the ETC1 parameter bit
    (set by `C3D_TexInitWithParams` for every texture), the shader binary's size (152 of 512
    instructions, 37 of 128 operand descriptors). The 14:01 build:
    - saves what the GPU drew as PNGs, `sdmc:/3ds/nova3/shots/fNNNNN.png` (frames 60, 150,
      300, 450, 600, then every 900; 12 at most; `screenshot:` lines): `read_screen` now has the
      GPU copy the colour buffer from VRAM to linear memory (GX TextureCopy) and reads that, not
      VRAM through the CPU. Azahar's same frames: `build/3ds/screens/ref_azahar/`;
    - clears the GPU's texture cache whenever a texture's storage is written in place
      (`tex_written`: flushed, every unit bound again at the next draw; citro3d clears it only
      when a binding changes, and Azahar has no texture cache);
    - checks every flush of GPU data (`pica: the flush of ... failed`).
12. The console's screenshots (14:01 build) against Azahar's: the title menu's full-screen
    background right, its panels and text slivers; in play almost nothing; the loading screen's
    black full-screen quad (indices 0 1 2 1 3 2) a red ramp peaking at its vertex 1's corner,
    where the previous frame's spinner (red, drawn last) has its own vertex 1. Each draw got
    vertices of the draw before by index number: the PICA's post-vertex cache keeps transformed
    vertices by index, and citro3d's GPUREG_VTX_FUNC write after each draw (the same sequence in
    `draw_elements`) did not clear it on the console; Azahar has no such cache (citro2d, the
    common citro3d user, draws arrays only). The 14:27 build gives every indexed draw index
    numbers no draw used lately (`fresh_indices`: the draw's indices rewritten into the ring as
    16-bit `i - lo + k`, k from a running counter that wraps at 65536; the vertex buffers'
    addresses moved back by k vertices, so the GPU reads the same vertices; the terrain's second
    pass takes its own k). The element buffers are no longer read by the GPU directly. Azahar:
    the title menu and chapter 0 as the references, chapter 3 drawn right; its bench 26.1 fps
    (31.8 before: scanning and rewriting the indices, GL 6.0 -> 10.1 ms in Azahar, about a third
    of that on a console; a cache of the element buffers' index ranges is the next step).
13. The console with the 14:27 build: drawn as wrong as before (so not, or not only, the
    post-vertex cache). Checked since: citro3d uploads float uniforms exactly as `send_uniforms`
    (`0x80000000 | index`, four words a register) and the shaders' constants from its own packed
    copy (`C3Di_ShaderFVecData`), not from `C3D_FVUnif` (whose constant registers `setup` poisons
    with NaN); the shaders' uniforms (c0-c44) and constants (c95) do not overlap. The 14:46
    build runs a startup draw test (`draw_test`, `pica: draw test N` lines): the game's
    generic.v.pica draws a red quad over the left half of a blue 64x64 target, read back through
    the GPU (2048 red, 2048 blue is right), in five variants that each add one thing the game's
    draws have: 1 citro3d alone with every input from a vertex buffer and C3D_DrawArrays, 2 fixed
    attributes, 3 C3D_DrawElements, 4 the backend's uniforms and `draw_elements`, 5 positions in
    pixels through a scaling matrix and alpha blending (the loading screen's first draw). Azahar:
    all five right.
14. The console with the 14:46 build: all five draw tests right, and the game drawn right, 2D
    and 3D (the earlier "still the same" run was most likely an older build: the fresh indices of
    14:27 were the fix). Left: floors in chapter 0's office in jagged patches, a grey tiled floor
    breaking through the wooden one, ragged edges on pillars: depth fighting between surfaces a
    few millimetres to centimetres apart. Cause: the console's vertex shaders compute in float24
    (16-bit mantissa), Azahar in float32; the level's static geometry is in world coordinates
    (x, y about 2000-2300) and the depth is a sum of terms that large cancelling to a few metres
    (an error near 0.03 units). The 15:08 build stores a buffer object's float positions relative
    to the centre of the vertices each draw uses and folds that centre into the draw's
    matrices on the CPU in float32 (`position_origin`, Graphics: Precision). Azahar: chapter 0's
    office and chapter 3 drawn as before (25.7 fps in chapter 3's bench).
15. The console with the 15:08 build (requests): the HOME button crashed the game (the
    watchdog's hang probe after 120 s suspended: now paused while suspended), the hands cut into
    the weapon (float24 again: client-memory positions now recentred too), the sound late (the
    queue at most 80 ms on the 3DS, was 128: about 50 ms less), the enemies too strong and the aim
    assist weak (Gameplay fixes, Easier campaign on the 3DS), the Controls page offered Keyboard &
    Mouse (Input). Loading on weapon changes: to be measured on the console (the slow frames' file
    I/O and unpacking times).

## Console status (2026-10-12, New 3DS with Luma3DS, the SD card image)

- Works on the console: the boot, the title menu, campaign levels (play about 26 fps in chapter
  0: GPU ~19 ms, game code ~19 ms a frame), 2D and 3D drawn right since the fresh index numbers,
  sound through ndsp, the game files from the SD card image, exit through HOME. Threads: the game
  thread on core 0, the others on core 2 (Luma's syscore redirection included).
- Built, not yet run on the console (15:58 build): the floors' and the hands' depth fighting
  (positions recentred, Graphics: Precision), the HOME Menu crash (the watchdog paused while
  suspended), the easier campaign and stronger aim snap ([Gameplay fixes](gameplay-fixes.md#easier-campaign-on-the-3ds)),
  the Controls page with one Controller button ([Input](input.md#the-controls-page)), the sound
  queue at 80 ms ([Audio](audio.md)).
- Open: the pause when changing weapons (streamed models and textures: whether SD reads,
  unpacking or the game's own work, from a console log with the slow frames' times); the CPU
  cost of rewriting every draw's indices (chapter 3 in Azahar 31.8 -> 25.7-27.9 fps); the game's
  other auto-aim fields (not known well enough to change).

## Status (2026-10-10, Azahar 2126.2, New 3DS mode)

- Works: loading (25 MB image, 63,000 relocations, 143,859 patch-set sites, 137 hooks, 53
  kernel helper fixups), the 174 static constructors, JNI, the boot sequence, the game's
  profiles (`GameOptions profile CPU = CPU_0`, `GPU = 0`, `MEM = MEM_256`), the title menu at
  30 fps, the campaign chapters below at or near 30 fps. Azahar's CPU timing is only indicative
  (it runs the ARM11 at about a third of its speed and does not model the GPU's time).
- Chapters in Azahar (`bench CHAPTER 900 walk`: from the title menu into the level, 900 frames
  walking forward and turning; "load" is from the menu's StartLevel to the first frame in
  play; perf windows in play):

  | Chapter (level file) | Load | fps (900 frames) | Busiest perf window: fps, step = GL + game code | Guest heap live (peak) | Linear free |
  |---|---|---|---|---|---|
  | 0 (San_Francisco_1) | 51 s | 30.0 | 30.0, 9.1 ms = 2.4 + 6.4 | 44 MB (57) | 14 MB |
  | 2 (Ship_01) | 44 s | 29.6 | 29.7, 12.4 ms = 7.8 + 4.1 | 40 MB (53) | 19 MB |
  | 3 (therrius_2) | 45 s | 25.4 -> 28.3 | 21.0, 47.0 ms = 20.4 + 24.1 -> 25.5, 37.9 ms = 14.1 + 21.7 (Graphics: send_state) | 44 MB (63) | 17 MB |
  | 4 (Boreas_1) | 39 s | 30.0 | 30.0, 11.5 ms = 4.3 + 7.2 | 40 MB (49) | 22 MB |
  | 5 (Boreas_2, terrain) | 32 s | 28.6 | 26.8, 30.9 ms = 8.4 + 17.8 (+ file 4.4) | 30 MB (36) | 25 MB |
  | 6 (08_volteron2) | 51 s | 29.5 | 28.9, 21.4 ms = 5.6 + 14.2 | 42 MB (60) | 17 MB |
  | 8 (13_volteron_1) | 42 s | 30.0 | 30.0, 23.1 ms = 10.7 + 12.2 | 39 MB (46) | 24 MB |
  | local match, map 0 (host) | - | - | 30.0, 13.7 ms = 4.5 + 8.7 | - | - |
  | 3 at the 60 fps cap | 44 s | 27.4 | 34.1 in the window after the busiest (21.3 there) | | |

  Title menu: 30 fps, step 7.5 ms = GL 1.5 + game code 6.0 (9.1 = 1.6 + 7.1 before).
- Menus on the bottom screen (Platform: Screens), Azahar, the package's build
  (`build/3ds/screens/`: `t6.txt` and its captures `final_*.png`, both screens in each): the
  title menu at 320x240 with touch (Multiplayer -> its screen with Local Wi-Fi, Back, Campaign ->
  campaign select; the Local Wi-Fi create / join / armory screen only on the PC at 320x240), chapter 0 started -> its loading screen on the bottom (`loading ...` on the top) ->
  the cinematic and play on top with the HUD as on the PC at 400x240 -> Start: the pause menu on
  the bottom, Options and the exit dialog by touch -> Resume by touch: play on top, HUD and view
  as before -> `EndLevel()`: the results on top -> Back to Menu (a touch, scaled) -> the title
  menu on the bottom again, Campaign by touch. No crash in seven runs (Azahar itself exited once
  11 s into a boot, `0xC0000409`, before the game drew a frame; the same build's next run went
  through). Title menu 30 fps, step 7.55 ms (unchanged). `bench 3 900 walk`: load 29.5 s, the
  walk 27.8 fps, busiest window 24.7 fps (13.3 + 2.5 + 23.4 ms), linear memory free 20.9 MB in
  the level: as before (27.8-28.3; 24.6-25.7; 20.9-21.0). Not checked: a level's own end (a
  whole chapter) and the end of a match (GS_EndMultiplayer, a 10-minute match), the death and
  respawn screens, GS_IGMLoadout (multiplayer loadout, on the bottom), touch on hardware.

  (GL is measured in 1 frame of 8, through the gates; the other frames call most GL functions
  directly.) Before this session: `bench 0 900 walk` 24.5 fps, chapter 0's load 75-222 s.
- Level loads are still one long step of the game (the bottom screen shows `loading ... s`
  meanwhile): 30-50 s in Azahar with the full data; the slim data (textures stored at the size
  the profile uploads them) takes chapter 3's from 44.9 to 37.5 s, and the request work of Boot
  and level loads to 28.3 s, now mostly the game's own CPU work (inflating, building meshes,
  collision trees, textures) with reads a fifth of it. The boot to the title menu: 35.4 -> 15.9 s.
  On a console the CPU is about three times faster and the SD card has its own speed: to be
  measured there.
- Fixed: the **title-menu hang** (about 1 run in 4): the game thread (`GS_Menu::Create` ->
  `CUpgradesManager::Load` -> `Application::DecryptAndLoad`) waited for Gaia's mutex, held by a
  Gaia worker (`Gaia::PerformAsyncAction` -> `InternalInitialize` -> `Pandora` ->
  `BaseServiceManager`) waiting for glwebtools' core mutex, held by the glwebtools update thread
  (`GlWebToolsCore::Update` -> `ThreadPool::Update` -> `~JobRunner` -> `JobRunner::Join` ->
  `pthread_join`), which waited for a pool thread that had ended long before (the report:
  `joins thread 20`, and `thread 20: ends` early in the log). Its wakeup was lost: the join
  slept on the runtime's condition variable and was never woken, though the thread ended after
  it started waiting. The runtime's side was the textbook pattern (the ending thread sets
  `finished` under the threads' mutex, then notifies; the joiner checks `finished` under the same
  mutex in a predicate loop), and the same build never hangs on the PC or the Switch: a lost
  wakeup in libctru's `CondVar` (over `svcArbitrateAddress`) or in Azahar's address arbiter, not
  pinned down further. `pthread_join` now polls
  the thread's end (2 ms) and never sleeps on a condition variable; it also behaves as bionic's
  (a thread is joined once: a second join or a join of a detached thread fails at once, and a
  released `pthread_t` is not handed out again for 1024 threads, so a stale handle cannot name
  a live thread). As a defence the 3DS's guest condition waits, `sem_wait` and the emulated
  pipes' reads wait in 100 ms slices (an expiry is a spurious wakeup, which POSIX allows).
- Fixed: level loads 4x faster (Files, above); the guest heap apart from the host's (Memory
  layout); most hot imports without the gate (Native execution).
- Fixed on the way: the null renderer reported no uniforms, so Glitch's pink-wireframe fallback
  recursed until the game thread's stack overflowed (`gles_null.cpp` now reflects the GLSL
  declarations); boost::asio's reactor spun on its edge-triggered interrupter pipe
  (`libc_io.cpp`: EPOLLET on emulated pipes, a Switch fix too); connections outside the local
  network are refused at once (`net.cpp`); the log writer runs above the game threads.
- Open (then): real-console testing, since done (Console status, above); chapter 3's busiest scenes (20.9-24.3 fps in Azahar before
  `send_state`, 25.5 in one run after: the game's own code ~22 ms, GL 14 ms on the timed frame).

## Changes outside the 3DS files

`host/hooks.cpp` / `host/hooks.h` (`hooks::top_state` public; under `__3DS__` `g_font_width`,
read by the font-size wraps: Menu layout's bottom screen), `host/input.cpp` (the 3DS's touch
mapping starts at 320x240),
`runtime/recomp.h` (`g_load_base` a variable under `__3DS__`), `runtime/runtime.h`
(`kNativeLoadBase`, `kNativeImageReserve`, `native_gate_address`, `native_emulator`;
`report_threads`, `import_profile_log`, `PerfTimes::gl_timed`; `IoStats`' unpacked bytes and
time), `runtime/runtime.cpp` (the PC's
memory, loader, dispatch and `rt::call` under `#ifndef __3DS__`; import addresses are gates on
the 3DS; `NOVA3_IMPORT_PROFILE`; GL calls timed in 1 frame of 8 on the 3DS), `runtime/thread.cpp`
(3DS guest threads use host stacks; a guest thread is recognised by its start routine; the
per-thread creation log only with `NOVA3_TRACE_THREADS=1`; a smaller sync table on the 3DS;
each guest mutex notes its holder and lock count and each thread the mutex it waits for, for
`report_threads`; a report at 40, 80, ... running threads; a failed `pthread_create` is
logged), `runtime/libc_io.cpp` (the Switch's in-process pipes on the 3DS too; EPOLLET on
emulated pipes; host request counters and times (`io_stats`, `io_now`); on the 3DS and the
Switch: known-missing paths, closed game files kept open, 16 KB read buffers; on the 3DS: the
lookups, mount index, descriptor reads and kept files of Boot and level loads, and the packed
archives (`Packed`, `kPackedFd`; `_GNU_SOURCE` for newlib's `fopencookie`); `sscanf` without
a 64 KB buffer per call and `NOVA3_TRACE_READS` on every platform), `runtime/libc_misc.cpp` and
`runtime/runtime.cpp` (`lib_export`: exports through the image's ELF hash table when the loader
gives one, the 3DS's; `guest_symbol` reads that symbol table too), `runtime/net.cpp`
(3DS socket constants, SO_BROADCAST ignored, the subnet from SOCU_GetIPInfo; connections outside
the local network refused on all platforms),
`runtime/os.h` (`os::ticks` reads the system tick on the 3DS), `runtime/os_win.cpp` (the
watchdog also lists the guest threads),
`runtime/gles_null.cpp` (program reflection), `runtime/gles.cpp` (`NOVA3_GLPROFILE` subtracts
the timer's own cost per call, on every platform), `host/framestats.h` (the import profile line;
"host heap"; `GL -` for untimed frames; on the 3DS the `files: ... unpacked` line), `host/eventlog.cpp` (the bench logs the level file
and its load time; `NOVA3_PROFILE_LOAD=1`: the Windows profiler over the bench's load),
`CMakeLists.txt` (the `NINTENDO_3DS` branch with `runtime/guest_heap_3ds.c`, zlib and
`host/3ds/dspfirm.cpp`, and the `--wrap` of newlib's allocator for its checks; `heap.cpp` per
platform), `runtime/thread.cpp` (a sync handle counts only in the word it was installed in, on
every platform), `host/audio.cpp` / `host/audio.h` (in the 3DS
branch: the firmware check's place, CSND without the firmware, `audio::stop_output`; the 3DS's queue at most 80 ms), `extern/dlmalloc/malloc.c` (dlmalloc 2.8.6, MIT-0), `runtime/runtime.cpp` (the 3DS's log flushed to the SD card, `fsync`, at most once a second and in `log_flush`), `host/hooks.cpp` and `recomp/arm2c.py`'s OVERRIDES (the easier campaign: `pc_hook_npc_fire`, `pc_hook_auto_aim_coefs`, `pc_hook_player_damage`'s scaling; no change off the 3DS), `host/controls_menu.cpp` (the 3DS's Controls page: one Controller button). Tools: `tools/3ds_shadercheck.py`, `tools/tour.py`
(`NOVA3_TOUR_ARGS`), `tools/3ds_glreplay/picapack.py` (the data folder as an argument),
`tools/3ds_slimdata.py` (`index`, `lib`, `pack`, `verify`), `tools/3ds_package.py` and
`tools/3ds_run.py` (`X` left out where `X.z` is there; the package is the `.cia` alone, `--sdmc`
the old layout; `--no-sd-data`, `--dspfirm`), `tools/3ds_cia.py` (the game files in the RomFS).

## Performance budget (30 fps = 33 ms a frame)

Measured on the PC under `NOVA3_DEVICE=3ds` (the game's CPU_0 / GPU_0 / MEM_256 profiles) at
400x240, `--bench CHAPTER:3000:walk`:

| Chapter | Game thread CPU per frame (PC, translated code) | Draws per frame | Uniform calls (unchanged) |
|---|---|---|---|
| 0 | 0.32 ms | 12 | 65 (84%) |
| 3 | 0.90 ms | 35-125 (up to 80 from client memory) | 246-795 (70-83%) |
| 6 | 0.61 ms | 34 | 376 (69%) |

Estimate: native ARMv6 code on the 804 MHz in-order ARM11 runs roughly 20 times slower than this
PC's translated code, so the busiest walking scenes need about 18 ms of game logic, leaving
6-8 ms for the GL backend's CPU side and the rest. Fights with many enemies cost more and must
be measured on the console. The levers: the New 3DS speedup (804 MHz + L2), the game thread
alone on core 0, Vox and the other guest threads on core 2 (and core 1 within its time limit),
the GL backend's per-draw CPU cost (uniform cache, precomputed materials, one copy per client
array into a per-frame linear ring, one cache flush per frame).

## Rules for work on this port

- Windows only; heavy jobs through `python tools/memcap.py --limit 10G -- ...`.
- **One emulator or game instance at a time across all work**: hold the lock directory
  `build/3ds/run.lock` (create it with `mkdir`; if it exists, wait) while Azahar or `nova3.exe`
  runs, and remove it afterwards.
- The game's files and everything built from them stay local.
