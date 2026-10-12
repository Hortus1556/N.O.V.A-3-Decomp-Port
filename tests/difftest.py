"""Differential tester: recompiled instruction semantics vs. Unicorn (reference ARM CPU).

Real instructions are sampled from the game binary. Each one becomes a snippet
`insn; bx lr`, which is
  * translated by recomp/arm2c.py and compiled into tests/difftest.cpp (native), and
  * executed for one instruction in Unicorn,
from the same random initial state (registers, flags, VFP registers, a 4 KiB memory
window). Final states are then compared.

Usage: difftest.py [--thumb] [--per-mnemonic N] [--cases N] [--seed S] [--filter REGEX]
  --thumb  test 16-bit Thumb instructions (snippet `insn; bx lr` in Thumb state)
"""
import argparse, collections, os, random, re, struct, subprocess, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..'))
sys.path.insert(0, os.path.join(ROOT, 'recomp'))
import arm2c  # noqa: E402
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB  # noqa: E402
from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM  # noqa: E402
from unicorn.arm_const import *  # noqa: E402,F403

OUT = os.path.join(ROOT, 'build', 'diff')
GEN = os.path.join(OUT, 'gen')
SNIP_BASE = 0x00100000      # link-time address of snippet i is SNIP_BASE + 16*i
WIN = 0x20000000            # guest address of the memory window
WIN_SIZE = 0x1000
BX_LR = 0xe12fff1e
T_BX_LR = 0x4770
CASE_FMT = '<I16I5I256s'    # snippet, r[16], n z c v fpscr, vfp regs
RES_FMT = '<16I5I256sI'     # r[16], n z c v fpscr, vfp regs, changed-byte count


# ---------------------------------------------------------------------------
# Sampling

def touches_pc(w):
    """Instructions we don't test in isolation: control flow, pc access, stateful ops."""
    cond, op = w >> 28, (w >> 25) & 7
    if cond == 0xf:
        return True
    if op == 5 or w & 0x0ffffff0 in (0x012fff10, 0x012fff30):
        return True
    if op == 4 and w & (1 << 15):
        return True
    if op == 4 and (w >> 16) & 15 == 15:
        return True
    if op in (0, 1, 2, 3) and 15 in ((w >> 16) & 15, (w >> 12) & 15, w & 15, (w >> 8) & 15):
        return True
    if op in (6, 7) and (w >> 16) & 15 == 15 and (w >> 25) & 7 == 6:
        return True
    if w & 0x0f900ff0 == 0x01800f90 or w & 0x0f900ff0 == 0x01900f90:  # ldrex/strex family
        return True
    if op == 7 and (w >> 24) & 1:  # svc
        return True
    return False


def snippet_code(words, thumb):
    """16-byte slot per snippet: the instruction followed by `bx lr`."""
    if thumb:
        return b''.join(struct.pack('<HH12x', w, T_BX_LR) for w in words)
    return b''.join(struct.pack('<4I', w, BX_LR, 0, 0) for w in words)


class SnippetImage:
    """Stand-in for arm2c.Image covering only the synthetic snippet area."""
    def __init__(self, words, thumb):
        self.text = snippet_code(words, thumb)
        self.text_addr = SNIP_BASE
        self.text_end = SNIP_BASE + len(self.text)
        self.syms, self.plt, self.imports = {}, {}, []

    def word(self, a):
        return struct.unpack_from('<I', self.text, a - SNIP_BASE)[0]

    def in_text(self, a):
        return self.text_addr <= a < self.text_end


class SnippetGen:
    """Minimal Generator facade for FuncGen."""
    def __init__(self, img):
        self.img = img
        self.emit_set, self.mid_entries = set(), set()
        self.entries_by_func, self.import_index = {}, {}
        self.thumb_set = set()

    def func_of(self, a):
        return a - (a - SNIP_BASE) % 16


