"""Compares a replay's frames with the PC's screenshots of the same frames.

Usage: compare.py [TRACE ...]      (default: every trace in build/3ds/replay that has frames)

For each build/3ds/replay/<trace>/frame_<n>.png with a PC screenshot build/3ds/traces/<trace>/f<n>.png:
the mean absolute difference per channel (of 255), the share of pixels off by more than 32 in
some channel, and cmp_<n>.png (PC left, 3DS right, below them the difference x4).
"""
import pathlib
import re
import sys

import numpy as np
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[2]
REPLAY = ROOT / 'build' / '3ds' / 'replay'
TRACES = ROOT / 'build' / '3ds' / 'traces'


def compare(trace):
    out = []
    for f in sorted((REPLAY / trace).glob('frame_*.png')):
        m = re.fullmatch(r'frame_(\d+)\.png', f.name)
        if not m:
            continue
        pc = TRACES / trace / ('f%s.png' % m.group(1))
        if not pc.exists():
            continue
        a = np.asarray(Image.open(pc).convert('RGB'), dtype=np.int16)
        b = np.asarray(Image.open(f).convert('RGB'), dtype=np.int16)
        if a.shape != b.shape:
            continue
        d = np.abs(a - b)
        mean = float(d.mean())
        bad = float((d.max(axis=2) > 32).mean() * 100)
        top = np.concatenate([a, b], axis=1)
        diff = np.clip(d * 4, 0, 255)
        bottom = np.concatenate([diff, np.zeros_like(diff)], axis=1)
        Image.fromarray(np.concatenate([top, bottom], axis=0).astype(np.uint8)).save(REPLAY / trace / ('cmp_%s.png' % m.group(1)))
        out.append((int(m.group(1)), mean, bad))
    return out


def main():
    traces = sys.argv[1:] or sorted(p.name for p in REPLAY.iterdir() if p.is_dir())
    for t in traces:
        for frame, mean, bad in compare(t):
            print('%-6s frame %5d: difference %5.2f, %5.2f%% of pixels off by > 32' % (t, frame, mean, bad))


if __name__ == '__main__':
    main()
