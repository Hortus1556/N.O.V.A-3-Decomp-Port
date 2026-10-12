"""Plays GL traces in Azahar with the 3DS replay app and collects their frames and timings.

Usage: run.py TRACE [TRACE ...] [--repeat N] [--timeout S] [--out DIR] [--shot window.png] [--debug F,F,...]

  TRACE      a gltrace.bin recorded with NOVA3_GLTRACE (runtime/gles.cpp); several play in one
             emulator start, each one's results in its own folder
  --repeat   draw each recorded frame N times (timings from all, screenshot from the first)
  --out      where the frame PNGs and the log go (default build/3ds/replay/<trace folder name>)
  --debug    per trace: the backend's experiment flags (pica::set_debug; 1: the state through
             citro3d's own path), for A/B comparisons within one emulator start

The traces are copied to the emulated SD card (sdmc:/3ds/glreplay/trace.bin, trace1.bin ...) with
replay.txt; tools/3ds_run.py starts Azahar with build/3ds/glreplay/glreplay.3dsx (make in this
folder), holding build/3ds/run.lock, and stops at "replay: done" or the timeout.
"""
import argparse
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SD = pathlib.Path('E:/Emulators/Azahar/user/sdmc/3ds/glreplay')
APP = ROOT / 'build' / '3ds' / 'glreplay' / 'glreplay.3dsx'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trace', nargs='+')
    ap.add_argument('--repeat', type=int, default=1)
    ap.add_argument('--timeout', type=float, default=240, help='per trace')
    ap.add_argument('--out', default=None, help='(one trace)')
    ap.add_argument('--shot', default=None)
    ap.add_argument('--no-pack', action='store_true', help='without pica_textures.pak (large images converted at load)')
    ap.add_argument('--targets', action='store_true', help='also save the render-target textures of each frame')
    ap.add_argument('--debug', default=None, help='per trace, comma-separated: experiment flags for the backend (pica::set_debug)')
    args = ap.parse_args()
    traces = [pathlib.Path(t).resolve() for t in args.trace]
    outs = [pathlib.Path(args.out) if args.out and len(traces) == 1 else ROOT / 'build' / '3ds' / 'replay' / t.parent.name
            for t in traces]
    SD.mkdir(parents=True, exist_ok=True)
    for old in list(SD.glob('frame_*.png')) + list(SD.glob('t*_frame_*.png')) + list(SD.glob('trace*.bin')):
        old.unlink()
    lines = []
    for k, t in enumerate(traces):
        name = 'trace%s' % (k or '')
        shutil.copy(t, SD / (name + '.bin'))
        lines.append('%s=sdmc:/3ds/glreplay/%s.bin' % (name, name))
    lines.append('repeat=%d' % args.repeat)
    for k, flags in enumerate((args.debug or '').split(',') if args.debug else []):
        lines.append('debug%s=%s' % (k or '', flags))
    if args.targets:
        lines.append('targets=1')
    pack = ROOT / 'dist' / 'nova3_data' / 'pica_textures.pak'
    if pack.exists() and not args.no_pack:  # (tools/3ds_glreplay/picapack.py)
        if not (SD / pack.name).exists() or (SD / pack.name).stat().st_mtime < pack.stat().st_mtime:
            shutil.copy(pack, SD / pack.name)
        lines.append('pack=sdmc:/3ds/glreplay/' + pack.name)
    (SD / 'replay.txt').write_text('\n'.join(lines) + '\n')
    cmd = [sys.executable, str(ROOT / 'tools' / '3ds_run.py'), str(APP), '--log', '3ds/glreplay/replay.log',
           '--wait', 'replay: done', '--fail', 'FATAL|crash|backend failed|cannot read',
           '--timeout', str(args.timeout * len(traces)), '--tail', '400']
    if args.shot:
        cmd += ['--shot', args.shot]
    r = subprocess.run(cmd, capture_output=True, text=True)
    log = SD / 'replay.log'
    text = log.read_text(errors='replace') if log.exists() else ''
    # each trace's part of the log (the start's lines kept for the later ones), and its frames
    parts = re.split(r'(?m)^replay: next trace .*\n', text)
    head = parts[0].split('\nreplay: frame', 1)[0] + '\n'
    for k, out in enumerate(outs):
        out.mkdir(parents=True, exist_ok=True)
        (out / 'run.txt').write_text(r.stdout + r.stderr)
        prefix = 't%d_' % k if k else ''
        for f in SD.glob(prefix + 'frame_*.png'):
            shutil.copy(f, out / f.name[len(prefix):])
        (out / 'replay.log').write_text((head if k else '') + (parts[k] if k < len(parts) else ''))
        print('%s: frames and log in %s' % (traces[k].parent.name, out))
    print(r.stdout[-3000:])
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
