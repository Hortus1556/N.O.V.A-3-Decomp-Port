"""Disassemble functions by (mangled) name substring or address, annotating branch targets.

Usage: disasm.py <name-substring|0xaddr> [max_insns]
       disasm.py --callers <name-substring>     list functions that BL to a symbol
"""
import sys, re, bisect
sys.path.insert(0, 'E:/nova3/tools')
from elf import load
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM

e = load()
md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
md.skipdata = True  # literal pools are interleaved with code
syms = {}
for n, v, s, t, b, shn in e.symbols():
    if shn and n and t in (1, 2):
        syms.setdefault(v & ~1, (n, s))
addrs = sorted(syms)
text_addr, text = e.sec('.text')


def name_at(a):
    i = bisect.bisect_right(addrs, a) - 1
    if i < 0: return hex(a)
    base = addrs[i]; n, s = syms[base]
    return n if a == base else f'{n}+{a - base:#x}'


def func_range(a):
    n, s = syms[a]
    if not s:
        i = bisect.bisect_right(addrs, a)
        s = addrs[i] - a if i < len(addrs) else 0x400
    return a, s


def disasm(a, size, limit=4000):
    for k, ins in enumerate(md.disasm(e.read(a, size), a)):
        if k >= limit: print('  ...'); break
        note = ''
        m = re.match(r'#(0x[0-9a-f]+)$', ins.op_str)
        if ins.mnemonic.startswith('b') and m:
            note = '  ; ' + name_at(int(m.group(1), 16))
        m = re.search(r'\[pc, #(-?0x[0-9a-f]+|-?\d+)\]', ins.op_str)
        if m and ins.mnemonic.startswith('ldr'):
            lit = ins.address + 8 + int(m.group(1), 0)
            v = e.u32(lit)
            note = f'  ; ={v:#x} {name_at(v) if text_addr <= v < text_addr + len(text) else ""}'
        print(f'  {ins.address:08x}: {ins.mnemonic:8} {ins.op_str}{note}')


def main(argv):
    if argv[0] == '--callers':
        targets = {a for a in addrs if argv[1] in syms[a][0]}
        hits = set()
        for addr, size, mn, ops in md.disasm_lite(text, text_addr):
            if mn.startswith('bl') and ops.startswith('#') and int(ops[1:], 16) in targets:
                hits.add(name_at(addr).split('+')[0])
        print('\n'.join(sorted(hits)))
        return
    limit = int(argv[1]) if len(argv) > 1 else 4000
    if argv[0].startswith('0x'):
        a = int(argv[0], 16); print(name_at(a)); disasm(a, 0x200, limit); return
    for a in addrs:
        if argv[0] in syms[a][0]:
            a, s = func_range(a)
            print(f'{syms[a][0]} @ {a:#x} size {s:#x}')
            disasm(a, s, limit)


if __name__ == '__main__':
    main(sys.argv[1:])
