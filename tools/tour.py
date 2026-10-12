"""Every campaign chapter from start to finish (nova3.exe --tour), with a report.

Usage: tour.py [chapters...] [--profile DIR] [--jobs N]
       tour.py --mp [maps...]          host a local match on each multiplayer map (default all 7)

For each chapter (default: all ten): a short run reads its level file, tools/tourplan.py gives
that level's section triggers, then nova3.exe --tour plays it through checkpoint by checkpoint
(host/eventlog.cpp) on a copy of the test profile (default build/zk2), with a screenshot per
checkpoint. Results go to build/tours/<chapter>/ (log, screenshots) and a summary is printed:
exit status, checkpoints, deaths, crashes and hangs, section-load fallbacks. NOVA3_TOUR_ARGS adds
arguments to every run (e.g. "--obb E:/nova3/build/3ds/slim_data" with NOVA3_DEVICE=3ds: the
3DS's slim data set).
"""
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tourplan  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'build', 'cmake_full', 'nova3.exe')
OUT = os.path.join(ROOT, 'build', 'tours')
SPEED = 4  # --speed: game time runs this much faster (cutscenes, scripts, waits)
PROFILE = os.path.join(ROOT, 'build', 'zk_tour')  # the test profile: HD textures and VSync off
EXTRA = os.environ.get('NOVA3_TOUR_ARGS', '').split()


def level_file(chapter, profile):
    """The chapter's level file: the tour logs it once the title menu is up; stop there."""
    user = os.path.join(OUT, f'probe{chapter}')
    shutil.rmtree(user, ignore_errors=True)
    shutil.copytree(profile, user)
    p = subprocess.Popen([EXE, '-silent', '--user', user, '--size', '640x360', '--tour', str(chapter)] + EXTRA,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    name = None
    try:
        for line in p.stdout:
            m = re.search(r'tour: chapter \d+ of \d+: (\S+)', line)
            if m and 'multiplayer maps' in line:
                print(line.strip(), flush=True)
            if m:
                name = m.group(1)
                break
    finally:
        p.kill()
        p.wait()
    shutil.rmtree(user, ignore_errors=True)
    return name


def run(chapter, profile, speed=None):
    name = level_file(chapter, profile)
    import zipfile
    triggers = ''
    if not name:
        return {'chapter': chapter, 'level': None, 'code': 'no level file (probe failed)', 'checkpoints': 0,
                'sections': 0, 'dead': 0, 'crash': [], 'stuck': False, 'no_ground': 0, 'missing_triggers': []}
    if name:
        z = zipfile.ZipFile(tourplan.LEVELS)
        triggers = ','.join(f'{o}:{h}:{s}' for o, h, s in tourplan.triggers(z.read(name)))
    folder = os.path.join(OUT, str(chapter))
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    user = os.path.join(folder, 'profile')
    shutil.copytree(profile, user)
    cmd = [EXE, '-silent', '--user', user, '--size', '960x540', '--speed', str(speed or SPEED), '--tour', str(chapter),
           '--tour-shots', folder] + EXTRA
    if triggers:
        cmd += ['--tour-triggers', triggers]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=3600)
        log, code = p.stdout + p.stderr, p.returncode
    except subprocess.TimeoutExpired as e:
        log, code = (e.stdout or b'').decode(errors='replace') if isinstance(e.stdout, bytes) else (e.stdout or ''), 'timeout'
    open(os.path.join(folder, 'tour.log'), 'w', encoding='utf-8').write(log)
    shutil.rmtree(user, ignore_errors=True)
    if 'tour: stuck' in log and (speed or SPEED) != 1:
        # a timed sequence the idle player fails at speed (the last chapter): again at normal speed
        return run(chapter, profile, speed=1)
    return {
        'chapter': chapter, 'level': name, 'code': code, 'speed': speed or SPEED,
        'ended': 'the level ended' in log,
        'checkpoints': len(re.findall(r'tour: checkpoint', log)),
        'sections': len(re.findall(r'tour: into trigger', log)),
        'dead': len(re.findall(r' DEAD', log)),
        'crash': re.findall(r'(guest \w+ fault at \w+|unhandled|FATAL[^\n]*|watchdog[^\n]*)', log)[:3],
        'stuck': 'tour: stuck' in log,
        'no_ground': len(re.findall(r'section load: no ground', log)),
        'missing_triggers': re.findall(r'tour: trigger (\d+) \(section \d+\) not in', log),
    }


def run_mp(index, profile):
    """A local Wi-Fi match on map `index` (Menu_SetMPLevel before Continue): walk, look, fire."""
    folder = os.path.join(OUT, f'mp{index}')
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    user = os.path.join(folder, 'profile')
    shutil.copytree(profile, user)
    cmd = [EXE, '-silent', '--user', user, '--size', '1280x720',
           '--tap', '800:780,525', '--tap', '1300:785,517', '--tap', '2000:640,243',
           '--lua', f'2400:Menu_SetMPLevel({index})', '--tap', '2600:895,575',
           '--hold', '4200:W,900', '--look', '4600:1500,0', '--hold', '5000:Mouse1,300', '--hold', '5500:D,600',
           '--look', '6000:-2500,200', '--hold', '6200:W,900',
           '--screenshot', f'5200:{folder}/a.png', '--screenshot', f'7400:{folder}/b.png', '--frames', '7600'] + EXTRA
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=1200)
        log, code = p.stdout + p.stderr, p.returncode
    except subprocess.TimeoutExpired:
        log, code = '', 'timeout'
    open(os.path.join(folder, 'mp.log'), 'w', encoding='utf-8').write(log)
    shutil.rmtree(user, ignore_errors=True)
    return {'map': index, 'code': code, 'in_game': 'input: gameplay mode' in log,
            'crash': re.findall(r'(guest \w+ fault at \w+|FATAL[^\n]*|watchdog[^\n]*)', log)[:3]}


def main(args):
    if args and args[0] == '--mp':
        maps = [int(a) for a in args[1:]] or list(range(7))  # GetMPLevelsCount: 7
        profile = os.path.join(ROOT, 'build', 'zk2')  # the menu taps are timed for VSync at normal speed
        for m in maps:
            r = run_mp(m, profile)
            print(f"map {r['map']}: exit {r['code']}, reached gameplay {r['in_game']}, crash {r['crash'] or '-'}", flush=True)
        return
    jobs, profile, chapters = 1, PROFILE, []  # each game takes several GB
    it = iter(args)
    for a in it:
        if a == '--jobs':
            jobs = int(next(it))
        elif a == '--profile':
            profile = next(it)
        else:
            chapters.append(int(a))
    chapters = chapters or list(range(10))
    os.makedirs(OUT, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        results = list(pool.map(lambda c: run(c, profile), chapters))
    for r in results:
        print(f"chapter {r['chapter']} ({r['level']}): exit {r['code']}, speed {r.get('speed')}, "
              f"end screen {r.get('ended')}, {r['checkpoints']} checkpoints, "
              f"{r['sections']} section triggers, dead {r['dead']}, stuck {r['stuck']}, "
              f"no-ground fallbacks {r['no_ground']}, crash {r['crash'] or '-'}, "
              f"triggers not reached {r['missing_triggers'] or '-'}")


if __name__ == '__main__':
    main(sys.argv[1:])
