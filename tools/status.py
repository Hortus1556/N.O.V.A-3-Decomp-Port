"""Status of every port: the images and README tables, from docs/status/milestones.json.

    python tools/status.py

Writes, in docs/status/:
  progress.svg       one panel per port: a treemap of its areas (sized by their number of
                     features), one cell per feature coloured done / in progress / to do, and
                     the share done in the heading;
  history.svg        the share done of every port by day;
  badge-<port>.svg   the share done as a badge;
and the per-port feature tables in README.md between the `status:begin` / `status:end` markers.
Edit the JSON, run this, commit them together.
"""
import datetime, html, json, math, os, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'docs', 'status')
FONT = "-apple-system,'Segoe UI',Helvetica,Arial,sans-serif"
BG, FG, MUTED, GRID = '#0d1117', '#ffffff', '#8b949e', '#30363d'
STATE_COLOR = {'done': '#2ea043', 'partial': '#bb8009', 'todo': '#6e7681'}
STATE_LABEL = {'done': 'Done', 'partial': 'In progress', 'todo': 'To do'}
STATE_MARK = {'done': '✅', 'partial': '\U0001f7e1', 'todo': '⬜'}
PORT_COLOR = ['#2f81f7', '#f85149', '#a371f7', '#2ea043', '#d29922']
day = datetime.date.fromisoformat


def esc(s):
    return html.escape(str(s), quote=True)


def items(port):
    return [it for area in port['areas'] for it in area['items']]


def share(port, until=None):
    """Features done (by `until`, a date, when given), their total, the share done in percent."""
    its = items(port)
    done = sum(1 for it in its if it['state'] == 'done' and (until is None or day(it['date']) <= until))
    return done, len(its), 100.0 * done / len(its)


def write(name, text):
    with open(os.path.join(OUT, name), 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def label(x, y, s, size, anchor='start', weight='normal', fill=FG):
    return (f'<text x="{x:.2f}" y="{y:.2f}" font-size="{size}" font-weight="{weight}" fill="{fill}" '
            f'text-anchor="{anchor}" stroke="{BG}" stroke-width="3" paint-order="stroke">{esc(s)}</text>')


def squarify(values, x, y, w, h):
    """Squarified treemap (Bruls, Huizing, van Wijk): rectangles for `values` (sorted large
    first) filling x, y, w, h, each with an area proportional to its value."""
    scale = w * h / sum(values)
    areas = [v * scale for v in values]
    rects = []

    def worst(row, side):
        s = sum(row)
        return max(max(side * side * a / (s * s), s * s / (side * side * a)) for a in row)

    while areas:
        side = min(w, h)
        row = [areas.pop(0)]
        while areas and worst(row + [areas[0]], side) <= worst(row, side):
            row.append(areas.pop(0))
        thick = sum(row) / side
        pos = 0.0
        for a in row:
            length = a / thick
            # the row runs along the shorter side: a column at the left of a wide rectangle
            rects.append((x, y + pos, thick, length) if w >= h else (x + pos, y, length, thick))
            pos += length
        if w >= h:
            x, w = x + thick, w - thick
        else:
            y, h = y + thick, h - thick
    return rects


def cells(n, x, y, w, h):
    """n cells filling a rectangle in rows, as square as the shape allows; the last row's
    cells widen to fill it."""
    cols = max(1, min(n, round(math.sqrt(n * w / h))))
    rows = math.ceil(n / cols)
    out = []
    for i in range(n):
        r, c = divmod(i, cols)
        in_row = cols if r < rows - 1 else n - cols * (rows - 1)
        cw = w / in_row
        out.append((x + c * cw, y + r * h / rows, cw, h / rows))
    return out


def panel(port, x0, y0, w, h):
    done, total, pct = share(port)
    o = [label(x0 + 4, y0 + 21, f'{port["name"]}: {pct:.1f}% ({done}/{total})', 16)]
    top = y0 + 30
    areas = sorted(port['areas'], key=lambda a: -len(a['items']))
    for area, (ax, ay, aw, ah) in zip(areas, squarify([len(a['items']) for a in areas], x0 + 1, top, w - 2, y0 + h - top - 1)):
        a_done = sum(1 for it in area['items'] if it['state'] == 'done')
        o.append(f'<g><title>{esc(area["name"])}: {a_done}/{len(area["items"])}</title>')
        for it, (cx, cy, cw, ch) in zip(area['items'], cells(len(area['items']), ax, ay, aw, ah)):
            o.append(f'<rect x="{cx:.2f}" y="{cy:.2f}" width="{cw:.2f}" height="{ch:.2f}" '
                     f'fill="{STATE_COLOR[it["state"]]}" stroke="{BG}" stroke-width="1"><title>'
                     f'{esc(area["name"])}: {esc(it["name"])} ({STATE_LABEL[it["state"]]})</title></rect>')
        o.append(f'<rect x="{ax:.2f}" y="{ay:.2f}" width="{aw:.2f}" height="{ah:.2f}" fill="none" '
                 f'stroke="{BG}" stroke-width="3"/>')
        o.append(label(ax + 5, ay + 16, area['name'], 12) + '</g>')
    return o


def progress_svg(ports):
    pw, ph, gap = 500, 300, 0
    cols = 2
    rows = math.ceil(len(ports) / cols)
    width, height = pw * cols, ph * rows + 28
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" '
         f'height="{height}" font-family="{FONT}" role="img" aria-label="Status of every port">',
         f'<rect width="{width}" height="{height}" fill="{BG}"/>']
    for i, p in enumerate(ports):
        r, c = divmod(i, cols)
        o += panel(p, c * pw + (4 if c else 0), r * ph, pw - 4, ph - 4)
    x, y = 6, height - 9
    for state in ('done', 'partial', 'todo'):
        o.append(f'<rect x="{x}" y="{y - 10}" width="11" height="11" fill="{STATE_COLOR[state]}"/>')
        o.append(f'<text x="{x + 16}" y="{y}" font-size="12" fill="{MUTED}">{STATE_LABEL[state]}</text>')
        x += 32 + len(STATE_LABEL[state]) * 7
    o.append(f'<text x="{width - 6}" y="{y}" font-size="12" fill="{MUTED}" text-anchor="end">'
             f'One cell per feature, grouped by area; the percentage counts features done</text>')
    o.append('</svg>\n')
    return '\n'.join(o)


