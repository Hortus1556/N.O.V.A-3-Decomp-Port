"""ARMv7 -> ARMv6K patch set: lets the game's own ARM code run natively on the 3DS's ARM11.

The game is ARMv7-A + VFPv3 code. The ARM11 MPCore (ARMv6K, VFPv2) runs nearly all of it as
it is; the rest (dmb, movw/movt, ubfx/sbfx, bfi/bfc, mls, VFPv3 vmov immediates and fixed-point
vcvt: about 3% of the reachable instructions) is replaced in place by a branch, with the same
condition, to a stub that does the same with ARMv6K / VFPv2 instructions and branches back.
The ARM code does not move, so its addresses, vtables, unwind tables and landing pads stay
valid; only Thumb functions with Thumb-2 long calls are copied (thumb_island), with an unwind
table of their own.

Left alone: NEON and d16-d31 code (only on paths the game takes when /proc/cpuinfo reports
NEON, which the runtime never does) and unreachable data that decodes as ARMv7 forms. Both
fault as undefined instructions if ever run.

    python tools/v6patch.py [out_dir]        (default build/3ds)
      writes v6patch.bin: 'N3V6', stub base, stub bytes, patch count, stubs, (address, word)
      pairs, then the Thumb island's start and code end and its unwind table's address and
      entry count; all addresses are link addresses (the loader adds its load base)
    python tools/v6patch.py --verify [N]     checks every stub against the original instruction
      in Unicorn (Cortex-A15 runs the original, ARM11 MPCore the patched site), N random
      register/VFP states each (default 3)

The stubs are assembled by devkitARM's GNU as with `.arch armv6k` / `.fpu vfpv2`, so an
instruction the ARM11 lacks cannot slip into them.
"""
import bisect, json, os, random, struct, subprocess, sys, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'recomp'))

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DKA = 'C:/msys64/opt/devkitpro/devkitARM/bin/arm-none-eabi-'
STUB_BASE = 0x018f1000  # past the end of the image (0x18f0158), page aligned


def bits(w, hi, lo):
    return (w >> lo) & ((1 << (hi - lo + 1)) - 1)


def sreg(vd, d):
    return f's{(vd << 1) | d}'


def dreg(vd, d):
    return f'd{(d << 4) | vd}'


def vfp_expand(imm8, double):
    """VFPExpandImm: the value an 8-bit VFPv3 immediate stands for, as raw bits."""
    s, b, c = imm8 >> 7, (imm8 >> 6) & 1, imm8 & 0x3f
    if double:
        exp = ((b ^ 1) << 10) | ((0xff if b else 0) << 2) | (c >> 4)
        return (s << 63) | (exp << 52) | ((c & 15) << 48)
    exp = ((b ^ 1) << 7) | ((0x1f if b else 0) << 2) | (c >> 4)
    return (s << 31) | (exp << 23) | ((c & 15) << 19)


def bic_chunks(mask):
    """Rotated 8-bit immediates whose union is `mask` (for bic/orr of an arbitrary mask)."""
    out, m = [], mask
    while m:
        lo = (m & -m).bit_length() - 1
        lo -= lo & 1  # rotations are by even amounts
        chunk = m & (0xff << lo) & 0xffffffff
        out.append(chunk)
        m &= ~chunk
    return out


def scratch(*used):
    return next(f'r{i}' for i in range(13) if i not in used)


