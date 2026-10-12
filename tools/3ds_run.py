"""Runs a New 3DS build in Azahar (E:/Emulators/Azahar, portable) unattended.

Usage: 3ds_run.py [--deploy | --no-sd-data] [--dspfirm stub|none] [--wait REGEX] [--log FILE] [--timeout S]
                  [--shot out.png] [--test FILE] [rom]

  rom          the .cxi / .3dsx / .3ds to boot (default build/3ds/cmake/nova3.cxi, from tools/3ds_cia.py:
               the game files in its RomFS)
  --no-sd-data remove the game files from the emulated SD card first, so that those of the ROM's
               RomFS are read (the game prefers sdmc:/3ds/nova3/nova3_data when it is there)
  --dspfirm X  the emulated SD card's sdmc:/3ds/dspfirm.cdc for this run: `stub` puts the 768-byte
               stub there (DSP1 magic and size: Azahar's HLE DSP needs the file, not its contents),
               `none` removes it (the game then extracts it or falls back to CSND)
  --deploy     put the game files on the emulated SD card first: sdmc:/3ds/nova3/nova3_data
               (the slim set build/3ds/slim_data when it is there, tools/3ds_slimdata.py; otherwise
               dist/nova3_data without the PC's hd_textures.pak; hard links where possible); as on
               the console, X is left out (and removed from the card) where the set has X.z
               (libGloftNAHM.so.z, the packed archives X.gla.z)
  --data DIR   deploy these game files instead
  --log FILE   the SD card file to watch (default 3ds/nova3/nova3.log); it is deleted before the
               run, and printed (its last lines) afterwards
  --wait REGEX stop when a line of the log matches (otherwise run until --timeout)
  --fail REGEX stop early when a line matches this (default: a crash or fatal error)
  --mark REGEX print each matching log line with the seconds since the boot as it appears
  --timeout S  seconds (default 120)
  --shot PNG   capture Azahar's window at the end (PrintWindow, as build/winshot.py does)
  --test FILE  copy a test script to sdmc:/3ds/nova3/test.txt for this run (removed afterwards)
  --visible    let Azahar take the focus as it starts (by default its window opens normally but
               without the focus: the focus goes back to the window that had it)
  --minimized  keep Azahar minimized for the whole run (captures restore it off-screen for the
               moment of the capture)

One emulator or game instance runs at a time across all work on this machine: the run holds
the lock directory build/3ds/run.lock (waits while another run holds it) and removes it at the
end. The emulated SD card is E:/Emulators/Azahar/user/sdmc; Azahar's own log is
user/log/azahar_log.txt. Azahar is set to New 3DS mode in user/config/qt-config.ini.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import pathlib
import re
import shutil
import subprocess
import sys
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
EMU = pathlib.Path('E:/Emulators/Azahar')
USER = EMU / 'user'
SDMC = USER / 'sdmc'
LOCK = ROOT / 'build' / '3ds' / 'run.lock'
DATA = ROOT / 'dist' / 'nova3_data'
SLIM = ROOT / 'build' / '3ds' / 'slim_data'
SKIP = {'hd_textures.pak'}  # PC only


def take_lock():
    waited = False
    while True:
        try:
            LOCK.mkdir(parents=False)
            return
        except FileExistsError:
            if not waited:
                print(f'waiting for {LOCK} (another emulator or game run)', flush=True)
                waited = True
            time.sleep(5)


def configure():
    """New 3DS mode, no update check / telemetry prompts, in the portable user folder."""
    cfg = USER / 'config' / 'qt-config.ini'
    cfg.parent.mkdir(parents=True, exist_ok=True)
    text = cfg.read_text(encoding='utf-8') if cfg.exists() else ''
    want = {'System': {'is_new_3ds': 'true'},
            'UI': {'check_for_update_on_start': 'false', 'confirmClose': 'false', 'firstStart': 'false',
                   'pauseWhenInBackground': 'false',  # (minimized: it must keep running)
                   'enable_discord_presence': 'false', 'calloutFlags': '4294967295'}}
    for section, keys in want.items():
        for key, value in keys.items():
            line = f'{key}={value}'
            pat = re.compile(rf'^{re.escape(key)}=.*$', re.M)
            dpat = re.compile(rf'^{re.escape(key)}\\default=.*$', re.M)
            if pat.search(text):
                text = pat.sub(line, text)
                text = dpat.sub(f'{key}\\\\default=false', text)
            else:
                block = re.search(rf'^\[{section}\]\s*$', text, re.M)
                add = f'{key}\\default=false\n{line}\n'
                if block:
                    text = text[:block.end()] + '\n' + add + text[block.end() + 1:]
                else:
                    text += f'\n[{section}]\n{add}'
    cfg.write_text(text, encoding='utf-8')


def place(src, dst):
    if dst.exists() and (os.path.samefile(src, dst) or
                         (dst.stat().st_size, dst.stat().st_mtime) == (src.stat().st_size, src.stat().st_mtime)):
        return
    if dst.exists():
        dst.unlink()
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def deploy(data):
    dst = SDMC / '3ds' / 'nova3' / 'nova3_data'
    dst.mkdir(parents=True, exist_ok=True)
    top = {p.name for p in data.iterdir()}
    n = 0
    for src in data.rglob('*'):
        rel = src.relative_to(data)
        if rel.parts[0] in SKIP:
            continue
        if len(rel.parts) == 1 and rel.name + '.z' in top:  # the console's set: X.z instead of X (3ds_package.py)
            if (dst / rel).exists():
                (dst / rel).unlink()
            continue
        out = dst / rel
        if src.is_dir():
            out.mkdir(parents=True, exist_ok=True)
        else:
            out.parent.mkdir(parents=True, exist_ok=True)
            place(src, out)
            n += 1
    print(f'deployed {n} files from {data} to {dst}')


user32, gdi32 = ctypes.windll.user32, ctypes.windll.gdi32


def find_windows(pid):
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def enum(hwnd, _):
        owner = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            r = wt.RECT()
            user32.GetWindowRect(hwnd, ctypes.byref(r))
            found.append(((r.right - r.left) * (r.bottom - r.top), hwnd))
        return True

    user32.EnumWindows(enum, 0)
    return [h for _, h in sorted(found, reverse=True)]


SW_SHOWMINNOACTIVE, SW_SHOWNOACTIVATE = 7, 4


class WINDOWPLACEMENT(ctypes.Structure):
    _fields_ = [('length', wt.UINT), ('flags', wt.UINT), ('showCmd', wt.UINT), ('ptMinPosition', wt.POINT),
                ('ptMaxPosition', wt.POINT), ('rcNormalPosition', wt.RECT)]


CAPTURING = threading.Event()  # set while capture() has the window restored


def keep_minimized(proc, previous, minimize=True, seconds=None):
    """While the emulator runs (or for its first `seconds`): with `minimize`, minimizes its windows,
    without activating them, whenever they appear (Qt shows its windows itself, not always as
    STARTUPINFO asks); and when it takes the foreground (Qt activates its window at start), hands
    the foreground back to the window that had it (attached to the emulator's input queue, which
    SetForegroundWindow from a background process needs)."""
    kernel32 = ctypes.windll.kernel32
    start = time.time()
    while proc.poll() is None and (seconds is None or time.time() - start < seconds):
        if not CAPTURING.is_set():
            if minimize:
                for hwnd in find_windows(proc.pid):
                    if not user32.IsIconic(hwnd):
                        user32.ShowWindow(hwnd, SW_SHOWMINNOACTIVE)
            fg = user32.GetForegroundWindow()
            owner = wt.DWORD()
            fg_thread = user32.GetWindowThreadProcessId(fg, ctypes.byref(owner)) if fg else 0
            if previous and owner.value == proc.pid and user32.IsWindow(previous):
                me = kernel32.GetCurrentThreadId()
                user32.AttachThreadInput(me, fg_thread, True)
                user32.SetForegroundWindow(previous)
                user32.AttachThreadInput(me, fg_thread, False)
        time.sleep(0.1)


def capture(pid, out, minimized=True):  # (minimized: restore off-screen if the window is iconic)
    from PIL import Image
    user32.SetProcessDPIAware()
    wins = find_windows(pid)
    if not wins:
        print('no window to capture')
        return
    hwnd = wins[0]
    if minimized and user32.IsIconic(hwnd):
        CAPTURING.set()
        # restored off-screen, without activation, for the capture (a minimized window has no
        # client area to print), then minimized again
        wp = WINDOWPLACEMENT()
        wp.length = ctypes.sizeof(WINDOWPLACEMENT)
        user32.GetWindowPlacement(hwnd, ctypes.byref(wp))
        r = wp.rcNormalPosition
        w0, h0 = r.right - r.left, r.bottom - r.top
        wp.rcNormalPosition = wt.RECT(-20000, -20000, -20000 + w0, -20000 + h0)
        wp.showCmd = SW_SHOWNOACTIVATE
        user32.SetWindowPlacement(hwnd, ctypes.byref(wp))
        time.sleep(0.7)  # a frame or two
        try:
            capture(pid, out, minimized=False)
        finally:
            user32.ShowWindow(hwnd, SW_SHOWMINNOACTIVE)
            CAPTURING.clear()
        return
    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    hdc = user32.GetWindowDC(hwnd)
    mdc = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mdc, bmp)
    user32.PrintWindow(hwnd, mdc, 2)  # PW_RENDERFULLCONTENT

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [('biSize', wt.DWORD), ('biWidth', wt.LONG), ('biHeight', wt.LONG), ('biPlanes', wt.WORD),
                    ('biBitCount', wt.WORD), ('biCompression', wt.DWORD), ('biSizeImage', wt.DWORD),
                    ('biXPelsPerMeter', wt.LONG), ('biYPelsPerMeter', wt.LONG), ('biClrUsed', wt.DWORD),
                    ('biClrImportant', wt.DWORD)]

    bi = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bi), 0)
    Image.frombuffer('RGBA', (w, h), buf, 'raw', 'BGRA', 0, 1).convert('RGB').save(out)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mdc)
    user32.ReleaseDC(hwnd, hdc)
    print(f'window {w}x{h} -> {out}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rom', nargs='?', default=str(ROOT / 'build' / '3ds' / 'cmake' / 'nova3.cxi'))
    ap.add_argument('--deploy', action='store_true')
    ap.add_argument('--no-sd-data', action='store_true')
    ap.add_argument('--dspfirm', choices=['stub', 'none'], default=None)
    ap.add_argument('--data', default=None)
    ap.add_argument('--log', default='3ds/nova3/nova3.log')
    ap.add_argument('--wait', default=None)
    ap.add_argument('--fail', default=r'FATAL|=== (host exception|crash)')
    ap.add_argument('--mark', default=None)
    ap.add_argument('--timeout', type=float, default=120)
    ap.add_argument('--shot', default=None)
    ap.add_argument('--shot-every', type=float, default=0, help='also capture the window every S seconds (<shot>_<t>.png)')
    ap.add_argument('--test', default=None)
    ap.add_argument('--tail', type=int, default=60, help='log lines printed at the end')
    ap.add_argument('--visible', action='store_true', help='let the emulator take the focus as it starts')
    ap.add_argument('--minimized', action='store_true', help='keep the emulator minimized (default: a normal window that does not take the focus)')
    args = ap.parse_args()
    rom = pathlib.Path(args.rom).resolve()
    if not rom.exists():
        sys.exit(f'missing {rom}')
    take_lock()
    proc = None
    test = SDMC / '3ds' / 'nova3' / 'test.txt'
    try:
        configure()
        if args.deploy:
            deploy(pathlib.Path(args.data) if args.data else SLIM if (SLIM / 'pica_textures.pak').exists() else DATA)
        elif args.no_sd_data:
            shutil.rmtree(SDMC / '3ds' / 'nova3' / 'nova3_data', ignore_errors=True)  # (hard links)
            print('removed the game files from the emulated SD card: the RomFS is read')
        firm = SDMC / '3ds' / 'dspfirm.cdc'
        if args.dspfirm == 'stub':
            stub = bytearray(768)
            stub[0x100:0x108] = b'DSP1' + len(stub).to_bytes(4, 'little')
            firm.parent.mkdir(parents=True, exist_ok=True)
            firm.write_bytes(stub)
        elif args.dspfirm == 'none' and firm.exists():
            firm.unlink()
        log = SDMC / args.log
        log.parent.mkdir(parents=True, exist_ok=True)
        if log.exists():
            log.unlink()
        if args.test:
            shutil.copy(args.test, test)
        emu_log = USER / 'log' / 'azahar_log.txt'
        start = time.time()
        si = None
        if not args.visible:  # a normal window (or minimized), without the focus from the start
            si = subprocess.STARTUPINFO()
            si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            si.wShowWindow = SW_SHOWMINNOACTIVE if args.minimized else SW_SHOWNOACTIVATE
        previous = user32.GetForegroundWindow()  # (the user's: it gets the foreground back)
        proc = subprocess.Popen([str(EMU / 'azahar.exe'), str(rom)], cwd=EMU, startupinfo=si)
        if not args.visible:  # minimized for the whole run, or just the focus handed back as it starts
            threading.Thread(target=keep_minimized, args=(proc, previous, args.minimized, None if args.minimized else 10),
                             daemon=True).start()
        wait = re.compile(args.wait) if args.wait else None
        fail = re.compile(args.fail) if args.fail else None
        mark = re.compile(args.mark) if args.mark else None
        outcome, seen, next_shot = 'timeout', 0, args.shot_every
        while time.time() - start < args.timeout:
            time.sleep(1)
            if args.shot and args.shot_every and time.time() - start >= next_shot and proc.poll() is None:
                capture(proc.pid, args.shot.replace('.png', f'_{int(next_shot):04d}.png'))
                next_shot += args.shot_every
            if proc.poll() is not None:
                outcome = f'azahar exited ({proc.returncode})'
                break
            if log.exists():
                lines = log.read_text(encoding='utf-8', errors='replace').splitlines()
                new, seen = lines[seen:], len(lines)
                for line in new:
                    if mark and mark.search(line):
                        print(f'[{time.time() - start:5.0f} s] {line}', flush=True)
                    if wait and wait.search(line):
                        outcome = f'matched: {line}'
                    elif fail and fail.search(line):
                        outcome = f'failed: {line}'
                if outcome != 'timeout':
                    time.sleep(2)  # the rest of a report
                    break
        print(f'--- {outcome} after {time.time() - start:.0f} s')
        if args.shot and proc.poll() is None:
            capture(proc.pid, args.shot)
        if log.exists():
            lines = log.read_text(encoding='utf-8', errors='replace').splitlines()
            print(f'--- {log} ({len(lines)} lines), last {args.tail}:')
            print('\n'.join(lines[-args.tail:]))
        else:
            print(f'--- no {log}')
            if emu_log.exists():
                print('--- azahar log, last 30 lines:')
                print('\n'.join(emu_log.read_text(encoding='utf-8', errors='replace').splitlines()[-30:]))
        return 0 if outcome.startswith('matched') or (not wait and outcome == 'timeout') else 1
    finally:
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
        if args.test and test.exists():
            test.unlink()
        LOCK.rmdir()


if __name__ == '__main__':
    sys.exit(main())