def history_svg(ports, updated):
    width, height = 1000, 380
    left, right, top, bottom = 50, 800, 20, 325
    dates = [day(it['date']) for p in ports for it in items(p) if it.get('date')]
    first, last = min(dates) - datetime.timedelta(days=1), day(updated)
    days = [first + datetime.timedelta(days=k) for k in range((last - first).days + 1)]
    sx = lambda i: left + (right - left) * i / max(len(days) - 1, 1)
    sy = lambda pct: bottom - (bottom - top) * pct / 100
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" '
         f'height="{height}" font-family="{FONT}" font-size="12" fill="#c9d1d9" role="img" '
         f'aria-label="Share of features done over time">',
         f'<rect width="{width}" height="{height}" rx="6" fill="{BG}"/>']
    for pct in range(0, 101, 10):
        o.append(f'<line x1="{left}" x2="{right}" y1="{sy(pct):.1f}" y2="{sy(pct):.1f}" stroke="{GRID}"/>'
                 f'<text x="{left - 6}" y="{sy(pct) + 4:.1f}" text-anchor="end">{pct}%</text>')
    for i, d in enumerate(days):
        o.append(f'<line x1="{sx(i):.1f}" x2="{sx(i):.1f}" y1="{top}" y2="{bottom}" stroke="{GRID}" stroke-opacity="0.5"/>'
                 f'<text transform="translate({sx(i) + 4:.1f},{bottom + 10}) rotate(60)">{d:%m-%d}</text>')
    ends = []
    for k, p in enumerate(ports):
        colour = PORT_COLOR[k % len(PORT_COLOR)]
        start = min(day(it['date']) for it in items(p) if it.get('date')) - datetime.timedelta(days=1)
        pts = [(sx(i), sy(share(p, d)[2])) for i, d in enumerate(days) if d >= start]
        line = ' '.join(f'{x:.1f},{y:.1f}' for x, y in pts)
        o.append(f'<polygon points="{pts[0][0]:.1f},{bottom} {line} {pts[-1][0]:.1f},{bottom}" fill="{colour}" fill-opacity="0.10"/>')
        o.append(f'<polyline points="{line}" fill="none" stroke="{colour}" stroke-width="2"/>')
        o += [f'<circle cx="{x:.1f}" cy="{y:.1f}" r="3" fill="{colour}"/>' for x, y in pts]
        ends.append([pts[-1][1], colour, f'{p["name"]} {share(p)[2]:.1f}%'])
    ends.sort()
    for i in range(1, len(ends)):  # keep the end labels at least a line apart
        ends[i][0] = max(ends[i][0], ends[i - 1][0] + 15)
    for y, colour, text in ends:
        o.append(f'<text x="{right + 8}" y="{y + 4:.1f}" fill="{colour}" font-weight="600">{esc(text)}</text>')
    o.append('</svg>\n')
    return '\n'.join(o)


