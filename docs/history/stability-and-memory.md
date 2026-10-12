# Stability and memory

References: [Runtime](../runtime.md), [Nintendo Switch: Memory](../switch.md), [Open issues](../open-issues.md).

## The green screen (heap exhausted by thread stacks)

- **Report**: after a while in play, everything turned green, menus included. The log showed
  `glCompressedTexImage2D ... error 0505` (GL_OUT_OF_MEMORY) at a section load, after the heap
  had grown about 16 MB every 10 seconds (34,000 frames, about 11 minutes).
- **Ruled out**: JNI objects stayed flat (about 180-200 over 9,000 frames), and the game
  streams no textures or buffers during play (new `gl resources` log line).
- **Found** with a new allocation log (`NOVA3_ALLOC_LOG`): every 16 MB block was a thread
  stack (`threadCreate`). The game's web services (glwebtools' thread pool) start threads every
  few seconds and leave one in three unjoined. libnx's `pthread_detach` does nothing (it returns
  ENOSYS); only `pthread_join` frees a stack.
- **Fixed for all platforms**:
  - every host thread is detached in the runtime's sense, and on the Switch an ended thread
    queues itself and the next thread start joins it;
  - a guest thread's stack and registers are freed when it ends, and `pthread_join` waits on a
    condition variable.

  The heap now stays flat in play (772 to 761 MB over a match, before 756 to 857 and rising).

## The physics-thread race

The game steps Bullet's physics world on a thread of its own while the game thread ray-tests and
adds and removes objects in the same world. On truly parallel cores that hit objects being torn
down, a pure virtual call or a freed vtable, in a few percent of multiplayer loads. Waiting
hooks were not enough. The physics thread is now off (`pc_hook_use_physics_thread`;
`NOVA3_PHYSICS_THREAD=1` restores it), and every multiplayer map passes.

## Hangs and crashes fixed along the way

- Exit crashes on the Switch: see [Switch port](switch-port.md).
- The log writer hung at startup with `std::thread` / `call_once` and a timed condition wait:
  it now runs on a libnx thread with an atomic flag.
- An out-of-bounds write in the import-kind table (sized by generated imports, not all slots)
  corrupted memory: it is sized to every slot.
- The optimized translation once hung at startup (a list splice into itself) and crashed in
  Lua; both were analysis bugs, found by bisecting with `NOVA3_X_ONLY`.
- Thumb-to-ARM veneers entered mid-function now dispatch correctly.

## The 3DS boot crash in newlib's malloc (2026-10-10)

- **Report**: one Azahar smoke run of the 3DS build crashed at about frame 2 of the boot,
  inside newlib's `_malloc_r` / `_free_r`; the same `.cia` passed on the next start. Azahar's
  log (recovered from that session): unmapped writes of `0x00278A70` / `0x00278A98` to
  `0x00534D88`, then reads at `0x4` / `0xC` from the same functions. Decoded against that
  build's ELF, these are the size written at the end of a free chunk, for a "chunk" at
  `0x2BC2F0` / `0x2BC318`: inside the runtime's sync-object table (`g_sync`, in `.bss`). A
  pointer that was never newlib's had entered its free lists.
- **Looked at**: the guest heap is dlmalloc in memory of its own, so the game's own overruns
  can't reach newlib's blocks; the library's compressed copy (one reader thread, joined), the
  mount index (its parts live until the boot's end and are only switched off), the RomFS lock,
  the 16 KB stacks of the reader and audio threads, `thread_local` destructors (libctru never
  runs them: a leak, not damage), newlib's stdio streams (global in devkitARM: a thread's
  `__sinit` does not reset them), libstdc++'s thread-safety switch (on).
- **Instrumented**: every host free, realloc and allocation now checks its block, and
  `NOVA3_HEAP_CHECK=1` walks the whole heap (see [New 3DS: Memory layout](../3ds.md)). One run
  of the boot and chapter 3's bench with the heap walked after every boot and load frame and
  at every thread start and end found nothing; the crash did not recur.
- **Fixed on that path** (the frame-2 timing is where four of the game's threads end together):
  - libctru frees a detached thread's stack inside `threadExit` while still on it: threads are
    joinable now, and an ended one is joined and freed by the next start or the watchdog.
  - A guest mutex / cond / sem word that only looked like a handle (bit 30 set in memory the
    game never initialised, or a copied object) named another live sync object, and destroying
    it deleted that object under the threads using it: a use-after-free in newlib's heap. A
    handle now counts only in the word it was installed in (all platforms).
- **Open**: not reproduced, so not proven to be either of these. If it recurs, the block checks
  stop at the first bad call with its caller and the damaged chunks.

## Smaller leaks noted

JNI local references are never freed: the web services leave a few small strings per request,
about a megabyte over hours of play. Left as is.

## Unexplained

- **Probe hang**: once, a PC test probe spun at 100% of a core before reaching the title menu,
  while the machine was building the Switch version. It didn't recur in 12 repeats or later full
  runs.
- **10 s stall**: once, the game thread waited about 10 s for the file table's lock at startup.
  It didn't recur in later runs. The lock fix in [Loading times](loading.md) removes the known
  long holder.
