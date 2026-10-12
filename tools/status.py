"""Milestone status of every port: the images and README tables, from docs/status/milestones.json.

    python tools/status.py

Writes docs/status/progress.svg (the milestone map: one row per port, one cell per milestone),
docs/status/history.svg (milestones done per port over time), docs/status/badge-<port>.svg, and
the per-port milestone tables in README.md between the `status:begin` / `status:end` markers.
Edit the JSON, run this, commit both.
"""
import datetime, html, json, os, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'docs', 'status')
FONT = "-apple-system,'Segoe UI',Helvetica,Arial,sans-serif"
BG, FG, MUTED, GRID = '#0d1117', '#e6edf3', '#8b949e', '#30363d'
STATE_COLOR = {'done': '#2ea043', 'partial': '#d29922', 'todo': '#21262d'}
STATE_LABEL = {'done': 'Done', 'partial': 'In progress', 'todo': 'To do'}
STATE_MARK = {'done': '✅', 'partial': '\U0001f7e1', 'todo': '⬜'}
PORT_COLOR = ['#58a6ff', '#ff7b72', '#d2a8ff', '#3fb950', '#f0883e']


def esc(s):
    return html.escape(str(s), quote=True)


def counts(port):
    states = [m['state'] for m in port['milestones']]
    return states.count('done'), states.count('partial'), len(states)


def write(name, text):
    with open(os.path.join(OUT, name), 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def text_width(s, size):
    return len(s) * size * 0.6


def progress_svg(ports):
    width, label_w, row_h, top = 1000, 230, 46, 44
    most = max(len(p['milestones']) for p in ports)
    cell = (width - label_w - 16) / most
    height = top + row_h * len(ports) + 40
    done = sum(counts(p)[0] for p in ports)
    partial = sum(counts(p)[1] for p in ports)
    total = sum(counts(p)[2] for p in ports)
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" '
         f'height="{height}" font-family="{FONT}" role="img" aria-label="Milestones of every port">',
         f'<rect width="{width}" height="{height}" rx="8" fill="{BG}"/>',
         f'<text x="16" y="28" font-size="17" font-weight="600" fill="{FG}">Milestones: {done} of {total} '
         f'done, {partial} in progress</text>']
    for r, p in enumerate(ports):
        y = top + r * row_h
        d, pa, n = counts(p)
        o.append(f'<text x="16" y="{y + 18}" font-size="14" font-weight="600" fill="{FG}">{esc(p["name"])}</text>')
        o.append(f'<text x="{label_w - 12}" y="{y + 18}" font-size="13" fill="{MUTED}" text-anchor="end">{d}/{n}</text>')
        bar = label_w - 28
        o.append(f'<rect x="16" y="{y + 26}" width="{bar}" height="6" rx="3" fill="{STATE_COLOR["todo"]}"/>')
        o.append(f'<rect x="16" y="{y + 26}" width="{bar * (d + pa) / n:.1f}" height="6" rx="3" fill="{STATE_COLOR["partial"]}"/>')
        o.append(f'<rect x="16" y="{y + 26}" width="{bar * d / n:.1f}" height="6" rx="3" fill="{STATE_COLOR["done"]}"/>')
        for i, m in enumerate(p['milestones']):
            x = label_w + i * cell
            colour = STATE_COLOR[m['state']]
            num = FG if m['state'] != 'todo' else MUTED
            o.append(f'<g><title>{esc(p["name"])} #{i + 1}: {esc(m["name"])} ({STATE_LABEL[m["state"]]})</title>'
                     f'<rect x="{x:.1f}" y="{y + 2}" width="{cell - 4:.1f}" height="{row_h - 10}" rx="5" '
                     f'fill="{colour}" stroke="{GRID}"/>'
                     f'<text x="{x + (cell - 4) / 2:.1f}" y="{y + row_h / 2 + 2}" font-size="12" fill="{num}" '
                     f'text-anchor="middle">{i + 1}</text></g>')
    y = height - 16
    x = 16
    for state in ('done', 'partial', 'todo'):
        o.append(f'<rect x="{x}" y="{y - 11}" width="12" height="12" rx="3" fill="{STATE_COLOR[state]}" stroke="{GRID}"/>')
        o.append(f'<text x="{x + 18}" y="{y}" font-size="12" fill="{MUTED}">{STATE_LABEL[state]}</text>')
        x += 30 + text_width(STATE_LABEL[state], 12)
    o.append(f'<text x="{width - 16}" y="{y}" font-size="12" fill="{MUTED}" text-anchor="end">'
             f'Numbers match the tables in the README</text>')
    o.append('</svg>\n')
    return '\n'.join(o)


