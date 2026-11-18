#!/usr/bin/env python3
"""Stage opt-in native system-bag management after keybag-state, in a copy only."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

p = Path(__file__).resolve().parent
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--xnu', type=Path, required=True)
ap.add_argument('--check', action='store_true')
a = ap.parse_args()
target = a.xnu / 'iokit/Kernel/MiniAppleKeyStore.cpp'
pin = json.loads((p.parent / 'keybag-state/source-lock.json').read_text())
if hashlib.sha256((target.parent / 'UmiAksLab.h').read_bytes()).hexdigest() != pin['patched_sha256']:
    raise SystemExit('Requires the tested keybag-state and constant-time AES stages')
trace = json.loads((p / 'trace-hashes.json').read_text())
management = json.loads((p / 'management-hashes.json').read_text())
events = json.loads((p / 'events-hashes.json').read_text())
access = json.loads((p / 'device-access-hashes.json').read_text())
digest = hashlib.sha256(target.read_bytes()).hexdigest()
patches = []
if digest == trace['base']:
    patches = ['trace.patch', 'management.patch', 'events.patch', 'device-access.patch']
elif digest == management['base']:
    patches = ['management.patch', 'events.patch', 'device-access.patch']
elif digest == events['base']:
    patches = ['events.patch', 'device-access.patch']
elif digest == access['base']:
    patches = ['device-access.patch']
elif digest != access['patched']:
    raise SystemExit('Unexpected native AKS prerequisite; nothing changed')
# Validate the entire ordered patch chain before touching the target tree.
import tempfile
with tempfile.TemporaryDirectory() as td:
    copy = Path(td); (copy / 'iokit/Kernel').mkdir(parents=True)
    shutil.copyfile(target, copy / 'iokit/Kernel/MiniAppleKeyStore.cpp')
    for patch in patches:
        subprocess.run(['git', 'apply', str(p / patch)], cwd=copy, check=True)
    if hashlib.sha256((copy / 'iokit/Kernel/MiniAppleKeyStore.cpp').read_bytes()).hexdigest() != access['patched']:
        raise SystemExit('Native management reconstruction mismatch')
if not a.check:
    for patch in patches:
        subprocess.run(['git', 'apply', str(p / patch)], cwd=a.xnu, check=True)
    for name in ('UmiAksNativeTrace.h', 'UmiAksNative.h'):
        shutil.copyfile(p / name, target.parent / name)
print('PASS: native management prerequisites' if a.check else 'Staged explicit opt-in native management')
