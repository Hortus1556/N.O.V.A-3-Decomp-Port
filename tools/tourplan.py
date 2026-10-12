"""Section triggers of the campaign levels, for --tour (host/eventlog.cpp).

Usage: tourplan.py [level.lvc]      print "object:handler:section,..." per level

A level's script loads its later sections from trigger volumes as the player walks into them:
e.g. Obj13045_WhileIn does SaveGame(12814) LoadLevelSections(2). This lists, for each level
file in levels.gla, the trigger objects whose handler (OnEnter / WhileIn / OnExit) calls
LoadLevelSections (commented-out calls left out), first per section.
"""
import os
import re
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEVELS = os.path.join(ROOT, 'dist', 'nova3_data', 'levels.gla')


def triggers(script):
    text = re.sub(rb'[^\x20-\x7e\n\t]', b'\n', script).decode()
    found = {}
    # function bodies: from one "function ObjN_Handler(" to the next
    starts = [(m.start(), m.group(1), m.group(2)) for m in re.finditer(r'function Obj(\d+)_(\w+)\s*\(', text)]
    for i, (pos, obj, handler) in enumerate(starts):
        body = text[pos:starts[i + 1][0] if i + 1 < len(starts) else len(text)]
        for line in body.split('\n'):
            code = line.split('--')[0]
            m = re.search(r'LoadLevelSections\s*\(\s*(\d+)', code)  # also 'LoadLevelSections (2)'
            if m and handler in ('OnEnter', 'WhileIn', 'OnExit'):
                section = int(m.group(1))
                found.setdefault(section, (int(obj), handler))
    return [(obj, handler, section) for section, (obj, handler) in sorted(found.items())]


def main(names):
    z = zipfile.ZipFile(LEVELS)
    for name in names or sorted(n for n in z.namelist() if n.endswith('.lvc') and n != 'menus.lvc'):
        plan = triggers(z.read(name))
        print(name, ','.join(f'{o}:{h}:{s}' for o, h, s in plan))


if __name__ == '__main__':
    main(sys.argv[1:])