def history_svg(ports, updated):
    width, height = 1000, 320
    left, right, top, bottom = 50, 230, 40, 270
    day = datetime.date.fromisoformat
    dates = [day(m['date']) for p in ports for m in p['milestones'] if m.get('date')]
    first, last = min(dates) - datetime.timedelta(days=1), day(updated)
    span = max((last - first).days, 1)
    top_n = max(counts(p)[2] for p in ports)
    sx = lambda d: left + (width - right - left) * (d - first).days / span
    sy = lambda n: bottom - (bottom - top) * n / top_n
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" '
         f'height="{height}" font-family="{FONT}" font-size="12" role="img" aria-label="Milestones done over time">',
         f'<rect width="{width}" height="{height}" rx="8" fill="{BG}"/>',
         f'<text x="16" y="26" font-size="17" font-weight="600" fill="{FG}">Milestones done over time</text>']
    step = 5 if top_n > 10 else 2
    for n in range(0, top_n + 1, step):
        o.append(f'<line x1="{left}" x2="{width - right}" y1="{sy(n):.1f}" y2="{sy(n):.1f}" stroke="{GRID}"/>'
                 f'<text x="{left - 8}" y="{sy(n) + 4:.1f}" text-anchor="end" fill="{MUTED}">{n}</text>')
    for k in range(span + 1):
        d = first + datetime.timedelta(days=k)
        o.append(f'<text x="{sx(d):.1f}" y="{bottom + 20}" text-anchor="middle" fill="{MUTED}">{d:%b %d}</text>')
    for i, p in enumerate(ports):
        colour = PORT_COLOR[i % len(PORT_COLOR)]
        done = sorted(day(m['date']) for m in p['milestones'] if m['state'] == 'done' and m.get('date'))
        if not done:
            continue
        # A step line: flat through each day, up by that day's milestones at its tick.
        start = (done[0] - first).days - 1
        pts = [(sx(first + datetime.timedelta(days=start)), sy(0))]
        for k in range(start + 1, span + 1):
            d = first + datetime.timedelta(days=k)
            n = sum(1 for x in done if x <= d)
            pts += [(sx(d), pts[-1][1]), (sx(d), sy(n))]
        path = ' '.join(f'{x:.1f},{y:.1f}' for x, y in pts)
        o.append(f'<polyline points="{path}" fill="none" stroke="{colour}" stroke-width="2.5" stroke-linejoin="round"/>')
        o.append(f'<circle cx="{pts[-1][0]:.1f}" cy="{pts[-1][1]:.1f}" r="4" fill="{colour}"/>')
        ly = top + 10 + i * 24
        o.append(f'<rect x="{width - right + 24}" y="{ly - 9}" width="14" height="4" rx="2" fill="{colour}"/>'
                 f'<text x="{width - right + 46}" y="{ly - 3}" fill="{FG}">{esc(p["name"])} ({n})</text>')
    o.append('</svg>\n')
    return '\n'.join(o)


def badge_svg(label, value, ratio):
    colour = '#2ea043' if ratio >= 0.75 else '#97ca00' if ratio >= 0.5 else '#d29922' if ratio >= 0.25 else '#e05d44'
    lw, vw = text_width(label, 11) + 12, text_width(value, 11) + 12
    w = lw + vw
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w:.0f}" height="20" role="img" '
            f'aria-label="{esc(label)}: {esc(value)}"><title>{esc(label)}: {esc(value)}</title>'
            f'<clipPath id="r"><rect width="{w:.0f}" height="20" rx="3"/></clipPath><g clip-path="url(#r)">'
            f'<rect width="{lw:.0f}" height="20" fill="#555"/><rect x="{lw:.0f}" width="{vw:.0f}" height="20" fill="{colour}"/></g>'
            f'<g fill="#fff" text-anchor="middle" font-family="Verdana,Geneva,DejaVu Sans,sans-serif" font-size="11">'
            f'<text x="{lw / 2:.1f}" y="14">{esc(label)}</text><text x="{lw + vw / 2:.1f}" y="14">{esc(value)}</text></g></svg>\n')


def readme_tables(ports):
    o = []
    for p in ports:
        d, pa, n = counts(p)
        o.append(f'<details>\n<summary><b>{esc(p["name"])}</b>: {d}/{n} done, {pa} in progress. '
                 f'{esc(p["summary"])}</summary>\n')
        o.append(f'Branch [`{p["branch"]}`](../../tree/{p["branch"]}) · details in [{os.path.basename(p["doc"])}]({p["doc"]})\n')
        o.append('| # | Milestone | Status | Date |\n|---|---|---|---|')
        for i, m in enumerate(p['milestones']):
            o.append(f'| {i + 1} | {m["name"]} | {STATE_MARK[m["state"]]} {STATE_LABEL[m["state"]]} | {m.get("date", "")} |')
        o.append('\n</details>\n')
    return '\n'.join(o)


def main():
    with open(os.path.join(OUT, 'milestones.json'), encoding='utf-8') as f:
        data = json.load(f)
    ports = data['ports']
    for p in ports:
        for m in p['milestones']:
            assert m['state'] in STATE_COLOR, (p['id'], m)
    write('progress.svg', progress_svg(ports))
    write('history.svg', history_svg(ports, data['updated']))
    for p in ports:
        d, _, n = counts(p)
        write(f'badge-{p["id"]}.svg', badge_svg(p['name'], f'{d}/{n} milestones', d / n))
    readme = os.path.join(ROOT, 'README.md')
    with open(readme, encoding='utf-8') as f:
        text = f.read()
    block = f'<!-- status:begin (tools/status.py) -->\n{readme_tables(ports)}\n<!-- status:end -->'
    text, n = re.subn(r'<!-- status:begin.*?<!-- status:end -->', lambda _: block, text, flags=re.S)
    if not n:
        raise SystemExit('README.md has no status:begin / status:end markers')
    with open(readme, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print(f'{sum(counts(p)[2] for p in ports)} milestones, {len(ports)} ports written')


if __name__ == '__main__':
    main()
