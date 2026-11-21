#!/usr/bin/env python3
"""Apply after system-keybag in the prepared XNU copy; explicit boot opt-in only."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

p = Path(__file__).resolve().parent
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--xnu', required=True, type=Path)
ap.add_argument('--check', action='store_true')
a = ap.parse_args()
target = a.xnu / 'iokit/Kernel/UmiAksNative.h'
pin = json.loads((p / 'source-lock.json').read_text())
digest = hashlib.sha256(target.read_bytes()).hexdigest()
if digest not in (pin['base'], pin['patched']):
    raise SystemExit('Unexpected native bag source; nothing changed')
if digest == pin['base']:
    subprocess.run(['git', 'apply', '--check', str(p / 'native-volume.patch')], cwd=a.xnu, check=True)
if not a.check:
    if digest == pin['base']:
        subprocess.run(['git', 'apply', str(p / 'native-volume.patch')], cwd=a.xnu, check=True)
    shutil.copyfile(p / 'UmiSystemVolume.h', target.parent / 'UmiSystemVolume.h')
print('PASS: native volume stage prerequisites' if a.check else 'Staged isolated system-bag volume keys')