def sample(per_mnemonic, filt):
    g = arm2c.Generator()
    g.plan()
    g.emit_set = set(g.starts)
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    by_mn = collections.defaultdict(set)
    for f in g.starts:
        fg = arm2c.FuncGen(g, f, g.ranges[f])
        fg.explore()
        for a, w in fg.reach.items():
            if touches_pc(w):
                continue
            fg.out = []
            try:
                fg.insn(a, w)
            except Exception:
                continue  # unsupported: nothing to verify
            if any('UNIMPL' in x or 'CALL' in x or arm2c.PC_VAR.search(x) for x in fg.out if isinstance(x, str)):
                continue
            d = list(md.disasm_lite(w.to_bytes(4, 'little'), 0))
            if d and (not filt or re.search(filt, d[0][2])):
                by_mn[d[0][2]].add(w)
    return pick(by_mn, per_mnemonic)


def thumb_testable(h):
    """16-bit Thumb instructions that can run alone (no control flow / pc / traps)."""
    if h >> 11 in (0x1d, 0x1e, 0x1f, 0x09, 0x14, 0x1c) or h >> 12 == 0xd:
        return False  # 32-bit, ldr literal, adr, b, b<cond>/svc/udf
    if h & 0xf500 == 0xb100 or h & 0xff00 in (0xbd00, 0xbe00, 0xbf00):
        return False  # cbz/cbnz, pop {pc}, bkpt, it/hints
    if h >> 10 == 0x11 and (((h >> 4) & 8) | (h & 7) == 15 or (h >> 3) & 15 == 15 or (h >> 8) & 3 == 3):
        return False  # high-register ops on pc, bx/blx
    return True


def sample_thumb(per_mnemonic, filt):
    g = arm2c.Generator()
    g.plan()
    g.emit_set = set(g.starts)
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    by_mn = collections.defaultdict(set)
    for t in g.thumb:
        fg = arm2c.ThumbFuncGen(g, t, g.thumb_ranges[t])
        fg.explore()
        for a in fg.reach:
            h = fg.hw(a)
            if fg.size(a) != 2 or not thumb_testable(h):
                continue
            fg.out = []
            try:
                fg.tinsn(a)
            except Exception:
                continue
            if any('UNIMPL' in x or 'CALL' in x or arm2c.PC_VAR.search(x) for x in fg.out if isinstance(x, str)):
                continue
            d = list(md.disasm_lite(h.to_bytes(2, 'little'), 0))
            if d and (not filt or re.search(filt, d[0][2])):
                by_mn[d[0][2]].add(h)
    return pick(by_mn, per_mnemonic)


def pick(by_mn, per_mnemonic):
    rng = random.Random(1)
    words = []
    for mn in sorted(by_mn):
        ws = sorted(by_mn[mn])
        rng.shuffle(ws)
        words += ws[:per_mnemonic]
    return words


# ---------------------------------------------------------------------------
# Random states

EDGE = [0, 1, 2, 31, 32, 33, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000,
        0x7fffffff, 0x80000000, 0x80000001, 0xfffffffe, 0xffffffff]
F32 = [0.0, -0.0, 1.0, -1.0, 0.5, 3.75, 1e-40, float('inf'), float('-inf'), float('nan'),
       65536.0, -2147483648.0, 2147483647.0, 4294967296.0, 123456.789, -0.3]


def rand_value(rng):
    k = rng.random()
    if k < 0.3:
        return rng.choice(EDGE)
    if k < 0.5:
        return rng.randrange(0, 256)
    return rng.getrandbits(32)


