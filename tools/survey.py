"""Survey code: function boundaries, ARM/Thumb split, exidx coverage."""
import sys, struct, collections
sys.path.insert(0, 'E:/nova3/tools')
from elf import load

e = load()
text_addr, text = e.sec('.text')
print('segments:'); [print('  type=%d off=%x va=%x filesz=%x memsz=%x flags=%d' % (p[0], p[1], p[2], p[4], p[5], p[6])) for p in e.phdrs]
funcs = [(n, v, s) for n, v, s, t, b, shn in e.symbols() if t == 2 and shn]
thumb = sum(v & 1 for _, v, _ in funcs)
print('FUNC symbols', len(funcs), 'thumb', thumb, 'arm', len(funcs) - thumb, 'bytes', sum(s for *_, s in funcs))
# exidx: pairs (prel31 fn, data)
ex_addr, ex = e.sec('.ARM.exidx')
starts = []
for i in range(len(ex) // 8):
    w0, w1 = struct.unpack_from('<II', ex, i * 8)
    off = w0 & 0x7fffffff
    if off & 0x40000000: off -= 0x80000000
    starts.append((ex_addr + i * 8 + off) & 0xffffffff)
print('exidx entries', len(starts))
sym_starts = {v & ~1 for _, v, _ in funcs}
ex_set = set(starts)
print('exidx starts w/o symbol', len(ex_set - sym_starts), ' symbols w/o exidx', len(sym_starts - ex_set))
# coverage of .text by symbol ranges
cov = sorted(((v & ~1), s) for _, v, s in funcs)
covered = 0; last = 0
for a, s in cov:
    a2 = max(a, last); end = a + s
    if end > a2: covered += end - a2; last = end
print('.text %x..%x size %d, covered by sized symbols %.1f%%' % (text_addr, text_addr + len(text), len(text), 100 * covered / len(text)))
print('comment:', e.sec('.comment')[1].replace(b'\0', b' | '))