def stub_body(w):
    """ARMv6K / VFPv2 assembly doing what ARMv7 instruction `w` does, or None to leave it."""
    if w & 0xfffffff0 == 0xf57ff050:  # dmb
        return ['str r0, [sp, #-4]!', 'mov r0, #0', 'mcr p15, 0, r0, c7, c10, 5', 'ldr r0, [sp], #4']
    rd = bits(w, 15, 12)
    if w & 0x0ff00000 in (0x03000000, 0x03400000) and rd != 15:  # movw / movt
        imm = (bits(w, 19, 16) << 12) | bits(w, 11, 0)
        if w & 0x0ff00000 == 0x03000000:
            return [f'mov r{rd}, #{imm & 0xff00}', f'orr r{rd}, r{rd}, #{imm & 0xff}']
        return [f'uxth r{rd}, r{rd}', f'orr r{rd}, r{rd}, #{(imm & 0xff00) << 16}', f'orr r{rd}, r{rd}, #{(imm & 0xff) << 16}']
    if w & 0x0fa00070 == 0x07a00050:  # ubfx / sbfx
        width, lsb, rn = bits(w, 20, 16) + 1, bits(w, 11, 7), bits(w, 3, 0)
        sh = 'lsr' if w & (1 << 22) else 'asr'
        if lsb + width > 32 or 15 in (rd, rn):
            return None
        return [f'mov r{rd}, r{rn}, lsl #{32 - lsb - width}', f'mov r{rd}, r{rd}, {sh} #{32 - width}'] \
            if width < 32 else [f'mov r{rd}, r{rn}']
    if w & 0x0fe00070 == 0x07c00010:  # bfi / bfc (rn == 15)
        msb, lsb, rn = bits(w, 20, 16), bits(w, 11, 7), bits(w, 3, 0)
        if msb < lsb or rd == 15:
            return None
        width = msb - lsb + 1
        mask = ((1 << width) - 1) << lsb
        clear = [f'bic r{rd}, r{rd}, #{c}' for c in bic_chunks(mask)]
        if rn == 15:
            return clear
        if 13 in (rd, rn):
            return None  # the scratch push moves sp
        t = scratch(rd, rn)
        return [f'str {t}, [sp, #-4]!', f'mov {t}, r{rn}, lsl #{32 - width}'] + clear + \
               [f'orr r{rd}, r{rd}, {t}, lsr #{32 - width - lsb}' if 32 - width - lsb else f'orr r{rd}, r{rd}, {t}', f'ldr {t}, [sp], #4']
    if w & 0x0ff000f0 == 0x00600090:  # mls rd, rn, rm, ra: rd = ra - rn * rm
        rd, ra, rm, rn = bits(w, 19, 16), bits(w, 15, 12), bits(w, 11, 8), bits(w, 3, 0)
        if {13, 15} & {rd, ra, rm, rn}:
            return None  # the scratch push moves sp (only data decoded as code does this)
        t = scratch(rd, ra, rm, rn)
        return [f'str {t}, [sp, #-4]!', f'mul {t}, r{rn}, r{rm}', f'sub r{rd}, r{ra}, {t}', f'ldr {t}, [sp], #4']
    if w & 0x0fb00ef0 == 0x0eb00a00:  # VFPv3 vmov immediate
        d, vd, double = bits(w, 22, 22), bits(w, 15, 12), w & 0x100
        imm8 = (bits(w, 19, 16) << 4) | bits(w, 3, 0)
        v = vfp_expand(imm8, double)
        if double:
            if d:
                return None  # d16-d31
            return [f'vldr {dreg(vd, d)}, [pc, #0]', 'b .back', f'.word {v & 0xffffffff}', f'.word {v >> 32}']
        return [f'vldr {sreg(vd, d)}, [pc, #0]', 'b .back', f'.word {v}']
    if w & 0x0fba0e50 == 0x0eba0a40:  # VFPv3 vcvt between floating point and fixed point
        d, vd, to_fixed, unsigned = bits(w, 22, 22), bits(w, 15, 12), w & (1 << 18), w & (1 << 16)
        double, sx = w & 0x100, w & 0x80
        size = 32 if sx else 16
        fbits = size - ((bits(w, 3, 0) << 1) | bits(w, 5, 5))
        if not sx or (double and d):
            return None  # 16-bit fixed point, d16-d31: not in this binary
        it = 'u32' if unsigned else 's32'
        if double:
            dd = dreg(vd, d)
            lo = f's{vd * 2}'
            t = 'd1' if vd == 0 else 'd0'
            scale = struct.unpack('<Q', struct.pack('<d', 2.0 ** (fbits if to_fixed else -fbits)))[0]
            if to_fixed:
                # the 32-bit result goes to the low half and is extended into the high half
                return [f'vpush {{{t}}}', f'vldr {t}, .lit', f'vmul.f64 {dd}, {dd}, {t}', f'vcvt.{it}.f64 {lo}, {dd}',
                        'push {r0}', f'vmov r0, {lo}', 'mov r0, r0, asr #31' if not unsigned else 'mov r0, #0',
                        f'vmov s{vd * 2 + 1}, r0', 'pop {r0}', f'vpop {{{t}}}', 'b .back', '.align 3',
                        f'.lit: .word {scale & 0xffffffff}', f'.word {scale >> 32}']
            return [f'vpush {{{t}}}', f'vcvt.f64.{it} {dd}, {lo}', f'vldr {t}, .lit', f'vmul.f64 {dd}, {dd}, {t}',
                    f'vpop {{{t}}}', 'b .back', '.align 3', f'.lit: .word {scale & 0xffffffff}', f'.word {scale >> 32}']
        sd = sreg(vd, d)
        idx = (vd << 1) | d
        t = 'd1' if idx in (0, 1) else 'd0'
        ts = 's2' if t == 'd1' else 's0'
        scale = struct.unpack('<I', struct.pack('<f', 2.0 ** (fbits if to_fixed else -fbits)))[0]
        if to_fixed:
            return [f'vpush {{{t}}}', f'vldr {ts}, .lit', f'vmul.f32 {sd}, {sd}, {ts}', f'vcvt.{it}.f32 {sd}, {sd}',
                    f'vpop {{{t}}}', 'b .back', f'.lit: .word {scale}']
        return [f'vpush {{{t}}}', f'vcvt.f32.{it} {sd}, {sd}', f'vldr {ts}, .lit', f'vmul.f32 {sd}, {sd}, {ts}',
                f'vpop {{{t}}}', 'b .back', f'.lit: .word {scale}']
    return None


