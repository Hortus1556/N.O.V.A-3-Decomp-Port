"""Packs the Switch build into an installable .nsp (title 01004E4F56410000) with hacBrewPack.

Usage: switch_nsp.py --keys <prod.keys> [--data dist/nova3_data] [--out build/switch/nova3.nsp]

Inputs, from the Switch build (build/switch, the nova3_exefs target):
  * exefs: .dkp-generated/nova3_exefs/{main, main.npdm} (host/switch/nova3.json);
  * control: nova3.nacp plus the game's icon from the APK as a 256x256 JPEG;
  * romfs: the game files (dist/nova3_data, the packaged PC copy: libGloftNAHM.so and the
    OBBs' contents), without the PC's hd_textures.pak. The save games, nova3.ini and the log
    stay on the SD card in sdmc:/switch/nova3.
The keys are the user's own, dumped from their console (Lockpick_RCM); hacBrewPack needs them
to encrypt the NCAs. Like the rest of the build output, the .nsp is for the player's own copy.
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

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = ROOT / 'build' / 'switch'
TITLE_ID = '01004e4f56410000'
PACKER = ROOT / 'build' / 'tools' / 'hacbrewpack.exe'  # hacBrewPack 3.05 (from SAK)
APK = ROOT / 'NOVA-3-v1-0-1d.apk'
SKIP = {'hd_textures.pak'}  # PC only


def icon_jpeg(out):
    with zipfile.ZipFile(APK) as z:
        names = [n for n in z.namelist() if re.fullmatch(r'res/drawable[^/]*/icon\.png', n)]
        best = max((Image.open(io.BytesIO(z.read(n))) for n in names), key=lambda im: im.size[0])
    canvas = Image.new('RGB', (256, 256), (0, 0, 0))
    im = best.convert('RGBA').resize((256, 256), Image.LANCZOS)
    canvas.paste(im, mask=im.split()[3])
    canvas.save(out, format='JPEG', quality=95)


def place(src, dst):
    """Hard links where possible (same drive): the RomFS input is 1.6 GB."""
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--keys', required=True, help='prod.keys from your console')
    ap.add_argument('--data', default=str(ROOT / 'dist' / 'nova3_data'), help='the game files')
    ap.add_argument('--out', default=str(BUILD / 'nova3.nsp'))
    args = ap.parse_args()
    exefs = BUILD / '.dkp-generated' / 'nova3_exefs'
    data = pathlib.Path(args.data)
    for f in (exefs / 'main', exefs / 'main.npdm', BUILD / 'nova3.nacp', PACKER, data / 'libGloftNAHM.so'):
        if not f.exists():
            sys.exit(f'missing {f}: build the nova3_exefs target, put hacbrewpack.exe in build/tools, build dist/nova3_data first')
    work = BUILD / 'nsp'
    shutil.rmtree(work, ignore_errors=True)
    for d in ('exefs', 'control', 'out', 'temp'):
        (work / d).mkdir(parents=True)
    shutil.copy(exefs / 'main', work / 'exefs' / 'main')
    shutil.copy(exefs / 'main.npdm', work / 'exefs' / 'main.npdm')
    shutil.copy(BUILD / 'nova3.nacp', work / 'control' / 'control.nacp')
    icon_jpeg(work / 'control' / 'icon_AmericanEnglish.dat')
    shutil.copytree(data, work / 'romfs', copy_function=place, ignore=lambda d, names: [n for n in names if n in SKIP])
    cmd = [str(PACKER), '-k', str(pathlib.Path(args.keys).resolve()), '--titleid', TITLE_ID,
           '--exefsdir', 'exefs', '--controldir', 'control', '--romfsdir', 'romfs',
           '--nologo', '--tempdir', 'temp', '--nspdir', 'out']
    print(' '.join(cmd))
    subprocess.run(cmd, cwd=work, check=True)
    built = next((work / 'out').glob('*.nsp'))
    shutil.move(built, args.out)
    print(f'{args.out}: {pathlib.Path(args.out).stat().st_size / 1e6:.1f} MB')


if __name__ == '__main__':
    main()
