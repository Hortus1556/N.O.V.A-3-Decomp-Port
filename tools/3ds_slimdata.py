"""The New 3DS's copy of the game data: what its low-end profile uses, at the size it uses it.

Usage: 3ds_slimdata.py measure LOG... [--data DIR]  what each run read, by kind (NOVA3_TRACE_READS logs)
       3ds_slimdata.py build [--out DIR] [--mips N] [--no-ui] [--no-twins] [--no-unreachable] [--no-recompress]
                                                write the slim set (default build/3ds/slim_data); all of
                                                it by default, `--mips 0 --no-ui --no-twins
                                                --no-unreachable --no-recompress` is the first version
       3ds_slimdata.py index [DIR]              write DIR/mount.idx (default build/3ds/slim_data)
       3ds_slimdata.py lib [DIR]                write DIR/libGloftNAHM.so.z
       3ds_slimdata.py pack [DIR] [--no-zopfli] write DIR/X.gla.z for the stream and sound archives
       3ds_slimdata.py verify [DIR]             unpack every DIR/X.gla.z and compare it with DIR/X.gla
       3ds_slimdata.py uploads TRACE TRACE      compare two GL traces' texture uploads
       3ds_slimdata.py shots DIR DIR            compare two runs' screenshots

index: the bytes the game reads when it mounts the zip archives at boot, 1 KB at each member's
local header, gathered into one file (mount.idx) that the 3DS serves those reads from
(runtime/libc_io.cpp); build writes it too. lib: the library's loaded parts compressed in pieces
(libGloftNAHM.so.z, 43% of its size), which the 3DS's loader inflates while it reads the next
(runtime/native_3ds.cpp); build writes it too. pack: the OBB-format archives (*_stream.gla,
sounds_*.gla), which the game reads uncompressed, in deflated blocks (X.gla.z, 'N3BZ'), which the 3DS's
file layer unpacks as the game reads X.gla (runtime/libc_io.cpp); build writes them too (--no-pack:
not), and the console's set holds X.gla.z instead of X.gla.

measure: a PC run with NOVA3_DEVICE=3ds NOVA3_TRACE_READS=1 logs every read of the game's files
(runtime/libc_io.cpp: host path, offset, bytes); the reads are laid over the archives' members
(the zip .gla files and the OBB-format *_stream.gla / sounds_*.gla) and summed per kind. A
chapter's load is what follows its "bench: chapter N: <level>" line, a multiplayer match what
follows the match setup's Continue (Obj150346_OnClick).

Everything here is made from the player's own game files (dist/nova3_data, never changed) and
stays with their copy.
"""
import argparse
import bisect
import collections
import io
import os
import re
import shutil
import struct
import sys
import zipfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import obb  # noqa: E402  (the OBB container format)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, 'dist', 'nova3_data')
OUT = os.path.join(ROOT, 'build', '3ds', 'slim_data')
MB = 1e6


# ---- archives -------------------------------------------------------------------------------

def members(path):
    """[(name, start, end)] byte ranges of an archive's members as stored (a zip member from its
    local header to the end of its data), and the ranges of its index."""
    with open(path, 'rb') as f:
        head = f.read(4)
    out, index = [], []
    if head == b'PK\x03\x04' or (head == b'PK\x05\x06'):
        z = zipfile.ZipFile(path)
        with open(path, 'rb') as f:
            for i in z.infolist():
                f.seek(i.header_offset)
                local = f.read(30)
                n, m = struct.unpack_from('<HH', local, 26)
                out.append((i.filename, i.header_offset, i.header_offset + 30 + n + m + i.compress_size))
        end = max((e for _, _, e in out), default=0)
        index.append((end, os.path.getsize(path)))  # central directory
    else:
        try:
            o = obb.Obb(path)
        except (struct.error, ValueError, OSError):
            return None, None
        for name, off, size, _ in o.entries:
            out.append((name, off, off + size))
        index.append((0, min((s for _, s, _ in out), default=0)))  # header, table, names
    return out, index


def kind(archive, name):
    """What a member is, for the tables."""
    ext = os.path.splitext(name)[1].lower()
    if ext in ('.etc', '.pvr'):
        return 'textures'
    if ext == '.bdae':
        return 'meshes, animations' if archive.endswith('_stream.gla') else 'meshes'
    if ext in ('.wav', '.mpc', '.vxn', '.fxb') or archive.startswith('sounds_'):
        return 'sounds'
    if ext in ('.lvc', '.col', '.lua', '.bin', '.stb', '.atb', '.txt', '.xml', '.off') or \
            archive in ('levels.gla', 'strings.gla'):
        return 'levels, scripts, text'
    if ext == '.irr':
        return 'irradiance (.irr)'
    return 'other'


class Layout:
    """Each archive's members, for looking up which member a read falls in."""

    def __init__(self, data):
        self.data = data
        self.arch = {}
        for n in sorted(os.listdir(data)):
            p = os.path.join(data, n)
            if not os.path.isfile(p):
                continue
            if n.endswith('.gla'):
                m, idx = members(p)
                if m is not None:
                    m.sort(key=lambda t: t[1])
                    self.arch[n] = (m, idx)

    def split(self, archive, start, end):
        """(member name or '<index>', overlapping bytes) of the read [start, end). Mounting a zip
        archive, the game reads 1 KB at every member's local header: counted as the index."""
        m, idx = self.arch[archive]
        starts = [s for _, s, _ in m]
        i = max(0, bisect.bisect_right(starts, start) - 1)
        got = []
        if i < len(m) and m[i][1] == start and end - start <= 1024:
            return [('<index>', end - start)]  # the archive's mount: 1 KB at each member's header
        while i < len(m) and m[i][1] < end:
            name, s, e = m[i]
            o = min(e, end) - max(s, start)
            if o > 0:
                got.append((name, o))
            i += 1
        rest = (end - start) - sum(o for _, o in got)
        if rest > 0:
            got.append(('<index>', rest))
        return got


# ---- measure --------------------------------------------------------------------------------

READ = re.compile(r'^read: (.*) (\d+) (\d+)$')


