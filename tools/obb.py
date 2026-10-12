"""Gameloft OBB (CCustomPakReader) archive tool.

Format (reversed from CCustomPakReader::scanLocalHeader / openFile), big-endian u32:
  header : file_count, data_start, table_end, entry_count
  table  : entry_count x (offset, size, name_offset, flags)
  names  : [table_end, data_start) NUL-separated names, each encrypted with
           DECODE_XOR32(seed=0x3857A); name_offset indexes into this blob
  data   : raw files; flags bit0 => whole file encrypted with DECODE_XOR32(seed=0x3857A)
Many entries are themselves standard .zip archives that the game mounts.

Usage: obb.py list <obb>...              list entries (later OBBs override earlier)
       obb.py extract <outdir> <obb>...  write decrypted entries to outdir
"""
import os, struct, sys

SEED = 0x3857A


class Rand:
    """getRand(int): LCG used by DECODE_XOR32 (state is a global reseeded per call)."""
    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFF

    def __call__(self, n):
        self.s = (self.s * 0x19660D + 0x3C6EF35F) & 0xFFFFFFFF
        return self.s >> 16 if n == 0 else (self.s >> 16) % n


def _rot(v, amt, bits, right):
    """Rotate within `bits` (amt may be 0 or bits; result masked to width)."""
    mask = (1 << bits) - 1
    if right:
        return ((v >> amt) | (v << (bits - amt))) & mask
    return ((v << amt) | (v >> (bits - amt))) & mask


def decode_xor32(buf, seed=SEED):
    """Port of DECODE_XOR32(src, len, dst, seed)."""
    r = Rand(seed)
    out = bytearray(buf)
    n = len(buf)
    words = (n - 4) // 4 + 1 if n > 3 else 0
    for i in range(words):
        v = int.from_bytes(buf[i * 4:i * 4 + 4], 'little')
        right = r(2) != 0
        amt = r(32)
        # ror by amt when right, else ror by 32-amt (== rol amt)
        v = _rot(v, amt, 32, True) if right else _rot(v, amt, 32, False)
        k = r(256) << 24
        k |= r(256) << 16
        k |= r(256) << 8
        k |= r(256)
        out[i * 4:i * 4 + 4] = (v ^ k).to_bytes(4, 'little')
    tail = n - words * 4
    right = r(2) != 0
    if tail:
        bits = tail * 8
        o = words * 4
        v = int.from_bytes(buf[o:n], 'little')
        amt = r(bits)
        # dir!=0: (v << (bits-amt)) | (v >> amt) == ror amt ; else rol amt
        v = _rot(v, amt, bits, True) if right else _rot(v, amt, bits, False)
        k = 0
        for _ in range(tail):
            k = (k << 8) | r(256)
        out[o:n] = (v ^ k).to_bytes(tail, 'little')
    return bytes(out)


def encode_xor32(buf, seed=SEED):
    """The inverse of decode_xor32 (the key stream depends on the length only)."""
    r = Rand(seed)
    out = bytearray(buf)
    n = len(buf)
    words = (n - 4) // 4 + 1 if n > 3 else 0
    for i in range(words):
        right = r(2) != 0
        amt = r(32)
        k = 0
        for _ in range(4):
            k = (k << 8) | r(256)
        v = int.from_bytes(buf[i * 4:i * 4 + 4], 'little') ^ k
        out[i * 4:i * 4 + 4] = _rot(v, amt, 32, not right).to_bytes(4, 'little')
    tail = n - words * 4
    right = r(2) != 0
    if tail:
        bits = tail * 8
        o = words * 4
        amt = r(bits)
        k = 0
        for _ in range(tail):
            k = (k << 8) | r(256)
        v = int.from_bytes(buf[o:n], 'little') ^ k
        out[o:n] = _rot(v, amt, bits, not right).to_bytes(tail, 'little')
    return bytes(out)


class Obb:
    def __init__(self, path):
        self.path = path
        self.f = open(path, 'rb')
        self.file_count, data_start, table_end, count = struct.unpack('>4I', self.f.read(16))
        table = [struct.unpack('>4I', self.f.read(16)) for _ in range(count)]
        names = self.f.read(data_start - table_end)
        offs = [t[2] for t in table] + [len(names)]
        self.entries = []  # (name, offset, size, flags)
        for i, (off, size, noff, flags) in enumerate(table):
            name = decode_xor32(names[noff:offs[i + 1] - 1]).decode('latin1')
            self.entries.append((name, off, size, flags))

    def read(self, entry):
        name, off, size, flags = entry
        self.f.seek(off)
        data = self.f.read(size)
        return decode_xor32(data) if flags & 1 else data


def merged(paths):
    """name -> (Obb, entry); later archives (patch) override earlier ones (main)."""
    out = {}
    for p in paths:
        o = Obb(p)
        for e in o.entries:
            out[e[0].lower()] = (o, e)
    return out


def main(argv):
    if argv[0] == 'list':
        for name, (o, (n, off, size, flags)) in sorted(merged(argv[1:]).items()):
            print(f'{os.path.basename(o.path)[:5]}  {size:>11,}  {"enc" if flags & 1 else "   "}  {n}')
    elif argv[0] == 'extract':
        out = argv[1]
        for name, (o, e) in merged(argv[2:]).items():
            dst = os.path.join(out, e[0])
            os.makedirs(os.path.dirname(dst) or out, exist_ok=True)
            with open(dst, 'wb') as f:
                f.write(o.read(e))
        print('extracted to', out)
    else:
        print(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
