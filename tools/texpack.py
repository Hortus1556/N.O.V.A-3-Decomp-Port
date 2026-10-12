"""HD texture pack: the game's textures upscaled, in one file the game uses when enabled.

Usage: texpack.py extract              decode every texture to build/texwork/src (PNG) + manifest
       texpack.py upscale              Real-ESRGAN (and Lanczos for data textures) to build/texwork/up
       texpack.py build [out.pak]      compress (BC7, mipmaps) and write the pack
                                       (default: dist/nova3_data/hd_textures.pak)
       texpack.py export [dir]         every texture as a PNG for upscaling by hand (after extract):
                                       dir/original/{pictures,data}/<width>x<height>/, dir/textures.csv
                                       (default dir: hd_textures/ in the project)
       texpack.py import [dir] [out]   the PNGs in dir/upscaled/ (any scale, same aspect ratio,
                                       named as exported) compressed into the pack

The game decodes its ETC1 textures (.etc in the *4.gla archives: one PVR, or a SPLIT zip of
rgb.pvr + alpha.pvr) on the CPU and uploads them as RGB with their mipmaps. The pack is keyed
by what is uploaded: CRC-32 and size of the decoded level 0, so the runtime recognises a
texture by its pixels (host/texpack.cpp). Everything here comes from the player's own game
files, and so does the pack: it is for their copy, not for sharing.
"""
import io
import json
import os
import struct
import sys
import zipfile
import zlib

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, 'dist', 'nova3_data')
WORK = os.path.join(ROOT, 'build', 'texwork')
# texture archives (the *4 ones hold the .etc files); lut4 holds colour look-up tables: left alone
ARCHIVES = ['actors4.gla', 'weapons4.gla', 'menus4.gla', 'sprites4.gla', 'particlesystems4.gla',
            'l_001_terraforming_ship4.gla', 'l_002_SanFrancisco4.gla', 'l_003_Boreas4.gla',
            'l_013_Volteron4.gla', 'l_100_multiplayer4.gla', 'l_101_coop4.gla', 'l_999_sandbox4.gla']

# ---- ETC1 ----------------------------------------------------------------------------------

MODIFIERS = np.array([[2, 8, -2, -8], [5, 17, -5, -17], [9, 29, -9, -29], [13, 42, -13, -42],
                      [18, 60, -18, -60], [24, 80, -24, -80], [33, 106, -33, -106], [47, 183, -47, -183]],
                     dtype=np.int32)


def decode_etc1(data, w, h):
    """ETC1 (OES_compressed_ETC1_RGB8) level of w x h pixels -> uint8 array (h, w, 3)."""
    bw, bh = (w + 3) // 4, (h + 3) // 4
    blocks = np.frombuffer(data, dtype='>u8', count=bw * bh).astype(np.uint64)
    hi = (blocks >> np.uint64(32)).astype(np.uint32)
    lo = (blocks & np.uint64(0xffffffff)).astype(np.uint32)
    diff = (hi >> 1) & 1
    flip = hi & 1
    cw = np.stack([(hi >> 5) & 7, (hi >> 2) & 7], axis=1)  # table codewords of sub-blocks 0, 1

    def ch(shift_ind, shift_diff):
        # individual: 4-bit pairs; differential: 5-bit base + 3-bit signed delta
        a4 = (hi >> shift_ind) & 0xf
        b4 = (hi >> (shift_ind - 4)) & 0xf
        base5 = (hi >> shift_diff) & 0x1f
        d3 = ((hi >> (shift_diff - 3)) & 7).astype(np.int32)
        d3 = np.where(d3 >= 4, d3 - 8, d3)
        second5 = (base5.astype(np.int32) + d3) & 0x1f
        ind = np.stack([a4 * 17, b4 * 17], axis=1).astype(np.int32)
        dif = np.stack([(base5 << 3) | (base5 >> 2), (second5 << 3) | (second5 >> 2)], axis=1).astype(np.int32)
        return np.where(diff[:, None] == 1, dif, ind)

    base = np.stack([ch(28, 27), ch(20, 19), ch(12, 11)], axis=2)  # (n, 2 sub-blocks, rgb)
    out = np.zeros((bh * 4, bw * 4, 3), dtype=np.uint8)
    n = bw * bh
    by, bx = np.divmod(np.arange(n), bw)
    for x in range(4):
        for y in range(4):
            i = x * 4 + y  # pixel bit index: column-major
            idx = (((lo >> (i + 16)) & 1) << 1) | ((lo >> i) & 1)
            sub = np.where(flip == 1, int(y >= 2), int(x >= 2))
            table = cw[np.arange(n), sub]
            mod = MODIFIERS[table, idx]
            px = np.clip(base[np.arange(n), sub] + mod[:, None], 0, 255).astype(np.uint8)
            out[by * 4 + y, bx * 4 + x] = px
    return out[:h, :w]


