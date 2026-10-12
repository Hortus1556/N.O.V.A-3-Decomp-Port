# Test builds

The `.nsp` files handed over for real-hardware testing, all in `build/switch/`. Each one
includes everything in the earlier ones.

| File | Contents |
|---|---|
| `nova3_test_2026-10-09.nsp` | First installable build |
| `nova3_test_2026-10-09b.nsp` | Threaded GL (since turned off), 64 KB stdio buffers (since replaced), the physics-thread fix |
| `nova3_test_2026-10-09c.nsp` | Thread-stack leak fix (green screen), the merged translator optimizations, the multiplayer "Advanced" label fix |
| `nova3_test_2026-10-09d.nsp` | Controls page gyro layout fix, gyro invert X / Y, Swap A/B and X/Y, Switch button names, Player IP row |
| `nova3_test_2026-10-09e.nsp` | GL call filtering (uniforms and state), direct thread calls, narrowed import register traffic, glthread off by default, read-only file block cache, CPU boost on loading screens, shader program pool and prewarm, the file-table lock fix |
| `nova3_test_2026-10-09f.nsp` | The shader prewarm only runs after 2 s without input in the menus (the first version made the menus sluggish) |

The `.nsp` contains the game's files: it stays with the player.
