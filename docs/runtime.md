# Runtime

`runtime/` stands in for Android underneath the translated code: the guest address space,
loading, the 426 imported functions, a fake Java VM, files, sockets and GLES. It is built into
`nova3_core.lib` together with the generated code.

## Memory and loading

- The whole 32-bit guest space is reserved at start-up (`os::reserve(4 GiB)`) and committed on
  demand. A guest address `a` is host memory `g_mem + a` (`LD32`, `ST32`, `HOST()` in
  `runtime/recomp.h`).
- The ELF loader maps `libGloftNAHM.so` at `0x10000000` (`g_load_base`), applies relocations,
  fills import slots and runs the 174 static constructors.
- Indirect calls (vtables, function pointers) go through `rt_dispatch`, which maps a guest
  address to its translated function or mid-function entry (an open-addressed hash). The
  generated code first tries its call site's cache of the last target (`RT_ICALL`); only a
  miss reaches `rt_dispatch_ic`, which also refills the cache.
- `runtime/heap.cpp`: the guest `malloc` (size-class free lists plus coalescing page blocks).

## Imports

- Each import registers itself (`RT_IMPORT` / `RT_DATA`); `nova3.exe` links the core with
  `/WHOLEARCHIVE` so none is dropped. An unimplemented import traps with its name.
- `rt::thunk<fn>` unpacks AAPCS arguments for typed host functions (softfp: floats and
  doubles in core registers); variadic functions (`printf`, `scanf`) read guest arguments with
  `rt::Args`.
- `libc_string.cpp`: string / ctype / wchar (32-bit `wchar_t`), `_ctype_` tables, `qsort`,
  rand48.
- `libc_io.cpp`: path sandbox and file descriptors, bionic `stat` / `dirent`, stdio `FILE`,
  printf / scanf families, a generated `/proc/cpuinfo` (no NEON, so the game picks its C
  code paths), `/proc/meminfo`, CPU count files.
- `libc_misc.cpp`: libm, time, environment, process, signals, `setjmp` / `longjmp`, `dl*`
  (`dlsym` on the game itself resolves its exports), networking queries, Android logging and
  (absent) sensors.
- `thread.cpp`: pthreads, mutexes, condition variables, semaphores, TLS keys, errno, on
  portable C++ synchronisation and `runtime/os.h` threads. A bionic mutex / cond / sem is one
  guest word; its first use installs a handle there (bit 30 + an index into the object table).
  The object notes the word's address, and a handle counts only in that word: a word that
  merely looks like one (memory the game never initialised, an object copied elsewhere, one
  freed without being destroyed) gets a fresh object, and destroying it releases nothing (the
  first such words are logged: `thread: a lookup of ... not its handle`). Before, a stray
  word's destroy deleted whichever live object its index named, under the threads using it.

## setjmp, longjmp and C++ exceptions

Three places need a non-local jump back into a translated function's C frame:

