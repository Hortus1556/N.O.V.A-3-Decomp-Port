"""Resolve PIC string references (ldr rY,[pc,#k] ... add rX,pc,rY) and search them.

Usage: xrefs.py build                 writes build/analysis/strrefs.tsv (func, insn, string)
       xrefs.py grep <regex>          show functions referencing matching strings
"""
import sys, re, os, bisect
sys.path.insert(0, 'E:/nova3/tools')
from elf import load
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM

OUT = 'E:/nova3/build/analysis/strrefs.tsv'


def build():
    e = load()
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    md.skipdata = True  # literal pools are interleaved with code
    text_addr, text = e.sec('.text')
    ro_addr, ro = e.sec('.rodata')
    syms = {}
    for n, v, s, t, b, shn in e.symbols():
        if shn and n and t == 2: syms.setdefault(v & ~1, n)
    addrs = sorted(syms)
    pending = {}  # reg -> literal value
    rows = []
    rx_ldr = re.compile(r'(\w+), \[pc, #(-?0x[0-9a-f]+|-?\d+)\]$')
    rx_add = re.compile(r'(\w+), pc, (\w+)$')
    for addr, size, mn, ops in md.disasm_lite(text, text_addr):
        if mn == 'ldr' and (m := rx_ldr.match(ops)):
            lit = addr + 8 + int(m.group(2), 0)
            if text_addr <= lit < text_addr + len(text):
                pending[m.group(1)] = e.u32(lit)
            continue
        if mn == 'add' and (m := rx_add.match(ops)) and m.group(2) in pending:
            tgt = (addr + 8 + pending[m.group(2)]) & 0xffffffff
            if ro_addr <= tgt < ro_addr + len(ro):
                o = tgt - ro_addr
                end = ro.find(b'\0', o)
                s = ro[o:end]
                if 2 <= len(s) < 400 and all(32 <= c < 127 or c in (9, 10, 13) for c in s):
                    i = bisect.bisect_right(addrs, addr) - 1
                    rows.append((syms[addrs[i]] if i >= 0 else '?', addr, s.decode().encode('unicode_escape').decode()))
        if mn in ('bl', 'blx', 'bx', 'pop', 'b'):
            pending.clear()
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, 'w', encoding='utf-8') as f:
        for fn, a, s in rows: f.write(f'{fn}\t{a:08x}\t{s}\n')
    print(len(rows), 'string refs ->', OUT)


def grep(pat):
    rx = re.compile(pat, re.I)
    for line in open(OUT, encoding='utf-8'):
        fn, a, s = line.rstrip('\n').split('\t', 2)
        if rx.search(s): print(f'{a}  {fn[:90]:90}  "{s[:100]}"')


if __name__ == '__main__':
    build() if sys.argv[1] == 'build' else grep(sys.argv[2])
