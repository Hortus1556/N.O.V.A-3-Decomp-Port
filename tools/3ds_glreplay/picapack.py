"""3DS texture pack: the game's ETC1 images too large for the PICA (2048x2048 menu atlases),
halved and encoded again offline, so the console never does it at load time.

Usage: picapack.py build [out.pak] [data dir]   (default: dist/nova3_data/pica_textures.pak, from
                                                dist/nova3_data; tools/3ds_slimdata.py builds the
                                                3DS's from its slim data)
       picapack.py check gltrace.bin    which of a trace's large ETC1 uploads the pack covers

The PICA takes textures up to 1024x1024. runtime/gles_pica.cpp otherwise decodes such an upload,
halves it and encodes it as ETC1 again on the ARM11 (seconds per atlas). Here the same is done
with NumPy for every ETC1 level 0 over 1024 in the game's archives, and stored in the PICA's own
tiled block order. The key is the CRC-32 of the ETC1 bytes the game uploads (the PVR's level 0)
with the width and height, which is what the backend computes in glCompressedTexImage2D.

Pack (little-endian): 'N3PT', version 1, count; entries of (crc32, width, height, out width,
out height, data offset, data size); the data. Made from the player's own game files: for their
copy only.
"""
import os
import struct
import sys
import zipfile
import zlib

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import texpack  # noqa: E402  (ETC1 decoding, PVR and archive reading)

ROOT = texpack.ROOT
OUT = os.path.join(ROOT, 'dist', 'nova3_data', 'pica_textures.pak')
MODS = texpack.MODIFIERS  # (8, 4): +a, +b, -a, -b


