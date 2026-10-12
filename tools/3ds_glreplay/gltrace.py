"""Read a GL trace (runtime/gles.cpp, NOVA3_GLTRACE) and report what the game does.

Usage: gltrace.py programs <gltrace.bin> [out_dir]   every distinct linked program: its shaders' sources
                                                  (with Glitch's #defines), attributes and uniforms with
                                                  their locations and types; which recorded frames draw
                                                  with it, and how often
       gltrace.py stats <gltrace.bin>             call counts, data volume, per recorded frame
       gltrace.py cut <gltrace.bin> <out.bin> <frame,...>  only those recorded frames
       gltrace.py frames <gltrace.bin>           per recorded frame: draws, client vertex bytes, calls
       gltrace.py draws <gltrace.bin> [frame]     each draw: program, vertex sources, depth range, blending
       gltrace.py dump <gltrace.bin> [first] [n]  the records as text
"""
import collections
import hashlib
import os
import struct
import sys

FRAME, CLIENT = 0x1000, 0x1001
GL_TYPES = {0x1406: 'float', 0x8B50: 'vec2', 0x8B51: 'vec3', 0x8B52: 'vec4', 0x1404: 'int', 0x8B53: 'ivec2',
            0x8B54: 'ivec3', 0x8B55: 'ivec4', 0x8B56: 'bool', 0x8B5A: 'mat2', 0x8B5B: 'mat3', 0x8B5C: 'mat4',
            0x8B5E: 'sampler2D', 0x8B60: 'samplerCube'}


def records(path):
    """Yields (name, words, blobs) for each record."""
    with open(path, 'rb') as f:
        data = f.read()
    magic, version, count = struct.unpack_from('<3I', data, 0)
    assert magic == 0x5447334e, 'not a GL trace'
    pos = 12
    names = []
    for _ in range(count):
        n, = struct.unpack_from('<I', data, pos)
        names.append(data[pos + 4:pos + 4 + n].decode())
        pos += 4 + (n + 3) // 4 * 4
    names_by_id = dict(enumerate(names))
    names_by_id[FRAME] = 'FRAME'
    names_by_id[CLIENT] = 'CLIENT'
    while pos + 8 <= len(data):
        head, nw = struct.unpack_from('<2I', data, pos)
        pos += 8
        if pos + 4 * nw > len(data):
            break
        words = struct.unpack_from('<%dI' % nw, data, pos)
        pos += 4 * nw
        blobs = []
        cut = False
        for _ in range(head >> 16):
            if pos + 4 > len(data):
                cut = True
                break
            n, = struct.unpack_from('<I', data, pos)
            blobs.append(data[pos + 4:pos + 4 + n])
            pos += 4 + (n + 3) // 4 * 4
        if cut or pos > len(data):
            break  # a trace cut off by the end of the run
        yield names_by_id.get(head & 0xffff, '?%x' % (head & 0xffff)), words, blobs


def f32(w):
    return struct.unpack('<f', struct.pack('<I', w))[0]


def programs(path, out_dir=None):
    shaders = {}       # shader -> source
    attached = collections.defaultdict(list)
    linked = {}        # program -> key
    info = {}          # key -> {'sources', 'attribs', 'uniforms', 'locs'}
    current = None
    frame = None
    draws = collections.Counter()   # (key) -> draws in recorded frames
    frames_of = collections.defaultdict(set)
    for name, w, b in records(path):
        if name == 'glShaderSource':
            shaders[w[0]] = b[0].decode('latin-1')
        elif name == 'glAttachShader':
            attached[w[0]].append(w[1])
        elif name == 'glLinkProgram':
            srcs = tuple(shaders.get(s, '') for s in attached[w[0]])
            key = hashlib.sha1('\0'.join(srcs).encode('latin-1')).hexdigest()[:12]
            linked[w[0]] = key
            info.setdefault(key, {'sources': srcs, 'attribs': {}, 'uniforms': {}, 'locs': {}, 'programs': set()})
            info[key]['programs'].add(w[0])
        elif name == 'glGetActiveAttrib' and w[0] in linked:
            info[linked[w[0]]]['attribs'][b[0].rstrip(b'\0').decode()] = GL_TYPES.get(w[3], hex(w[3]))
        elif name == 'glGetActiveUniform' and w[0] in linked:
            info[linked[w[0]]]['uniforms'][b[0].rstrip(b'\0').decode()] = (GL_TYPES.get(w[3], hex(w[3])), w[2])
        elif name in ('glGetUniformLocation', 'glGetAttribLocation') and w[0] in linked:
            info[linked[w[0]]]['locs'][(name[5], b[0].rstrip(b'\0').decode())] = struct.unpack('<i', struct.pack('<I', w[1]))[0]
        elif name == 'glUseProgram':
            current = w[0]
        elif name == 'FRAME':
            frame = w[0] if w[1] else None
        elif name in ('glDrawArrays', 'glDrawElements') and current in linked:
            draws[linked[current]] += 1
            frames_of[linked[current]].add(frame)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    print('%d distinct programs linked, %d drawn with in the recorded frames' % (len(info), len(draws)))
    for key, i in sorted(info.items(), key=lambda kv: -draws[kv[0]]):
        vs = i['sources'][0] if i['sources'] else ''
        defines = [l.split()[1] for s in i['sources'] for l in s.splitlines() if l.startswith('#define') and len(l.split()) > 1]
        first = next((l for l in vs.splitlines() if l.strip() and not l.startswith('#')), '')
        print('%s draws %5d frames %-12s defines %s' % (key, draws[key], ','.join(str(f) for f in sorted(frames_of[key]) if f is not None)[:12],
                                                        ' '.join(sorted(set(defines)))[:150]))
        if out_dir:
            with open(os.path.join(out_dir, key + '.txt'), 'w', encoding='latin-1') as f:
                f.write('draws %d in frames %s\n' % (draws[key], sorted(x for x in frames_of[key] if x is not None)))
                f.write('attributes: %s\n' % i['attribs'])
                f.write('uniforms: %s\n' % i['uniforms'])
                f.write('locations: %s\n' % i['locs'])
                for n, s in enumerate(i['sources']):
                    f.write('\n======== shader %d ========\n%s\n' % (n, s))


