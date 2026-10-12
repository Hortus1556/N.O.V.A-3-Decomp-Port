"""Census of reachable instructions an ARMv6K + VFPv2 CPU (the 3DS's ARM11) cannot execute.

Walks every recompiled ARM function's reachable instructions (the recompiler's own discovery)
and classifies the ones outside ARMv6K / VFPv2: ARMv7 integer instructions, VFPv3 forms
(immediate vmov, fixed-point vcvt, d16-d31), NEON, and coprocessor accesses (TLS reads).

    python tools/v6census.py [--list CLASS]
"""
import collections, re, sys
sys.path.insert(0, 'E:/nova3/recomp')
import arm2c
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM

V7_INT = {'movw', 'movt', 'ubfx', 'sbfx', 'bfi', 'bfc', 'mls', 'rbit', 'dmb', 'dsb', 'isb',
          'sdiv', 'udiv', 'pli', 'dbg', 'smc', 'ldrht', 'ldrsht', 'ldrsbt', 'strht'}
NEON_TYPES = re.compile(r'\.(i8|i16|i32|i64|u8|u16|s8|s16|p8|8|16|64)\b')
HIGH_D = re.compile(r'\bd(1[6-9]|2\d|3[01])\b')


def classify(mn, ops):
    base = mn.split('.')[0]
    if base in V7_INT:
        return 'v7:' + base
    if base[-2:] in CONDS and base[:-2] in V7_INT:
        return 'v7:' + base[:-2]
    if not mn.startswith('v'):
        if base.startswith(('mrc', 'mcr')):
            return 'cp:' + mn + ' ' + ops
        return None
    if re.search(r'\bq\d', ops) or base.startswith(('vld', 'vst')) and '{' in ops and not base.startswith(('vldm', 'vstm', 'vldr', 'vstr')) \
            or NEON_TYPES.search(mn) and not base.startswith(('vcvt', 'vmov', 'vldr', 'vstr')):
        return 'neon'
    if HIGH_D.search(ops):
        return 'vfp3:d16-31'
    if base.startswith('vmov') and '#' in ops:
        return 'vfp3:vmov-imm'
    if base.startswith('vcvt') and ops.count(',') >= 2 and '#' in ops:
        return 'vfp3:vcvt-fixed'
    if base.startswith('vmov') and re.search(r'\.(8|16|32|s8|s16|u8|u16)\b', mn) and '[' in ops:
        return 'neon'  # vmov.32 d0[1], r0 (scalar lane) is VFPv2 only for .32
    return None


CONDS = {'eq', 'ne', 'cs', 'hs', 'cc', 'lo', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le', 'al'}


def main():
    g = arm2c.Generator()
    g.plan()
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    by_class = collections.Counter()
    funcs = collections.defaultdict(collections.Counter)
    sample = collections.defaultdict(list)
    total = 0
    for f in g.starts:
        fg = arm2c.FuncGen(g, f, g.ranges[f])
        fg.explore()
        for a, w in fg.reach.items():
            total += 1
            ins = next(md.disasm_lite(w.to_bytes(4, 'little'), a), None)
            if ins is None:
                continue
            c = classify(ins[2], ins[3])
            if c:
                by_class[c.split(' ')[0] if c.startswith('cp:') else c] += 1
                funcs[c.split(' ')[0] if c.startswith('cp:') else c][g.img.syms.get(f, hex(f))] += 1
                if len(sample[c]) < 3:
                    sample[c].append(f'{a:08x} {ins[2]} {ins[3]}')
    print(f'{total} reachable ARM instructions in {len(g.starts)} functions')
    for c, n in by_class.most_common():
        print(f'{c:20} {n:7}  in {len(funcs[c])} functions; e.g. {funcs[c].most_common(2)}')
    print('--- samples')
    for c, s in sorted(sample.items()):
        print(c, '|', ' ; '.join(s))
    if '--list' in sys.argv:
        want = sys.argv[sys.argv.index('--list') + 1]
        for fn, n in funcs[want].most_common():
            print(f'{n:5} {fn}')


if __name__ == '__main__':
    main()
