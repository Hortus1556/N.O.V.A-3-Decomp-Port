# Testing

Reference: [Debugging: Scripted test runs](../debugging.md).

## Rules followed

- Build and test on Windows only.
- Everything heavy runs through `tools/memcap.py` (a 10 GB job memory limit), and only one
  game instance runs at a time.
- The game always starts with `-silent`, and test runs use copies of the save folders.

## Tools

| Tool | What it does |
|---|---|
| `tools/tour.py` | PC: plays every campaign chapter checkpoint by checkpoint (`--mp`: hosts a match on each multiplayer map) and reports checkpoints, deaths, crashes and hangs |
| `nova3.exe --bench CHAPTER:FRAMES[:walk]` | PC: a fixed benchmark with game-thread CPU per frame (`--profile 1` adds a profile) |
| `build/swtour.py --emu citron` | Switch build: the campaign tour in Citron |
| `build/swmp.py` | Switch build: hosts a match on each map in Citron |
| `build/ryurun.sh` | Runs the `.nsp` in Citron or Ryujinx with a `test.txt` script (taps, holds, Lua, screenshots, env variables, docked or handheld) and window captures |

## Results of the final runs

| Run | Result |
|---|---|
| PC campaign | All 10 chapters with the baseline checkpoint counts (56, 32, 56, 29, 11, 18, 33, 31, 16, 17). The one death in chapter 4 was already there. |
| PC multiplayer | All 7 maps reach gameplay with no crash |
| Citron campaign | All 10 chapters with the same counts |
| Citron multiplayer | All 7 maps reach gameplay and finish. Maps 1 and 2 failed once, while the first shader prewarm slowed the menus (taps landed on the wrong buttons, and a run timed out); they pass with the prewarm limited to idle menus. |

Each change was also checked on its own: screenshots of the menus and of the chapters, the
heap trend per multiplayer map, and the shader reuse counts.

## Limits

Emulators don't reproduce real-hardware timing. Performance and loading gains on the Switch
need a log from the console to confirm.