def reachable_words():
    """Reachable ARM instructions, minus any inside a Thumb function. An ARM function's range ends
    at the next ARM start, so it can span Thumb functions (0x12d72f4 spans boost::exception's
    destructor at 0x12d73f0); its exploration then reads their halfword pairs as ARM words, and a
    pair that decoded as movw got an ARM branch written into the Thumb code. On the console that
    word ran as a Thumb instruction ARMv6 lacks (cbnz) during the static constructors (Boost
    destroys exception objects there) and the first start stopped; Azahar decodes cbnz anyway."""
    import arm2c
    import bisect
    g = arm2c.Generator()
    g.plan()
    thumb = sorted(g.thumb)

    def in_thumb(a):
        i = bisect.bisect_right(thumb, a) - 1
        return i >= 0 and a < g.thumb_ranges[thumb[i]]

    # One walk over the whole of .text from every known entry (function starts, pointer and
    # mid-function entries, landing pads), not one per function: a function's walk stopped at
    # branches out of its range, so code that only such a branch reaches (zlib's inflate jumps
    # between its parts) was never seen and kept its ARMv7 instructions; the console stopped at a
    # ubfx in inflate when the game first read its .obb, where Azahar runs ubfx anyway. The Thumb
    # functions are marked visited beforehand, so the walk never reads their halfwords as ARM.
    #
    # Then the code no entry leads to: functions that are only reached through pointers the
    # analysis does not follow (for example 0x9e9408 and 0x1071edc, after their literal pools,
    # no symbol and no direct call) and blocks behind jump tables it does not recognize. Every
    # unexplored stretch of .text gets a walk from its first word that decodes and is not a
    # literal (a word an ldr / ldrh / ldrd / vldr reads PC-relative); literals and Thumb code are
    # marked visited first, so no walk runs into them. Any word left that a stub would replace
    # starts a walk too, until none is left. On the PC and the Switch these are translated as
    # functions of their own or reached through the dispatcher, so missing them here only mattered
    # on the console.
    img = g.img
    base, n = img.text_addr, len(img.text) // 4
    text = struct.unpack_from(f'<{n}I', img.text)
    thumb_words = {a for t in thumb for a in range(t & ~3, g.thumb_ranges[t], 4)}
    roots = set(g.starts)
    for es in g.entries_by_func.values():
        roots |= set(es)
    roots = {r for r in roots if r not in thumb_words}

    def discover(literals):
        """Every word reachable without stepping on a literal or into Thumb code."""
        fg = arm2c.FuncGen(g, base, base + 4 * n)  # one walker: its visited set carries over
        for a in literals | thumb_words:
            fg.reach[a] = None
        known = fg.reach  # (the barriers hold None)

        def walk(entries):
            g.entries_by_func[base] = sorted(entries)
            fg.explore()

        walk(roots)
        while True:
            before = len(known)
            gap_roots, a = set(), base
            while a < base + 4 * n:
                if a in known or a in literals or a in thumb_words:
                    a += 4
                    continue
                start = a
                while a < base + 4 * n and a not in known and a not in literals and a not in thumb_words:
                    a += 4
                r = start
                while r < a and not _decodes(text[(r - base) // 4]):
                    r += 4
                if r < a:
                    gap_roots.add(r)
            walk(gap_roots)
            walk({base + 4 * i for i, w in enumerate(text)
                  if base + 4 * i not in known and base + 4 * i not in literals
                  and base + 4 * i not in thumb_words and stub_body(w) is not None})
            if len(known) == before:
                for a in [a for a, w in known.items() if w is None]:  # (in place: 5 million entries)
                    del known[a]
                return known

    # The literals are the words the code reads PC-relative; the code is what can be reached
    # without reading a literal as an instruction. Each pass's code gives the next pass's literals
    # until both settle. A walk that ran on into a pool (after a call that does not return, which
    # the walk cannot know) took its PIC offsets for instructions: 0x6a9994 decodes as mlseq, and
    # the patch set of 2026-10-11 rewrote 17 such offsets (CMenuManager::onEvent, Menu_ChangeLanguage,
    # GameMpManager::LoadSkinColorPresets, ...) into branches to mls stubs.
    #
    # The first pass (no literals known) also walks pools, and a pool word that decodes as a
    # PC-relative load points anywhere: such a target became a barrier in real code (a ubfx in
    # btQuantizedBvh::walkStacklessQuantizedTree, a dmb in GS_OnlineMenu::Update, ...) and its
    # walk stopped there. So once the literals settle, those no reached code reads but reached code
    # runs into (the instruction before falls through to it, or a branch targets it) are taken back
    # as code for good, and the passes go on. Not every stale literal: a pool whose reader a stale
    # barrier kept unreached is stale too, and as code it would be walked again. Nor only the last
    # pass's literals: that does not settle.
    literals, code = set(), set()
    while True:
        known = discover(literals)
        read = literals_read(known)
        if read - code <= literals:
            back = _flowed_into(known, literals - read)
            if not back:
                return known
            code |= back
            literals -= back
        else:
            literals |= read - code
        del known


def _flowed_into(code, words):
    """The words of `words` that `code` ({address: word}) falls through or branches (B / BL) to."""
    out = set()
    for a in words:
        w = code.get(a - 4)
        if w is not None and not (w >> 28 == 0xe and (
                w & 0x0f000000 == 0x0a000000 or  # b
                w & 0x0ffffff0 == 0x012fff10 or  # bx
                w & 0x0e108000 == 0x08108000 or  # ldm / pop with pc
                w & 0x0c10f000 == 0x0410f000 or  # ldr pc
                w & 0x0de0f000 == 0x01a0f000)):  # mov pc
            out.add(a)
    for a, w in code.items():
        if w >> 28 != 0xf and w & 0x0e000000 == 0x0a000000:
            t = a + 8 + ((w & 0xffffff) ^ 0x800000) * 4 - 0x2000000
            if t in words:
                out.add(t)
    return out


def literals_read(code):
    """The words that `code` ({address: word}) loads PC-relative: ldr / ldrb / ldrh / ldrsb /
    ldrsh read one, ldrd and a double-precision vldr two."""
    out = set()
    for a, w in code.items():
        if w >> 28 == 0xf:
            continue
        two = False
        if w & 0x0e5f0000 == 0x041f0000 and not w & 0x02000000 and w & 0x00100000:  # ldr / ldrb
            off = w & 0xfff
        elif w & 0x0e5f0090 == 0x004f0090 and w & 0x60:  # ldrh / ldrsb / ldrsh / ldrd (immediate)
            off = (w >> 4) & 0xf0 | w & 0xf
            two = w & 0x00100060 == 0x00000040
            if not w & 0x00100000 and not two:  # strh / strd
                continue
        elif w & 0x0f3f0e00 == 0x0d1f0a00 and w & 0x00100000:  # vldr
            off = (w & 0xff) * 4
            two = (w >> 8) & 0xf == 0xb
        else:
            continue
        t = a + 8 + (off if w & 1 << 23 else -off)
        out.add(t & ~3)
        if two:
            out.add((t + 4) & ~3)
    return out


def _disassembler():
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
    if not hasattr(_disassembler, 'md'):
        _disassembler.md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    return _disassembler.md


def _decodes(w):
    """An ARM word capstone decodes (and not 0, the padding between functions)."""
    return w != 0 and any(True for _ in _disassembler().disasm_lite(struct.pack('<I', w), 0))


def thumb_island(island_base):
    """Relocates the Thumb functions that make Thumb-2 long calls into an island at island_base.

    Boost.Thread and libgcc's Thumb functions call far ARM code (__cxa_guard_acquire, ...) with
    the 32-bit BL / BLX of Thumb-2, whose J1 / J2 bits reach +-16 MB. ARMv6 has Thumb-1 only: its
    BL is a prefix (LR = PC + offset << 12) and a suffix, +-4 MB, and a Thumb-2 suffix with a J bit
    clear is an ordinary B or Bcc there: the console jumped into the middle of other code during
    the static constructors (Boost's generic_category() calling __cxa_guard_acquire). No free
    space lies within 4 MB of most sites, so each such function is copied here whole (at the same
    address modulo 4: its PC-relative literal loads keep their offsets), every call out of it
    becomes a Thumb-1 BLX to an ARM veneer next to the copy (a PC-relative `bx ip` to the target),
    the PIC literals its `add rX, pc` use are corrected by the distance moved, and the original
    entry becomes `bx pc` + an ARM `b` (+-32 MB) to a veneer entering the copy in Thumb state.

    C++ exceptions: a function's code includes its landing pads (from its LSDA), which no
    branch reaches: their calls (`blx _Unwind_Resume` at the end of a cleanup) are converted as
    well, and a function whose only Thumb-2 calls are in its pads moves too. The copies get an
    unwind table of their own after the island's code, the original functions' entries with
    the copy's address (the unwind data and LSDAs stay where they are; a landing pad is an
    offset from the function's start, so the copy's pads are found): the runtime gives it to the
    game's unwinder for addresses in the island (__gnu_Unwind_Find_exidx, runtime/libc_misc.cpp).
    Returns (island bytes, {link address: patched word}, [(start, end, copy)],
    (island start, its code's end, its unwind table, entries))."""
    import arm2c
    g = arm2c.Generator()
    g.plan()
    img = g.img

    def hw(a):
        return struct.unpack_from('<H', img.text, a - img.text_addr)[0]

    def t2_call(a):  # a 32-bit BL / BLX that Thumb-1 cannot express
        h, h2 = hw(a), hw(a + 2)
        return h >> 11 == 0x1e and h2 >> 14 == 3 and (h2 >> 11) not in (0x1f, 0x1d)

    def writes(h, rd):  # Thumb-1 instructions that set rd (enough for the code before an `add rd, pc`)
        if h >> 13 == 0 and h >> 11 != 3 or h >> 11 == 3:  # shifts, add / sub
            return h & 7 == rd
        if h >> 13 == 1:  # mov / cmp / add / sub immediate
            return (h >> 11) & 3 != 1 and (h >> 8) & 7 == rd
        if h >> 10 == 0x10:  # ALU
            return (h >> 6) & 0xf not in (8, 10, 11) and h & 7 == rd
        if h >> 10 == 0x11:  # hi register add / mov
            return (h >> 8) & 3 in (0, 2) and (h & 7 | (h >> 4) & 8) == rd
        if h >> 12 in (5, 6, 7, 8):  # loads and stores
            return (h >> 11) & 1 and h & 7 == rd if h >> 12 != 5 else (h >> 9) & 7 >= 3 and h & 7 == rd
        if h >> 11 in (0x13, 0x14, 0x15):  # ldr sp-relative, add pc / sp
            return (h >> 8) & 7 == rd
        if h >> 9 == 0x5e or h >> 11 == 0x19:  # pop, ldm
            return bool(h & (1 << rd)) if h >> 9 == 0x5e else (bool(h & (1 << rd)) or (h >> 8) & 7 == rd)
        return False

    def veneer(v, target):
        # position-independent (the loader moves the stubs and the image by the same load base):
        # ldr ip, [pc, #4]; add ip, pc, ip; bx ip; the target's distance from v + 12 (bit 0: Thumb)
        return struct.pack('<IIII', 0xe59fc004, 0xe08fc00c, 0xe12fff1c, (target - (v + 12)) & 0xffffffff)

    pads = img.landing_pads()
    island, patched, ranges, copied = bytearray(), {}, [], []
    for t in g.thumb:
        end = g.thumb_ranges[t]
        fg = arm2c.ThumbFuncGen(g, t, end)
        fg.explore(pads.get(t, ()))
        insns = sorted(fg.reach)
        if not any(t2_call(a) for a in insns):
            continue
        while (island_base + len(island)) % 4 != t % 4:
            island += b'\0'
        new = island_base + len(island)
        delta = new - t
        body = bytearray(img.text[t - img.text_addr:end - img.text_addr])
        veneers = []  # (offset of the site in body, target with its state bit)
        adjusted = {}
        for i, a in enumerate(insns):
            h, o = hw(a), a - t
            if h >> 11 == 0x1e and hw(a + 2) >> 14 == 3:
                kind, target = arm2c.thumb_bl(h, hw(a + 2), a)
                if t <= target < end:
                    if t2_call(a):
                        raise SystemExit(f'thumb island: {a:08x} long call inside its function')
                    continue
                veneers.append((o, target | 1 if kind == 'bl' else target & ~3))
            elif h >> 11 == 0x1c:  # b: within the function only
                target = a + 4 + arm2c.sext((h & 0x7ff) << 1, 12)
                if not t <= target < end:
                    raise SystemExit(f'thumb island: {a:08x} branches out of its function')
            elif h >> 12 == 0xd and (h >> 8) & 0xf < 0xe:
                target = a + 4 + arm2c.sext((h & 0xff) << 1, 9)
                if not t <= target < end:
                    raise SystemExit(f'thumb island: {a:08x} conditional branch out of its function')
            elif h >> 11 in (0x09, 0x14):  # ldr rd, [pc, #imm] / adr rd: data inside the copy
                at = ((a + 4) & ~3) + (h & 0xff) * 4
                if not t <= at < end:
                    raise SystemExit(f'thumb island: {a:08x} reads {at:08x} outside its function')
            elif h & 0xff78 == 0x4478:  # add rd, pc: a PIC literal (value - delta)
                rd = h & 7 | (h >> 4) & 8
                for b in reversed(insns[max(0, i - 16):i]):
                    hb = hw(b)
                    if hb >> 11 == 0x09 and (hb >> 8) & 7 == rd:
                        lit = ((b + 4) & ~3) + (hb & 0xff) * 4
                        if adjusted.get(lit, a) != a:
                            raise SystemExit(f'thumb island: literal {lit:08x} used by two add-pc sites')
                        adjusted[lit] = a
                        v = struct.unpack_from('<I', body, lit - t)[0]
                        struct.pack_into('<I', body, lit - t, (v - delta) & 0xffffffff)
                        break
                    if writes(hb, rd):
                        raise SystemExit(f'thumb island: {a:08x} add pc without its literal load')
                else:
                    raise SystemExit(f'thumb island: {a:08x} add pc without its literal load')
            elif h & 0xff78 == 0x4678 or h == 0x4778:  # mov rd, pc / bx pc
                raise SystemExit(f'thumb island: {a:08x} reads pc ({h:04x})')
        island += body
        while (island_base + len(island)) % 4:
            island += b'\0'
        for o, target in veneers:  # the call becomes a Thumb-1 BLX (offset bit 1 clear) to the veneer
            v = island_base + len(island)
            island += veneer(v, target)
            site = new + o
            off = v - (site + 4)
            off += off & 2
            assert -(1 << 22) <= off < (1 << 22), hex(site)
            struct.pack_into('<HH', island, site - island_base, 0xf000 | (off >> 12) & 0x7ff, 0xe800 | (off >> 1) & 0x7ff)
        entry = island_base + len(island)  # ARM: into the copy, in Thumb state
        island += veneer(entry, new | 1)
        # the original entry: `bx pc` from a word-aligned address, then an ARM b to the entry veneer
        bx = t if t % 4 == 0 else t + 2
        if bx + 8 > end:
            raise SystemExit(f'thumb island: {t:08x} too short for its entry redirect')
        redirect = {bx: struct.pack('<HH', 0x4778, 0x46c0)}
        if bx != t:
            redirect[t] = struct.pack('<H', 0x46c0)
        off = (entry - (bx + 4 + 8)) >> 2
        assert -(1 << 23) <= off < (1 << 23), hex(t)
        redirect[bx + 4] = struct.pack('<I', 0xea000000 | off & 0xffffff)
        for at, data in redirect.items():
            for k, byte in enumerate(data):
                w = (at + k) & ~3
                cur = bytearray(patched.get(w, img.text[w - img.text_addr:w - img.text_addr + 4]))
                cur[(at + k) - w] = byte
                patched[w] = bytes(cur)
        ranges.append((t, end, new))
        copied += [a + delta for a in insns]
    for a in copied:  # no Thumb-2-only call is left where the copies have instructions
        h, h2 = struct.unpack_from('<HH', island, a - island_base)
        if h >> 11 == 0x1e and h2 >> 14 == 3 and (h2 >> 11) not in (0x1f, 0x1d):
            raise SystemExit(f'thumb island: a Thumb-2 call is left at {a:08x}')

    # the copies' unwind table: an entry per copy (prel31 function, then the original's second
    # word: inline unwind data or EXIDX_CANTUNWIND as they are, an .ARM.extab entry made relative
    # to the new place), sorted as the copies are, and a CANTUNWIND end past the last copy
    ex_addr, ex = img.e.sec('.ARM.exidx')
    entries = []  # (function with its Thumb bit, second word, address of the entry)
    for i in range(len(ex) // 8):
        w0, w1 = struct.unpack_from('<II', ex, i * 8)
        at = ex_addr + i * 8
        fn = (at + arm2c.sext(w0 & 0x7fffffff, 31)) & 0xffffffff
        entries.append((fn, w1, at))
    entries.sort()
    starts = [fn & ~1 for fn, _, _ in entries]
    while len(island) % 4:
        island += b'\0'
    table = island_base + len(island)
    rows = [(new | 1, None, t) for t, _, new in ranges] + [(table, 1, None)]
    for k, (fn, w1, t) in enumerate(rows):
        at = table + 8 * k
        if t is not None:
            i = bisect.bisect_right(starts, t) - 1
            if i < 0:
                raise SystemExit(f'thumb island: {t:08x} has no unwind entry')
            ofn, w1, oat = entries[i]
            if starts[i] != t and not (w1 == 1 or w1 & 0x80000000):
                raise SystemExit(f'thumb island: {t:08x} is inside {starts[i]:08x}, whose entry has an LSDA')
            fn = (fn & ~1) | (ofn & 1)
            if not (w1 == 1 or w1 & 0x80000000):  # .ARM.extab: prel31 to the same place
                extab = (oat + 4 + arm2c.sext(w1 & 0x7fffffff, 31)) & 0xffffffff
                w1 = (extab - (at + 4)) & 0x7fffffff
        island += struct.pack('<II', (fn - at) & 0x7fffffff, w1)
    words = {a: struct.unpack('<I', w)[0] for a, w in patched.items()}
    return bytes(island), words, ranges, (ranges[0][2] if ranges else table, table, table, len(rows))


def build(out_dir):
    words = reachable_words()
    literals = literals_read(words)
    sites = []
    for a in sorted(words):
        if a in literals:  # data the code reads (reachable_words leaves none; a guard)
            continue
        body = stub_body(words[a])
        if body is not None:
            sites.append((a, words[a], body))
    # one stub per site: its body, then a branch back past the patched instruction
    lines = ['.arch armv6k', '.fpu vfpv2', '.arm', '.text', '.align 2']
    for i, (a, w, body) in enumerate(sites):
        back = a + 4
        lines.append(f'.global s{i}\ns{i}:')
        text = '\n'.join(body).replace('.back', f'0x{back:x}').replace('.lit', f'l{i}')
        lines.append(text)
        if 'b 0x' not in text:
            lines.append(f'b 0x{back:x}')
        lines.append('.align 2')
    os.makedirs(out_dir, exist_ok=True)
    tmp = tempfile.mkdtemp()
    src, obj, elf = (os.path.join(tmp, n) for n in ('stubs.s', 'stubs.o', 'stubs.elf'))
    open(src, 'w').write('\n'.join(lines) + '\n')
    subprocess.run([DKA + 'as', '-o', obj, src], check=True)
    subprocess.run([DKA + 'ld', '-Ttext', f'0x{STUB_BASE:x}', '-e', '0', '-o', elf, obj], check=True)
    syms = {}
    for line in subprocess.run([DKA + 'nm', elf], check=True, capture_output=True, text=True).stdout.splitlines():
        v, t, n = line.split()
        if n[0] == 's' and n[1:].isdigit():
            syms[int(n[1:])] = int(v, 16)
    binf = os.path.join(tmp, 'stubs.bin')
    subprocess.run([DKA + 'objcopy', '-O', 'binary', '-j', '.text', elf, binf], check=True)
    code = open(binf, 'rb').read()
    code += bytes(-len(code) % 16)
    island, thumb_words, thumb_ranges, island_eh = thumb_island(STUB_BASE + len(code))
    code += island
    with open(os.path.join(out_dir, 'thumb_island.json'), 'w') as f:  # tools/hooktable.py: kuser literals in the copies
        json.dump(thumb_ranges, f)
    patches = []
    for i, (a, w, _) in enumerate(sites):
        cond = w >> 28
        if cond == 0xf:
            cond = 0xe  # dmb is unconditional
        off = (syms[i] - (a + 8)) >> 2
        assert -(1 << 23) <= off < (1 << 23), hex(a)
        patches.append((a, (cond << 28) | 0x0a000000 | (off & 0xffffff)))
    patches += sorted(thumb_words.items())
    out = os.path.join(out_dir, 'v6patch.bin')
    with open(out, 'wb') as f:
        f.write(b'N3V6' + struct.pack('<III', STUB_BASE, len(code), len(patches)))
        f.write(code)
        for a, nw in patches:
            f.write(struct.pack('<II', a, nw))
        f.write(struct.pack('<IIII', *island_eh))  # the island's range, its unwind table and entries
    print(f'{len(sites)} sites patched, {len(thumb_ranges)} Thumb functions moved ({len(island)} bytes, '
          f'{island_eh[3]} unwind entries), '
          f'{len(code)} bytes of stubs at 0x{STUB_BASE:x}: {out}')
    return sites, syms, code, dict(patches)


def verify(states):
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UcError
    from unicorn import arm_const as A
    sites, syms, code, patches = build(os.path.join(ROOT, 'build', '3ds'))
    STACK = 0x02000000

    def make(cpu):
        u = Uc(UC_ARCH_ARM, UC_MODE_ARM, cpu)
        u.mem_map(0, 0x02100000)
        u.reg_write(A.UC_ARM_REG_C1_C0_2, u.reg_read(A.UC_ARM_REG_C1_C0_2) | (0xf << 20))
        u.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)
        return u

    a15 = make(A.UC_CPU_ARM_CORTEX_A15)
    v6 = make(A.UC_CPU_ARM_11MPCORE)
    v6.mem_write(STUB_BASE, code)
    regs = [getattr(A, f'UC_ARM_REG_R{i}') for i in range(13)] + [A.UC_ARM_REG_SP, A.UC_ARM_REG_LR]
    vregs = [getattr(A, f'UC_ARM_REG_D{i}') for i in range(16)]
    rng = random.Random(1)
    bad = 0
    for n, (a, w, body) in enumerate(sites):
        for k in range(states):
            r = [rng.getrandbits(32) for _ in range(13)] + [STACK - 0x100 - 4 * rng.randrange(16), rng.getrandbits(32) & ~3]
            if k == 0:  # also plain values: small integers and floats
                fl = [struct.unpack('<Q', struct.pack('<d', rng.uniform(-1e4, 1e4)))[0] for _ in range(16)]
            else:
                fl = [rng.getrandbits(64) for _ in range(16)]
            flags = rng.getrandbits(4) << 28
            out = []
            for u, word in ((a15, w), (v6, patches[a])):
                u.mem_write(a, struct.pack('<I', word))
                for reg, v in zip(regs, r):
                    u.reg_write(reg, v)
                for reg, v in zip(vregs, fl):
                    u.reg_write(reg, v)
                u.reg_write(A.UC_ARM_REG_CPSR, (u.reg_read(A.UC_ARM_REG_CPSR) & 0x0fffffff) | flags)
                u.reg_write(A.UC_ARM_REG_FPSCR, 0)
                try:
                    u.emu_start(a, a + 4, count=64)
                    err = None
                except UcError as e:
                    err = str(e)
                out.append((err, [u.reg_read(x) for x in regs], [u.reg_read(x) for x in vregs],
                            u.reg_read(A.UC_ARM_REG_CPSR) >> 28, u.reg_read(A.UC_ARM_REG_PC)))
            if out[0] != out[1]:
                bad += 1
                if bad <= 20:
                    print(f'MISMATCH {a:08x} {w:08x}: {body}\n  a15 {out[0]}\n  v6  {out[1]}')
        if n % 20000 == 0:
            print(f'{n}/{len(sites)} checked, {bad} mismatches', flush=True)
    print(f'{len(sites)} sites x {states} states: {bad} mismatches')
    return bad == 0


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--verify':
        sys.exit(0 if verify(int(sys.argv[2]) if len(sys.argv) > 2 else 3) else 1)
    build(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build', '3ds'))
