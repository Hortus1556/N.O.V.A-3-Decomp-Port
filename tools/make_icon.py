"""The game's launcher icon from the APK, as a Windows .ico for nova3.exe (build step).

Usage: make_icon.py <apk> <out.ico>

Takes the largest res/drawable*/icon.png and writes the sizes Windows asks for (16 to 256
pixels; the 192-pixel original is scaled up for 256). The icon is part of the game's data:
like the rest of the build output, the result is for the player's own copy.
"""
import io
import re
import sys
import zipfile

from PIL import Image

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]


def main(apk, out):
    with zipfile.ZipFile(apk) as z:
        names = [n for n in z.namelist() if re.fullmatch(r'res/drawable[^/]*/icon\.png', n)]
        if not names:
            sys.exit(f'{apk}: no res/drawable*/icon.png')
        images = [Image.open(io.BytesIO(z.read(n))).convert('RGBA') for n in names]
    best = max(images, key=lambda im: im.size[0])
    best.save(out, format='ICO', sizes=[(s, s) for s in SIZES])
    print(f'{out}: {best.size[0]}x{best.size[1]} -> {len(SIZES)} sizes')


if __name__ == '__main__':
    main(*sys.argv[1:3])