def phases(log):
    """The log's reads, split into (boot and menu, load and play): [(path, offset, bytes)] each."""
    first, second = [], []
    cur = first
    with open(log, encoding='utf-8', errors='replace') as f:
        for line in f:
            if cur is first and (re.match(r'bench: chapter \d+: \S+\.lvc', line) or
                                 line.startswith('lua: Obj150346_OnClick')):
                cur = second
                continue
            m = READ.match(line.rstrip('\n'))
            if m:
                cur.append((m.group(1).replace('\\', '/'), int(m.group(2)), int(m.group(3))))
    return first, second


def tally(layout, reads):
    """kind -> [bytes read, distinct bytes], archive -> bytes, member -> bytes, other files -> bytes."""
    kinds = collections.defaultdict(lambda: [0, 0])
    by_arch = collections.Counter()
    by_member = collections.Counter()
    seen = collections.defaultdict(list)  # (archive) -> ranges, for the distinct bytes
    data = os.path.abspath(layout.data).replace('\\', '/').rstrip('/') + '/'
    for path, off, n in reads:
        if not path.startswith(data):
            continue  # saves, settings, /proc: not game data
        rel = path[len(data):]
        if rel in layout.arch:
            by_arch[rel] += n
            seen[rel].append((off, off + n))
            for name, o in layout.split(rel, off, off + n):
                k = 'archive indexes' if name == '<index>' else kind(rel, name)
                kinds[k][0] += o
                by_member[(rel, name)] += o
        else:
            by_arch[rel] += n
            kinds['other'][0] += n
            seen[rel].append((off, off + n))
            by_member[(rel, '')] += n
    # distinct bytes per kind: merge each archive's ranges, then split again
    for rel, rs in seen.items():
        rs.sort()
        merged = []
        for s, e in rs:
            if merged and s <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], e)
            else:
                merged.append([s, e])
        for s, e in merged:
            if rel in layout.arch:
                for name, o in layout.split(rel, s, e):
                    kinds['archive indexes' if name == '<index>' else kind(rel, name)][1] += o
            else:
                kinds['other'][1] += e - s
    return kinds, by_arch, by_member


def on_disk(layout):
    """kind -> stored bytes over the whole data set."""
    out = collections.Counter()
    for a, (m, idx) in layout.arch.items():
        for name, s, e in m:
            out[kind(a, name)] += e - s
        for s, e in idx:
            out['archive indexes'] += e - s
    return out


def measure(logs, data):
    layout = Layout(data)
    disk = on_disk(layout)
    kinds_all = sorted(set(disk) | {'other'})
    opened = set()
    cols = []
    for log in logs:
        boot, load = phases(log)
        name = os.path.basename(os.path.dirname(log)) or log
        k1, a1, _ = tally(layout, boot)
        k2, a2, m2 = tally(layout, load)
        opened |= set(a1) | set(a2)
        cols.append((name, k1, k2, a1, a2, m2))
    print('MB read (distinct MB in brackets): boot + title menu | the load and play after it')
    head = f'{"kind":24} {"on disk":>8} ' + ' '.join(f'{c[0]:>24}' for c in cols)
    print(head)
    for k in kinds_all + ['TOTAL']:
        def cell(t):
            if k == 'TOTAL':
                return sum(v[0] for v in t.values()), sum(v[1] for v in t.values())
            return t.get(k, [0, 0])
        row = f'{k:24} {(sum(disk.values()) if k == "TOTAL" else disk.get(k, 0)) / MB:8.1f} '
        for _, k1, k2, *_ in cols:
            a, b = cell(k1), cell(k2)
            row += f' {a[0] / MB:5.1f} ({a[1] / MB:4.1f}) |{b[0] / MB:5.1f} ({b[1] / MB:4.1f})'
        print(row)
    never = [a for a in sorted(os.listdir(data)) if os.path.isfile(os.path.join(data, a)) and a not in opened]
    print('\nnever read in these runs:',
          ', '.join(f'{a} ({os.path.getsize(os.path.join(data, a)) / MB:.1f} MB)' for a in never))
    print('\nper archive, MB read after the load starts:')
    for name, k1, k2, a1, a2, m2 in cols:
        top = ', '.join(f'{a} {n / MB:.1f}' for a, n in a2.most_common(12) if n >= 0.05 * MB)
        print(f'  {name}: {top}')
    return cols


# ---- build ----------------------------------------------------------------------------------
#
# Textures. The 3DS's profiles (GPU_0 "Mipmaps to skip" 2, MEM_256 "Max texture size" 2) make the
# game drop the largest mip levels of a texture at load (CustomTexturePolicy::getMipmapCountToSkip,
# below): it reads the whole PVR (deflated in the zip, so inflated whole) and uploads from a lower
# level on. The slim copy stores each mipmapped PVR from that level on, and the slim
# GameOptions.json sets both values to 0, so the game takes the stored levels as they are: the
# same uploads, a sixteenth of the bytes or less to read and inflate.

def skip_options(w, h, mips, name, skip=2, max_size=2):
    """CustomTexturePolicy::getMipmapCountToSkip with the GPU_0 / MEM_256 values (`skip`: DeviceOptions
    +0x44, `max_size`: +0x40; the "GPU_0.xml" profile name with memory type 0): none for a texture
    without mip levels or (skip > 1) of 64 x 64 or less (skip <= 1: 256 x 256 or less); otherwise
    `skip`, then one more while the larger side is still at least the limit (256 for max_size 2,
    512 for 1; twice that for names containing "mc_" or "earth_main"). (g_bIsAlphaTexture would add
    one; it is not set: the split alpha maps follow the same rule.)"""
    if mips <= 1:
        return 0
    if skip > 1 and w <= 64 and h <= 64 or skip <= 1 and w <= 256 and h <= 256:
        return 0
    if max_size > 0:
        lower = name.lower()
        limit = 512 if max_size == 1 else 256
        if 'mc_' in lower or 'earth_main' in lower:
            limit *= 2
        side = max(w, h) >> skip
        while side >= limit:
            side >>= 1
            skip += 1
    return skip


def levels_skipped(w, h, mips, name):
    """How many levels the game drops: one less than getMipmapCountToSkip returns, never below 0
    (checked against the uploads of 517 textures in the PC traces of the title menu, chapters 0, 3,
    5 and a local match: all of them; the 6 others are single-colour images whose levels equal
    other textures' levels)."""
    return max(0, skip_options(w, h, mips, name) - 1)


