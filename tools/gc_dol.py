"""Split the GameCube port's ELF into the DOL the console boots and gcode.bin (docs/gamecube.md).

The link (tools/gc/ld/ogc.ld) puts the translated game code in a segment of its own at 0x40000000:
that segment becomes gcode.bin, which the pager maps from the game files page by page; the other
loadable segments (at 0x80003100 and up) become the DOL's text and data sections, with the bss.

  python tools/gc_dol.py build/gc/cmake/nova3.elf build/gc/out     # -> nova3.dol, gcode.bin
"""
import os, struct, sys

GCODE_BASE = 0x40000000
PT_LOAD = 1
PF_X = 1


def segments(elf):
    if elf[:4] != b'\x7fELF' or elf[4] != 1 or elf[5] != 2:
        raise SystemExit('not a 32-bit big-endian ELF')
    phoff, = struct.unpack_from('>I', elf, 0x1c)
    phentsize, phnum = struct.unpack_from('>HH', elf, 0x2a)
    entry, = struct.unpack_from('>I', elf, 0x18)
    out = []
    for i in range(phnum):
        typ, off, vaddr, _, filesz, memsz, flags, _ = struct.unpack_from('>8I', elf, phoff + i * phentsize)
        if typ == PT_LOAD and memsz:
            out.append((vaddr, elf[off:off + filesz], memsz, flags))
    return entry, out


def main():
    elf_path, out_dir = sys.argv[1], sys.argv[2]
    elf = open(elf_path, 'rb').read()
    entry, segs = segments(elf)
    os.makedirs(out_dir, exist_ok=True)
    texts, datas, bss_lo, bss_hi = [], [], None, None
    for vaddr, data, memsz, flags in sorted(segs):
        if vaddr == GCODE_BASE:
            # the paged code: whole pages (the pager reads 4 KB at a time)
            data = data + bytes(-len(data) % 4096)
            open(os.path.join(out_dir, 'gcode.bin'), 'wb').write(data)
            print(f'gcode.bin: {len(data) >> 10} KB at {vaddr:08x}')
            continue
        if data:
            (texts if flags & PF_X else datas).append((vaddr, data))
        if memsz > len(data):
            lo, hi = vaddr + len(data), vaddr + memsz
            bss_lo = lo if bss_lo is None else min(bss_lo, lo)
            bss_hi = hi if bss_hi is None else max(bss_hi, hi)
    if len(texts) > 7 or len(datas) > 11:
        raise SystemExit(f'too many sections for a DOL: {len(texts)} text, {len(datas)} data')
    header = bytearray(0x100)
    pos = 0x100
    body = bytearray()
    for kind, items, base in (('text', texts, 0), ('data', datas, 7)):
        for i, (vaddr, data) in enumerate(items):
            data = data + bytes(-len(data) % 32)
            struct.pack_into('>I', header, 4 * (base + i), pos)          # file offset
            struct.pack_into('>I', header, 0x48 + 4 * (base + i), vaddr)  # load address
            struct.pack_into('>I', header, 0x90 + 4 * (base + i), len(data))
            body += data
            pos += len(data)
    if bss_lo is not None:
        struct.pack_into('>II', header, 0xd8, bss_lo, bss_hi - bss_lo)
    struct.pack_into('>I', header, 0xe0, entry)
    open(os.path.join(out_dir, 'nova3.dol'), 'wb').write(header + body)
    print(f'nova3.dol: {(0x100 + len(body)) >> 10} KB ({len(texts)} text, {len(datas)} data sections, '
          f'bss {((bss_hi or 0) - (bss_lo or 0)) >> 10} KB), entry {entry:08x}')


if __name__ == '__main__':
    main()
