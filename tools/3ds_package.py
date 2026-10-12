"""Lays out a New 3DS test package: the .cia, which holds the game files in its RomFS.

Usage: 3ds_package.py [--out build/3ds/package] [--cia build/3ds/cmake/nova3.cia] [--sdmc [--data DIR]]
       3ds_package.py --sd [--out build/3ds/package_sd] [--data DIR]

  <out>/nova3.cia                          install with FBI (Luma3DS): nothing else to copy

The .cia comes from tools/3ds_cia.py, which puts the console's game files (the slim set,
build/3ds/slim_data, from python tools/3ds_slimdata.py build) in its RomFS: romfs:/nova3_data.
--sdmc also lays out the old SD card copy, <out>/sdmc/3ds/nova3/nova3_data/ (copy the sdmc
folder's contents to the SD card's root): the game reads it instead of the RomFS's when it is
there (to try other game files without rebuilding the .cia: e.g. --data dist/nova3_data, the
full set without the PC's hd_textures.pak; a .cia built with 3ds_cia.py --no-data is then 5 MB).
The same files as the RomFS's: X is left out where X.z is there (the library's compressed copy,
the packed archives), no test.txt, no nova3.ini (the game writes its defaults at the first
start: 30 fps). The game files as hard links where possible (same drive). Build first:
ninja -C build/3ds/cmake nova3, then python tools/3ds_cia.py.

--sd (console testing, quick to repeat): the .cia without the game files (3ds_cia.py --no-data
--name nova3_sd: about 5 MB, the program and the ARMv6K patch set; the same title, shown as
"N.O.V.A. 3 (SD test)"), and the game files as one RomFS image for the SD card,
<out>/sdmc/3ds/nova3/nova3_data.romfs (devkitPro's mkromfs3ds; the game mounts it and reads it
through one file system handle, as the .cia's RomFS: a folder of game files on a console ran out
of open files). Copy it once; after that a new build is the small .cia alone, reinstalled in
seconds. The image is made again only when the game files changed (build/3ds/sd_data_manifest.json),
and then said so: copy it again. Back to the full .cia: delete sdmc:/3ds/nova3/nova3_data.romfs.
"""
import argparse
import importlib
import json
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
cia_tool = importlib.import_module('3ds_cia')  # console_set, place
MANIFEST = ROOT / 'build' / '3ds' / 'sd_data_manifest.json'
MKROMFS = pathlib.Path('C:/msys64/opt/devkitpro/tools/bin/mkromfs3ds.exe')


def sd_image(files):
    """build/3ds/sd_image/nova3_data.romfs: the game files under nova3_data/, made again when they
    changed (size and time); whether it was."""
    now = {rel.as_posix(): [src.stat().st_size, int(src.stat().st_mtime)] for rel, src in files}
    before = json.loads(MANIFEST.read_text(encoding='utf-8')) if MANIFEST.exists() else None
    image = ROOT / 'build' / '3ds' / 'sd_image' / 'nova3_data.romfs'
    if before == now and image.exists():
        return image, False
    stage = image.parent / 'stage'
    shutil.rmtree(stage, ignore_errors=True)
    for rel, src in files:
        cia_tool.place(src, stage / 'nova3_data' / rel)
    image.unlink(missing_ok=True)
    subprocess.run([str(MKROMFS), str(stage), str(image)], check=True)
    shutil.rmtree(stage, ignore_errors=True)
    MANIFEST.write_text(json.dumps(now, indent=1), encoding='utf-8')
    return image, True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=None, help='default build/3ds/package (--sd: build/3ds/package_sd)')
    ap.add_argument('--cia', default=str(ROOT / 'build' / '3ds' / 'cmake' / 'nova3.cia'))
    ap.add_argument('--sdmc', action='store_true', help='also the game files for the SD card (the old layout)')
    ap.add_argument('--sd', action='store_true', help='testing: a 5 MB .cia without the game files, and the game files for the SD card')
    ap.add_argument('--data', default=str(cia_tool.SLIM), help='with --sdmc / --sd: the game files (default the slim set)')
    args = ap.parse_args()
    out = pathlib.Path(args.out or ROOT / 'build' / '3ds' / ('package_sd' if args.sd else 'package'))
    cia = pathlib.Path(args.cia)
    if args.sd:
        subprocess.run([sys.executable, str(ROOT / 'tools' / '3ds_cia.py'), '--no-data', '--name', 'nova3_sd',
                        '--title', 'N.O.V.A. 3 (SD test)'], check=True)
        cia = ROOT / 'build' / '3ds' / 'cmake' / 'nova3_sd.cia'
    if not cia.exists():
        sys.exit(f'missing {cia} (python tools/3ds_cia.py)')
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    shutil.copy2(cia, out / 'nova3.cia')  # (a copy: makerom rewrites its output in place)
    print(f'{out / "nova3.cia"}: {cia.stat().st_size / 1e6:.1f} MB')
    if args.sdmc or args.sd:
        data = pathlib.Path(args.data)
        files = list(cia_tool.console_set(data)) if data.is_dir() else []
        if not any(rel.as_posix() == 'pica_textures.pak' for rel, _ in files):
            sys.exit(f'{data} is not a game data folder' + (' (python tools/3ds_slimdata.py build)' if data == cia_tool.SLIM else ''))
        total = sum(src.stat().st_size for _, src in files)
        if args.sd:
            image, made = sd_image(files)
            dst = out / 'sdmc' / '3ds' / 'nova3' / 'nova3_data.romfs'
            cia_tool.place(image, dst)
            print(f'{dst}: {len(files)} files, {image.stat().st_size / 1e9:.2f} GB from {data} (copy the contents of {out / "sdmc"} to the SD card root)')
            print('game files: changed: copy nova3_data.romfs to the SD card again' if made
                  else 'game files: unchanged since the last SD package: install the .cia alone')
            return
        dst = out / 'sdmc' / '3ds' / 'nova3' / 'nova3_data'
        for rel, src in files:
            cia_tool.place(src, dst / rel)
        print(f'{dst}: {len(files)} files, {total / 1e9:.2f} GB from {data} (copy the contents of {out / "sdmc"} to the SD card root)')

if __name__ == '__main__':
    main()