def pvr_levels(pvr):
    """PVR v2 (52-byte header): (width, height, level count, [(offset, size)] of each level)."""
    hsize, h, w, mips, flags, dsize, bpp = struct.unpack_from('<7I', pvr)
    fmt, count = flags & 0xff, (mips + 1 if flags & 0x100 else 1)
    out, off = [], hsize
    for k in range(count):
        lw, lh = max(1, w >> k), max(1, h >> k)
        n = (max(1, (lw + 3) // 4) * max(1, (lh + 3) // 4) * 8) if fmt == 0x36 else lw * lh * bpp // 8
        out.append((off, n))
        off += n
    return w, h, count, out


EXTRA_MIN = 32  # --mips: levels beyond the game's own are dropped while the larger side stays at least this


def slim_pvr(pvr, name, extra=0):
    """The PVR from the first level the game uploads, or `extra` levels lower (while the larger side
    stays at least EXTRA_MIN): width, height, mip count and data size in the header adjusted (the
    loader checks that the mip count is log2 of the larger side), the rest as it was."""
    if pvr[44:48] != b'PVR!':
        return pvr
    w, h, count, lv = pvr_levels(pvr)
    k = levels_skipped(w, h, count, name)
    for _ in range(extra if count > 1 else 0):
        if k + 1 < count and max(w, h) >> (k + 1) >= EXTRA_MIN:
            k += 1
    if not k:
        return pvr
    head = bytearray(pvr[:52])
    data = pvr[lv[k][0]:lv[-1][0] + lv[-1][1]]
    struct.pack_into('<3I', head, 4, max(1, h >> k), max(1, w >> k), count - 1 - k)
    struct.pack_into('<I', head, 20, len(data))
    return bytes(head) + data


def copy_info(zi):
    n = zipfile.ZipInfo(zi.filename, zi.date_time)
    n.compress_type, n.external_attr, n.create_system = zi.compress_type, zi.external_attr, zi.create_system
    return n


def slim_etc(blob, name, extra=0):
    """An .etc member: one PVR, or a SPLIT zip (stored) of rgb.pvr and alpha.pvr."""
    if blob[:4] != b'PK\x03\x04':
        return slim_pvr(blob, name, extra)
    src = zipfile.ZipFile(io.BytesIO(blob))
    out = io.BytesIO()
    with zipfile.ZipFile(out, 'w') as dst:
        for zi in src.infolist():
            data = src.read(zi)
            if zi.filename.lower().endswith('.pvr'):
                data = slim_pvr(data, name, extra)
            dst.writestr(copy_info(zi), data, compress_type=zi.compress_type)
    return out.getvalue()


def game_options(src, dst):
    """GameOptions.json with every profile's "Mipmaps to skip" and "Max texture size" at 0 (the 3DS
    always takes GPU_0 and MEM_256): the stored textures are already the size those would make."""
    with open(src, 'rb') as f:
        text = f.read().decode('utf-8')
    if not re.search(r'"GPU_0"\s*:\s*\{[^}]*"Mipmaps to skip"\s*:\s*2', text) or \
            not re.search(r'"MEM_256"\s*:\s*\{[^}]*"Max texture size"\s*:\s*2', text):
        sys.exit(f'{src}: not the GPU_0 / MEM_256 texture options this slim set was made for')
    new = re.sub(r'("Mipmaps to skip"|"Max texture size")(\s*:\s*)\d+', r'\g<1>\g<2>0', text)
    if os.path.exists(dst):
        os.remove(dst)
    with open(dst, 'wb') as f:
        f.write(new.encode('utf-8'))


# Sprite sets. A menu image comes in sets for the screen classes (X.bsprite + X.etc for 480x320,
# X_960x640.*, X_1024x768.*, ...: each set with its own module coordinates); CSpriteManager::
# GetSpriteFilename lists the files matching X and takes the set for the screen, with the scale from
# its size to the layout's. The 3DS's 400x240 is in the 960x640 class, but its interface layer is
# drawn at half size (a 512x512 target for the 1024x1024 layer), so the 480x320 sets carry all the
# detail it shows. --ui leaves out the other sets of the loading screens (LOADING_*: 17 images of
# 1024x1024, the largest UI files a load reads) and the splash screens, so the game takes the 480x320
# one, scaled up (a loading screen at 400x240: mean difference 3.7 of 255). Not the other families
# with a 480x320 set: the match setup's level image (MP_*, multiplayer_locations_a) stays empty with
# its 480x320 set, and images the menus name with their resolution (menu_level_images_nova3_960x640,
# flags_960x640) are needed as they are (without them the menus do not load). Halving an image in
# place breaks its sprites: the game computes their texture coordinates from the texture's own size.
UI_ARCHIVES = ('sprites.gla', 'sprites4.gla')
UI_FAMILIES = ('loading_', 'splash')
RES_SET = re.compile(r'^(.*)_(\d+)x(\d+)(\.[a-z0-9]+)$', re.I)


def ui_dropped(archive, name, names):
    """A member of another screen class's set of a family whose 480x320 set is in the archive."""
    m = RES_SET.match(name)
    return archive in UI_ARCHIVES and m is not None and m.group(1).lower().startswith(UI_FAMILIES) and \
        (m.group(1) + m.group(4)).lower() in names


# Unreachable content. Every .gla is mounted at boot, but the co-op and sandbox locations
# (l_101_coop*, l_999_sandbox*; levels.gla/locations.bin) have no level file: their levels
# (CO_OP_1_Therrius, Ship_02_Physics, ...) are not in levels.gla, so they cannot be loaded. Some of
# their files are used elsewhere all the same (chapter 3's teleporter models take dm2_coop_lm from
# l_101_coop4.gla, SF3_Skybox.bdae sf2_skybox, multiplayer map 2 SF_concrete_redline_wall, the
# library SpotLightMap from l_999_sandbox4.gla; levels.gla/gol.bin, the object templates, names
# co-op models and animations), so --unreachable keeps every member of theirs that something reachable
# names (<name>.<bdae|etc|tga|png|pvr|irr|col|glsl> in any member of the other archives but textures
# and irradiance, or in the library; then in the members so kept, until nothing is added; X_low.bdae
# with X.bdae) and leaves the rest out (~95% of them). A name in both is taken from the archive
# mounted first (chapter 3 reads TS_prop_skybox.bdae from l_001_terraforming_ship_stream.gla, not
# l_101_coop_stream.gla's different copy), so dropping these copies changes nothing either.
# Irradiance files are named after the level that loads them (CLevel::LoadNextObject:
# "<level>_<object id>.irr"); those of levels without a level file go too (all of l_101_coop6.gla and
# l_999_sandbox6.gla, San_Francisco_1_NEW and SF_Terraformed in l_002_SanFrancisco6.gla).
UNREACHABLE = ('l_101_coop', 'l_999_sandbox')
NAMED = re.compile(rb'([A-Za-z0-9_\-.]{2,80})\.(?:bdae|etc|tga|png|pvr|irr|col|glsl)', re.I)


def level_names(data):
    """The levels the game can load: the .lvc files in levels.gla (lower case, no extension)."""
    with zipfile.ZipFile(os.path.join(data, 'levels.gla')) as z:
        return {n[:-4].lower() for n in z.namelist() if n.lower().endswith('.lvc')}


def irr_unreachable(name, levels):
    m = re.match(r'^(.*)_\d+\.irr$', name, re.I)
    return m is not None and m.group(1).lower() not in levels


def archive_members(path):
    """[(name, reader)] of a zip or OBB archive."""
    with open(path, 'rb') as f:
        zipped = f.read(2) == b'PK'
    if zipped:
        z = zipfile.ZipFile(path)
        return [(n, lambda n=n: z.read(n)) for n in z.namelist()]
    o = obb.Obb(path)
    return [(e[0], lambda e=e: o.read(e)) for e in o.entries]


def named_in(blob):
    return {os.path.basename(m.group(1).decode('latin1')).lower() for m in NAMED.finditer(blob)}


def reachable(data):
    """The members of the UNREACHABLE archives that reachable data names: {(archive, member)}."""
    named, closed = set(), {}
    with open(os.path.join(data, 'libGloftNAHM.so'), 'rb') as f:
        named |= named_in(f.read())
    for a in sorted(os.listdir(data)):
        if not a.endswith('.gla') or a.startswith('sounds_'):
            continue
        try:
            members = archive_members(os.path.join(data, a))
        except (zipfile.BadZipFile, struct.error, ValueError):
            continue
        if a.startswith(UNREACHABLE):
            closed[a] = members
            continue
        for n, read in members:
            if not n.lower().endswith(('.etc', '.pvr', '.irr', '.ttf')):
                named |= named_in(read())
    keep, grown = set(), True
    while grown:
        grown = False
        for a, members in closed.items():
            for n, read in members:
                stem = os.path.splitext(os.path.basename(n))[0].lower()
                if (a, n) not in keep and (stem in named or stem.endswith('_low') and stem[:-4] in named):
                    keep.add((a, n))
                    grown = True
                    if not n.lower().endswith(('.etc', '.pvr', '.irr')):
                        named |= named_in(read())
    return keep


# Low-detail twins. With GPU_0's "Geometry level" 1, CCustomFileSystem::createAndOpenFile (the way
# the engine opens every file) tries X_low.bdae first whenever it is asked for X.bdae, and takes
# X.bdae only if there is no X_low.bdae; nothing else reads models (fastCreateAndOpenFile and
# customCreateAndOpenFile serve data files and sounds). So X.bdae is never read where an X_low.bdae
# exists. --twins stores the _low model's content under both names in a zip (its reader walks the
# local headers, so two names cannot share one copy) and, in an OBB (*_stream.gla, whose reader opens
# an entry as a window of the file at its offset and size), points X.bdae's entry at X_low.bdae's
# bytes. Should X.bdae be read after all, it is a valid model.

def twin_of(name, names):
    """The member whose content goes under `name` (names: lower case -> name): its _low twin, or itself."""
    low = name.lower()
    if low.endswith('.bdae') and not low.endswith('_low.bdae'):
        return names.get(low[:-5] + '_low.bdae', name)
    return name


def slim_obb(src, dst, keep, twins):
    """An OBB (*_stream.gla) with the entries keep(name) is false for left out and, with `twins`,
    each X.bdae pointing at its X_low.bdae's bytes; False when nothing changes (it is linked instead).
    The header's first word is kept, the names encrypted again (obb.encode_xor32), one after another
    with a NUL each (CCustomPakReader::scanLocalHeader takes a name's length from the next one's
    offset), the data in its order."""
    with open(src, 'rb') as f:
        blob = f.read()
    first, _, _, count = struct.unpack_from('>4I', blob)
    table = [struct.unpack_from('>4I', blob, 16 + 16 * i) for i in range(count)]
    entries = obb.Obb(src).entries
    rows = [i for i, e in enumerate(entries) if keep(e[0])]
    names = {entries[i][0].lower(): entries[i][0] for i in rows}
    index = {entries[i][0]: i for i in rows}
    source = {}
    for i in rows:
        j = index[twin_of(entries[i][0], names)] if twins else i
        source[i] = j if not (table[i][3] | table[j][3]) & 1 else i
    if len(rows) == count and all(source[i] == i for i in rows):
        return False
    name_blob, name_at = bytearray(), []
    for i in rows:
        name_at.append(len(name_blob))
        name_blob += obb.encode_xor32(entries[i][0].encode('latin1')) + b'\0'
    table_end = 16 + 16 * len(rows)
    out = bytearray(struct.pack('>4I', first, table_end + len(name_blob), table_end, len(rows)))
    out += bytes(16 * len(rows)) + name_blob
    data_at = {}
    for j in sorted(set(source.values()), key=lambda j: table[j][0]):
        data_at[j] = len(out)
        out += blob[table[j][0]:table[j][0] + table[j][1]]
    for k, i in enumerate(rows):
        j = source[i]
        struct.pack_into('>4I', out, 16 + 16 * k, data_at[j], table[j][1], name_at[k], table[j][3])
    if os.path.exists(dst):
        os.remove(dst)
    with open(dst + '.tmp', 'wb') as f:
        f.write(out)
    os.replace(dst + '.tmp', dst)
    return True


# Zip archives. The game mounts a zip .gla by walking its local headers one after another
# (glitch::io::CZipReader::scanLocalHeader; the central directory is not read) and inflates a member
# whole when it is opened. The slim set's zips are written here, members in their order: a member
# whose content is unchanged keeps its stored bytes, a changed one is deflated at zlib level 9; with
# --recompress every deflated member is deflated again by zopfli and the smaller kept (the same
# format, inflated as fast). No data descriptors or extra fields; a standard central directory.
ZOPFLI_ITERATIONS = 5  # 15 gains under 0.1% more for three times the time


def raw_member(f, zi):
    """A zip member's bytes as stored."""
    f.seek(zi.header_offset)
    n, m = struct.unpack('<HH', f.read(30)[26:])
    f.seek(zi.header_offset + 30 + n + m)
    return f.read(zi.compress_size)


def deflate(data, zopfli=False):
    z = zlib.compressobj(9, zlib.DEFLATED, -15)
    out = z.compress(data) + z.flush()
    if zopfli and data:
        import zopfli.zopfli  # pip install zopfli
        best = zopfli.zopfli.compress(data, numiterations=ZOPFLI_ITERATIONS)[2:-4]  # the zlib wrapper off
        if len(best) < len(out):
            out = best
    return out


_zips = {}


def member_job(job):
    """(archive, member, extra mip levels, zopfli) -> (changed, method, CRC, size, stored bytes): the
    member's content as the slim set stores it (run in a pool)."""
    src, name, extra, zopfli = job
    z = _zips.get(src) or _zips.setdefault(src, zipfile.ZipFile(src))
    zi = z.getinfo(name)
    with open(src, 'rb') as f:
        raw = raw_member(f, zi)
    data = z.read(zi)
    new = slim_etc(data, name, extra) if name.lower().endswith(('.etc', '.pvr')) else data
    if zi.compress_type != zipfile.ZIP_DEFLATED:
        return new != data, zi.compress_type, zlib.crc32(new), len(new), new if new != data else raw
    if new == data and not zopfli:
        return False, zi.compress_type, zi.CRC, zi.file_size, raw
    blob = deflate(new, zopfli)
    if new == data and len(raw) <= len(blob):
        blob = raw
    return new != data or blob != raw, zi.compress_type, zlib.crc32(new), len(new), blob


def write_zip(dst, members):
    """members: [(ZipInfo, method, CRC, size, stored bytes)] -> a zip file."""
    central, at = [], 0
    with open(dst + '.tmp', 'wb') as f:
        for zi, method, crc, size, blob in members:
            name = zi.filename.encode('utf-8')
            flags = 0 if name.isascii() else 0x800
            y, mo, d, hh, mm, ss = zi.date_time
            t, day = hh << 11 | mm << 5 | ss // 2, (y - 1980) << 9 | mo << 5 | d
            f.write(struct.pack('<IHHHHHIIIHH', 0x04034b50, 20, flags, method, t, day, crc, len(blob), size,
                                len(name), 0) + name)
            f.write(blob)
            central.append(struct.pack('<IHHHHHHIIIHHHHHII', 0x02014b50, zi.create_system << 8 | 20, 20, flags,
                                       method, t, day, crc, len(blob), size, len(name), 0, 0, 0, 0,
                                       zi.external_attr, at) + name)
            at += 30 + len(name) + len(blob)
        cd = b''.join(central)
        f.write(cd + struct.pack('<IHHHHIIH', 0x06054b50, 0, 0, len(central), len(central), len(cd), at, 0))
    if os.path.exists(dst):
        os.remove(dst)
    os.replace(dst + '.tmp', dst)


def slim_zip(src, dst, opts, levels, keep, pool):
    """Writes a zip archive's slim copy: textures from opts.mips levels below the game's own, members
    left out (--ui, --unreachable), _low twins (--twins), zopfli (--recompress). False when nothing in
    it changes (it is linked instead)."""
    archive = os.path.basename(src)
    z = zipfile.ZipFile(src)
    names = {zi.filename.lower(): zi.filename for zi in z.infolist()}
    plan, dropped = [], 0
    for zi in z.infolist():
        if opts.ui and ui_dropped(archive, zi.filename, names) or opts.unreachable and (
                irr_unreachable(zi.filename, levels) or
                archive.startswith(UNREACHABLE) and (archive, zi.filename) not in keep):
            dropped += 1
            continue
        plan.append((zi, twin_of(zi.filename, names) if opts.twins else zi.filename))
    textures = any(n.endswith(('.etc', '.pvr')) for n in names)
    if not dropped and not opts.recompress and not textures and all(c == zi.filename for zi, c in plan):
        return False
    jobs = sorted({(src, c, opts.mips, opts.recompress) for _, c in plan}, key=lambda j: -z.getinfo(j[1]).file_size)
    done = dict(zip(jobs, pool.map(member_job, jobs, chunksize=1)))
    members, changed = [], dropped > 0
    for zi, c in plan:
        ch, method, crc, size, blob = done[(src, c, opts.mips, opts.recompress)]
        changed |= ch or c != zi.filename
        members.append((zi, method, crc, size, blob))
    if not changed:
        return False
    write_zip(dst, members)
    return True


SKIP = {'hd_textures.pak', 'pica_textures.pak'}  # the PC's pack; the 3DS's is made again from the slim data


def link(src, dst):
    if os.path.exists(dst):
        os.remove(dst)
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def build(data, out, opts):
    import multiprocessing
    os.makedirs(out, exist_ok=True)
    levels = level_names(data)
    keep = reachable(data) if opts.unreachable else set()
    before = after = 0
    with multiprocessing.Pool(opts.jobs) as pool:
        for n in sorted(os.listdir(data)):
            src, dst = os.path.join(data, n), os.path.join(out, n)
            if not os.path.isfile(src) or n in SKIP:
                continue
            with open(src, 'rb') as f:
                zipped = f.read(2) == b'PK'
            note = None
            if n == 'GameOptions.json':
                game_options(src, dst)
                note = 'texture options at 0'
            elif n.endswith('.gla') and zipped and slim_zip(src, dst, opts, levels, keep, pool):
                note = 'rewritten'
            elif n.endswith('_stream.gla') and not zipped and slim_obb(
                    src, dst, lambda m: not (opts.unreachable and n.startswith(UNREACHABLE)) or (n, m) in keep,
                    opts.twins):
                note = 'entries left out or shared'
            else:
                link(src, dst)
            before += os.path.getsize(src)
            after += os.path.getsize(dst)
            if note:
                print(f'{n:36} {os.path.getsize(src) / MB:8.1f} -> {os.path.getsize(dst) / MB:7.1f} MB  ({note})',
                      flush=True)
    for n in os.listdir(out):  # left from an earlier build
        if not os.path.exists(os.path.join(data, n)) and n not in ('pica_textures.pak', INDEX, LIB_Z) and \
                not (n.endswith('.gla' + PACK_EXT) and os.path.exists(os.path.join(data, n[:-len(PACK_EXT)]))):
            os.remove(os.path.join(out, n))
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '3ds_glreplay'))
    import picapack
    pak = os.path.join(out, 'pica_textures.pak')
    picapack.build(pak, out)
    after += os.path.getsize(pak)
    write_index(out)
    write_lib(out)
    after += os.path.getsize(os.path.join(out, INDEX)) + os.path.getsize(os.path.join(out, LIB_Z))
    if opts.pack:
        pack_all(out, opts.recompress, opts.jobs)
    else:
        for n in os.listdir(out):
            if n.endswith('.gla' + PACK_EXT):
                os.remove(os.path.join(out, n))
    print(f'{out}: {after / MB:.1f} MB (the game files without hd_textures.pak and pica_textures.pak: '
          f'{before / MB:.1f} MB); the console\'s set: {console_size(out) / MB:.1f} MB')