def pvr_level0(pvr):
    """PVR v2 (52-byte header): (width, height, ETC1 level-0 bytes)."""
    hsize, height, width, mips, flags, dsize = struct.unpack_from('<6I', pvr)
    if pvr[44:48] != b'PVR!' or (flags & 0xff) != 0x36:
        raise ValueError(f'not an ETC1 PVR (format {flags & 0xff:#x})')
    size0 = max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * 8
    return width, height, pvr[hsize:hsize + size0]


def etc_parts(blob):
    """The PVRs of one .etc file: [('', pvr)] or [('rgb', pvr), ('alpha', pvr)]."""
    if blob[:4] == b'PK\x03\x04':
        z = zipfile.ZipFile(io.BytesIO(blob))
        return [(os.path.splitext(n)[0], z.read(n)) for n in z.namelist() if n.endswith('.pvr')]
    return [('', blob)]


def kind_of(name, part):
    """'photo': a picture, upscaled; 'data': normal maps, split alpha masks, lightmaps and other
    values that are not a picture, or are smooth anyway: left as they are."""
    stem = os.path.splitext(os.path.basename(name))[0].lower()
    tokens = stem.replace('-', '_').split('_')
    if part == 'alpha' or tokens[-1] in ('n', 'nm', 'normal', 's', 'spec', 'mask') or 'lm' in tokens \
            or 'lightmap' in stem:
        return 'data'
    return 'photo'


def extract():
    src = os.path.join(WORK, 'src')
    os.makedirs(src, exist_ok=True)
    manifest = {}
    for archive in ARCHIVES:
        path = os.path.join(DATA, archive)
        if not os.path.exists(path):
            print('missing', archive)
            continue
        z = zipfile.ZipFile(path)
        count = 0
        for name in z.namelist():
            if not name.lower().endswith('.etc'):
                continue
            for part, pvr in etc_parts(z.read(name)):
                try:
                    w, h, level0 = pvr_level0(pvr)
                except ValueError as e:
                    print(f'skip {archive}/{name} {part}: {e}')
                    continue
                rgb = decode_etc1(level0, w, h)
                key = f'{zlib.crc32(rgb.tobytes()):08x}_{w}x{h}'
                entry = manifest.setdefault(key, {'w': w, 'h': h, 'kind': kind_of(name, part), 'sources': []})
                entry['sources'].append(f'{archive}/{name}' + (f':{part}' if part else ''))
                out = os.path.join(src, key + '.png')
                if not os.path.exists(out):
                    Image.fromarray(rgb).save(out)
                count += 1
        print(f'{archive}: {count} textures')
    with open(os.path.join(WORK, 'manifest.json'), 'w') as f:
        json.dump(manifest, f, indent=1)
    print(f'{len(manifest)} distinct textures')


# ---- upscale -------------------------------------------------------------------------------

TOOLS = os.path.join(ROOT, 'build', 'tools')
ESRGAN = os.path.join(TOOLS, 'realesrgan', 'realesrgan-ncnn-vulkan.exe')  # Real-ESRGAN ncnn (Vulkan)
TEXCONV = os.path.join(TOOLS, 'texconv.exe')                               # DirectXTex
SCALE = 2  # the pack's textures: twice the original width and height


MIN_SIZE = 64  # smaller ones are fills, gradients and blanks: nothing to gain


def photos(manifest):
    """The textures that are upscaled: a texture in several files is a picture when any is."""
    def is_photo(source):
        name, _, part = source.partition(':')
        return kind_of(name, part) == 'photo'
    return {k: e for k, e in manifest.items()
            if min(e['w'], e['h']) >= MIN_SIZE and any(is_photo(s) for s in e['sources'])}


