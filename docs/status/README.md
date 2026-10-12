# Status and port branches

The status shown at the top of the repository's README comes from one file:

- `milestones.json`: every port (name, branch, guide, one-line summary), its areas (code,
  runtime, graphics, input, ...) and each area's features, each `done`, `partial` (in
  progress) or `todo`, with the day it was reached or started (required when done).
- `tools/status.py` reads it and writes, here:
  - `progress.svg`: one panel per port, in the style of AnyPS5's status map: a squarified
    treemap of the port's areas, each sized by its number of features, one cell per feature
    (green done, amber in progress, grey to do), and the share done in the heading. Every cell
    and area carries its name as a tooltip when the image is opened on its own;
  - `history.svg`: each port's share done by day (features done by then over today's total);
  - `badge-<port>.svg`: the share done as a badge;
  - the per-port feature tables in `README.md` between the `status:begin` / `status:end`
    markers.

The percentage counts only features done; a feature in progress adds nothing until it is done.

To change the status: edit `milestones.json`, run `python tools/status.py`, commit the JSON, the
images and the README together. A feature's state should match the port's guide (the
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
