"""Run a GameCube program (DOL, ELF or disc image) in Dolphin unattended and print its log.

Dolphin runs with a user folder of its own (build/gc/dolphin_user: the player's Dolphin settings
are not touched), MMU emulation on (the port pages memory through the page table), the
OSReport channel (the program's stdout through SYS_STDIO_Report) written to a file. The run
ends when a log line matches --until (default: "gc: done" or "gc: FAIL"), or after --timeout
seconds; Dolphin is then closed.

  python tools/gc_run.py build/gc/hello.dol
  python tools/gc_run.py build/gc/pager_test.dol --mem1 64     # MEM1 size override (MB)
  python tools/gc_run.py build/gc/out/nova3.dol --disc build/gc/nova3.iso   # the game, its disc inserted
"""
import argparse, os, re, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOLPHIN = r'E:\Emulators\Dolphin\Dolphin.exe'
USER = os.path.join(ROOT, 'build', 'gc', 'dolphin_user')


def write_config(mem1_mb, fast_disc, extra_logs, mmu=True, verbosity=4, disc=None):
    cfg = os.path.join(USER, 'Config')
    os.makedirs(cfg, exist_ok=True)
    core = ['[Core]', f'MMU = {mmu}', 'SkipIPL = True', 'SerialPort1 = 0', 'CPUThread = True', 'EnableCheats = False',
            f'FastDiscSpeed = {fast_disc}']
    if disc:  # inserted when the DOL boots (Dolphin's "default ISO")
        core += [f'DefaultISO = {os.path.abspath(disc)}']
    if mem1_mb:
        core += ['RAMOverrideEnable = True', f'MEM1Size = {mem1_mb << 20:#x}']
    else:
        core += ['RAMOverrideEnable = False']
    with open(os.path.join(cfg, 'Dolphin.ini'), 'w') as f:
        f.write('\n'.join(core + ['', '[Interface]', 'ConfirmStop = False', 'UsePanicHandlers = False',
                                  'OnScreenDisplayMessages = False', '', '[Analytics]', 'PermissionAsked = True',
                                  'Enabled = False', '', '[AutoUpdate]', 'UpdateTrack = ', '']))
    # the console's behaviour for what the port depends on: EFB copies written to memory (Dolphin's
    # default keeps them in its own texture cache only, where the CPU does not see them), the
    # EFB's pixel format changes emulated
    with open(os.path.join(cfg, 'GFX.ini'), 'w') as f:
        f.write('[Hacks]\nEFBToTextureEnable = False\nEFBEmulateFormatChanges = True\nEFBAccessEnable = True\n')
    with open(os.path.join(cfg, 'Logger.ini'), 'w') as f:
        f.write('[Logs]\nOSREPORT = True\nOSREPORT_HLE = True\n' + ''.join(f'{n} = True\n' for n in extra_logs) +
                f'[Options]\nVerbosity = {verbosity}\nWriteToFile = True\nWriteToConsole = False\nWriteToWindow = False\n')


def screenshot(pid, path):
    """The process's largest visible window (Dolphin's render window) as a PNG, through
    PrintWindow (also when other windows cover it). False when there is none yet."""
    import ctypes
    from ctypes import wintypes
    from PIL import Image
    user32, gdi32 = ctypes.windll.user32, ctypes.windll.gdi32
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def each(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            r = wintypes.RECT()
            user32.GetClientRect(hwnd, ctypes.byref(r))
            found.append((r.right * r.bottom, hwnd, r.right, r.bottom))
        return True
    user32.EnumWindows(each, 0)
    if not found:
        return False
    _, hwnd, w, h = max(found)
    if w <= 0 or h <= 0:
        return False
    dc = user32.GetDC(hwnd)
    mem = gdi32.CreateCompatibleDC(dc)
    bmp = gdi32.CreateCompatibleBitmap(dc, w, h)
    gdi32.SelectObject(mem, bmp)
    user32.PrintWindow(hwnd, mem, 3)  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    buf = ctypes.create_string_buffer(w * h * 4)
    bmi = (ctypes.c_uint32 * 10)(40, w, (-h) & 0xffffffff, 1 | (32 << 16), 0, 0, 0, 0, 0, 0)
    gdi32.GetDIBits(mem, bmp, 0, h, buf, bmi, 0)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(hwnd, dc)
    Image.frombuffer('RGB', (w, h), buf, 'raw', 'BGRX', 0, 1).save(path)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('program')
    ap.add_argument('--mem1', type=int, default=0, help='MEM1 size override in MB (Dolphin only)')
    ap.add_argument('--timeout', type=float, default=60)
    ap.add_argument('--until', default=r'gc: (done|FAIL)')
    ap.add_argument('--fast-disc', action='store_true', help='Dolphin\'s fast disc transfers (not a console\'s timing)')
    ap.add_argument('--no-mmu', action='store_true', help='Dolphin without page-table emulation (faster)')
    ap.add_argument('--verbosity', type=int, default=4, help='Dolphin log level (5: debug)')
    ap.add_argument('--disc', help='disc image in the drive while the DOL runs')
    ap.add_argument('--log', action='append', default=[], help='another Dolphin log channel (BOOT, EXI, ...)')
    ap.add_argument('--shot', action='append', default=[], metavar='SECONDS:PNG',
                    help='a screenshot of the Dolphin window that many seconds after the start')
    a = ap.parse_args()
    write_config(a.mem1, a.fast_disc, a.log, mmu=not a.no_mmu, verbosity=a.verbosity, disc=a.disc)
    log = os.path.join(USER, 'Logs', 'dolphin.log')
    if os.path.exists(log):
        os.remove(log)
    p = subprocess.Popen([DOLPHIN, '-u', USER, '-b', '-e', os.path.abspath(a.program)])
    until = re.compile(a.until)
    end = time.time() + a.timeout
    shown = 0
    status = 'timeout'
    start = time.time()
    shots = sorted((float(x.split(':', 1)[0]), x.split(':', 1)[1]) for x in a.shot)
    try:
        while time.time() < end:
            time.sleep(0.25)
            while shots and time.time() - start >= shots[0][0]:
                at, path = shots.pop(0)
                print(f'gc_run: screenshot {path}: ' + ('taken' if screenshot(p.pid, path) else 'no window'))
            if p.poll() is not None:
                status = f'Dolphin exited ({p.returncode})'
                break
            if not os.path.exists(log):
                continue
            with open(log, encoding='utf-8', errors='replace') as f:
                lines = f.read().splitlines()
            for line in lines[shown:]:
                print(line)
            shown = len(lines)
            if any(until.search(line) for line in lines):
                status = 'matched'
                break
    finally:
        if p.poll() is None:
            p.kill()
    print(f'gc_run: {status}')
    sys.exit(0 if status == 'matched' else 1)


if __name__ == '__main__':
    main()
