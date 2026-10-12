# Diagnostics

Reference: [Debugging: Performance in the log](../debugging.md).

Added so that a log from real hardware is enough to find what is slow or what went wrong.
Logging is asynchronous (a background writer), so it doesn't stall the game.

| Log line | Platforms | What it shows |
|---|---|---|
| `perf: frame N: ...` | both | Every 300 frames (PC) or 600 (Switch): frame rate, average and worst frame time, late frames, the game thread's step split into GL, file I/O, waits, allocation and game code, the swap, and the game thread's CPU load |
| `slow frame: ...` | both | Each frame over 50 ms (at most 10 per window) with its own split |
| `gl per frame: ...` | both | Draws (and from client memory), program, texture and enable calls with the share repeated, uniform calls with the share skipped, other state calls with the share skipped |
| `gl resources in N frames: ...` | both | Texture and buffer uploads with their size, and deletions |
| `memory: ...` | both | Live JNI objects, guest threads running and ended but not joined; on the Switch also the heap in use |
| `profile: ...` | PC | Every 30 s: the game thread sampled about 500 times a second, by module and hottest functions (`NOVA3_PROFILE=0` turns it off) |
| `imports per frame: ...` / `hooks per frame: ...` | all | Only with `NOVA3_IMPORT_PROFILE=1`: the game thread's import calls per frame and the busiest by name (the 3DS also its hooks) |
| `guest threads:` | all | In watchdog reports, and when 40, 80, ... guest threads run at once: each thread's start routine, the guest mutex it waits for and that mutex's holder, the thread it joins, its guest callers |
| `alloc: ...` | Switch | Only with `NOVA3_ALLOC_LOG=1`: every heap block of 1 MB or more allocated or freed, with its caller for `addr2line` |
| `cpu: boost on/off` | Switch | Loading screens' CPU boost |
| `gl: N programs reused` / `compiled ahead` | both / Switch | The shader program pool and prewarm |
| `audio: underrun N` | both | Output device running dry |

Import timing reads the CPU's cycle counter (`rdtsc`, `cntpct_el0`), and its cost was
measured as negligible. Other options:
- `NOVA3_SAMPLE`: log the game thread's guest callers every second (Switch).
- `NOVA3_TRACE_FILES`: log the game's file lookups.
- The watchdog: prints the game thread's stack after 10 s without a frame.
- Samplers that suspend the game thread (the watchdog, the PC's profilers) must not allocate,
  log or take locks until it is resumed: it may hold the heap's lock. The continuous profiler
  appended its sample before resuming; once in a long while the vector grew while the game
  thread was inside the heap, and the game stopped for good (a tour run sat 20 minutes at 100%
  of a core). It now resumes first.
