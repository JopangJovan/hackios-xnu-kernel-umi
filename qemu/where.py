#!/usr/bin/env python3
"""Ask a running QEMU (its monitor: port 4444, or 4444 + 10 * QEMU_INSTANCE) where the guest CPU is, a few times."""
import socket, time, re, subprocess, sys, os
n = int(sys.argv[1]) if len(sys.argv) > 1 else 3
try:
    instance = int(os.environ.get('QEMU_INSTANCE') or open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'instance')).read())
except (OSError, ValueError):
    instance = 0
s = socket.create_connection(('127.0.0.1', 4444 + 10 * instance)); s.settimeout(2)
def cmd(c):
    s.sendall(c.encode() + b'\n'); time.sleep(0.4)
    out = b''
    try:
        while True:
            d = s.recv(65536)
            if not d: break
            out += d
    except socket.timeout:
        pass
    return out.decode(errors='replace')
cmd('')
pcs = []
for i in range(n):
    r = cmd('info registers')
    m = re.search(r'PC=([0-9a-f]+)', r); l = re.search(r'X30=([0-9a-f]+)', r)
    if m: pcs.append(('0x' + m.group(1), '0x' + l.group(1) if l else '0x0'))
    time.sleep(1.0)
for pc, lr in pcs:
    out = subprocess.run([os.path.join(os.path.dirname(__file__), 'sym.py'), pc, lr], capture_output=True, text=True).stdout.strip().replace('\n', '   <- lr ')
    print(out)
