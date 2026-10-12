"""Hook table, trampolines and fixups for the New 3DS's native code loader (runtime/native_3ds.cpp).

The 3DS runs the game's own ARM code (docs/3ds.md). The host functions that replace or wrap game
functions on the PC (OVERRIDES in recomp/arm2c.py) are reached the native way: the hooked
function's first two words become `ldr pc, [pc, #-4]` + the address of a hook gate, and a wrap's
`original` runs a trampoline: the two displaced instructions, then a branch to the function's
third instruction.

    python tools/hooktable.py [out_dir]     (default build/3ds; needs build/3ds/v6patch.bin)

Writes native_table.inc (included by runtime/native_3ds.cpp):
  * the hooks: link address, trampoline (link address, 0 for a replacement), kind, host function;
  * the trampolines' code, assembled by devkitARM at their link address (right after the patch
    set's stubs, inside the loaded image, so their branches reach the game's code): an
    instruction the patch set replaced is emitted as its ARMv6K stub body, a PC-relative literal
    load gets a copy of its literal;
  * the kernel user helper fixups: literal-pool words holding 0xffff0fc0 (__kuser_cmpxchg),
    0xffff0fa0 (__kuser_memory_barrier), 0xffff0fe0 (__kuser_get_tls), 0xffff0f60
    (__kuser_cmpxchg64) or 0xffff0ffc (__kuser_helper_version) that `ldr rX, [pc, #imm]` loads
    before a `blx rX` / `bx rX` (Boost.Thread's Thumb-1 atomics, libgcc's ARM ones): the loader
    writes the address of a native implementation there;
  * the PLT import names (the runtime's static import slots, as the PC build's table.c).

Skipped: the recompiler-only overrides (restore_core_regs: the native unwinder restores its own
registers; host/native_hot.cpp's native versions of hot functions, which are written against the
recompiled code's conventions). A hook is rejected, with the reason printed, if its function is
Thumb, shorter than 8 bytes, has a branch to its second word or a loop back to its first, or
starts with a PC-relative instruction other than a literal load.
"""
import json, os, re, struct, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'recomp'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import arm2c  # noqa: E402
import v6patch  # noqa: E402
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM  # noqa: E402

DKA = v6patch.DKA
KUSER = {0xffff0fc0: 'kuser_cmpxchg', 0xffff0fa0: 'kuser_barrier', 0xffff0fe0: 'kuser_get_tls',
         0xffff0f60: 'kuser_cmpxchg64', 0xffff0ffc: 'kuser_version'}
