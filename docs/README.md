# Documentation

N.O.V.A. 3 (Android 1.0.1d) running natively on Windows: the game's ARM code is translated to
C ahead of time, compiled for x86-64, and run on a small runtime that stands in for Android.
On top of that sit the PC parts: window and launcher, keyboard / mouse / gamepad controls,
audio, save folder, fixes for problems a 240 fps desktop exposes, and the removal of the dead
online services. The same code also builds for the Nintendo Switch as a homebrew title
(`.nsp` / `.nro`), and the New Nintendo 3DS runs the game's own ARM code natively on the same
runtime (`.cia`). A GameCube port is being designed and built.

| Document | What it covers |
|---|---|
| [Recompiler](recompiler.md) | Reverse engineering of the binary and OBBs, the ARM → C translator, verification, how hooks are declared |
| [Runtime](runtime.md) | The Android stand-in: memory, libc, threads, setjmp and C++ exceptions, fake JNI/Java, files, sockets, GLES, OS layer, crash reports, Linux |
| [Application and settings](app-and-settings.md) | `nova3.exe`: SDL3 window, ANGLE, launcher, `nova3.ini`, save folder, display modes, VSync, FOV, logging |
| [Input](input.md) | Gameplay and menu input modes, every control and how it reaches the game, gamepads, menu navigation |
| [Gameplay fixes](gameplay-fixes.md) | Loading screens, campaign tutorial and QTEs, slow motion at high frame rates, the jeep ride's cars backing up at high frame rates, cutscene skip, falling through the world after a section load, enemies falling out of the level |
| [Audio](audio.md) | Vox's AudioTrack backend on SDL3 audio, the click fix, verification |
| [HD textures](hd-textures.md) | The optional upscaled texture pack: how it is made (`tools/texpack.py`, also from textures upscaled by hand) and swapped in at upload |
| [Offline game](offline.md) | Removed store, ads, social networks, logins, online play, the gold coins and credits bought or watched for; the pause menu's Military Shop behind its button; what stays |
| [Nintendo Switch](switch.md) | The Switch port: building with devkitPro, the .nsp, memory layout, platform differences, testing in emulators |
| [New Nintendo 3DS](3ds.md) | The 3DS port: the game's ARM code run natively (ARMv6K patch set, gates into the runtime), the PICA200 GL backend, memory in 178 MB, controls, audio, building the .cia, testing in Azahar and on a console (the SD card image, screenshots, startup checks), what the console's GPU needs that Azahar does not check (caches, shader outputs, index numbers, float24 precision), the console bring-up, performance |
| [Nintendo GameCube](gamecube.md) | The GameCube port in progress: why it is hard (24 MB, one 486 MHz core), the measurements that decided its design (code and heap working sets, compressibility), the hardware facts it relies on, demand paging through the MMU to ARAM and the disc, big-endian translation, the GX backend, the plan |
| [Debugging](debugging.md) | Event log, scripted test runs, debug options, analysis tools, how the harder bugs were found |
| [Open issues](open-issues.md) | Known problems, untested paths, ideas for later |
| [Status and branches](status/README.md) | The milestone status of every port (`milestones.json`, `tools/status.py`), the per-port branches, what is never committed |
| [Work history](history/README.md) | What was done, topic by topic (Switch port, performance, loading, stability, audio, controls, diagnostics, testing, test builds), with measurements and what is still open |

Conventions used throughout:

- Addresses like `0x107ac88` are link-time addresses in `libGloftNAHM.so` (the game is loaded
  at `0x10000000`, so the runtime address is `0x10000000` higher).
- Offsets like `CLevel+0x2e8` are byte offsets into game objects, found by disassembly.
- Function names are the game's own (the binary is not stripped), demangled.
- "Tested" means checked in a scripted run with screenshots or log output; anything not
  verified that way is called out as untested.

The game's files (APK, OBBs, the extracted library) and everything generated from them stay
on your machine: they are Gameloft's. Only the tools, runtime and host code are this project's.