- **`setjmp` / `longjmp`** (Lua's error handling uses them): `setjmp` is expanded inline at the
  call site (`GUEST_SETJMP`): the C library `setjmp` on a host `jmp_buf` allocated in the
  caller's frame (`alloca`), its address stored in the guest `jmp_buf` together with the
  guest's callee-saved registers. `longjmp` restores those and jumps.
- **C++ exceptions** use the game's own libgcc unwinder. Only its last step,
  `restore_core_regs`, is replaced (`runtime/eh.cpp`): every translated function that owns
  landing pads registers a catch frame on entry (`EH_BEGIN`), and the replacement jumps to the
  innermost matching frame, which dispatches to the landing pad.
- **`pthread_exit`** jumps back to the thread's start.

All three originally used clang's `__builtin_setjmp` / `__builtin_longjmp`. On Win64 that pair
restores the frame pointer at a fixed offset from the stack pointer, which is wrong in any
function whose frame pointer is not at its frame base (for example one that saves xmm6–15).
The resumed code then read its spilled values from the wrong stack slots. It showed up as a
crash right after `longjmp` whenever a **Lua script error** was caught (`luaD_throw` →
`luaD_rawrunprotected`), about 14 minutes into the campaign. All three now use the C library
pair. Checked with a `lua_pcall` of a chunk that fails three times in a row: the error
message comes back each time and the game carries on.

## Fake Java

The game calls into Java for paths, device information, social SDKs, ads, billing and audio.
`runtime/jni.{h,cpp}` builds `JNIEnv` / `JavaVM` function tables in guest memory, with Java
objects, strings, arrays and all `Call*Method` variants. `runtime/java.cpp` implements the
classes the game uses: `android.os.Build`, `Bundle`, SharedPreferences (stored as text under
the save folder), Gameloft's DataSharing store (likewise, `shared_prefs/data_sharing.txt`:
it keeps the account ID every save is tied to, see
[Gameplay fixes](gameplay-fixes.md#progress-lost-on-every-restart)), and Gameloft's helpers
(`SUtils`, `Device`, ...). Identity values are fixed
anonymous placeholders. Unknown methods are logged once and return 0 / null. Network, social
SDKs, ads and push notifications report "offline / not logged in". `AudioTrack` is
implemented by the host (see [Audio](audio.md)).

### Boot sequence

What Java does on Android, replayed by `nova3.exe`:

1. `JNI_OnLoad`.
2. `nativeInit` of `SUtils`, `Device`, `SendInfo`, `DataSharing`, `SimplifiedAndroidUtils`
   and the GLSocialLib classes.
3. `GL2JNILib.init`.
4. GL side: `initGL`, `InitViewSettings`, `GL2JNIActivity.nativeInit`, `resize(w, h)`.
5. Every frame: `step()`, then the buffer swap.

The native side asks for paths (`GL2JNILib.setupPaths()`); the emulation answers by calling
native `setPaths(obbFolder, sdFolder, filesDir, cacheDir)`.

## Files

Guest paths are mapped into host folders (`FsConfig`, `rt::host_path`):

| Guest path | Host folder |
|---|---|
| `/sdcard/Android/obb/<package>/...` | the OBB folder (`nova3_data\` itself in a packaged copy) |
| `/data/app/<package>-1.apk` | the APK |
| `/data/data/<package>/...` except `cache/` (save games, shared prefs) | `save/` in the player's folder (see [Application and settings](app-and-settings.md)) |
| everything else (`/sdcard`, `cache/`, `/tmp`, `/proc`, ...) | the scratch tree: `%TEMP%\nova3` (`--data` chooses another); the generated `/proc` and `/sys` files are rewritten at every start |

The campaign save is `files/a1.dat` (`Application::SaveCampaign`). The other `a*.dat` hold
profile, upgrades and statistics, and `gv3/` is the game's analytics queue.

Files opened for reading only get a block cache in front of the host `FILE` (`ReadCache` in
`libc_io.cpp`). The game reads its archives with a seek before nearly every read: over a
recorded chapter, 40,000 reads (93% under 4 KB) and 31,000 seeks, a fifth of them to where the
file already was and half within 64 KB. On the Switch each refill of a stdio buffer is a file
system request (about 0.2 ms), and a seek drops the buffer. With the cache a seek only moves
the position. Small reads come from 16 KB blocks, the 16 most recent kept per file, and reads
of a block or more go to the file directly. Replayed against a cost model of 0.1-0.3 ms per
request and 60-150 MB/s, those reads took 5.1-6.1 s instead of 7.3-13.3 s with the C library's
1 KB buffer, or 29-47 s with a 64 KB buffer dropped at each seek. Any other stdio call on the
file first puts the host `FILE` at the guest's position. Files opened for writing (saves) get
a 64 KB buffer.

The descriptor table's lock (`g_io_lock`) is held only for table lookups. `epoll_wait` (the
game's network thread, every millisecond) used to poll its sockets while holding it, and on
Windows a socket poll is slow: every file call of every other thread waited, about 0.1 ms a
call. In the campaign's first load the save files are read in about 17,000 small calls, which
took 1.5-2 s. The watched descriptors are now copied under the lock and polled outside it.

**Missing files.** The game probes for loose files before reading its archives: a chapter-0
load makes about 14,500 `fopen`s, 12,800 of them for the same few hundred names that don't
exist. Paths found missing are remembered and fail at once; anything that creates, renames or
removes files forgets them all. On the PC that saves about 0.2 s a load; on the consoles, where
each probe is a file-system service request, far more (the 3DS's chapter-0 load went from 222 to
51 s with this and the keep-open of the archives, docs/3ds.md). The 3DS goes further (a listing
of the game files' folder, missing folders, misses kept by the guest's path, the archives' mount
index, reads through the descriptor): docs/3ds.md, Boot and level loads.

**Edge-triggered pipes.** boost::asio's reactor (the analytics tracker's io_service) registers its
interrupter pipe with `EPOLLET` and never drains it. `epoll_wait` used to report a pipe readable
as long as data was in it, so after the first wake-up it returned at once forever and that thread
spun a whole core (a 60 fps menu: game thread 3%, process 108%). Writes are now counted per pipe
(Windows pipes: in `hio::write`, both ends sharing the count; the consoles' emulated pipes
likewise), and an `EPOLLET` pipe is reported once per write: the same menu runs at 10% process CPU.

## Network

`runtime/net.cpp`: BSD sockets on Winsock (or POSIX sockets), sharing the guest file
descriptor space. This is what local Wi-Fi multiplayer uses. The connection type is reported
as Wi-Fi, because local multiplayer requires it. DNS and outbound internet services fail
cleanly; there are none left to talk to.

**Finding games on a virtual LAN.** Joining a local game sends a discover packet to
255.255.255.255 (`Comms::SendDiscoverPacket_WiFi`, about once a second while the list is
open); the host answers it, and the game then talks to the address the answer came from. The
host listens on UDP port 7892. Windows sends a limited broadcast out of one interface only,
the preferred one, so two copies on one PC found each other, but a host on a virtual LAN
(ZeroTier, Hamachi, a VPN) or behind a second adapter never saw the packet. `sendto` to
255.255.255.255 now also sends to the subnet broadcast of every IPv4 interface that is up
(`GetAdaptersAddresses` / `getifaddrs`, loopback left out, refreshed every 5 s, logged as
`net: broadcasts go to ...`). Tested with two copies on one PC: the joining copy listed the
host and joined the match. Where the two machines share more than one network (two copies on
one PC, or a LAN plus ZeroTier), the host receives a discover once per network and lists the
extra senders as known but unconnected clients; joining is not affected.

**Joining from a console.** Joining (Multiplayer > Local Wi-Fi > Join) opens a UDP socket
with `SO_BROADCAST` and sends the discover packet to 255.255.255.255:7892 about once a second
(`Comms::SendDiscoverPacket_WiFi`); a failed `sendto` or `setsockopt` there makes the game clean
up its comms and leave the search (Create binds a socket and only answers, so it worked). What
broke Join on the consoles, and the fixes in `runtime/net.cpp`:

- Switch: libnx's sockets are BSD's: a length byte, then an 8-bit family (AF_INET6 is 28).
  The game's bionic sockaddrs (a 16-bit family) went through as they were, so the Switch's BSD
  service got length 2 and family 0 (AF_UNSPEC) for every address the game built: the
  broadcast's among them (Citron logged `Unexpected SockAddrIn len=2` and `Unhandled sockaddr
  family=0` and sent it anyway; the console refuses it). Addresses are now translated both ways,
  and the lengths returned to the game are bionic's (16, 28): the game drops a datagram whose
  sender's length is not 16 (Citron reported other lengths, and the host's answers were lost).
- 3DS: libctru's `SO_BROADCAST` is 0, no option for soc:U (its header: "unrequired"), so the
  game's `setsockopt` failed; it is accepted and ignored there. Azahar then refuses the limited
  broadcast (EACCES, its host socket has no SO_BROADCAST).
- Every platform: the subnet broadcast of each interface goes out with the limited broadcast
  (above), and the consoles now know theirs (nifm's `nifmGetCurrentIpConfigInfo` on the Switch,
  `SOCU_GetIPInfo` on the 3DS; also used by the non-local connect refusal). When the limited
  broadcast fails but a subnet broadcast or a listed player's copy went out, the game is told
  the send worked: Azahar's join search now goes out as 192.168.x.255 alone.
- Switch: a UDP socket's readability is found by receiving its next datagram ahead (whole, non-
  blocking) and handing it to the game's next `recvfrom` / `recv`, instead of a 1-byte `MSG_PEEK`
  (see [Switch](switch.md)): Citron ignores `MSG_PEEK`, so each peek consumed a datagram, and in a
  joined match the host's pings were lost and the player was dropped before the clock sync
  (`Removed idle player ... before sync!`).
- Windows: UDP sockets no longer report another machine's ICMP "port unreachable" as
  WSAECONNRESET on the next `recvfrom` (`SIO_UDP_CONNRESET` off, as Linux behaves): the game
  counted each as a comms error after a player quit.

The first failure of each socket call per error is logged (`net: <call> failed: host error N`);
`NOVA3_TRACE_NET=1` logs every datagram ([Debugging](debugging.md)). Tested: Join's list stays
open on the Switch (Citron) and the New 3DS (Azahar); a PC hosting map 0, joined from Citron and
from Azahar on the same PC: the PC listed in the console's search, joined, clock sync, the match
played (the emulators' sockets are the PC's, so the console is the PC's address); two PC copies
as before.

**Windows Firewall.** Games from another computer arrive as incoming UDP, which Windows
Firewall blocks for programs without an allow rule. When its notifications are off
(`NotifyOnListen` false, as on the development PC) it does so silently, without asking: two
copies on one PC worked, a friend over ZeroTier was never answered. The launcher's
Multiplayer section shows whether a rule exists (`netsh advfirewall firewall show rule`,
exit code 0) and offers "Allow through Windows Firewall...": an inbound allow rule for this
nova3.exe, all profiles, named `N.O.V.A. 3 (<path>)`, added by netsh after the administrator
prompt. Every PC that plays needs it.

**Players' IPs.** `[multiplayer] peers` (launcher: Players' IPs; on the Switch match setup >
Advanced > Player IP, applied at once), a list of IPv4 addresses
(`net_set_peers`), also gets every broadcast: the discover packet then reaches the listed
host directly, for networks that drop broadcasts (or a ZeroTier network with broadcast
turned off). Tested on one PC with its own ZeroTier address: listed (`net: broadcasts also go
to ...`), joined.

## Graphics

`runtime/gles.cpp` resolves the game's 100 GLES2 functions through the host's loader (SDL's
`SDL_GL_GetProcAddress` on ANGLE), shadows `glMapBufferOES`, and writes PNG screenshots.
Anisotropic filtering (`[video] anisotropy`) is added whenever the game picks a mipmapped
minification filter. `runtime/gles_null.cpp` is a headless renderer (`--headless 1`).

Shader sources get every `uniform lowp` / `uniform mediump` raised to `highp` in
`glShaderSource`. The lightmap shaders declare `WaveTime` highp in the vertex shader and lowp
in the fragment shader; phone drivers linked that, ANGLE refuses ("Precisions of uniform
'WaveTime' differ"), and the game drew those surfaces as pink wireframe. Direct3D computes in
32-bit float anyway.

Calls that would not change GL state are skipped: each one is a driver call (Mesa on the
Switch: about 2 us). Uniform values are cached per program and location. The game sets every
uniform before each draw, and in a chapter's busiest frames (3,000 uniform calls) 66-76% only
repeat the program's value. The cache is cleared when the program is linked again or deleted.
Vertex attribute pointers and enables, buffer bindings, the active texture unit, blending,
the depth mask, the front face and constant attributes are filtered too (1-18% of those
calls). The log's `gl per frame` line shows both shares. Only the game changes this state:
the runtime's own GL calls (screenshots, the shader cache) leave it alone, and the starting
values are GL's initial state.

Programs are pooled. Every level and section load deletes the materials' programs and links
the same sources again: one chapter linked 519 programs, about 220 of them different. A
program the game deletes stays linked, and a later program with the same sources borrows it
(its name is an alias, resolved in every call that takes a program). The uniforms the
previous user set are reset to 0 first, as after a link. Shaders are compiled only when a link
needs them. On the PC the binary cache covers first loads too. The Switch has no program
binaries, so every program linked is recorded in `programs.list` in the cache folder. The next
run compiles those programs into the pool while the game sits in its menus
(`gl_prewarm_step`, one a frame, once the player has left the menus alone for 2 s: a compile
slows its frame, and menus in use felt it), and a level's first load finds them linked. The log
counts it: `gl: N programs reused`, `gl: N programs compiled ahead`. On Citron a chapter's
GL time in loading frames fell from 7.9 to 5.8 s on the first run after the list was made,
and the section loads compile about 2 programs instead of about 140.

A shader that fails to compile is logged with its info log
(`gl: shader N failed to compile`). The "ClassicBloom … shader creation failed" lines are the
game's own: its files (`ClassicBloom-v.glsl`, `-f.glsl`) were never shipped, and the bloom in
use is another effect.

## OS layer

The runtime touches the OS only through `runtime/os.h` (`os_win.cpp`, `os_posix.cpp`,
`os_switch.cpp`): address space, clocks, sleeping, threads, process exit, crash handler and
stack printing, the watchdog, and disk space. Process exit never runs static destructors
(the guest's `exit()` included): guest threads may still be running and would use freed host
state. `rt::log` writes each line in one call, so lines from different threads do not mix.

Threads: the host thread under a guest thread is always detached (the system takes it back
when it ends; the Switch, whose `pthread_detach` does nothing, joins ended threads at the next
start). The guest's `pthread_join` waits for its thread's end on a condition variable, and a
guest thread's stack and registers are freed as it ends: games leave threads unjoined (the
web services do), and only a small record (its `pthread_t` and return value) waits for a join
that may never come.

## Crash reports

A fault prints the faulting address (guest or host), the last import called, the guest
registers at the last call boundary, and a symbolized stack: host frames of translated code
are named `f_<link address>`, and each generated C line ends in `/*@<guest address>*/`, so
frames map back to game functions and instructions. Debug builds carry line tables and a PDB.
A watchdog reports a frame that stops advancing. On Windows it suspends the game thread only
long enough to copy its registers and the top of its stack, and walks the copy after resuming
it: the thread may hold the heap or loader lock (a long load), and DbgHelp takes both, so
walking it while suspended froze the game (the first symbol lookup also loads the whole PDB).

A fatal error names the thread (the game thread, or a guest thread by its start routine), and
a guest `abort()` (`std::terminate`, a pure virtual call) also prints the guest's callers: the
return addresses found on the guest stack up to its top (words just after a BL / BLX in the
library), as link addresses for `tools/disasm.py`. Where host stacks cannot be walked (the
Switch) that is the main clue; the Switch watchdog prints the same scan of the game thread.

## Linux

The same code builds on Linux with clang, CMake, Ninja and SDL3. It was tested once, on
Ubuntu 24.04 in WSL. That release has no SDL3 package, so SDL3 3.4.18 was built from source
with the X11 / Wayland / EGL / audio `-dev` packages installed. The generated files are large,
so limit parallel jobs to the available memory (`-j4` fits in 15 GB). Development and testing
otherwise happen on Windows only.

```sh
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
      -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_PREFIX_PATH=$HOME/sdl3
ninja -C build-linux -j4
LD_LIBRARY_PATH=$HOME/sdl3/lib build-linux/nova3
```
