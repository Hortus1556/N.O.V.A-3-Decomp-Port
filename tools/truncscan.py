"""Find integer accumulators fed by a truncated float product (high frame rate time loss).

Pattern inside one function: a float is converted to int (vcvt.s32.f32), moved to a core
register, added to (or subtracted from) a value loaded from [base, #k], and the result is
stored back to [base, #k]. At 4 ms frames, `counter += int(dt * rate)` rounds every
step down and can lose most of the time, or all of it.

Usage: truncscan.py            list candidates (function, address, field offset)
"""
import re
import sys
sys.path.insert(0, 'E:/nova3/tools')
from elf import load
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM

e = load()
md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
md.skipdata = True
text_addr, text = e.sec('.text')
funcs = sorted({(v & ~1, s, n) for n, v, s, t, b, shn in e.symbols() if shn and n and t == 2 and s > 0})

ldr_re = re.compile(r'(\w+), \[(\w+), #(0x[0-9a-f]+|\d+)\]$')
hits = []
for addr, size, name in funcs:
    off = addr - text_addr
    if off < 0 or off + size > len(text):
        continue
    insns = list(md.disasm(text[off:off + size], addr))
    loads = {}  # reg -> (base, k) of the last ldr into it
    for i, ins in enumerate(insns):
        if ins.mnemonic == 'ldr':
            m = ldr_re.match(ins.op_str)
            if m:
                loads[m.group(1)] = (m.group(2), m.group(3))
        if ins.mnemonic != 'vcvt.s32.f32':
            continue
        sreg = ins.op_str.split(',')[0].strip()
        core = None
        for j in insns[i + 1:i + 6]:
            if j.mnemonic == 'vmov' and j.op_str.endswith(', ' + sreg):
                core = j.op_str.split(',')[0].strip()
                break
        if not core:
            continue
        for j, ins2 in enumerate(insns[i + 1:i + 12], i + 1):
            if ins2.mnemonic in ('add', 'sub', 'rsb') and core in ins2.op_str.split(', ')[1:]:
                ops = ins2.op_str.split(', ')
                dst, srcs = ops[0], ops[1:]
                other = [r for r in srcs if r != core]
                if not other or other[0] not in loads:
                    continue
                base, k = loads[other[0]]
                for ins3 in insns[j + 1:j + 6]:
                    if ins3.mnemonic == 'str' and ins3.op_str == f'{dst}, [{base}, #{k}]':
                        hits.append((name, ins.address, k))
                        break
                break

# Second form: an int field loaded into a float register, a product added, truncated and
# stored back: field = int(float(field) + dt * rate).
vldr_re = re.compile(r'(s\d+), \[(\w+), #(0x[0-9a-f]+|\d+)\]$')
for addr, size, name in funcs:
    off = addr - text_addr
    if off < 0 or off + size > len(text):
        continue
    insns = list(md.disasm(text[off:off + size], addr))
    for i, ins in enumerate(insns):
        if ins.mnemonic != 'vcvt.s32.f32':
            continue
        sreg = ins.op_str.split(',')[0].strip()
        store = next((j for j in insns[i + 1:i + 4] if j.mnemonic == 'vstr' and j.op_str.startswith(sreg + ',')), None)
        if not store:
            continue
        m = vldr_re.match(store.op_str)
        if not m:
            continue
        window = insns[max(0, i - 12):i]
        loaded = any(w.mnemonic == 'vldr' and w.op_str.endswith(f'[{m.group(2)}, #{m.group(3)}]') for w in window)
        product = any(w.mnemonic in ('vmul.f32', 'vmla.f32', 'vfma.f32') for w in window)
        summed = any(w.mnemonic in ('vadd.f32', 'vsub.f32', 'vmla.f32') for w in window)
        if loaded and product and summed:
            hits.append((name, ins.address, m.group(3)))

for name, a, k in hits:
    print(f'{a:#x}  +{k:<6} {name}')
print(len(hits), 'candidates', file=sys.stderr)