def rand_state(rng, pointer_mode):
    regs = []
    for i in range(15):
        if pointer_mode and (i == 13 or rng.random() < 0.6):
            regs.append(WIN + 0x400 + rng.randrange(0, 0x800 // 8) * 8)
        elif pointer_mode:
            regs.append(rng.randrange(0, 64) * rng.choice((1, 4)))  # small index/offset
        else:
            regs.append(rand_value(rng))
    regs.append(0)
    flags = [rng.getrandbits(1) for _ in range(4)]
    fpscr = rng.getrandbits(4) << 28
    vfp = bytearray()
    for _ in range(32):
        if rng.random() < 0.5:
            vfp += struct.pack('<d', rng.choice(F32 + [rng.uniform(-1e6, 1e6)]))
        else:
            vfp += struct.pack('<ff', rng.choice(F32 + [rng.uniform(-1e3, 1e3)]),
                               rng.choice(F32 + [rng.uniform(-1e3, 1e3)]))
    return regs, flags, fpscr, bytes(vfp)


# ---------------------------------------------------------------------------
# Reference execution

class Ref:
    def __init__(self, words, window, thumb):
        self.uc = uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.thumb = thumb
        uc.mem_map(SNIP_BASE, (16 * len(words) + 0xfff) & ~0xfff)
        uc.mem_write(SNIP_BASE, snippet_code(words, thumb))
        uc.mem_map(WIN, WIN_SIZE)
        self.window = window
        uc.reg_write(UC_ARM_REG_C1_C0_2, uc.reg_read(UC_ARM_REG_C1_C0_2) | (0xf << 20))
        uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        self.regs = [UC_ARM_REG_R0 + i for i in range(13)] + [UC_ARM_REG_SP, UC_ARM_REG_LR]
        self.dregs = [UC_ARM_REG_D0 + i for i in range(32)]

    def run(self, idx, state):
        uc = self.uc
        regs, flags, fpscr, vfp = state
        uc.mem_write(WIN, self.window)
        for r, v in zip(self.regs, regs):
            uc.reg_write(r, v)
        cpsr = uc.reg_read(UC_ARM_REG_CPSR) & 0x0fffffff
        uc.reg_write(UC_ARM_REG_CPSR, cpsr | flags[0] << 31 | flags[1] << 30 | flags[2] << 29 | flags[3] << 28)
        uc.reg_write(UC_ARM_REG_FPSCR, fpscr)
        for i, r in enumerate(self.dregs):
            uc.reg_write(r, struct.unpack_from('<Q', vfp, 8 * i)[0])
        addr = SNIP_BASE + 16 * idx
        try:
            if self.thumb:
                uc.emu_start(addr | 1, addr + 2, count=1)
            else:
                uc.emu_start(addr, addr + 4, count=1)
        except UcError:
            return None
        out_regs = [uc.reg_read(r) for r in self.regs] + [0]
        cpsr = uc.reg_read(UC_ARM_REG_CPSR)
        out_flags = [(cpsr >> s) & 1 for s in (31, 30, 29, 28)]
        out_fpscr = uc.reg_read(UC_ARM_REG_FPSCR) & 0xf0000000
        out_vfp = b''.join(struct.pack('<Q', uc.reg_read(r)) for r in self.dregs)
        mem = bytes(uc.mem_read(WIN, WIN_SIZE))
        changed = {i: mem[i] for i in range(WIN_SIZE) if mem[i] != self.window[i]}
        return out_regs, out_flags, out_fpscr, out_vfp, changed


# ---------------------------------------------------------------------------

def emit_c(words, thumb):
    img = SnippetImage(words, thumb)
    sg = SnippetGen(img)
    os.makedirs(GEN, exist_ok=True)
    with open(os.path.join(GEN, 'snippets.c'), 'w') as f:
        f.write('#include "recomp.h"\n\n')
        for i in range(len(words)):
            a = SNIP_BASE + 16 * i
            fg = (arm2c.ThumbFuncGen if thumb else arm2c.FuncGen)(sg, a, a + (4 if thumb else 8))
            f.write(fg.generate() + '\n\n')
        f.write('const FuncEntry g_funcs[] = {\n')
        for i in range(len(words)):
            a = SNIP_BASE + 16 * i
            if thumb:
                f.write(f'    {{0x{arm2c.LOAD_BASE + a | 1:08x}u, t_{a:08x}, 0x{a:08x}u}},\n')
            else:
                f.write(f'    {{0x{arm2c.LOAD_BASE + a:08x}u, f_{a:08x}, 0x{a:08x}u}},\n')
        f.write('};\nconst uint32_t g_func_count = sizeof(g_funcs) / sizeof(g_funcs[0]);\n')
        f.write(f'const uint32_t g_load_base = 0x{arm2c.LOAD_BASE:08x}u;\n')
        f.write('const char *const g_import_names[] = {""};\nconst uint32_t g_import_count = 0;\n')


def build():
    cmd = [os.path.join(ROOT, 'build.cmd'), os.path.join(OUT, 'cmake'), GEN, '--target', 'difftest']
    r = subprocess.run(cmd, capture_output=True, text=True, shell=True)
    if r.returncode:
        sys.exit(r.stdout[-4000:] + r.stderr[-2000:])


def f_eq(a, b, is_double):
    """Bitwise equal, or both NaN (NaN payload propagation differs between ARM and x86)."""
    if a == b:
        return True
    if is_double:
        x, y = struct.unpack('<d', struct.pack('<Q', a))[0], struct.unpack('<d', struct.pack('<Q', b))[0]
        return x != x and y != y
    x, y = struct.unpack('<f', struct.pack('<I', a))[0], struct.unpack('<f', struct.pack('<I', b))[0]
    return x != x and y != y


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--per-mnemonic', type=int, default=12)
    ap.add_argument('--cases', type=int, default=6)
    ap.add_argument('--seed', type=int, default=7)
    ap.add_argument('--filter')
    ap.add_argument('--thumb', action='store_true')
    args = ap.parse_args()

    words = (sample_thumb if args.thumb else sample)(args.per_mnemonic, args.filter)
    print(f'{len(words)} instruction snippets')
    rng = random.Random(args.seed)
    window = bytes(rng.getrandbits(8) for _ in range(WIN_SIZE))
    ref = Ref(words, window, args.thumb)
    cases, expected = [], []
    for i in range(len(words)):
        for k in range(args.cases):
            st = rand_state(rng, pointer_mode=k % 2 == 0)
            r = ref.run(i, st)
            if r is not None:
                cases.append((i, st))
                expected.append(r)
    print(f'{len(cases)} cases accepted by the reference CPU')

    emit_c(words, args.thumb)
    build()
    case_path, res_path = os.path.join(OUT, 'cases.bin'), os.path.join(OUT, 'results.bin')
    with open(case_path, 'wb') as f:
        f.write(struct.pack('<II', WIN, WIN_SIZE) + window + struct.pack('<I', len(cases)))
        for i, (regs, flags, fpscr, vfp) in cases:
            f.write(struct.pack(CASE_FMT, SNIP_BASE + 16 * i + args.thumb, *regs, *flags, fpscr, vfp))
    exe = os.path.join(OUT, 'cmake', 'difftest.exe')
    r = subprocess.run([exe, case_path, res_path], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'difftest.exe failed: {r.stderr[-2000:]}')

    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB if args.thumb else CS_MODE_ARM)
    fails = collections.defaultdict(list)
    with open(res_path, 'rb') as f:
        for (i, st), (eregs, eflags, efpscr, evfp, echanged) in zip(cases, expected):
            vals = struct.unpack(RES_FMT, f.read(struct.calcsize(RES_FMT)))
            regs, flags, fpscr, vfp, nchanged = vals[:16], vals[16:20], vals[20] & 0xf0000000, vals[21], vals[22]
            changed = dict(struct.unpack('<HB', f.read(3)) for _ in range(nchanged))
            diffs = [f'r{k}={regs[k]:#x} want {eregs[k]:#x}' for k in range(15) if regs[k] != eregs[k]]
            diffs += [f'{"nzcv"[k]}={flags[k]} want {eflags[k]}' for k in range(4) if flags[k] != eflags[k]]
            if fpscr != efpscr:
                diffs.append(f'fpscr={fpscr:#x} want {efpscr:#x}')
            for k in range(32):
                a, b = struct.unpack_from('<Q', vfp, 8 * k)[0], struct.unpack_from('<Q', evfp, 8 * k)[0]
                lo = f_eq(a & 0xffffffff, b & 0xffffffff, False) and f_eq(a >> 32, b >> 32, False)
                if not (lo or f_eq(a, b, True)):
                    diffs.append(f'd{k}={a:#018x} want {b:#018x}')
            if changed != echanged:
                diffs.append(f'memory: {len(changed)} bytes changed, want {len(echanged)}')
            if diffs:
                w = words[i]
                d = list(md.disasm_lite(w.to_bytes(2 if args.thumb else 4, 'little'), SNIP_BASE + 16 * i))
                text = f'{d[0][2]} {d[0][3]}' if d else f'{w:08x}'
                fails[d[0][2] if d else '?'].append((w, text, st, diffs))
    bad = sum(len(v) for v in fails.values())
    print(f'{len(cases) - bad}/{len(cases)} cases match; {len(fails)} mnemonics with mismatches')
    for mn, lst in sorted(fails.items(), key=lambda kv: -len(kv[1])):
        w, text, st, diffs = lst[0]
        print(f'  {len(lst):4}x {text:40} [{w:08x}] {"; ".join(diffs[:4])}')


if __name__ == '__main__':
    main()