def badge_svg(name, pct):
    colour = '#4c1' if pct >= 80 else '#97ca00' if pct >= 60 else '#dfb317' if pct >= 40 else '#fe7d37' if pct >= 20 else '#e05d44'
    value = f'{pct:.1f}%'
    lw, vw = len(name) * 6.6 + 12, len(value) * 6.6 + 12
    w = lw + vw
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w:.0f}" height="20" role="img" '
            f'aria-label="{esc(name)}: {value}"><title>{esc(name)}: {value}</title>'
            f'<clipPath id="r"><rect width="{w:.0f}" height="20" rx="3"/></clipPath><g clip-path="url(#r)">'
            f'<rect width="{lw:.0f}" height="20" fill="#555"/><rect x="{lw:.0f}" width="{vw:.0f}" height="20" fill="{colour}"/></g>'
            f'<g fill="#fff" text-anchor="middle" font-family="Verdana,Geneva,DejaVu Sans,sans-serif" font-size="11">'
            f'<text x="{lw / 2:.1f}" y="14">{esc(name)}</text><text x="{lw + vw / 2:.1f}" y="14">{value}</text></g></svg>\n')


def readme_tables(ports):
    o = []
    for p in ports:
        done, total, pct = share(p)
        partial = sum(1 for it in items(p) if it['state'] == 'partial')
        o.append(f'<details>\n<summary><b>{esc(p["name"])}</b>: {pct:.1f}% ({done}/{total} done, '
                 f'{partial} in progress). {esc(p["summary"])}</summary>\n')
        o.append(f'Branch [`{p["branch"]}`](../../tree/{p["branch"]}) · details in '
                 f'[{os.path.basename(p["doc"])}]({p["doc"]})\n')
        o.append('| Area | Feature | Status | Date |\n|---|---|---|---|')
        for area in p['areas']:
            for it in area['items']:
                o.append(f'| {area["name"]} | {it["name"]} | {STATE_MARK[it["state"]]} '
                         f'{STATE_LABEL[it["state"]]} | {it.get("date", "")} |')
        o.append('\n</details>\n')
    return '\n'.join(o)


def main():
    with open(os.path.join(OUT, 'milestones.json'), encoding='utf-8') as f:
        data = json.load(f)
    ports = data['ports']
    for p in ports:
        for it in items(p):
            assert it['state'] in STATE_COLOR, (p['id'], it)
            assert it['state'] != 'done' or it.get('date'), ('a done feature needs its date', p['id'], it)
    write('progress.svg', progress_svg(ports))
    write('history.svg', history_svg(ports, data['updated']))
    for p in ports:
        write(f'badge-{p["id"]}.svg', badge_svg(p['name'], share(p)[2]))
    readme = os.path.join(ROOT, 'README.md')
    with open(readme, encoding='utf-8') as f:
        text = f.read()
    block = f'<!-- status:begin (tools/status.py) -->\n{readme_tables(ports)}\n<!-- status:end -->'
    text, n = re.subn(r'<!-- status:begin.*?<!-- status:end -->', lambda _: block, text, flags=re.S)
    if not n:
        raise SystemExit('README.md has no status:begin / status:end markers')
    with open(readme, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    for p in ports:
        print(f'{p["name"]}: {share(p)[2]:.1f}% ({share(p)[0]}/{share(p)[1]})')


if __name__ == '__main__':
    main()
