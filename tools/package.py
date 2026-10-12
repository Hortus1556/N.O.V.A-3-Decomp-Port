"""Assemble a self-contained game folder: nova3.exe with its DLLs, and the game files in
nova3_data next to it (host/main.cpp finds them there).

Usage: package.py [--obb] [dest]    (default: dist in the repository)

    dest/nova3.exe, SDL3.dll         the build (build/cmake_full)
    dest/libEGL.dll, libGLESv2.dll,  ANGLE (GLES on Direct3D 11), from the newest Microsoft
         d3dcompiler_47.dll          Edge installation, unless already there
    dest/nova3_data/                 all in one folder:
        libGloftNAHM.so              game/libGloftNAHM.so (the game's library: data and symbols)
        *.gla, GameOptions.json, ... the OBBs' contents (56 files), or with --obb the two OBB
                                     files themselves
        hd_textures.pak              optional, made separately (tools/texpack.py): left alone

The game opens each archive as a loose file in the OBB folder when its OBB does not have it
(CCustomFileSystem::getFileInOBB falls back to the plain file system), so the extracted files
work as they are; it only notes "could not find main.obb pack!" at start. Extracting takes
1.6 GB of space next to the OBBs. The library (and with --obb the OBBs) are hard-linked when
source and destination are on the same drive (no copy, no extra space), otherwise copied;
files already there and unchanged are left alone. The APK is not needed (its only reader, an
analytics event, is stubbed); the game's scratch files go to the system temp folder. Saves and settings stay in the player's folder (Saved
Games\\N.O.V.A. 3). The folder holds the game's own files and code derived from them: it is
for the player's own copy, not for sharing.
After a build, CMake copies nova3.exe into dist again when the folder exists.
"""
import glob
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import obb as obbfile  # noqa: E402  (tools/obb.py)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, 'build', 'cmake_full')
PACKAGE = 'com.gameloft.android.ANMP.GloftNAHM'


def same(a, b):
    if not os.path.exists(b):
        return False
    sa, sb = os.stat(a), os.stat(b)
    return (sa.st_ino and sa.st_ino == sb.st_ino) or (sa.st_size == sb.st_size and sa.st_mtime <= sb.st_mtime)


def place(src, dst, link=False):
    if not os.path.exists(src):
        sys.exit(f'missing: {src}')
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if same(src, dst):
        return 'kept'
    if os.path.exists(dst):
        os.remove(dst)
    if link:
        try:
            os.link(src, dst)
            return 'linked'
        except OSError:
            pass
    shutil.copy2(src, dst)
    return 'copied'


def angle_folder():
    dirs = []
    for base in (os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'),
                 os.environ.get('ProgramFiles', r'C:\Program Files')):
        dirs += [d for d in glob.glob(os.path.join(base, 'Microsoft', 'Edge', 'Application', '*'))
                 if os.path.exists(os.path.join(d, 'libGLESv2.dll'))]
    return max(dirs, key=lambda d: [int(p) if p.isdigit() else 0 for p in os.path.basename(d).split('.')],
               default=None)


def clean_old_layout(data, keep_obbs):
    """What earlier packages put in nova3_data and this one does not."""
    for old in ('obb', 'userdata'):
        if os.path.isdir(os.path.join(data, old)):
            shutil.rmtree(os.path.join(data, old))
            print(f'removed {old}')
    for name in os.listdir(data) if os.path.isdir(data) else []:
        if name.endswith('.apk') or (name.endswith('.obb') and not keep_obbs) or \
                (keep_obbs and name.endswith(('.gla', '.json', '.dat', '.bar'))):
            os.remove(os.path.join(data, name))
            print(f'removed {name}')


def extract_obbs(obbs, folder):
    """The OBBs' entries as files in `folder` (patch entries override main ones)."""
    os.makedirs(folder, exist_ok=True)
    entries = obbfile.merged(obbs)
    for name in sorted(entries):
        archive, entry = entries[name]
        dst = os.path.join(folder, entry[0])
        if os.path.exists(dst) and os.path.getsize(dst) == entry[2] and \
                os.path.getmtime(dst) >= os.path.getmtime(archive.path):
            print(f'kept    {os.path.relpath(dst, os.path.dirname(folder))}')
            continue
        with open(dst, 'wb') as f:
            f.write(archive.read(entry))
        print(f'extract {os.path.relpath(dst, os.path.dirname(folder))}')


def main(dest, keep_obbs):
    dest = os.path.abspath(dest)
    data = os.path.join(dest, 'nova3_data')
    files = [
        (os.path.join(BUILD, 'nova3.exe'), os.path.join(dest, 'nova3.exe'), False),
        (os.path.join(BUILD, 'SDL3.dll'), os.path.join(dest, 'SDL3.dll'), False),
        (os.path.join(ROOT, 'game', 'libGloftNAHM.so'), os.path.join(data, 'libGloftNAHM.so'), True),
    ]
    obbs = sorted(glob.glob(os.path.join(ROOT, PACKAGE, '*.obb')), key=lambda p: not os.path.basename(p).startswith('main'))
    if not obbs:
        sys.exit(f'missing: the OBB files in {os.path.join(ROOT, PACKAGE)}')
    clean_old_layout(data, keep_obbs)
    if keep_obbs:
        files += [(o, os.path.join(data, os.path.basename(o)), True) for o in obbs]
    angle = angle_folder()
    if angle:
        for dll in ('libEGL.dll', 'libGLESv2.dll', 'd3dcompiler_47.dll'):
            if os.path.exists(os.path.join(angle, dll)):
                files.append((os.path.join(angle, dll), os.path.join(dest, dll), False))
    elif not os.path.exists(os.path.join(dest, 'libGLESv2.dll')):
        print('warning: no ANGLE found (Microsoft Edge); nova3.exe will look for it at start')
    for src, dst, link in files:
        print(f'{place(src, dst, link):7} {os.path.relpath(dst, dest)}')
    if not keep_obbs:
        extract_obbs(obbs, data)
    print(f'done: {dest}')


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if a != '--obb']
    main(args[0] if args else os.path.join(ROOT, 'dist'), '--obb' in sys.argv[1:])