def console_set(data):
    """The files of a set that go on the console (tools/3ds_package.py, tools/3ds_run.py --deploy): X
    is left out where X.z is there (libGloftNAHM.so.z, the packed archives)."""
    names = set(os.listdir(data))
    return [n for n in sorted(names) if n + PACK_EXT not in names]


def console_size(data):
    return sum(os.path.getsize(os.path.join(data, n)) for n in console_set(data) if os.path.isfile(os.path.join(data, n)))


# ---- the mount index -------------------------------------------------------------------------

INDEX = 'mount.idx'


def write_index(data, out=None):
    """mount.idx: for each zip archive in `data`, its size and the bytes [h, h + 1 KB) at each local
    header h (clipped to the file, merged where they overlap), which the game reads to mount it.
    "N3MI", version 1, archive count, directory size (u32 each); the directory: per archive u16
    name length, the name, u32 size, u32 offset of its part, u32 part size; a part: u32 n, n x
    (u32 offset, u32 length), then the n ranges' bytes (runtime/libc_io.cpp reads it)."""
    out = out or os.path.join(data, INDEX)
    parts = []
    for n in sorted(os.listdir(data)):
        p = os.path.join(data, n)
        if not n.endswith('.gla') or not os.path.isfile(p):
            continue
        with open(p, 'rb') as f:
            if f.read(4) != b'PK\x03\x04':
                continue
        size = os.path.getsize(p)
        ranges = []
        for a, b in sorted((i.header_offset, min(i.header_offset + 1024, size)) for i in zipfile.ZipFile(p).infolist()):
            if ranges and a <= ranges[-1][1]:
                ranges[-1][1] = max(ranges[-1][1], b)
            else:
                ranges.append([a, b])
        table, blob = [struct.pack('<I', len(ranges))], []
        with open(p, 'rb') as f:
            for a, b in ranges:
                f.seek(a)
                blob.append(f.read(b - a))
                table.append(struct.pack('<II', a, b - a))
        parts.append((n.encode(), size, b''.join(table + blob)))
    directory = b''.join(struct.pack('<H', len(n)) + n + bytes(12) for n, _, _ in parts)
    at, entries = 16 + len(directory), []
    for n, size, part in parts:
        entries.append(struct.pack('<H', len(n)) + n + struct.pack('<III', size, at, len(part)))
        at += len(part)
    with open(out + '.tmp', 'wb') as f:
        f.write(b'N3MI' + struct.pack('<III', 1, len(parts), len(directory)))
        f.write(b''.join(entries))
        for _, _, part in parts:
            f.write(part)
    os.replace(out + '.tmp', out)
    print(f'{out}: {len(parts)} archives, {os.path.getsize(out) / MB:.1f} MB')