def stats(path):
    count = collections.Counter()
    size = collections.Counter()
    per_frame = collections.Counter()
    frame = None
    for name, w, b in records(path):
        count[name] += 1
        size[name] += 8 + 4 * len(w) + sum(4 + len(x) for x in b)
        if name == 'FRAME':
            frame = w[0] if w[1] else None
        elif frame is not None and name in ('glDrawArrays', 'glDrawElements'):
            per_frame[frame] += 1
    for name, n in count.most_common():
        print('%-28s %8d calls %10.1f KB' % (name, n, size[name] / 1024))
    print('draws per recorded frame:', dict(sorted(per_frame.items())))


def draws(path, frame_wanted=None):
    """Each draw of the recorded frames: program (key and defines), vertex count, where the vertices
    come from (b: buffer object, c: client memory, u: a buffer updated in this frame), depth range,
    blending, polygon offset, and the copies to textures."""
    shaders, attached, linked, defs = {}, collections.defaultdict(list), {}, {}
    current, frame, array_buffer = None, None, 0
    attr_buf = {}
    updated = set()
    st = {'range': (0.0, 1.0), 'blend': False, 'po': False, 'po_v': (0.0, 0.0), 'fb': 0, 'depth': False, 'mask': True, 'bf': '1,0', 'df': 0x201,
          'cm': 'rgba'}
    caps = {0x0BE2: 'blend', 0x8037: 'po', 0x0B71: 'depth'}
    n = 0
    for name, w, b in records(path):
        if name == 'glShaderSource':
            shaders[w[0]] = b[0].decode('latin-1')
        elif name == 'glAttachShader':
            attached[w[0]].append(w[1])
        elif name == 'glLinkProgram':
            srcs = tuple(shaders.get(s, '') for s in attached[w[0]])
            linked[w[0]] = hashlib.sha1('\0'.join(srcs).encode('latin-1')).hexdigest()[:12]
            d = set()
            for s in srcs:
                for l in s.splitlines():
                    if l.startswith('#define') and len(l.split()) == 2 and 'GLITCH' not in l and 'PRECISION' not in l:
                        d.add(l.split()[1])
            defs[linked[w[0]]] = ' '.join(sorted(d))
        elif name == 'glUseProgram':
            current = w[0]
        elif name == 'glBindBuffer':
            if w[0] == 0x8892:
                array_buffer = w[1]
        elif name in ('glBufferData', 'glBufferSubData') and frame is not None:
            updated.add(array_buffer)
        elif name == 'glVertexAttribPointer':
            attr_buf[w[0]] = array_buffer
        elif name in ('glEnable', 'glDisable') and w[0] in caps:
            st[caps[w[0]]] = name == 'glEnable'
        elif name == 'glDepthRangef':
            st['range'] = (f32(w[0]), f32(w[1]))
        elif name == 'glDepthMask':
            st['mask'] = bool(w[0])
        elif name == 'glDepthFunc':
            st['df'] = w[0]
        elif name == 'glClear' and frame is not None and frame == frame_wanted:
            print('%5s       glClear %x fb%d' % (frame, w[0], st['fb']))
        elif name == 'glBlendFunc':
            st['bf'] = '%x,%x' % (w[0], w[1])
        elif name == 'glBlendFuncSeparate':
            st['bf'] = '%x,%x/%x,%x' % tuple(w[:4])
        elif name == 'glColorMask':
            st['cm'] = ''.join('rgba'[i] if w[i] else '-' for i in range(4))
        elif name == 'glPolygonOffset':
            st['po_v'] = (f32(w[0]), f32(w[1]))
        elif name == 'glBindFramebuffer':
            st['fb'] = w[1]
        elif name == 'FRAME':
            frame = w[0] if w[1] else None
            updated = set()
            n = 0
        elif name in ('glCopyTexSubImage2D', 'glCopyTexImage2D') and frame is not None:
            print('%5s       %s %s' % (frame, name, ' '.join(str(x) for x in w)))
        elif name in ('glDrawArrays', 'glDrawElements') and frame is not None:
            n += 1
            if frame_wanted is not None and frame != frame_wanted:
                continue
            key = linked.get(current, '?')
            src = ''.join('u' if attr_buf.get(i) in updated and attr_buf.get(i) else 'b' if attr_buf.get(i) else 'c' for i in range(4))
            count = w[2] if name == 'glDrawArrays' else w[1]
            print('%5s %4d %s %6d %s fb%-3d %s range %.4g-%.4g%s%s%s %s' % (
                frame, n, key, count, src, st['fb'], 'z%x' % (st['df'] & 0xf) if st['depth'] else '--', st['range'][0], st['range'][1], ' blend ' + st['bf'] if st['blend'] else '',
                (' nodepthw' if not st['mask'] else '') + (' mask ' + st['cm'] if st['cm'] != 'rgba' else ''), ' PO %g,%g' % st['po_v'] if st['po'] else '', defs.get(key, '')))


