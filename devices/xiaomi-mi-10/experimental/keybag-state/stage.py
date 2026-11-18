#!/usr/bin/env python3
"""Stage after crypt-volume. Never enable the system keybag or alter live data."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--xnu', type=Path, required=True)
parser.add_argument('--check', action='store_true')
args = parser.parse_args()
target = args.xnu / 'iokit/Kernel/UmiAksLab.h'
pins = json.loads((root / 'source-lock.json').read_text())
digest = hashlib.sha256(target.read_bytes()).hexdigest()
if digest not in (pins['base_sha256'], pins['patched_sha256']):
    raise SystemExit('Unexpected independent-bag base; nothing changed')
if digest == pins['base_sha256']:
    subprocess.run(['git', 'apply', '--check', str(root / 'state.patch')], cwd=args.xnu, check=True)
if not args.check:
    if digest == pins['base_sha256']:
        subprocess.run(['git', 'apply', str(root / 'state.patch')], cwd=args.xnu, check=True)
    shutil.copyfile(root / 'umi_aks_state.h', target.parent / 'umi_aks_state.h')
print('PASS: isolated AKS state prerequisites' if args.check else 'Staged isolated AKS state interfaces')
