"""Packs a New 3DS build into a .cxi (Azahar loads it directly) and a .cia (installs on a console).

Usage: 3ds_cia.py [--elf build/3ds/cmake/nova3.elf] [--name nova3] [--title "N.O.V.A. 3"] [--unique-id 0xF4E33]
                  [--data DIR | --no-data] [--romfs DIR | --no-romfs]

makerom (build/tools/makerom.exe, Project_CTR v0.19) builds the NCCH from the ELF with the
exheader of host/3ds/nova3.rsf: the New 3DS's 178 MB application memory (SystemModeExt), the
804 MHz clock with the L2 cache, and the SVCs the native code loader needs
(ControlProcessMemory 0x70, Backdoor 0x7B, FlushProcessDataCache 0x54). The icon is the game's
own (from the APK) as an SMDH, when the APK is there; otherwise libctru's default icon. No banner.

RomFS (laid out in <dir of the ELF>/romfs, or romfs_<name> for another --name; hard links): the ARMv6K patch set the native code
loader applies (build/3ds/v6patch.bin, from tools/v6patch.py) as romfs:/v6patch.bin, and the game
files as romfs:/nova3_data/: the console set of --data DIR (default build/3ds/slim_data, from
tools/3ds_slimdata.py build: 59 files, 0.98 GB), i.e. without the PC's hd_textures.pak, without
test.txt / nova3.ini / nova3.log, and without X where X.z is there (the library's compressed copy,
the packed archives). One file to install; the game reads sdmc:/3ds/nova3/nova3_data/ instead
when that is there (a developer's override). --no-data leaves the game files out (they must then
be on the SD card: 3ds_run.py --deploy, 3ds_package.py --sdmc); --romfs DIR packs a folder of
one's own instead, --no-romfs none.
Outputs: <dir of the ELF>/<name>.cxi, <name>.cia, <name>.smdh.
"""
import argparse
import io
import os
import pathlib
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
MAKEROM = ROOT / 'build' / 'tools' / 'makerom.exe'
DKP = pathlib.Path('C:/msys64/opt/devkitpro')
SMDHTOOL = DKP / 'tools' / 'bin' / 'smdhtool.exe'
RSF = ROOT / 'host' / '3ds' / 'nova3.rsf'
APK = ROOT / 'NOVA-3-v1-0-1d.apk'
SLIM = ROOT / 'build' / '3ds' / 'slim_data'
SKIP = {'hd_textures.pak'}  # PC only
NOT_PACKAGED = {'test.txt', 'nova3.ini', 'nova3.log'}


def console_set(data):
    """The console's game files of a data folder: (path relative to it, source) for each file. X is
    left out where X.z is there (the console reads the library's compressed copy, runtime/native_3ds.cpp,
    and the packed archives X.gla.z, runtime/libc_io.cpp)."""
    data = pathlib.Path(data)
    top = {p.name for p in data.iterdir()}
    for src in sorted(data.rglob('*')):
        rel = src.relative_to(data)
        if rel.parts[0] in SKIP or rel.name in NOT_PACKAGED or (len(rel.parts) == 1 and rel.name + '.z' in top):
            continue
        if src.is_file():
            yield rel, src


