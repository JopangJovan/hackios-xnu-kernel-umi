#!/usr/bin/env python3
"""
Pack device UEFI (with the hook that starts the XNU loader) and an optional payload
into an Android boot image for `fastboot boot`.

  kernel = gzip(BootShim + UEFI FD + ["XNUP" container]) + DTB
"""
import argparse, gzip, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
MU = os.path.join(HERE, 'Mu-Silicium')
if not os.path.isdir(MU) and os.path.isdir('/home/jovan/woa/Mu-Silicium'):
    MU = '/home/jovan/woa/Mu-Silicium'

DEVICES = {
    'poco-x5-pro': {
        'fd_base': 0x9FD00000,
        'fd_size': 0x200000,
        'payload_base': 0xF0000000,
        'header_version': 1,
        'pkg': 'redwoodPkg',
        'dtb': 'redwood.dtb',
    },
    'poco-f5': {
        'fd_base': 0xA7100000,
        'fd_size': 0x300000,
        'payload_base': 0xB0000000,
        'header_version': 4,
        'pkg': 'marblePkg',
        'dtb': 'marble.dtb',
    },
}

DEFAULT_DEV = os.environ.get('DEVICE', 'poco-f5')

ap = argparse.ArgumentParser()
ap.add_argument('-d', '--device', default=DEFAULT_DEV, choices=list(DEVICES.keys()))
ap.add_argument('-p', '--payload')
ap.add_argument('-o', '--output')
ap.add_argument('--mode', default='RELEASE')
args = ap.parse_args()

dev_cfg = DEVICES[args.device]
FD_BASE = dev_cfg['fd_base']
FD_SIZE = dev_cfg['fd_size']
PAYLOAD_BASE = dev_cfg['payload_base']
PAYLOAD_MAX = 0x04000000

if not args.output:
    args.output = os.path.join(HERE, f'xnu-{args.device}.img')

PREBUILT = os.path.join(HERE, 'devices', args.device, 'prebuilt')
fd_path = os.path.join(MU, f'Build/{dev_cfg["pkg"]}/{args.mode}_CLANGPDB/FV/SILICIUM_UEFI.fd')

if os.path.isfile(fd_path):
    shim_dir = os.path.join(MU, 'BootShim')
    for f in ('BootShim.elf', 'BootShim.bin'):
        try: os.unlink(os.path.join(shim_dir, f))
        except FileNotFoundError: pass
    subprocess.check_call([
        'clang', '--target=aarch64-linux-gnu', '-c', 'BootShim.S', '-o', 'BootShim.elf',
        '-DREQUIRES_KERNEL_HEADER=1', f'-DFD_BASE={FD_BASE:#x}', f'-DFD_SIZE={FD_SIZE:#x}',
        f'-DPAYLOAD_BASE={PAYLOAD_BASE:#x}'
    ], cwd=shim_dir)
    subprocess.check_call(['llvm-objcopy', '-O', 'binary', 'BootShim.elf', 'BootShim.bin'], cwd=shim_dir)
    dtb_candidate = os.path.join(HERE, 'devices', args.device, 'prebuilt', 'device-tree.dtb')
    if not os.path.isfile(dtb_candidate):
        dtb_candidate = os.path.join(MU, 'Resources/DTBs', dev_cfg['dtb'])
    parts = {
        'shim': os.path.join(shim_dir, 'BootShim.bin'),
        'fd': fd_path,
        'dtb': dtb_candidate,
        'ramdisk': os.path.join(MU, 'Resources/ramdisk'),
        'mkbootimg': os.path.join(MU, 'Resources/Scripts/mkbootimg.py')
    }
else:
    parts = {
        'shim': os.path.join(PREBUILT, 'BootShim.bin'),
        'fd': os.path.join(PREBUILT, 'SILICIUM_UEFI.fd'),
        'dtb': os.path.join(PREBUILT, 'device-tree.dtb'),
        'ramdisk': os.path.join(PREBUILT, 'ramdisk'),
        'mkbootimg': os.path.join(PREBUILT, 'mkbootimg.py')
    }

shim = open(parts['shim'], 'rb').read()
fd = open(parts['fd'], 'rb').read()
assert len(fd) == FD_SIZE, f'FD size mismatch: {len(fd):#x} != {FD_SIZE:#x}'

blob = shim + fd
if args.payload:
    data = open(args.payload, 'rb').read()
    assert data[:4] == b'XNUP' and len(data) <= PAYLOAD_MAX - 0x1000, 'bad or oversized payload'
    blob += data
    blob += b'\0' * (-len(blob) % 16)
    print(f'payload: {len(data)} bytes, bytesum={sum(data) & 0xffffffff:08x}')

dtb = open(parts['dtb'], 'rb').read()
kernel = args.output + '.kernel~'
open(kernel, 'wb').write(gzip.compress(blob, 6) + dtb)
try:
    subprocess.check_call([
        sys.executable, parts['mkbootimg'], '--kernel', kernel, '--ramdisk', parts['ramdisk'],
        '--header_version', str(dev_cfg.get('header_version', 1)), '-o', args.output
    ])
finally:
    if os.path.exists(kernel):
        os.unlink(kernel)
print(f'{args.output}: {os.path.getsize(args.output)} bytes (shim {len(shim)}, uncompressed {len(blob)})')
