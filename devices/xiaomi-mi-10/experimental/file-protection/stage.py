#!/usr/bin/env python3
"""Stage isolated class-A HFS protection after system-volume, in a copy only."""
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
# The low-stack MAC implementation is a required crypt-volume prerequisite.
if (a.xnu / 'bsd/dev/umi_volume_crypto.h').read_bytes() != (p.parent / 'crypt-volume/umi_volume_crypto.h').read_bytes():
    raise SystemExit('Update crypt-volume prerequisites first; no changes')
pins = json.loads((p / 'source-lock.json').read_text())
states = set()
for name, pin in pins.items():
    digest = hashlib.sha256((a.xnu / name).read_bytes()).hexdigest()
    match = next((state for state in ('base', 'patched') if digest == pin[state]), None)
    if match is None:
        raise SystemExit('Unknown source; no changes: ' + name)
    states.add(match)
if len(states) != 1:
    raise SystemExit('Partial stage; no changes')
if states == {'base'}:
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        for name in pins:
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(a.xnu / name, root / name)
        subprocess.run(['git', 'apply', str(p / 'file-protection.patch')], cwd=root, check=True)
        for name, pin in pins.items():
            if hashlib.sha256((root / name).read_bytes()).hexdigest() != pin['patched']:
                raise SystemExit('Reconstruction mismatch; no changes: ' + name)
    if not a.check:
        subprocess.run(['git', 'apply', str(p / 'file-protection.patch')], cwd=a.xnu, check=True)
if not a.check:
    for src, dst in (('UmiHfsKeys.h', 'iokit/Kernel'), ('umi_hfs_io.h', 'bsd/dev'), ('umi_hfs_crypto.h', 'bsd/dev')):
        shutil.copyfile(p / src, a.xnu / dst / src)
print('PASS: isolated HFS stage prerequisites' if a.check else 'Staged isolated class-A HFS prototype')
