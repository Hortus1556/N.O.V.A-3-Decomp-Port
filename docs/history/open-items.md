# Open items

The full list is in [Open issues](../open-issues.md). From this work:

**Needs a real-hardware log**
- Whether the GL filtering, direct thread calls and glthread-off reach a steady 60 fps
  (before them: 52-56 fps handheld).
- How much the read cache, CPU boost, file-table lock fix and shader prewarm shorten loads.
- The heap staying flat over a long session after the thread-stack fix.

**Untested**
- Entering a Player IP on the console (the keyboard opens; typing into it couldn't be scripted).
- Local multiplayer between two consoles.
- The libnx touchscreen and keyboard paths beyond what the test scripts drive (the controller
  reading has been played on hardware).

**Unexplained, not reproduced**
- One PC test probe spun at 100% of a core before the title menu, while the machine was
  loaded with a build.
- One 10 s stall at startup on the file table's lock.

**Known, left alone**
- JNI local references are never freed (about a megabyte over hours).
- Texture uploads dominate the remaining loading time in Citron; the console decodes the
  formats in hardware, so this is likely the emulator's cost.
- The game's own menu icons (Back's "(B)") don't follow the A/B swap.