def upscale():
    import subprocess
    manifest = json.load(open(os.path.join(WORK, 'manifest.json')))
    todo = photos(manifest)
    stage, x4, up = (os.path.join(WORK, d) for d in ('stage', 'x4', 'up'))
    for d in (stage, x4, up):
        os.makedirs(d, exist_ok=True)
    pending = [k for k in todo if not os.path.exists(os.path.join(up, k + '.png'))]
    print(f'{len(todo)} pictures, {len(pending)} to do', flush=True)
    # in batches: the 4x intermediates of a 2048 texture take ~100 MB each
    for i in range(0, len(pending), 40):
        batch = pending[i:i + 40]
        for name in os.listdir(stage):
            os.remove(os.path.join(stage, name))
        for k in batch:
            os.link(os.path.join(WORK, 'src', k + '.png'), os.path.join(stage, k + '.png'))
        # Real-ESRGAN x4plus: 4x, and it cleans up the ETC1 block artefacts
        subprocess.run([ESRGAN, '-i', stage, '-o', x4, '-n', 'realesrgan-x4plus', '-s', '4', '-f', 'png'], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for k in batch:
            e = todo[k]
            img = Image.open(os.path.join(x4, k + '.png')).convert('RGB')
            img.resize((e['w'] * SCALE, e['h'] * SCALE), Image.LANCZOS).save(os.path.join(up, k + '.png'))
            os.remove(os.path.join(x4, k + '.png'))
        print(f'upscaled {min(i + 40, len(pending))}/{len(pending)}', flush=True)
    print('upscaled', flush=True)


# ---- build ---------------------------------------------------------------------------------

def dds_levels(path):
    """DDS (BC7, DX10 header) -> (width, height, [mip level bytes, largest first])."""
    d = open(path, 'rb').read()
    assert d[:4] == b'DDS ', path
    height, width = struct.unpack_from('<2I', d, 12)
    mips = max(1, struct.unpack_from('<I', d, 28)[0])
    off = 4 + 124 + (20 if d[84:88] == b'DX10' else 0)
    levels = []
    w, h = width, height
    for _ in range(mips):
        size = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 16
        levels.append(d[off:off + size])
        off += size
        w, h = max(1, w // 2), max(1, h // 2)
    return width, height, levels


def bc7(pngs, dds):
    """PNG files (named <key>.png) -> <dds>/<key>.dds, BC7 on the GPU (DirectCompute), full mip chain."""
    import subprocess
    os.makedirs(dds, exist_ok=True)
    for i in range(0, len(pngs), 64):
        subprocess.run([TEXCONV, '-f', 'BC7_UNORM', '-m', '0', '-y', '-nologo', '-o', dds] + pngs[i:i + 64], check=True,
                       stdout=subprocess.DEVNULL)
        print(f'bc7 {min(i + 64, len(pngs))}/{len(pngs)}', flush=True)


def build(out):
    manifest = json.load(open(os.path.join(WORK, 'manifest.json')))
    todo = photos(manifest)
    up, dds = os.path.join(WORK, 'up'), os.path.join(WORK, 'dds')
    bc7([os.path.join(up, k + '.png') for k in todo if os.path.exists(os.path.join(up, k + '.png'))
         and not os.path.exists(os.path.join(dds, k + '.dds'))], dds)
    write_pack(out, {k: os.path.join(dds, k + '.dds') for k in todo if os.path.exists(os.path.join(dds, k + '.dds'))})


def write_pack(out, dds_of):
    """The pack from {key: its replacement's DDS}."""
    # hd_textures.pak, little-endian:
    #   header  "N3HD", version 1, entry count
    #   entries the original level 0 as the game uploads it (CRC-32 of its RGB bytes, width,
    #           height), then the replacement: GL format (0x8E8C, BC7), width, height, mip
    #           count, data offset (u64), data size
    #   data    each replacement's mip levels, largest first
    keys = sorted(dds_of)
    entry_size = 4 * 7 + 8 + 4
    offset = 12 + entry_size * len(keys)
    tmp = out + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(b'N3HD' + struct.pack('<2I', 1, len(keys)))
        layout = []
        for k in keys:
            w, h, levels = dds_levels(dds_of[k])
            size = sum(len(l) for l in levels)
            crc, dims = k.split('_')
            w0, h0 = map(int, dims.split('x'))
            f.write(struct.pack('<7IQI', int(crc, 16), w0, h0, 0x8E8C, w, h, len(levels), offset, size))
            layout.append(k)
            offset += size
        for k in layout:
            _, _, levels = dds_levels(dds_of[k])
            for l in levels:
                f.write(l)
    os.replace(tmp, out)
    print(f'{out}: {len(keys)} textures, {offset / 1e9:.2f} GB')


# ---- upscaling by hand -----------------------------------------------------------------------

HAND = os.path.join(ROOT, 'hd_textures')


def file_name(key, entry):
    """<key>__<first source's name>[_alpha].png: the key (CRC-32 and size of what the game
    uploads) is what the pack needs; the rest is for finding one's way."""
    name, _, part = entry['sources'][0].partition(':')
    stem = os.path.splitext(os.path.basename(name))[0]
    return f'{key}__{stem}' + ('_alpha' if part == 'alpha' else '') + '.png'


def export(folder):
    import shutil
    manifest = json.load(open(os.path.join(WORK, 'manifest.json')))
    rows = ['file,width,height,kind,sources']
    os.makedirs(os.path.join(folder, 'upscaled'), exist_ok=True)
    for k, e in sorted(manifest.items(), key=lambda kv: file_name(*kv).split('__')[1].lower()):
        kind = 'pictures' if e['kind'] == 'photo' else 'data'
        size = f'{e["w"]}x{e["h"]}'
        os.makedirs(os.path.join(folder, 'original', kind, size), exist_ok=True)
        out = os.path.join(folder, 'original', kind, size, file_name(k, e))
        if not os.path.exists(out):
            try:
                os.link(os.path.join(WORK, 'src', k + '.png'), out)
            except OSError:
                shutil.copyfile(os.path.join(WORK, 'src', k + '.png'), out)
        rows.append(f'{kind}/{size}/{file_name(k, e)},{e["w"]},{e["h"]},{kind},{" ".join(e["sources"])}')
    with open(os.path.join(folder, 'textures.csv'), 'w') as f:
        f.write('\n'.join(rows) + '\n')
    print(f'{len(manifest)} textures in {os.path.join(folder, "original")}')


def import_(folder, out):
    manifest = json.load(open(os.path.join(WORK, 'manifest.json')))
    up = os.path.join(folder, 'upscaled')
    stage, dds = os.path.join(WORK, 'hand_stage'), os.path.join(WORK, 'hand_dds')
    os.makedirs(stage, exist_ok=True)
    found, problems = {}, []
    for dirpath, _, files in os.walk(up):
        for name in files:
            if not name.lower().endswith('.png'):
                continue
            key = os.path.splitext(name)[0].split('__')[0]
            path = os.path.join(dirpath, name)
            e = manifest.get(key)
            if not e:
                problems.append(f'{name}: not a texture of the game (the name must start as exported)')
                continue
            w, h = Image.open(path).size
            if w < e['w'] or h < e['h'] or w * e['h'] != h * e['w']:
                problems.append(f'{name}: {w}x{h}, the original is {e["w"]}x{e["h"]}: '
                                'needs the same shape, as large or larger')
                continue
            found[key] = path
    # the game uploads these as RGB: an alpha channel would reach the shaders (dropped here);
    # a PNG changed since its last compression is compressed again
    redo = []
    for key, path in found.items():
        target = os.path.join(dds, key + '.dds')
        if os.path.exists(target) and os.path.getmtime(target) >= os.path.getmtime(path):
            continue
        staged = os.path.join(stage, key + '.png')
        Image.open(path).convert('RGB').save(staged)
        redo.append(staged)
    bc7(redo, dds)
    for name in os.listdir(stage):
        os.remove(os.path.join(stage, name))
    for p in problems:
        print('skipped', p)
    if not found:
        sys.exit(f'no upscaled textures in {up}')
    write_pack(out, {k: os.path.join(dds, k + '.dds') for k in found})


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    if cmd == 'extract':
        extract()
    elif cmd == 'upscale':
        upscale()
    elif cmd == 'build':
        build(sys.argv[2] if len(sys.argv) > 2 else os.path.join(DATA, 'hd_textures.pak'))
    elif cmd == 'export':
        export(sys.argv[2] if len(sys.argv) > 2 else HAND)
    elif cmd == 'import':
        import_(sys.argv[2] if len(sys.argv) > 2 else HAND,
                sys.argv[3] if len(sys.argv) > 3 else os.path.join(DATA, 'hd_textures.pak'))
    else:
        sys.exit(__doc__)
