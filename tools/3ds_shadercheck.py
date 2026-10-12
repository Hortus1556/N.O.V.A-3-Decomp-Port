"""Checks the PICA200 vertex shaders' binary (build/3ds/cmake/pica.shbin, from runtime/pica/*.v.pica)
against the GPU's rules that Azahar does not enforce (3dbrew GPU/Pitfalls: "Each configured output
component has to be written exactly once or the PICA freezes"; "Having 2 consecutive MOVA
instructions will freeze the PICA").

Usage: 3ds_shadercheck.py [path to the .shbin]

For each entry point (DVLE): its output table, the output map libctru makes of it
(DVLE_GenerateOutmap: the components each output register gives the rasterizer), and every write
to an output register between its start and its end, by component. An error: a component of a
declared output register written other than once (all four, also those the map leaves unused, 0x1F:
the strict reading of the rule, and what the console-tested shaders do, citro2d's and hbmenu's
`mov outtc0, intc0`; the 2026-10-12 console builds wrote texture coordinates' .xy alone and the
GPU never finished their first draw), a write inside a conditional block or loop (it may run zero
or many times), or two MOVAs in a row.
"""
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TYPES = {0: ('position', 0x00, 4), 1: ('normalquat', 0x04, 4), 2: ('color', 0x08, 4), 3: ('texcoord0', 0x0C, 2),
         4: ('texcoord0w', 0x10, 1), 5: ('texcoord1', 0x0E, 2), 6: ('texcoord2', 0x16, 2), 8: ('view', 0x12, 3)}
COMP = 'xyzw'


def words(data, off, n):
    return struct.unpack_from(f'<{n}I', data, off)


def outmap(entries):
    """libctru's DVLE_GenerateOutmap: per output register, component -> semantic (0x1F unused)."""
    regs = {}
    for typ, reg, mask in entries:
        if typ not in TYPES:
            continue
        _, sem, num = TYPES[typ]
        m = regs.setdefault(reg, [0x1F] * 4)
        for j in range(4):
            if num and mask >> j & 1:
                m[j] = sem
                sem += 1
                num -= 1
    return regs


def decode(word):
    """(opcode, dst register or None, opdesc index or None, ends a block at ...): the fields that matter here."""
    op = word >> 26
    if op < 0x20 and op not in (0x10, 0x11, 0x14, 0x15, 0x16, 0x17, 0x1C, 0x1D, 0x1E, 0x1F) or 0x18 <= op <= 0x1B:
        return op, word >> 21 & 0x1F, word & 0x7F
    if op >= 0x30:  # MAD / MADI
        return op, word >> 24 & 0x1F, word & 0x1F
    return op, None, None


def main():
    path = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'build' / '3ds' / 'cmake' / 'pica.shbin'
    data = path.read_bytes()
    magic, n = struct.unpack_from('<4sI', data, 0)
    assert magic == b'DVLB', 'not a DVLB'
    dvle_offs = words(data, 8, n)
    dvlp = 8 + 4 * n
    assert data[dvlp:dvlp + 4] == b'DVLP'
    p = words(data, dvlp, 6)
    code = words(data, dvlp + p[2], p[3])
    opdesc = [words(data, dvlp + p[4] + 8 * i, 1)[0] for i in range(p[5])]
    errors = 0
    for e in dvle_offs:
        d = words(data, e, 14)
        assert data[e:e + 4] == b'DVLE'
        main_off, end_off = d[2], d[3]
        out_off, out_n = d[10], d[11]
        entries = []
        for i in range(out_n):
            typ, reg, mask = struct.unpack_from('<HHB', data, e + out_off + 8 * i)
            entries.append((typ, reg, mask))
        regs = outmap(entries)
        writes = {}  # (reg, comp) -> count
        conditional = []
        depth_end = []  # ends of the conditional blocks / loops we are in
        prev_mova = False
        for pc in range(main_off, min(end_off + 1, len(code))):
            while depth_end and pc >= depth_end[-1]:
                depth_end.pop()
            w = code[pc]
            op, dst, desc = decode(w)
            if op == 0x22:  # END
                break
            if op == 0x12 and prev_mova:
                print(f'  error: two MOVAs in a row at {pc}')
                errors += 1
            prev_mova = op == 0x12
            if op in (0x27, 0x28):  # IFU / IFC: the else part ends at the given offset
                dst_off, num = w >> 10 & 0xFFF, w & 0xFF
                depth_end.append(max(dst_off + num, pc + 1))
            elif op == 0x29:  # LOOP
                depth_end.append((w >> 10 & 0xFFF) + 1)
            elif op in (0x24, 0x25, 0x26, 0x2C, 0x2D):
                print(f'  note: call / jump at {pc} (not followed)')
            if dst is not None and dst < 0x10:
                mask = opdesc[desc] & 0xF  # bit 3 = x .. bit 0 = w
                for j in range(4):
                    if mask >> (3 - j) & 1:
                        writes[(dst, j)] = writes.get((dst, j), 0) + 1
                        if depth_end:
                            conditional.append((pc, dst, j))
        name = f'entry at {main_off}-{end_off}'
        print(f'{name}: outputs ' + ', '.join(f'o{reg} {TYPES.get(t, ("?",))[0]} mask {m:x}' for t, reg, m in entries))
        for reg in sorted(set(regs) | {r for r, _ in writes}):
            m = regs.get(reg, [0x1F] * 4)
            for j in range(4):
                declared, count = reg in regs, writes.get((reg, j), 0)
                if declared and count != 1:
                    print(f'  error: o{reg}.{COMP[j]} (semantic {m[j]:#x}) written {count} times')
                    errors += 1
                elif not declared and count:
                    print(f'  error: o{reg}.{COMP[j]} written {count} times, not a declared output')
                    errors += 1
            print(f'  o{reg}: map ' + ' '.join(f'{s:02x}' for s in m) + ', writes ' +
                  ' '.join(str(writes.get((reg, j), 0)) for j in range(4)))
        for pc, reg, j in conditional:
            print(f'  error: o{reg}.{COMP[j]} written inside a conditional block or loop (at {pc})')
            errors += 1
    print('ok' if not errors else f'{errors} error(s)')
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