LIB_Z = 'libGloftNAHM.so.z'
LIB_PIECE = 256 * 1024


def write_lib(data, out=None):
    """libGloftNAHM.so.z: what the 3DS's loader reads of the library (runtime/native_3ds.cpp), 43% of
    its size: the ELF header and program headers as they are, then each PT_LOAD segment's bytes in
    raw deflate pieces of 256 KB, which the loader inflates while the next ones are read. "N3LZ",
    version 1, the library's size, the piece size, the header size (u32 each), the header bytes
    (padded to 4); u32
    segment count, per segment u32 file offset, size, first piece, piece count; u32 piece count,
    per piece u32 offset in this file and compressed size; then the pieces."""
    import zlib
    so = os.path.join(data, 'libGloftNAHM.so')
    out = out or os.path.join(data, LIB_Z)
    with open(so, 'rb') as f:
        lib = f.read()
    phoff, = struct.unpack_from('<I', lib, 0x1c)
    phnum, = struct.unpack_from('<H', lib, 0x2c)
    head = lib[:phoff + 32 * phnum]
    head += bytes(-len(head) % 4)
    segments, pieces = [], []
    for i in range(phnum):
        kind, off, _, _, filesz = struct.unpack_from('<5I', lib, phoff + 32 * i)
        if kind != 1:  # PT_LOAD
            continue
        segments.append((off, filesz, len(pieces), -(-filesz // LIB_PIECE)))
        for at in range(off, off + filesz, LIB_PIECE):
            z = zlib.compressobj(9, zlib.DEFLATED, -15)
            pieces.append(z.compress(lib[at:min(at + LIB_PIECE, off + filesz)]) + z.flush())
    tables = struct.pack('<I', len(segments)) + b''.join(struct.pack('<4I', *s) for s in segments)
    at = 20 + len(head) + len(tables) + 4 + 8 * len(pieces)
    table = [struct.pack('<I', len(pieces))]
    for p in pieces:
        table.append(struct.pack('<II', at, len(p)))
        at += len(p)
    with open(out + '.tmp', 'wb') as f:
        f.write(b'N3LZ' + struct.pack('<4I', 1, len(lib), LIB_PIECE, len(head)) + head + tables + b''.join(table))
        f.write(b''.join(pieces))
    os.replace(out + '.tmp', out)
    print(f'{out}: {len(segments)} segments, {len(pieces)} pieces, {len(lib) / MB:.1f} -> {os.path.getsize(out) / MB:.1f} MB')


# ---- packed archives ------------------------------------------------------------------------
#
# The OBB-format archives (*_stream.gla: animations and models; sounds_*.gla: ADPCM and Musepack) are
# stored uncompressed: the game's reader (CCustomPakReader) opens an entry as a window of the file and
# reads it in pieces, without decompression. The console's set holds them as X.gla.z, deflated in
# blocks, and the 3DS's file layer (runtime/libc_io.cpp) serves the game's reads of X.gla from them:
# the same bytes, the original's size. Block sizes (measured over the slim set and the read traces of
# the title menu, chapters 0, 3 and 5 and a match): the streams 64 KB (0.53 of their size; 32 KB:
# 0.56, 128 KB: 0.52 but twice the bytes unpacked per load), the sounds 32 KB (0.80-0.92 at any size;
# the game streams them 1 KB at a time). An archive is packed only when that saves over 5%
# (sounds_hi_p4.gla, Musepack: 0.2%). With zopfli (--recompress) 2-5% smaller than zlib's level 9.
#
# "N3BZ", version 1, the original's size, the block size, the block count n, the CRC-32 of the
# original (u32 each, little endian); n + 1 u32 offsets (block k is [at[k], at[k + 1]) of this file);
# n u32 CRC-32s of each block's original bytes; then the blocks: raw deflate, or stored as they are
# where deflate does not make a block smaller (its stored size is then its original size).

PACK_EXT = '.z'
PACK_MIN_GAIN = 0.05
PACK_HEAD = 24


def pack_block_size(name):
    """The block size an archive is packed with, or None when it is not packed (not OBB-format)."""
    if name.startswith('sounds_') and name.endswith('.gla'):
        return 32 * 1024
    if name.endswith('_stream.gla'):
        return 64 * 1024
    return None


def pack_job(job):
    """(archive, offset, length, zopfli) -> (stored bytes, CRC-32 of the original bytes) of one block."""
    src, at, n, zopfli = job
    with open(src, 'rb') as f:
        f.seek(at)
        data = f.read(n)
    blob = deflate(data, zopfli)
    return (blob if len(blob) < len(data) else data), zlib.crc32(data)


def pack_header(path):
    """(original size, block size, block count, CRC-32, offsets, block CRCs) of a container, or None."""
    with open(path, 'rb') as f:
        head = f.read(PACK_HEAD)
        if len(head) < PACK_HEAD or head[:4] != b'N3BZ':
            return None
        version, size, block, n, crc = struct.unpack_from('<5I', head, 4)
        if version != 1:
            return None
        tables = f.read(4 * (2 * n + 1))
    at = struct.unpack_from(f'<{n + 1}I', tables)
    return size, block, n, crc, at, struct.unpack_from(f'<{n}I', tables, 4 * (n + 1))


def write_pack(src, dst, block, pool, zopfli):
    """Writes the container dst of the archive src; an up-to-date one (the same size, block size and
    CRC-32) is kept. None (and no dst) when packing saves PACK_MIN_GAIN or less."""
    size = os.path.getsize(src)
    with open(src, 'rb') as f:
        crc = zlib.crc32(f.read())
    old = pack_header(dst) if os.path.exists(dst) else None
    if old and old[:2] == (size, block) and old[3] == crc and old[4][-1] == os.path.getsize(dst):
        return 'kept'
    jobs = [(src, at, min(block, size - at), zopfli) for at in range(0, size, block)]
    blocks = pool.map(pack_job, jobs, chunksize=4)
    n = len(blocks)
    at, offsets = PACK_HEAD + 4 * (2 * n + 1), []
    for blob, _ in blocks:
        offsets.append(at)
        at += len(blob)
    offsets.append(at)
    if os.path.exists(dst):
        os.remove(dst)
    if at > size * (1 - PACK_MIN_GAIN):
        return None
    with open(dst + '.tmp', 'wb') as f:
        f.write(b'N3BZ' + struct.pack('<5I', 1, size, block, n, crc))
        f.write(struct.pack(f'<{n + 1}I', *offsets) + struct.pack(f'<{n}I', *(c for _, c in blocks)))
        for blob, _ in blocks:
            f.write(blob)
    os.replace(dst + '.tmp', dst)
    return 'packed'


def pack_all(data, zopfli=True, jobs=None):
    """Packs every OBB-format stream and sound archive of `data` that gains over PACK_MIN_GAIN."""
    import multiprocessing
    total = [0, 0]
    with multiprocessing.Pool(jobs or max(1, min(12, (os.cpu_count() or 2) - 2))) as pool:
        for n in sorted(os.listdir(data)):
            src, block = os.path.join(data, n), pack_block_size(n)
            if not block or not os.path.isfile(src):
                continue
            with open(src, 'rb') as f:
                if f.read(2) == b'PK':  # (a zip: deflated already, and the mount index's)
                    continue
            how = write_pack(src, src + PACK_EXT, block, pool, zopfli)
            size = os.path.getsize(src)
            packed = os.path.getsize(src + PACK_EXT) if how else size
            total[0] += size
            total[1] += packed
            print(f'{n:36} {size / MB:8.1f} -> {packed / MB:7.1f} MB  {packed / max(1, size):.3f}  '
                  f'({how or "not packed: gains 5% or less"}, {block // 1024} KB blocks)', flush=True)
    print(f'packed archives: {total[0] / MB:.1f} -> {total[1] / MB:.1f} MB')


def verify(data):
    """Unpacks every X.gla.z of `data` and compares it with X.gla byte for byte (and the CRC-32s)."""
    bad = 0
    for n in sorted(os.listdir(data)):
        if not n.endswith('.gla' + PACK_EXT):
            continue
        path, plain = os.path.join(data, n), os.path.join(data, n[:-len(PACK_EXT)])
        head = pack_header(path)
        problems = []
        if not head:
            problems.append('not a container')
        else:
            size, block, count, crc, at, crcs = head
            with open(path, 'rb') as f:
                blob = f.read()
            if at[-1] != len(blob) or count != -(-size // block):
                problems.append('tables do not match the file')
            out = bytearray()
            for k in range(count):
                n_k = min(block, size - k * block)
                stored = blob[at[k]:at[k + 1]]
                part = stored if len(stored) == n_k else zlib.decompress(stored, -15)
                if len(part) != n_k or zlib.crc32(part) != crcs[k]:
                    problems.append(f'block {k} differs')
                    break
                out += part
            if zlib.crc32(out) != crc or len(out) != size:
                problems.append('CRC-32 of the whole differs')
            if os.path.exists(plain):
                with open(plain, 'rb') as f:
                    if f.read() != out:
                        problems.append(f'differs from {os.path.basename(plain)}')
            else:
                problems.append(f'no {os.path.basename(plain)} to compare with (CRC-32s checked)')
        print(f'{n:38} ' + ('; '.join(problems) if problems else
                            f'identical to {n[:-len(PACK_EXT)]} ({len(out):,} bytes, {count} blocks of {block // 1024} KB, '
                            f'{len(blob) / len(out):.3f})'))
        bad += any(not p.startswith('no ') for p in problems)
    print('verify:', 'all identical' if not bad else f'{bad} containers differ')
    return bad == 0


# ---- uploads --------------------------------------------------------------------------------

def uploads(traces):
    """Compares the texture uploads of two GL traces (NOVA3_GLTRACE): every glCompressedTexImage2D /
    glTexImage2D as (level, format, width, height, CRC-32 of the data), in order and as a multiset."""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '3ds_glreplay'))
    import gltrace
    import zlib
    seqs = []
    for t in traces:
        seq = []
        for name, w, b in gltrace.records(t):
            if name in ('glCompressedTexImage2D', 'glTexImage2D'):
                seq.append((name, w[1], w[2], w[3], w[4], zlib.crc32(b[0]) if b and b[0] else None))
        seqs.append(seq)
        print(f'{t}: {len(seq)} uploads, {sum(1 for s in seq if s[1] == 0)} of level 0')
    a, b = seqs
    ca, cb = collections.Counter(a), collections.Counter(b)
    print(f'identical sequence: {a == b}; as multisets: {ca == cb} '
          f'(only in the first: {sum((ca - cb).values())}, only in the second: {sum((cb - ca).values())})')
    for x in list((ca - cb).elements())[:10]:
        print('  first only:', x)
    for x in list((cb - ca).elements())[:10]:
        print('  second only:', x)
    return a == b


def shots(dirs):
    """Compares the screenshots (*.png) two runs took at the same frames: mean absolute difference
    per channel (of 255) and the share of pixels that differ."""
    import numpy as np
    from PIL import Image
    a, b = dirs
    same = True
    for n in sorted(f for f in os.listdir(a) if f.endswith('.png')):
        if not os.path.exists(os.path.join(b, n)):
            continue
        x = np.asarray(Image.open(os.path.join(a, n)).convert('RGB'), dtype=np.int16)
        y = np.asarray(Image.open(os.path.join(b, n)).convert('RGB'), dtype=np.int16)
        d = np.abs(x - y)
        print(f'{n}: mean difference {d.mean():.3f}, pixels differing {100 * (d.max(axis=2) > 0).mean():.2f}%')
        same &= not d.any()
    return same


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('measure')
    p.add_argument('logs', nargs='+')
    p.add_argument('--data', default=DATA)
    p = sub.add_parser('build')
    p.add_argument('--data', default=DATA)
    p.add_argument('--out', default=OUT)
    p.add_argument('--mips', type=int, default=1, help='texture levels dropped below the uploaded ones')
    on = argparse.BooleanOptionalAction
    p.add_argument('--ui', action=on, default=True, help='the 480x320 sprite sets only, where the game picks the set')
    p.add_argument('--twins', action=on, default=True, help='X.bdae with the content of its X_low.bdae')
    p.add_argument('--unreachable', action=on, default=True, help='co-op and sandbox archives empty, orphan .irr out')
    p.add_argument('--recompress', action=on, default=True, help='zopfli for every deflated zip member')
    p.add_argument('--pack', action=on, default=True, help='the stream and sound archives packed (X.gla.z)')
    p.add_argument('--jobs', type=int, default=max(1, min(12, (os.cpu_count() or 2) - 2)))
    p = sub.add_parser('index')
    p.add_argument('dir', nargs='?', default=OUT)
    p = sub.add_parser('lib')
    p.add_argument('dir', nargs='?', default=OUT)
    p = sub.add_parser('pack')
    p.add_argument('dir', nargs='?', default=OUT)
    p.add_argument('--zopfli', action=on, default=True, help='zopfli for the blocks (2-5%% smaller, 3 minutes)')
    p = sub.add_parser('verify')
    p.add_argument('dir', nargs='?', default=OUT)
    p = sub.add_parser('uploads')
    p.add_argument('traces', nargs=2)
    p = sub.add_parser('shots')
    p.add_argument('dirs', nargs=2)
    args = ap.parse_args()
    if args.cmd == 'measure':
        measure(args.logs, args.data)
    elif args.cmd == 'build':
        build(args.data, args.out, args)
    elif args.cmd == 'index':
        write_index(args.dir)
    elif args.cmd == 'lib':
        write_lib(args.dir)
    elif args.cmd == 'pack':
        pack_all(args.dir, args.zopfli)
    elif args.cmd == 'verify':
        sys.exit(0 if verify(args.dir) else 1)
    elif args.cmd == 'uploads':
        sys.exit(0 if uploads(args.traces) else 1)
    elif args.cmd == 'shots':
        sys.exit(0 if shots(args.dirs) else 1)


if __name__ == '__main__':
    main()
