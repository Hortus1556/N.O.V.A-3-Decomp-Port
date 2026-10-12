# Work history

What was done to the port, topic by topic: what changed, why, what it measured, and how it
was checked. The reference documents in [`docs/`](../README.md) describe how each part works
today; these files record how it got there and what is still open.

| File | Topic |
|---|---|
| [Switch port](switch-port.md) | From the PC build to an installable `.nsp`: packaging, memory, threads and cores, exit, emulators |
| [Performance](performance.md) | Removing translation and runtime overhead: the translator's analysis, inlining, native hot paths, PGO, GL call filtering |
| [Loading times](loading.md) | File reads, CPU boost, shader reuse and prewarm, the file-table lock |
| [Stability and memory](stability-and-memory.md) | The physics-thread race, the thread-stack leak behind the green screen, hangs and crashes fixed |
| [Audio](audio.md) | The audio redesign after drop-outs on real hardware |
| [Controls and menus](controls-and-menus.md) | Gyro, rumble, toggle run, button layout and names, Graphics page, multiplayer menu fixes, Player IP |
| [Diagnostics](diagnostics.md) | What the logs report for real-hardware testing: performance split, profiler, GL and memory lines, allocation log |
| [Testing](testing.md) | Test tools, emulators, regression runs and their results |
| [Test builds](test-builds.md) | The `.nsp` files handed over for real-hardware testing and what each contains |
| [3DS console bring-up](../3ds.md#console-bring-up-2026-10-11--12-new-3ds-with-luma3ds) | 2026-10-11 / 12: each console start of the New 3DS port, what it found and what changed (kept in the 3DS document) |
| [Open items](open-items.md) | What is still unresolved or untested |

All dates are 2026-10-09 unless stated otherwise. The game's files and everything generated
from them stay local; nothing here distributes them.