RECOMPILER_ONLY = {'restore_core_regs'}
COND_NAMES = ['eq', 'ne', 'cs', 'cc', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le', '', '']


def native_hot_functions():
    src = open(os.path.join(ROOT, 'host', 'native_hot.cpp'), encoding='utf-8').read()
    return set(re.findall(r'extern "C" void (\w+)\(', src))


def read_v6patch(path):
    d = open(path, 'rb').read()
    magic, stub_base, stub_size, count = struct.unpack_from('<4sIII', d, 0)
    assert magic == b'N3V6', path
    o = 16 + stub_size
    pairs = dict(struct.unpack_from('<II', d, o + 8 * i) for i in range(count))
    return stub_base, stub_size, pairs


def branch_targets(img):
    """Every ARM B / BL in .text: target -> list of (site, is_link)."""
    out = {}
    text, base = img.text, img.text_addr
    for o in range(0, len(text) - 3, 4):
        w, = struct.unpack_from('<I', text, o)
        if w & 0x0e000000 == 0x0a000000 and w >> 28 != 0xf:
            t = (base + o + 8 + (arm2c.sext(w & 0xffffff, 24) << 2)) & 0xffffffff
            out.setdefault(t, []).append((base + o, bool(w & 0x01000000)))
    return out


def kuser_fixups(img):
    """Literal words holding a kernel helper address, with the instruction that loads each."""
    text, base = img.text, img.text_addr
    words = {}
    for o in range(0, len(text) - 3, 4):
        w, = struct.unpack_from('<I', text, o)
        if w in KUSER:
            words[base + o] = w
    loaders = {}
    for o in range(0, len(text) - 3, 4):  # ARM: ldr rX, [pc, #+-imm12]
        w, = struct.unpack_from('<I', text, o)
        if w & 0x0f7f0000 == 0x051f0000:
            imm = w & 0xfff
            t = base + o + 8 + (imm if w & 0x00800000 else -imm)
            if t in words:
                loaders.setdefault(t, []).append(('arm', base + o))
    for o in range(0, len(text) - 1, 2):  # Thumb: ldr rX, [pc, #imm8 * 4]
        h, = struct.unpack_from('<H', text, o)
        if h & 0xf800 == 0x4800:
            t = ((base + o + 4) & ~3) + (h & 0xff) * 4
            if t in words:
                loaders.setdefault(t, []).append(('thumb', base + o))
    out = []
    for a, w in sorted(words.items()):
        if a not in loaders:
            print(f'kuser: {a:08x} holds {w:08x} but no literal load reads it: left alone')
            continue
        out.append((a, w))
    return out


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build', '3ds')
    stub_base, stub_size, pairs = read_v6patch(os.path.join(out_dir, 'v6patch.bin'))
    tramp_base = (stub_base + stub_size + 0xfff) & ~0xfff
    img = arm2c.Image()
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    md.detail = True
    starts = sorted(set(v & ~1 for v in img.sym_by_name.values() if img.in_text(v & ~1)))
    import bisect
    targets = branch_targets(img)
    skip = RECOMPILER_ONLY | native_hot_functions()

    hooks, rejected, lines = [], [], ['.arch armv6k', '.fpu vfpv2', '.arm', '.text', '.align 2']
    seen = {}
    for sym, how in sorted(arm2c.OVERRIDES.items(), key=lambda kv: kv[0]):
        kind, host = ('wrap', how[1]) if isinstance(how, tuple) else ('replace', how)
        if sym in skip or host in skip:
            continue
        if sym not in img.sym_by_name:
            rejected.append((sym, 'not in the library'))
            continue
        v = img.sym_by_name[sym]
        if v & 1:
            rejected.append((sym, 'Thumb function'))
            continue
        if v in seen:  # an alias of a function already hooked
            if seen[v] != (kind, host):
                rejected.append((sym, f'alias of a function hooked by {seen[v][1]}'))
            continue
        i = bisect.bisect_right(starts, v)
        nxt = starts[i] if i < len(starts) else img.text_end
        if nxt - v < 8:
            rejected.append((sym, f'only {nxt - v} bytes long'))
            continue
        bad = [s for s, link in targets.get(v + 4, [])]
        bad += [s for s, link in targets.get(v, []) if not link and v <= s < nxt]
        if bad:
            rejected.append((sym, 'branch into the first two words from ' + ', '.join(f'{s:08x}' for s in bad)))
            continue
        k = len(hooks)
        tramp = None
        if kind == 'wrap':
            body = [f't{k}:']
            ok = True
            for j in range(2):
                a = v + 4 * j
                w = img.word(a)
                if a in pairs:  # an ARMv7 instruction the patch set replaced: its stub body here
                    sb = v6patch.stub_body(w)
                    cond = w >> 28
                    back = f'tb{k}_{j}'
                    if cond not in (0xe, 0xf):
                        body.append(f'b{COND_NAMES[cond ^ 1]} {back}')
                    text = '\n'.join(sb).replace('.back', back).replace('.lit', f'tl{k}_{j}')
                    body.append(text)
                    body.append(f'{back}:')
                    continue
                insn = next(md.disasm(struct.pack('<I', w), a), None)
                if insn is None:
                    ok = False
                    rejected.append((sym, f'undecodable word {w:08x} at +{4 * j}'))
                    break
                cond = COND_NAMES[w >> 28]
                rd, rn, rm = (w >> 12) & 15, (w >> 16) & 15, w & 15
                if w & 0x0e000000 == 0x0a000000 and w >> 28 != 0xf:  # b / bl #imm: re-targeted
                    t = (a + 8 + (arm2c.sext(w & 0xffffff, 24) << 2)) & 0xffffffff
                    body.append(f'{"bl" if w & 0x01000000 else "b"}{cond} 0x{t:x}')
                elif 'pc' not in insn.op_str:  # (bx / blx through a register are position independent)
                    body.append(f'.word 0x{w:08x}')
                elif w & 0x0fff0ff0 == 0x079f0000 and rd not in (15, rm) and rm != 15:
                    # ldr rd, [pc, rm]: the same address computed from a PC-relative offset
                    body.append(f'ldr{cond} r{rd}, tl{k}_{j}')
                    body.append(f'tp{k}_{j}: add{cond} r{rd}, pc, r{rd}')
                    body.append(f'ldr{cond} r{rd}, [r{rd}, r{rm}]')
                    body.append(f'b tb{k}_{j}')
                    body.append(f'tl{k}_{j}: .word 0x{a + 8:x} - (tp{k}_{j} + 8)')
                    body.append(f'tb{k}_{j}:')
                elif w & 0x0f7f0000 == 0x051f0000 and rd != 15:  # ldr rX, [pc, #imm]: copy the literal
                    imm = w & 0xfff
                    lit = img.word(a + 8 + (imm if w & 0x00800000 else -imm))
                    body.append(f'ldr{cond} r{rd}, tl{k}_{j}')
                    body.append(f'b tb{k}_{j}')
                    body.append(f'tl{k}_{j}: .word 0x{lit:08x}')
                    body.append(f'tb{k}_{j}:')
                else:
                    ok = False
                    rejected.append((sym, f'PC-relative instruction at +{4 * j}: {insn.mnemonic} {insn.op_str}'))
                    break
            if not ok:
                continue
            body.append(f'b 0x{v + 8:x}')
            body.append('.align 2')
            lines += body
            tramp = k
        seen[v] = (kind, host)
        hooks.append((v, sym, kind, host, tramp))

    # the trampolines: assembled at their link address, inside the image. Two passes: the first
    # places the labels, the second fills in the offsets that depend on them (PC-relative
    # address computations: an absolute minus a label is not a relocation GNU as can emit).
    def assemble(text):
        tmp = tempfile.mkdtemp()
        src, obj, elf, binf = (os.path.join(tmp, n) for n in ('t.s', 't.o', 't.elf', 't.bin'))
        open(src, 'w').write(text)
        subprocess.run([DKA + 'as', '-o', obj, src], check=True)
        subprocess.run([DKA + 'ld', '-Ttext', f'0x{tramp_base:x}', '-e', '0', '-o', elf, obj], check=True)
        labels = {}
        for line in subprocess.run([DKA + 'nm', elf], check=True, capture_output=True, text=True).stdout.splitlines():
            val, _, name = line.split()
            labels[name] = int(val, 16)
        subprocess.run([DKA + 'objcopy', '-O', 'binary', '-j', '.text', elf, binf], check=True)
        return labels, open(binf, 'rb').read() if os.path.exists(binf) else b''

    text = '\n'.join(lines) + '\n'
    reloc = re.compile(r'0x([0-9a-f]+) - \((tp\d+_\d)\s*\+ 8\)')
    labels, _ = assemble(reloc.sub('0', text))
    labels, code = assemble(reloc.sub(lambda m: f'0x{(int(m.group(1), 16) - labels[m.group(2)] - 8) & 0xffffffff:x}', text))
    syms = {int(n[1:]): v for n, v in labels.items() if re.fullmatch(r't\d+', n)}

    fixups = kuser_fixups(img)
    # the Thumb functions tools/v6patch.py moved: their copies' literals need the same fixups
    island = os.path.join(out_dir, 'thumb_island.json')
    if os.path.exists(island):
        moved = json.load(open(island))
        fixups += [(copy + a - start, w) for a, w in list(fixups) for start, end, copy in moved if start <= a < end]
    out = [f'// Generated by tools/hooktable.py from recomp/arm2c.py OVERRIDES and {os.path.basename(out_dir)}/v6patch.bin.',
           '// Do not edit.', '']
    for host in sorted({h[3] for h in hooks}):
        kinds = {h[2] for h in hooks if h[3] == host}
        if 'wrap' in kinds:
            out.append(f'extern "C" void {host}(Cpu *cpu, uint32_t entry, GuestFn original);')
        else:
            out.append(f'extern "C" void {host}(Cpu *cpu);')
    out += ['', f'constexpr uint32_t kTrampolineBase = 0x{tramp_base:08x};',
            f'constexpr uint32_t kTrampolineSize = 0x{len(code):x};',
            'alignas(4) const uint8_t kTrampolines[] = {']
    for o in range(0, len(code), 16):
        out.append('    ' + ' '.join(f'0x{b:02x},' for b in code[o:o + 16]))
    out += ['    0};', '', '// sorted by link address (native_original looks them up)', 'const NativeHook kNativeHooks[] = {']
    for v, sym, kind, host, tramp in sorted(hooks):
        t = f'0x{syms[tramp]:08x}' if tramp is not None else '0'
        fn = f'reinterpret_cast<void *>(&{host})'
        out.append(f'    {{0x{v:08x}, {t}, {1 if kind == "wrap" else 0}, {fn}, "{sym}"}},')
    out += ['};', '', 'const KuserFixup kKuserFixups[] = {']
    for a, w in fixups:
        out.append(f'    {{0x{a:08x}, 0x{w:08x}u}},  // {KUSER[w]}')
    out += ['};', '', '// the PLT imports, sorted: the runtime\'s static import slots (as table.c on the PC)',
            'extern "C" const char *const g_import_names[] = {']
    out += [f'    "{n}",' for n in img.imports]
    out += ['};', 'extern "C" const uint32_t g_import_count = sizeof(g_import_names) / sizeof(g_import_names[0]);', '']
    path = os.path.join(out_dir, 'native_table.inc')
    text = '\n'.join(out)
    try:
        same = open(path).read() == text
    except FileNotFoundError:
        same = False
    if not same:
        open(path, 'w').write(text)
    for sym, why in rejected:
        print(f'rejected {sym}: {why}')
    print(f'{len(hooks)} hooks ({sum(1 for h in hooks if h[4] is not None)} trampolines, {len(code)} bytes at '
          f'0x{tramp_base:x}), {len(rejected)} rejected, {len(fixups)} kernel helper fixups, '
          f'{len(img.imports)} imports: {path}')


if __name__ == '__main__':
    main()
