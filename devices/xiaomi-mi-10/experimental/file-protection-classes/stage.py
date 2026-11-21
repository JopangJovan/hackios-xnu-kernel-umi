#!/usr/bin/env python3
"""Extend the known class-A fixture stage with isolated class-C key lifetime."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile

p = Path(__file__).resolve().parent
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--xnu', type=Path, required=True)
ap.add_argument('--check', action='store_true')
a = ap.parse_args()
pins = json.loads((p / 'source-lock.json').read_text())
# Pin the unchanged parts of the prerequisite, too: accepting just the key
# header would not establish that filesystem ABI and encrypted I/O agree.
base = p.parent / 'file-protection'
for name, pin in json.loads((base / 'source-lock.json').read_text()).items():
    if name not in pins and hashlib.sha256((a.xnu / name).read_bytes()).hexdigest() != pin['patched']:
        raise SystemExit('Mismatched class-A prerequisite; no changes: ' + name)
for source, name in ((base / 'umi_hfs_io.h', 'bsd/dev/umi_hfs_io.h'),
                     (base / 'umi_hfs_crypto.h', 'bsd/dev/umi_hfs_crypto.h'),
                     (p.parent / 'crypt-volume/umi_volume_crypto.h', 'bsd/dev/umi_volume_crypto.h')):
    if (a.xnu / name).read_bytes() != source.read_bytes():
        raise SystemExit('Mismatched crypto prerequisite; no changes: ' + name)
states = set()
for name, pin in pins.items():
    digest = hashlib.sha256((a.xnu / name).read_bytes()).hexdigest()
    match = next((s for s in ('base', 'patched') if digest == pin[s]), None)
    if match is None:
        raise SystemExit('Unknown source; no changes: ' + name)
    states.add(match)
if len(states) != 1:
    raise SystemExit('Partial class extension; no changes')
if hashlib.sha256((p / 'UmiHfsKeys.h').read_bytes()).hexdigest() != pins['iokit/Kernel/UmiHfsKeys.h']['patched']:
    raise SystemExit('Header/patch version mismatch; no changes')
if states == {'base'}:
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        for name in pins:
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(a.xnu / name, root / name)
        subprocess.run(['git', 'apply', str(p / 'classes.patch')], cwd=root, check=True)
        for name, pin in pins.items():
            if hashlib.sha256((root / name).read_bytes()).hexdigest() != pin['patched']:
                raise SystemExit('Reconstruction mismatch; no changes: ' + name)
    if not a.check:
        subprocess.run(['git', 'apply', str(p / 'classes.patch')], cwd=a.xnu, check=True)
print('PASS: class extension prerequisites' if a.check else 'Staged isolated class-A/C extension')
