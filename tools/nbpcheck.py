"""Check AI-remastered textures against their originals before they go into the HD pack.

Usage: nbpcheck.py [dir]    dir: the hd_textures folder (default: hd_textures/ in the project)

Reads dir/nbp/raw/*.png (remastered by Nano Banana Pro, any size, named as exported), finds each
one's original in dir/original/ through dir/textures.csv and measures, at the original's size
(a square result of a non-square original was made from it padded to a centred square: dir/nbp/padded/,
and is cropped back first):
  shift   how far the content moved (phase correlation, worst of a 4x4 grid of tiles, in original
          pixels): a texture sheet is UV-mapped, so any drift puts the texture in the wrong place
  ssim    structural similarity of the luminance (1 = same structure)
  colour  mean absolute difference of the RGB channel means (0..255)
Each result that passes is saved at its own resolution, but at most 4x its original, into
dir/nbp/final/<size folder>/ (ready for dir/upscaled/); every result is listed, with
a side-by-side, in dir/nbp/report.html. Nothing goes into dir/upscaled/: that stays a choice.
"""
import csv
import html
import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCALE = 4
MAX_SHIFT = 1.0     # original pixels
MIN_SSIM = 0.80
MAX_COLOUR = 6.0


def luma(rgb):
    return rgb @ np.array([0.299, 0.587, 0.114])


def box_mean(a, r):
    """Mean over a (2r+1)^2 window, edges clamped."""
    p = np.pad(a, r + 1, mode='edge').cumsum(0).cumsum(1)
    k = 2 * r + 1
    s = p[k:, k:] - p[:-k, k:] - p[k:, :-k] + p[:-k, :-k]
    return s[:a.shape[0], :a.shape[1]] / (k * k)


def ssim(a, b, r=3):
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    ma, mb = box_mean(a, r), box_mean(b, r)
    va = box_mean(a * a, r) - ma * ma
    vb = box_mean(b * b, r) - mb * mb
    cov = box_mean(a * b, r) - ma * mb
    return float((((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2))).mean())


def shift(a, b):
    """Translation of b relative to a, in pixels (phase correlation, sub-pixel peak)."""
    h, w = a.shape
    win = np.outer(np.hanning(h), np.hanning(w))
    fa, fb = np.fft.fft2((a - a.mean()) * win), np.fft.fft2((b - b.mean()) * win)
    cross = fa.conj() * fb
    r = np.fft.ifft2(cross / (np.abs(cross) + 1e-9)).real
    y, x = np.unravel_index(np.argmax(r), r.shape)

    def sub(m, p, n):
        return 0.0 if m == n or m == p else 0.5 * (m - n) / (m - 2 * p + n) if (m - 2 * p + n) else 0.0
    dy = y + sub(r[(y - 1) % h, x], r[y, x], r[(y + 1) % h, x])
    dx = x + sub(r[y, (x - 1) % w], r[y, x], r[y, (x + 1) % w])
    dy = dy - h if dy > h / 2 else dy
    dx = dx - w if dx > w / 2 else dx
    return float(np.hypot(dx, dy))


def worst_shift(a, b, grid=4):
    h, w = a.shape
    if min(h, w) < 64:
        return shift(a, b)
    th, tw = h // grid, w // grid
    out = []
    for i in range(grid):
        for j in range(grid):
            ta, tb = a[i*th:(i+1)*th, j*tw:(j+1)*tw], b[i*th:(i+1)*th, j*tw:(j+1)*tw]
            if ta.std() > 4:        # flat tiles have nothing to align
                out.append(shift(ta, tb))
    return max(out) if out else 0.0


def check(folder):
    with open(os.path.join(folder, 'textures.csv'), newline='') as f:
        rows = {os.path.basename(r['file']): r for r in csv.DictReader(f)}
    raw = os.path.join(folder, 'nbp', 'raw')
    report = []
    for name in sorted(os.listdir(raw)):
        if not name.lower().endswith('.png'):
            continue
        row = rows.get(name)
        if row is None:
            print(f'{name}: not in textures.csv, skipped')
            continue
        orig = Image.open(os.path.join(folder, 'original', row['file'])).convert('RGB')
        out = Image.open(os.path.join(raw, name)).convert('RGB')
        w, h = orig.size
        if w != h and out.width == out.height:
            k = out.width / max(w, h)
            cw, ch = round(w * k), round(h * k)
            out = out.crop(((out.width - cw) // 2, (out.height - ch) // 2,
                            (out.width - cw) // 2 + cw, (out.height - ch) // 2 + ch))
        problems = []
        if abs(out.width / out.height - w / h) > 0.01:
            problems.append(f'shape {out.width}x{out.height}')
        o = np.asarray(orig, np.float64)
        d = np.asarray(out.resize((w, h), Image.LANCZOS), np.float64)
        s, sim = worst_shift(luma(o), luma(d)), ssim(luma(o), luma(d))
        colour = float(np.abs(o.mean((0, 1)) - d.mean((0, 1))).mean())
        if s > MAX_SHIFT:
            problems.append(f'moved {s:.2f} px')
        if sim < MIN_SSIM:
            problems.append(f'structure ssim {sim:.3f}')
        if colour > MAX_COLOUR:
            problems.append(f'colour {colour:.1f}')
        final = os.path.join(folder, 'nbp', 'final', os.path.dirname(row['file']).split('/')[-1], name)
        if problems:
            if os.path.exists(final):
                os.remove(final)
            shown = os.path.join(raw, name)
        else:
            os.makedirs(os.path.dirname(final), exist_ok=True)
            k = min(SCALE, out.width / w)
            out.resize((round(w * k), round(h * k)), Image.LANCZOS).save(final)
            shown = final
        verdict = 'FAIL: ' + ', '.join(problems) if problems else 'ok'
        print(f'{name}: {out.width}x{out.height}, shift {s:.2f} px, ssim {sim:.3f}, colour {colour:.1f} -> {verdict}')
        report.append((name, row['file'], shown, s, sim, colour, verdict))
    write_report(folder, report)


def write_report(folder, report):
    nbp = os.path.join(folder, 'nbp')
    rel = lambda p: os.path.relpath(p, nbp).replace(os.sep, '/')
    parts = ['<!doctype html><meta charset="utf-8"><title>Remaster check</title><style>'
             'body{font:14px sans-serif;background:#1b1b1b;color:#ddd;margin:16px}'
             '.pair{display:flex;gap:8px;flex-wrap:wrap}img{width:min(48vw,900px);image-rendering:pixelated}'
             '.fail{color:#f77}.ok{color:#7d7}</style><h1>Remaster check</h1>']
    for name, file, final, s, sim, colour, verdict in report:
        cls = 'ok' if verdict == 'ok' else 'fail'
        parts.append(f'<h2>{html.escape(name)}</h2><p class="{cls}">shift {s:.2f} px · ssim {sim:.3f} · '
                     f'colour {colour:.1f} · {html.escape(verdict)}</p><div class="pair">'
                     f'<img src="{rel(os.path.join(folder, "original", file))}" alt="original">'
                     f'<img src="{rel(final)}" alt="remastered"></div>')
    with open(os.path.join(nbp, 'report.html'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(parts))


if __name__ == '__main__':
    check(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'hd_textures'))
