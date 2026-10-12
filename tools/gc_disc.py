"""Build a GameCube disc image (GCM / .iso) for the port (docs/gamecube.md, "Storage").

Layout: the disc header (boot.bin, 0x440 bytes: game id, name, magic, the DOL's and the file
table's offsets), bi2.bin (0x2000 bytes: memory size, region), the apploader at 0x2440, the file
table (FST), the DOL, then the files, each at a 32 KB boundary (the drive reads in 32 KB ECC
blocks). The runtime reads the files through runtime/gc_dvdfs.c ("dvd:").

  python tools/gc_disc.py out.iso --dol build/gc/out/nova3.dol --file gcode.bin=build/gc/out/gcode.bin \\
      --dir nova3_data=build/3ds/slim_data [--apploader apploader.img]

FST entries (big-endian, 12 bytes): directory flag << 24 | name offset, then a file's offset and
size, or a directory's parent and the index past its last descendant; entry 0 (the root) holds
the entry count. Names follow the entries.
"""
import argparse, os, struct

ALIGN = 32 * 1024


def tree(args):
    """root: {name: path or subtree}"""
    root = {}
    for spec in args.file or []:
        name, path = spec.split('=', 1)
        root[name] = path
    for spec in args.dir or []:
        name, path = spec.split('=', 1)
        node = root.setdefault(name, {})

        def walk(d, into):
            for n in sorted(os.listdir(d)):
                p = os.path.join(d, n)
                if os.path.isdir(p):
                    walk(p, into.setdefault(n, {}))
                elif not (args.skip_suffix and any(n.endswith(s) for s in args.skip_suffix)):
                    into[n] = p
        walk(path, node)
    return root


def flatten(root):
    """FST order (depth first) as (name, path or None, parent index, subtree)"""
    out = [('', None, 0, root)]

    def add(node, parent):
        for name in sorted(node, key=lambda n: (isinstance(node[n], dict), n.lower())):
            v = node[name]
            i = len(out)
            out.append((name, v if not isinstance(v, dict) else None, parent, v if isinstance(v, dict) else None))
            if isinstance(v, dict):
                add(v, i)
                out[i] = out[i] + (len(out),)
    add(root, 0)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--dol', required=True)
    ap.add_argument('--file', action='append', help='NAME=PATH at the root')
    ap.add_argument('--dir', action='append', help='NAME=DIR: a directory and its contents')
    ap.add_argument('--skip-suffix', action='append', help='leave out files ending so (e.g. .z)')
    ap.add_argument('--apploader', help='apploader image (header + code); default: an empty one')
    ap.add_argument('--id', default='GNVA01')
    ap.add_argument('--name', default='N.O.V.A. 3 (GameCube port)')
    a = ap.parse_args()

    entries = flatten(tree(a))
    names = bytearray(b'\0')
    name_off = [0]
    for e in entries[1:]:
        name_off.append(len(names))
        names += e[0].encode('utf-8') + b'\0'
    fst_size = 12 * len(entries) + len(names)

    apploader = open(a.apploader, 'rb').read() if a.apploader else \
        b'2026/10/12'.ljust(16, b'\0') + struct.pack('>III', 0, 0, 0) + bytes(4)
    fst_off = (0x2440 + len(apploader) + 31) & ~31
    dol = open(a.dol, 'rb').read()
    dol_off = (fst_off + fst_size + ALIGN - 1) // ALIGN * ALIGN
    pos = (dol_off + len(dol) + ALIGN - 1) // ALIGN * ALIGN
    placed = {}
    for i, e in enumerate(entries):
        if e[1] is not None:
            placed[i] = pos
            pos = (pos + os.path.getsize(e[1]) + ALIGN - 1) // ALIGN * ALIGN
    total = pos

    fst = bytearray()
    for i, e in enumerate(entries):
        if i == 0:
            fst += struct.pack('>III', 1 << 24, 0, len(entries))
        elif e[1] is None:
            fst += struct.pack('>III', 1 << 24 | name_off[i], e[2], e[4])
        else:
            fst += struct.pack('>III', name_off[i], placed[i], os.path.getsize(e[1]))
    fst += names

    boot = bytearray(0x440)
    boot[0:6] = a.id.encode('ascii')
    struct.pack_into('>I', boot, 0x1c, 0xC2339F3D)
    nm = a.name.encode('ascii')[:0x3df]
    boot[0x20:0x20 + len(nm)] = nm
    struct.pack_into('>IIII', boot, 0x420, dol_off, fst_off, fst_size, fst_size)
    bi2 = bytearray(0x2000)
    struct.pack_into('>II', bi2, 0, 0, 0x01800000)  # debug monitor size, simulated memory size
    struct.pack_into('>I', bi2, 0x18, 1)               # region: NTSC-U

    with open(a.out, 'wb') as out:
        out.write(boot)
        out.write(bi2)
        out.write(apploader)
        out.seek(fst_off)
        out.write(fst)
        out.seek(dol_off)
        out.write(dol)
        for i, off in sorted(placed.items(), key=lambda kv: kv[1]):
            out.seek(off)
            with open(entries[i][1], 'rb') as f:
                while chunk := f.read(8 << 20):
                    out.write(chunk)
        out.truncate(total)
    print(f'{a.out}: {total / 2**20:.1f} MB, {len(placed)} files, {len(entries) - 1 - len(placed)} directories'
          + (' (over a GameCube disc\'s 1.46 GB)' if total > 1459978240 else ''))


if __name__ == '__main__':
    main()
