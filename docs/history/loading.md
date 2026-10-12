# Loading times

References: [Runtime: Files, Graphics](../runtime.md), [Nintendo Switch](../switch.md).

## Starting point

A real-hardware log showed one level load spending 2.8 s in 14,000 file calls (0.2 ms each)
and 0.5-1.5 s compiling shaders, with the rest in the game's own parsing.

## What was done

**File reads (`ReadCache`, `runtime/libc_io.cpp`)**
- A recorded chapter made 40,000 reads (93% under 4 KB) and 31,000 seeks. A fifth of the seeks
  go to where the file already is, and half land within 64 KB of the last position.
- An earlier change gave every file a 64 KB stdio buffer. Replayed against a cost model, that
  was a mistake: a seek drops the buffer, so each small read cost a 64 KB transfer.

  | Strategy | Replayed time (0.1-0.3 ms a request, 60-150 MB/s) |
  |---|---|
  | C library default, 1 KB buffer | 7.3-13.3 s |
  | 64 KB buffer dropped at each seek | 29-47 s |
  | 16 KB block cache, 16 blocks per file (now) | 5.1-6.1 s |

- Read-only files now get the block cache: a seek only moves the position, and reads of a
  block or more go straight to the file. Files written (saves) keep a 64 KB buffer.

**CPU boost on loading screens**
- While `GS_Loading` or `GS_LoadMenu` is the top game state, the Switch runs the CPU in boost
  mode (1785 MHz instead of 1020, `appletSetCpuBoostMode`), as retail games do. The GPU drops to
  its minimum, which a loading screen does not need. Seen working on every load in the Citron
  regression (40 on/off pairs).

**Shaders**
- PC: a program-binary cache (ANGLE) skips recompiling across runs.
- **Program pool** (all platforms): the game deletes and relinks the same programs at every
  level and section load (519 links in one chapter, about 220 different). A deleted program
  now stays linked, and the next program with the same sources borrows it, with its uniforms
  reset to 0 as after a link. Section loads now compile about 2 programs instead of about 140.
- **Prewarm** (the Switch, which has no program binaries): programs linked are recorded in
  `programs.list`, and the next run compiles them in the menus, one per frame, after 2 s
  without input. The first version compiled even while the menus were in use: they turned
  sluggish, and scripted tests' taps landed on the wrong buttons. On Citron a
  chapter's GL time in loading frames fell from 7.9 s to 5.8 s on the first run after the list
  existed. Most of what remains there is texture uploads, which the console does in hardware
  (native ETC1 / ASTC), so that cost is largely the emulator's.

**The file table's lock**
- `epoll_wait` (the game's network thread, every millisecond) polled its sockets while holding
  the descriptor table's lock, and every file call of every other thread waited, about 0.1 ms a
  call. The first level load reads the save files in about 17,000 small calls, and that frame
  took 1.5-2 s on the PC. The sockets are now polled outside the lock, and the frame takes
  milliseconds. On the Switch each socket poll is a request to the system's network service,
  so this probably explains much of the 2.8 s seen there.

**Missing files and reopened archives (the consoles, found on the 3DS)**
- In Azahar the chapter-0 load took 222 s, 90% of it in file calls. A trace of the same load on
  the PC (`NOVA3_TRACE_FILES=1`) showed why: 14,536 `fopen`s, 12,865 of them for loose files
  that do not exist (the game looks for each mesh and texture outside its archives first: one
  HUD model 1,212 times), and the rest reopening the two OBBs for every mesh (open, seek, read,
  close). On a console each of those is a request to the file system service, and each reopen
  started with a cold read cache.
- Now (3DS and Switch, `runtime/libc_io.cpp`): paths found missing are remembered and fail at
  once (`fopen`, `open`, `stat`, `access`; any create, rename, remove or mkdir forgets them
  all); a read-only game file the guest closes stays open with its block cache (the 4 most
  recent), and the next `fopen` of it takes it back; read-only files get a 16 KB stdio buffer,
  so a cache block is one request instead of sixteen 1 KB refills. Azahar: the chapter-0 load
  222 s -> 51 s, the whole boot-to-play run 341 s -> 141 s. The Switch shares the code (built,
  not yet measured on the console).

## To confirm on hardware

The second launch's log should show `gl: N programs compiled ahead`, `cpu: boost on/off` around
loads, and the file I/O share of the loading frames (`slow frame` lines).
