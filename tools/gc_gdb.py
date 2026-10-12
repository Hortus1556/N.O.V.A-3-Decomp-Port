"""Where is a GameCube program stuck? Runs it in Dolphin with the GDB stub on, lets it run for
a while, interrupts it and prints the registers with the symbols of the PC and LR (from the
ELF beside the DOL), sampled a few times.

  python tools/gc_gdb.py build/gc/hello.elf --after 5 --samples 3

Uses Dolphin's GDB remote stub ([General] GDBPort in the harness's own user folder, see
tools/gc_run.py) and the GDB remote serial protocol directly (no gdb needed).
"""
import argparse, os, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gc_run

PORT = 55020
NM = r'C:\msys64\opt\devkitpro\devkitPPC\bin\powerpc-eabi-nm.exe'


class Rsp:
    def __init__(self, sock):
        self.s = sock
        self.buf = b''

    def send(self, data):
        pkt = b'$' + data + b'#' + b'%02x' % (sum(data) & 0xff)
        self.s.sendall(pkt)

    def recv_packet(self, timeout=10):
        self.s.settimeout(timeout)
        while True:
            start = self.buf.find(b'$')
            end = self.buf.find(b'#', start + 1) if start >= 0 else -1
            if start >= 0 and end >= 0 and len(self.buf) >= end + 3:
                data = self.buf[start + 1:end]
                self.buf = self.buf[end + 3:]
                self.s.sendall(b'+')
                return data
            chunk = self.s.recv(65536)
            if not chunk:
                raise EOFError
            self.buf += chunk

    def drain(self):
        # Dolphin may send an extra reply after a stop: drop whatever is pending
        while True:
            try:
                self.recv_packet(0.3)
            except (socket.timeout, TimeoutError):
                return

    def cmd(self, data, timeout=10):
        self.drain()
        self.send(data)
        return self.recv_packet(timeout)

    def read32(self, addr):
        return int(self.cmd(b'm%x,4' % addr).decode(), 16)


def symbols(elf):
    out = subprocess.run([NM, '-n', elf], capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in 'tTwW':
            syms.append((int(parts[0], 16), parts[2]))
    return syms


def name(syms, addr):
    best = None
    for a, n in syms:
        if a > addr:
            break
        best = (a, n)
    return f'{best[1]}+{addr - best[0]:#x}' if best else '?'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('program')
    ap.add_argument('--after', type=float, default=5)
    ap.add_argument('--samples', type=int, default=3)
    ap.add_argument('--break', dest='brk', action='append', default=[],
                    help='stop at this function instead of after --after seconds (repeatable)')
    ap.add_argument('--elf', help='symbols (default: the .elf beside the program)')
    ap.add_argument('--disc', help='disc image in the drive while the DOL runs')
    ap.add_argument('--fast-disc', action='store_true')
    a = ap.parse_args()
    gc_run.write_config(0, a.fast_disc, [], disc=a.disc)
    ini = os.path.join(gc_run.USER, 'Config', 'Dolphin.ini')
    with open(ini, 'a') as f:
        f.write(f'[General]\nGDBPort = {PORT}\n')
    elf = a.elf or os.path.splitext(a.program)[0] + '.elf'
    syms = symbols(elf)
    addr_of = {n: v for v, n in syms}
    p = subprocess.Popen([gc_run.DOLPHIN, '-u', gc_run.USER, '-b', '-e', os.path.abspath(a.program)])
    try:
        sock = None
        for _ in range(60):
            try:
                sock = socket.create_connection(('127.0.0.1', PORT), timeout=2)
                break
            except OSError:
                time.sleep(0.5)
        if not sock:
            print('gc_gdb: no GDB stub')
            return
        r = Rsp(sock)
        r.cmd(b'?')
        for fn in a.brk:
            print(f'break {fn} at {addr_of[fn]:08x}:', r.cmd(b'Z0,%x,4' % addr_of[fn]).decode())
        for i in range(a.samples):
            r.send(b'c')
            if a.brk:
                stop = r.recv_packet(timeout=600).decode()
                pc = next((int(f[3:], 16) for f in stop.split(';') if f.startswith('40:') or f[3:6] == '40:'), None)
                print('stop:', stop, f'({name(syms, pc)})' if pc is not None else '')
            else:
                time.sleep(a.after if i == 0 else 1)
                sock.sendall(b'\x03')
                r.recv_packet()
            regs = bytes.fromhex(r.cmd(b'g').decode())
            gpr = [int.from_bytes(regs[k * 4:k * 4 + 4], 'big') for k in range(32)]
            # the PowerPC register block: 32 GPRs, 32 FPRs (8 bytes), then pc, msr, cr, lr, ctr, xer
            o = 32 * 4 + 32 * 8
            pc, msr, cr, lr, ctr = (int.from_bytes(regs[o + k * 4:o + k * 4 + 4], 'big') for k in range(5))
            print(f'sample {i}: pc {pc:08x} {name(syms, pc)}  lr {lr:08x} {name(syms, lr)}  msr {msr:08x}')
            print('  ' + ' '.join(f'r{k}={gpr[k]:08x}' for k in range(1, 13)))
            # the stack's back chain: each frame's word 0 is the caller's frame, word 1 its LR save
            sp = gpr[1]
            for depth in range(16):
                if not 0x80000000 <= sp < 0x81800000:
                    break
                try:
                    nxt = r.read32(sp)
                    ret = r.read32(nxt + 4) if 0x80000000 <= nxt < 0x81800000 else 0
                except (ValueError, EOFError):
                    break
                if ret:
                    print(f'  #{depth} {ret:08x} {name(syms, ret)}')
                sp = nxt
    finally:
        p.kill()


if __name__ == '__main__':
    main()