def encode_etc1(rgb):
    """(h, w, 3) uint8, h and w multiples of 4 -> ETC1 blocks (big-endian u64), row by row: the
    same fast encoder as runtime/gles_pica.cpp (partition by variance, differential bases when
    they fit, per subblock the table that best fits each pixel's brightness offset)."""
    h, w, _ = rgb.shape
    bh, bw = h // 4, w // 4
    # px[n, i, c], pixel i = column * 4 + row
    px = rgb.reshape(bh, 4, bw, 4, 3).transpose(0, 2, 3, 1, 4).reshape(bh * bw, 16, 3).astype(np.int64)
    i = np.arange(16)
    subs = [(i // 4 >= 2), (i % 4 >= 2)]  # flip 0: right half; flip 1: bottom half (subblock 1)
    var = []
    avg = []
    for s1 in subs:
        v = 0
        a = []
        for m in (~s1, s1):
            p = px[:, m, :]
            mean = p.mean(axis=1)
            v = v + ((p - mean[:, None, :]) ** 2).sum(axis=(1, 2))
            a.append(np.floor(mean + 0.5).astype(np.int64))
        var.append(v)
        avg.append(np.stack(a, axis=1))  # (n, 2, 3)
    flip = (var[1] < var[0]).astype(np.int64)
    avg = np.where(flip[:, None, None] == 1, avg[1], avg[0])
    q5 = (avg * 31 + 127) // 255
    q4 = (avg * 15 + 127) // 255
    d = q5[:, 1, :] - q5[:, 0, :]
    diff = np.all((d >= -4) & (d <= 3), axis=1)
    base = np.where(diff[:, None, None], (q5 << 3) | (q5 >> 2), q4 * 17)  # (n, 2, 3)
    sub_of = np.where(flip[:, None] == 1, subs[1][None, :], subs[0][None, :]).astype(np.int64)  # (n, 16)
    pbase = np.take_along_axis(base, sub_of[:, :, None].repeat(3, axis=2), axis=1)  # (n, 16, 3)
    dl = px.sum(axis=2) - pbase.sum(axis=2)  # (n, 16): 3 x brightness offset
    cand = 3 * MODS.astype(np.int64)  # (8, 4)
    e = (cand[None, None, :, :] - dl[:, :, None, None]) ** 2  # (n, 16, 8, 4)
    emin = e.min(axis=3)  # (n, 16, 8)
    table = np.zeros((len(px), 2), dtype=np.int64)
    for s in range(2):
        m = (sub_of == s)[:, :, None]
        table[:, s] = np.where(m, emin, 0).sum(axis=1).argmin(axis=1)
    tp = np.take_along_axis(table, sub_of, axis=1)  # (n, 16)
    k = np.take_along_axis(e, tp[:, :, None, None].repeat(4, axis=3), axis=2)[:, :, 0, :].argmin(axis=2)  # (n, 16)
    lo = ((k & 1) << i).sum(axis=1) | ((k >> 1) << (i + 16)).sum(axis=1)
    hi = (table[:, 0] << 5) | (table[:, 1] << 2) | (diff.astype(np.int64) << 1) | flip
    for c in range(3):
        shift = 24 - 8 * c
        hd = (q5[:, 0, c] << (shift + 3)) | ((d[:, c] & 7) << shift)
        hn = (q4[:, 0, c] << (shift + 4)) | (q4[:, 1, c] << shift)
        hi = hi | np.where(diff, hd, hn)
    return ((hi.astype(np.uint64) << np.uint64(32)) | lo.astype(np.uint64)).reshape(bh, bw)


def encode_etc1_best(rgb, chunk=1024):
    """(h, w, 3) uint8 -> ETC1 blocks (bh, bw) uint64, minimising the RGB squared error (the pack
    is made once, offline): both partitions, individual and differential bases (each subblock's
    average and brightness shifts of it), every table, the best modifier per pixel. On a menu atlas
    the mean error per channel falls from 4.4 (encode_etc1) to 3.1."""
    h, w, _ = rgb.shape
    bh, bw = h // 4, w // 4
    px_all = rgb.reshape(bh, 4, bw, 4, 3).transpose(0, 2, 3, 1, 4).reshape(bh * bw, 16, 3).astype(np.int32)
    i = np.arange(16)
    subs = [(i // 4 >= 2), (i % 4 >= 2)]  # flip 0: columns 2-3; flip 1: rows 2-3 (subblock 1)
    out = np.zeros(bh * bw, dtype=np.uint64)
    shifts = np.arange(-2, 3)
    for c0 in range(0, bh * bw, chunk):
        px = px_all[c0:c0 + chunk]
        n = len(px)
        best_err = np.full(n, np.iinfo(np.int64).max, dtype=np.int64)
        best_code = np.zeros(n, dtype=np.uint64)
        for flip in (0, 1):
            pix = [px[:, ~subs[flip], :], px[:, subs[flip], :]]  # (n, 8, 3) each, in index order
            idx = [i[~subs[flip]], i[subs[flip]]]
            avg = [p.mean(axis=1) for p in pix]  # (n, 3)
            for diff in (0, 1):
                bits = 5 if diff else 4
                top = (1 << bits) - 1
                # per subblock: candidate quantised bases (n, S, 3), their error and tables
                cands, errs, tabs, sels = [], [], [], []
                for s in range(2):
                    q = np.floor(avg[s] * top / 255 + 0.5).astype(np.int32)
                    qc = np.clip(q[:, None, :] + shifts[None, :, None], 0, top)  # (n, S, 3)
                    base = (qc << 3 | qc >> 2) if diff else qc * 17
                    # (n, S, T, 4 mods, 8 px, 3)
                    v = np.clip(base[:, :, None, None, None, :] + MODS.astype(np.int32)[None, None, :, :, None, None], 0, 255)
                    e = ((v - pix[s][:, None, None, None, :, :]) ** 2).sum(axis=5)  # (n, S, T, 4, 8)
                    sel = e.argmin(axis=3)  # (n, S, T, 8)
                    emin = e.min(axis=3).sum(axis=3)  # (n, S, T)
                    t = emin.argmin(axis=2)  # (n, S)
                    et = np.take_along_axis(emin, t[:, :, None], axis=2)[:, :, 0]  # (n, S)
                    st = np.take_along_axis(sel, t[:, :, None, None], axis=2)[:, :, 0, :]  # (n, S, 8)
                    cands.append(qc), errs.append(et), tabs.append(t), sels.append(st)
                if diff:
                    # pairs (s0, s1) with every channel's difference in -4..3
                    d = cands[1][:, None, :, :] - cands[0][:, :, None, :]  # (n, S0, S1, 3)
                    ok = np.all((d >= -4) & (d <= 3), axis=3)
                    tot = errs[0][:, :, None].astype(np.int64) + errs[1][:, None, :]
                    tot = np.where(ok, tot, np.iinfo(np.int64).max)
                    flat = tot.reshape(n, -1).argmin(axis=1)
                    k0, k1 = np.divmod(flat, len(shifts))
                    err = tot.reshape(n, -1)[np.arange(n), flat]
                else:
                    k0, k1 = errs[0].argmin(axis=1), errs[1].argmin(axis=1)
                    err = errs[0][np.arange(n), k0].astype(np.int64) + errs[1][np.arange(n), k1]
                ar = np.arange(n)
                b0, b1 = cands[0][ar, k0], cands[1][ar, k1]
                t0, t1 = tabs[0][ar, k0], tabs[1][ar, k1]
                s0, s1 = sels[0][ar, k0], sels[1][ar, k1]  # (n, 8): modifier index 0..3 (+a, +b, -a, -b)
                hi = (t0.astype(np.int64) << 5) | (t1.astype(np.int64) << 2) | (diff << 1) | flip
                for c in range(3):
                    shift = 24 - 8 * c
                    if diff:
                        hi |= (b0[:, c].astype(np.int64) << (shift + 3)) | (((b1[:, c] - b0[:, c]) & 7).astype(np.int64) << shift)
                    else:
                        hi |= (b0[:, c].astype(np.int64) << (shift + 4)) | (b1[:, c].astype(np.int64) << shift)
                lo = np.zeros(n, dtype=np.int64)
                # ETC1 pixel indices: (msb << 16 | lsb) at pixel i; modifier index k: lsb = k & 1, msb = k >> 1
                # with the table order (+a, +b, -a, -b) = (00, 01, 10, 11)
                for s, sel in ((0, s0), (1, s1)):
                    for j, pi in enumerate(idx[s]):
                        k = sel[:, j].astype(np.int64)
                        lo |= ((k & 1) << pi) | ((k >> 1) << (pi + 16))
                code = (hi.astype(np.uint64) << np.uint64(32)) | lo.astype(np.uint64)
                better = err < best_err
                best_err = np.where(better, err, best_err)
                best_code = np.where(better, code, best_code)
        out[c0:c0 + n] = best_code
    return out.reshape(bh, bw)


def pica_layout(blocks):
    """ETC1 blocks (bh, bw) in image order (GL's first row first) -> the PICA's layout: 8x8
    tiles left to right, bottom... in the order runtime/gles_pica.cpp tex_store writes them:
    tile (ty, tx) at (ty * tiles_w + tx) * 32, its four blocks in Z order, each little-endian."""
    bh, bw = blocks.shape
    out = np.zeros(bh * bw, dtype='<u8')
    by, bx = np.divmod(np.arange(bh * bw), bw)
    tile = (by >> 1) * (bw >> 1) + (bx >> 1)
    pos = tile * 4 + ((by & 1) << 1 | (bx & 1))
    out[pos] = blocks.reshape(-1)
    return out.tobytes()


def large_pvrs(data=texpack.DATA):
    """(archive/name, width, height, level-0 bytes) of every ETC1 image over 1024."""
    for archive in os.listdir(data):
        if not archive.endswith('.gla'):
            continue
        try:
            z = zipfile.ZipFile(os.path.join(data, archive))
        except zipfile.BadZipFile:
            continue
        for name in z.namelist():
            if not name.lower().endswith(('.etc', '.pvr')):
                continue
            for part, pvr in texpack.etc_parts(z.read(name)):
                try:
                    w, h, level0 = texpack.pvr_level0(pvr)
                except (ValueError, struct.error):
                    continue
                if w > 1024 or h > 1024:
                    yield f'{archive}/{name}' + (f':{part}' if part else ''), w, h, level0


def build(out, data=texpack.DATA):
    entries = {}
    for src, w, h, level0 in large_pvrs(data):
        key = (zlib.crc32(level0), w, h)
        if key in entries:
            continue
        f = 1
        while w // f > 1024 or h // f > 1024:
            f *= 2
        ow, oh = w // f, h // f
        rgb = texpack.decode_etc1(level0, w, h).astype(np.float32)
        small = rgb[:oh * f, :ow * f].reshape(oh, f, ow, f, 3).mean(axis=(1, 3))
        small = np.floor(small + 0.5).astype(np.uint8)
        entries[key] = (ow, oh, pica_layout(encode_etc1_best(small)))
        print(f'{src}: {w}x{h} -> {ow}x{oh} ({key[0]:08x})')
    data = b''
    index = []
    for (crc, w, h), (ow, oh, blob) in sorted(entries.items()):
        index.append((crc, w, h, ow, oh, len(data), len(blob)))
        data += blob
    head = struct.pack('<4sII', b'N3PT', 1, len(index))
    table_size = len(index) * 7 * 4
    with open(out, 'wb') as f:
        f.write(head)
        for crc, w, h, ow, oh, off, size in index:
            f.write(struct.pack('<7I', crc, w, h, ow, oh, len(head) + table_size + off, size))
        f.write(data)
    print(f'{out}: {len(index)} images, {os.path.getsize(out) / 1048576:.1f} MB')


def check(trace):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import gltrace
    keys = set()
    if os.path.exists(OUT):
        with open(OUT, 'rb') as f:
            magic, version, n = struct.unpack('<4sII', f.read(12))
            for _ in range(n):
                crc, w, h = struct.unpack('<7I', f.read(28))[:3]
                keys.add((crc, w, h))
    for name, w, b in gltrace.records(trace):
        if name == 'glCompressedTexImage2D' and w[1] == 0 and (w[3] > 1024 or w[4] > 1024):
            key = (zlib.crc32(b[0]), w[3], w[4])
            print(f'{key[0]:08x} {w[3]}x{w[4]}: {"in the pack" if key in keys else "MISSING"}')


if __name__ == '__main__':
    if sys.argv[1] == 'build':
        build(sys.argv[2] if len(sys.argv) > 2 else OUT, sys.argv[3] if len(sys.argv) > 3 else texpack.DATA)
    elif sys.argv[1] == 'check':
        check(sys.argv[2])