def place(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def icon_png(out):
    """The game's launcher icon, 48x48 (the SMDH's large icon; smdhtool makes the small one)."""
    if APK.exists():
        from PIL import Image
        with zipfile.ZipFile(APK) as z:
            names = [n for n in z.namelist() if re.fullmatch(r'res/drawable[^/]*/icon\.png', n)]
            best = max((Image.open(io.BytesIO(z.read(n))) for n in names), key=lambda im: im.size[0])
        best.convert('RGBA').resize((48, 48), Image.LANCZOS).save(out)
        return out
    return DKP / 'libctru' / 'default_icon.png'


def run(cmd):
    print(' '.join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', default=str(ROOT / 'build' / '3ds' / 'cmake' / 'nova3.elf'))
    ap.add_argument('--romfs', default=None, help='RomFS folder (default: build/3ds/v6patch.bin and the game files)')
    ap.add_argument('--data', default=str(SLIM), help='the game files for romfs:/nova3_data (default the slim set)')
    ap.add_argument('--no-data', action='store_true', help='no game files in the RomFS (they go on the SD card)')
    ap.add_argument('--no-romfs', action='store_true')
    ap.add_argument('--name', default=None, help='output base name (default: the ELF\'s)')
    ap.add_argument('--title', default='N.O.V.A. 3')
    ap.add_argument('--short-title', default='NOVA3', help='the exheader name (8 characters at most)')
    ap.add_argument('--description', default='Near Orbit Vanguard Alliance')
    ap.add_argument('--author', default='Gameloft (port)')
    ap.add_argument('--product-code', default='CTR-P-NOV3')
    ap.add_argument('--unique-id', default='0xF4E33', help='title id low bits (homebrew range)')
    ap.add_argument('--no-cia', action='store_true', help='only the .cxi (for Azahar)')
    args = ap.parse_args()
    elf = pathlib.Path(args.elf).resolve()
    if not elf.exists():
        sys.exit(f'missing {elf}: build it first')
    out = elf.parent
    name = args.name or elf.stem
    smdh = out / f'{name}.smdh'
    run([SMDHTOOL, '--create', args.title, args.description, args.author, icon_png(out / f'{name}_icon.png'), smdh])
    cxi = out / f'{name}.cxi'
    rsf = RSF
    if not args.no_romfs:
        romfs = pathlib.Path(args.romfs) if args.romfs else out / ('romfs' if name == elf.stem else f'romfs_{name}')
        if not args.romfs:
            patch = ROOT / 'build' / '3ds' / 'v6patch.bin'
            if not patch.exists():
                sys.exit(f'missing {patch}: run python tools/v6patch.py')
            shutil.rmtree(romfs, ignore_errors=True)
            romfs.mkdir()
            shutil.copy2(patch, romfs / 'v6patch.bin')
            if not args.no_data:
                data = pathlib.Path(args.data)
                files = list(console_set(data)) if data.is_dir() else []
                names = {rel.as_posix() for rel, _ in files}
                if 'pica_textures.pak' not in names or not names & {'libGloftNAHM.so', 'libGloftNAHM.so.z'}:
                    sys.exit(f'{data} is not a game data folder' + (' (python tools/3ds_slimdata.py build)' if data == SLIM else ''))
                for rel, src in files:
                    place(src, romfs / 'nova3_data' / rel)
                total = sum(src.stat().st_size for _, src in files)
                print(f'romfs:/nova3_data: {len(files)} files, {total / 1e9:.2f} GB from {data}')
        rsf = out / f'{name}.rsf'  # the exheader settings plus the RomFS folder
        rsf.write_text(RSF.read_text(encoding='utf-8') + f'\nRomFs:\n  RootPath: "{romfs.resolve().as_posix()}"\n',
                       encoding='utf-8')
    defs = [f'-DAPP_TITLE={args.short_title[:8]}', f'-DAPP_PRODUCT_CODE={args.product_code}', f'-DAPP_UNIQUE_ID={args.unique_id}']
    run([MAKEROM, '-f', 'cxi', '-o', cxi, '-rsf', rsf, '-target', 't', '-elf', elf, '-icon', smdh] + defs)
    if not args.no_cia:
        cia = out / f'{name}.cia'
        run([MAKEROM, '-f', 'cia', '-o', cia, '-target', 't', '-i', f'{cxi}:0:0'])
        print(f'{cia}: {cia.stat().st_size / 1e6:.1f} MB')
        if cia.stat().st_size >= 4 << 30:
            sys.exit(f'{cia} is 4 GB or more: too large for the SD card (FAT32) and FBI')
    print(f'{cxi}: {cxi.stat().st_size / 1e6:.1f} MB')


if __name__ == '__main__':
    main()