def frames(path):
    """Per recorded frame: draws (and how many read client memory), client-memory vertex bytes,
    calls, programs used, uniform calls: for picking the busiest frames of a sampled trace."""
    frame, row = None, None
    for name, w, b in records(path):
        if name == 'FRAME':
            if w[1]:
                frame, row = w[0], collections.Counter()
            elif frame is not None:
                print('%6d: %4d draws (%3d client), %7.1f KB client vertices, %5d calls, %3d glUseProgram, %4d uniform calls' % (
                    frame, row['draws'], row['client_draws'], row['client'] / 1024, row['calls'], row['programs'], row['uniforms']))
                frame = None
            continue
        if frame is None:
            continue
        row['calls'] += 1
        if name == 'CLIENT':
            row['client'] += len(b[0])
            row['pending_client'] = 1
        elif name in ('glDrawArrays', 'glDrawElements'):
            row['draws'] += 1
            row['client_draws'] += row['pending_client']
            row['pending_client'] = 0
        elif name == 'glUseProgram':
            row['programs'] += 1
        elif name.startswith('glUniform'):
            row['uniforms'] += 1


def cut(path, out, keep):
    """A copy of the trace with only the recorded frames in `keep`: the others lose their frame marks
    and their draws, clears and client data (their state calls stay; every recorded frame after a
    gap starts with a snapshot of the state anyway)."""
    with open(path, 'rb') as f:
        data = f.read()
    magic, version, count = struct.unpack_from('<3I', data, 0)
    pos = 12
    for _ in range(count):
        n, = struct.unpack_from('<I', data, pos)
        pos += 4 + (n + 3) // 4 * 4
    head = data[:pos]
    names = {}
    p = 12
    for i in range(count):
        n, = struct.unpack_from('<I', data, p)
        names[i] = data[p + 4:p + 4 + n].decode()
        p += 4 + (n + 3) // 4 * 4
    drop_in = {'glDrawArrays', 'glDrawElements', 'glClear', 'glCopyTexSubImage2D'}
    parts = [head]
    frame = None
    while pos + 8 <= len(data):
        start = pos
        h, nw = struct.unpack_from('<2I', data, pos)
        pos += 8 + 4 * nw
        for _ in range(h >> 16):
            n, = struct.unpack_from('<I', data, pos)
            pos += 4 + (n + 3) // 4 * 4
        if pos > len(data):
            break
        rid = h & 0xffff
        if rid == FRAME:
            w0, w1 = struct.unpack_from('<2I', data, start + 8)
            frame = w0 if w1 else None
            if w0 not in keep:
                continue
        elif frame is not None and frame not in keep and (rid == CLIENT or names.get(rid) in drop_in):
            continue
        parts.append(data[start:pos])
    with open(out, 'wb') as f:
        f.write(b''.join(parts))


def dump(path, first=0, n=200):
    for i, (name, w, b) in enumerate(records(path)):
        if i < first:
            continue
        if i >= first + n:
            break
        args = ' '.join(('%g' % f32(x) if name.endswith(('f', 'fv', 'Color', 'Depthf')) and 1e-6 < abs(f32(x)) < 1e7 else '0x%x' % x) for x in w)
        print('%7d %-26s %s %s' % (i, name, args, ' '.join('[%d bytes]' % len(x) for x in b)))


if __name__ == '__main__':
    cmd, path = sys.argv[1], sys.argv[2]
    if cmd == 'programs':
        programs(path, sys.argv[3] if len(sys.argv) > 3 else None)
    elif cmd == 'stats':
        stats(path)
    elif cmd == 'cut':
        cut(path, sys.argv[3], {int(x) for x in sys.argv[4].split(',')})
    elif cmd == 'frames':
        frames(path)
    elif cmd == 'draws':
        draws(path, int(sys.argv[3]) if len(sys.argv) > 3 else None)
    elif cmd == 'dump':
        dump(path, int(sys.argv[3]) if len(sys.argv) > 3 else 0, int(sys.argv[4]) if len(sys.argv) > 4 else 200)
