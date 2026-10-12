# HD textures

Status: work in progress (the loading side works; the upscaled texture set is still being made).

An optional pack of the game's textures, upscaled, that the game uses instead of the originals
when it is enabled (launcher: Graphics → HD textures, `[video] hd_textures`). The game files are
not changed: the pack is one extra file, `nova3_data/hd_textures.pak`, and with the option off
(or the file gone) the game looks as before. Like everything made from the game's data, the
pack is for the player's own copy.

## How the game uploads textures

Textures are ETC1, in the `*4.gla` archives as `.etc` files: one PVR (v2 header, format 0x36),
or a `SPLIT` zip of `rgb.pvr` and `alpha.pvr` (the alpha at half size, a separate texture).
ANGLE offers no ETC1, so the game decodes them on the CPU and uploads RGB with `glTexImage2D`,
level 0 and then each of its own mip levels (some get `glGenerateMipmap` instead). A first
level uploads ~415 textures, ~3,500 calls with the mip levels. About 10 menu sprite sheets are
RGB565 PVRs that the game merges into one BGRA texture; they are not in the pack.

## Making the pack (`tools/texpack.py`)

1. `extract`: decodes every texture's level 0 (an ETC1 decoder in NumPy) to
   `build/texwork/src/<crc32>_<w>x<h>.png`, with a manifest of where each comes from. The key
   is the CRC-32 of the decoded RGB bytes, exactly what the game uploads: checked against the
   first level, all 407 RGB uploads matched. 2,181 distinct textures, ~1 gigapixel.
2. `upscale`: pictures only (1,168 textures from 64 pixels up, ~735 megapixels). Normal maps,
   split alpha masks and lightmaps stay as they are (an upscaler would distort normals and
   gains nothing on smooth light), as do textures under 64 pixels. Real-ESRGAN (x4plus, ncnn /
   Vulkan build) upscales 4x, which also cleans the ETC1 block artefacts; Lanczos then brings
   that to 2x the original size. In batches of 40; about an hour on an RTX 4090.
3. `build`: texconv (DirectXTex) compresses each to BC7 with its full mip chain (on the GPU),
   and the pack is written:

   | Part | Content (little-endian) |
   |---|---|
   | header | `N3HD`, version 1, entry count |
   | entries | CRC-32, width, height of the original level 0; GL format (`0x8E8C`, BC7), width, height, mip count, data offset (u64), data size |
   | data | each replacement's mip levels, largest first |

Tools: `build/tools/realesrgan/realesrgan-ncnn-vulkan.exe` (Real-ESRGAN v0.2.5.0) and
`build/tools/texconv.exe`, downloaded from their GitHub releases; not part of the build.

### Upscaling by hand

Instead of step 2, any upscaler:

- `export [dir]` (after `extract`) puts every texture in `dir/original/` (default
  `hd_textures/` in the project): `pictures/` (1,311) and `data/` (870: normal maps, split alpha
  masks, lightmaps), each in a folder per size (`512x512/`, ...), named `<crc32>_<w>x<h>__<game file name>[_alpha].png`, and lists them in
  `dir/textures.csv` (size, kind, the archives and files each comes from).
- Upscaled PNGs go in `dir/upscaled/` (subfolders allowed) under the same names: the part
  before `__` is the key. Any scale, the original's shape (aspect ratio), as large or larger; an
  alpha channel is dropped (the game uploads these as RGB).
- `import [dir] [out.pak]` compresses the PNGs found to BC7 with mip chains
  (`build/texwork/hand_dds`, again only for PNGs changed since) and writes the pack, by default
  `dist/nova3_data/hd_textures.pak`, from those PNGs alone. Files whose name is not a texture's,
  or whose shape differs, are listed and left out.

Size: the pack at 2x is 3.9 GB; every 2x more in width and height is 4x the size, on disk and in
video memory. The runtime takes any replacement size (each entry has its own).

## Swapping them in (`host/texpack.cpp`)

`runtime/gles.cpp` passes `glTexImage2D` and `glGenerateMipmap` to hooks first
(`rt::gl_set_texture_hooks`). For a level-0 RGB upload whose size is in the pack, the hook takes
the CRC-32 of the pixels (slicing by 8; a level's ~0.5 GB of uploads adds a fraction of a
second to loading); on a match it uploads the replacement's BC7 levels with
`glCompressedTexImage2D` and remembers the texture object, whose later level uploads and
`glGenerateMipmap` it then drops. A new level 0 for that object clears the mark. Textures not
in the pack are untouched. BC7 needs `GL_EXT_texture_compression_bptc` (ANGLE on Direct3D 11
has it); without it the pack is not used. Video memory stays about where it was: the originals
are RGB (stored as RGBA8, 4 bytes per pixel), the replacements have 4x the pixels at 1 byte.

`NOVA3_TRACE_TEXTURES=1` logs each replacement (`textures: 322a93d3_512x512 -> 1024x1024`).
Checked with a partial pack in the first level: 83 textures replaced (weapons, characters,
HUD, effects, menus), the rest original; screenshots show the replaced ones sharper and free
of ETC1 blocks.
