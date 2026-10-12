# Performance

References: [Recompiler](../recompiler.md), [Runtime](../runtime.md), [Debugging](../debugging.md).

## Starting point

Real hardware (handheld): 52-56 fps in play. The game thread's core was 88-97% busy; a frame
step took 18-19 ms, of which game code was 13-15 ms and GL 4-5 ms. The audio also dropped out
on the Switch and, less often, on the PC (see [Audio](audio.md)).

## Translated code

- **Interprocedural register and flag analysis** (the `Flow` pass in `recomp/arm2c.py`, the
  `_X` macros): each call stores and reloads only what the callee can read or change.
  Measured per call site:

  | | Before | After |
  |---|---|---|
  | Direct call | 19 stores, 19 loads | 6.1 stores, 8.6 loads |
  | Indirect call | 19 / 19 | 8.1 / 10 |
  | Return | 19 stores | 5.5 |
  | Function entry | 19 loads | 6.4 |

  `NOVA3_X_ONLY=lo-hi` limits it to address ranges for bisecting.
- **Division helpers inlined** (`INLINE_HELPERS`): the EABI division routines become C division.
- **Imports compiled in place** (`INLINE_IMPORTS`): memory, string and math functions called
  directly, without the import table. Added this session: `strstr`, `isalnum`, and the game's
  most frequent thread calls (`pthread_getspecific`, `pthread_self`, mutex lock / unlock,
  about 1,000 per busy frame), which now call the runtime directly (`rt_im_*`). A lock that
  has to wait is still timed for the log.
- **Indirect calls** use a per-call-site cache (one 64-bit word); after the call only the
  registers AAPCS lets a callee change are reloaded.
- **Plain imports**: around GL, the allocator, condition variables and `clock` (host code that
  never runs guest code) only the arguments are stored and r0-r3 / r12 reloaded. A
  `glUniform4fv` call went from 15 stores and 15 loads to 6 and 5.
- **Native hot paths** (`host/native_hot.cpp`): skinning kernels, particle update, animation
  computations and the TrueType glyph map written in C++ (`NOVA3_NATIVE_CHECK` compares them
  with the translated originals).
- **PGO** on the PC (`-DNOVA3_PGO=generate|use`): about 11% less game-thread time.

Together: about 15-20% less game-thread CPU from the analysis and native paths, 3-6% more from
inlining and the call-site caches, and 11% from PGO on the PC.

## GL

- **Uniform values cached per program**: the game sets every uniform before every draw, and
  66-81% of those calls repeat the program's value. They are skipped: about 2,000 fewer Mesa
  calls in a busy frame (3,000 uniform calls).
- **Other state calls filtered**: attribute pointers and enables, buffer bindings, the active
  texture unit, blending, depth mask, front face, constant attributes (1-18% skipped).
- **Mesa's glthread turned off by default** (`[video] glthread`): this Mesa (20.1) cannot queue
  a draw from client memory and drains its queue before each one, and 40-50% of the game's
  draws are like that. Its library has no `_mesa_glthread_upload`; the sync fallback's names
  (`DrawElements`, `DrawArrays`) are in its marshalling code.
- **Mesa versus deko3d** was considered; Mesa stays.

## Measured

On the PC benchmark (`--bench 2:6000:walk`), game-thread CPU per frame went from 0.79 to about
0.74 ms with the GL filtering and direct thread calls. ANGLE's calls are cheap, so the Switch,
where each Mesa call costs about 2 us, should gain more; it needs a real-hardware log to
confirm.

Things measured and left alone:
- **Per-import timing for the log**: no measurable cost.
- **Catch frames** (functions that register a C++ catch frame on entry): under 2 per frame.
- **Remaining per-call costs** (prologue loads, guest memory access): inherent to the design.
  Return checks: since 2026-10-10 only after calls to entries that may return elsewhere (below).

## Second pass (2026-10-10): overhead on every port

Measured with each port's bench: the PC's `--bench CHAPTER:3000:walk --profile 1` (game code
per frame: the average of the perf windows in play), the Switch's new `bench` test-script
command in Citron (`bench 3 3000 walk`; Citron's GL time includes its 60 Hz pacing, so game code
is the figure), the 3DS's `bench 3 900 walk` in Azahar.

**PC.**
- The shipped build (`build/cmake_full`, packed by `tools/package.py`) had no PGO: its cache had
  `NOVA3_PGO` empty. It is a `use` build again, with a profile of the current generated code
  (`build/cmake_pgo`, chapters 0, 3 and 6; [Recompiler: PGO](../recompiler.md)).
- Return checks only where a callee may return elsewhere (`CALLN_X`, [Recompiler:
  Translation](../recompiler.md)): 271,754 of 272,674 direct call sites lose the load, compare
  and branch after the call.

  | Game code per frame (ms) | Before | PGO | PGO + `CALLN_X` |
  |---|---|---|---|
  | chapter 0 | 0.363 | 0.311 | 0.308 (-15%) |
  | chapter 3 | 0.938 | 0.902 | 0.852 (-9%) |
  | chapter 6 | 0.723 | 0.685 | 0.667 (-8%) |

  `tools/tour.py 1 3`: both chapters played through, exit 0, no crash; `--mp 0`: in the match.

**Switch.**
- `nova3_core` was compiled `-fPIC` (devkitPro's platform file sets
  `CMAKE_POSITION_INDEPENDENT_CODE`, and CMake gives a static library `-fPIC`): GCC treated every
  translated function as interposable, so nothing was inlined or optimised across calls, and
  each global was read through the GOT. It is `-fPIE` now ([Nintendo Switch:
  Building](../switch.md)); plus `CALLN_X`, as on the PC.
- Citron, chapter 3, the same 600-frame windows of the walk: game code 6.12 / 6.30 -> 5.80 / 5.69
  ms (-7%); the bench's later windows 5.44 and 4.47. A real console's split needs its log.
- Not done: PGO on the Switch (two more full builds and profiling runs in an emulator).

**New 3DS** (the game's own ARM code, natively).
- What is left to gain outside the game's code is small: in chapter 3's busiest frames 3,400 GL
  calls go straight to the wrappers; the gated imports in play are ~20 a frame (float-argument
  GL calls, `clock`); the driver takes 6.6 ms of 39.6 (195 draws, 29 us each in Azahar), the game
  code 23.8 (`NOVA3_IMPORT_PROFILE`, `NOVA3_GLPROFILE`, one run).
- The game runs ~20,600 barriers a frame there (counted in the PC's translation under
  `NOVA3_DEVICE=3ds`), each through a stub that saves a register on the stack. Cheaper forms
  (in place after an atomic loop, no stack before an `ldrex`) checked out in Unicorn but broke
  the boot in Azahar; dropped ([New 3DS: patch set](../3ds.md)).
- newlib's heap now checks every host free and allocation (the boot crash, [Stability and
  memory](stability-and-memory.md)): 0.15 us for a pair in Azahar, ~60 ms of chapter 3's 28 s
  load (840,000 host calls), 0.01 ms a frame in play.
- Result, Azahar, one run each: boot to steady frames 14.1 s, chapter 3's load 29.7 s, the walk
  27.7 fps, its busiest window 24.6 fps (39.6 ms = GL 13.4 + game code 23.8): as before (14.1 /
  29.8 / 27.8 / 24.5).
