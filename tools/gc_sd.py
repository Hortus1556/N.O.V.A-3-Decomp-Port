"""The GameCube port's SD card contents (docs/gamecube.md, Running on a console):

    OUT/nova3.dol               the program (Swiss lists and boots it)
    OUT/nova3/gcode.bin         the translated code (paged in from the card)
    OUT/nova3/nova3_data/...    the game files
    OUT/nova3/user/             the player's files (made by the game)
    OUT/nova3/log.txt           the log (written by the game)

python tools/gc_sd.py OUT [--dol build/gc/out/nova3.dol] [--code build/gc/out/gcode.bin]
                          [--data build/3ds/slim_data] [--skip-suffix .z ...]

OUT may be the card itself (e.g. F:\\) or a folder to copy to it. Files already there with the
same size and modification time are left as they are, so a new build copies only the program
and the code."""
import argparse
import os
import shutil


def copy(src, dst):
    if os.path.exists(dst):
        a, b = os.stat(src), os.stat(dst)
        if a.st_size == b.st_size and int(a.st_mtime) == int(b.st_mtime):
            return 0
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    return os.path.getsize(dst)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out')
    ap.add_argument('--dol', default='build/gc/out/nova3.dol')
    ap.add_argument('--code', default='build/gc/out/gcode.bin')
    ap.add_argument('--data', default='build/3ds/slim_data')
    ap.add_argument('--skip-suffix', action='append', default=None,
                    help='game files left out (default: .z .idx .pak, the 3DS packing)')
    a = ap.parse_args()
    skip = tuple(a.skip_suffix if a.skip_suffix is not None else ['.z', '.idx', '.pak'])
    copied = copy(a.dol, os.path.join(a.out, 'nova3.dol'))
    copied += copy(a.code, os.path.join(a.out, 'nova3', 'gcode.bin'))
    files = 0
    for root, _, names in os.walk(a.data):
        for n in names:
            if n.endswith(skip):
                continue
            src = os.path.join(root, n)
            copied += copy(src, os.path.join(a.out, 'nova3', 'nova3_data', os.path.relpath(src, a.data)))
            files += 1
    print(f'gc_sd: {a.out}: nova3.dol, nova3/gcode.bin, {files} game files; {copied >> 20} MB copied')


if __name__ == '__main__':
    main()
