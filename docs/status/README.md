# Status and port branches

The milestone status shown at the top of the repository's README comes from one file:

- `milestones.json`: every port (name, branch, guide, one-line summary) and its milestones, each
  `done`, `partial` (in progress) or `todo`, with the day it was reached or started.
- `tools/status.py` reads it and writes, here, `progress.svg` (the milestone map: one row per
  port, one numbered cell per milestone), `history.svg` (milestones done per port by day),
  `badge-<port>.svg`, and the per-port milestone tables in `README.md` between the
  `status:begin` / `status:end` markers.

To change the status: edit `milestones.json`, run `python tools/status.py`, commit the JSON, the
images and the README together. A milestone's state should match the port's guide (the
"Status" sections of [3DS](../3ds.md), [GameCube](../gamecube.md), [Switch](../switch.md) and
[Open issues](../open-issues.md)): "done" means checked in a run, as the rest of these
documents use the word.

## Branches

`main` holds the shared code: every port builds from the same recompiler, runtime and host
modules, each with its own platform layer (`runtime/os_*.cpp`, `host/<port>/`). Each port also
has a branch for its work in progress, merged back into `main` when it is ready:
`port/windows` (also Linux), `port/switch`, `port/3ds`, `port/gamecube`. GitHub does not let an
account fork its own repository, so the per-port lines of work are branches of this one.

## What is never committed

`.gitignore` keeps out the game's files and everything derived from them (the APK, OBBs, the
extracted library in `game/`, the translated code and builds in `build/`, packaged copies in
`dist/`, the HD textures, console and emulator dumps), console keys and signed titles
(`*.keys`, `*.nsp`, `*.cia`, ...), and local settings and logs.
