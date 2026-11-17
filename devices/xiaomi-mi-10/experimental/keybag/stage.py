#!/usr/bin/env python3
"""Stage only the opt-in independent-bag experiment in a prepared XNU tree."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--xnu', required=True, type=Path)
parser.add_argument('--loader', required=True, type=Path,
                    help='prepared temporary loader directory, including the RAW RNG fix')
parser.add_argument('--check', action='store_true')
args = parser.parse_args()
target = args.xnu / 'iokit/Kernel/MiniAppleKeyStore.cpp'
pin = json.loads((root / 'source-lock.json').read_text())
digest = hashlib.sha256(target.read_bytes()).hexdigest()
loader_digest = hashlib.sha256((args.loader/'loader.c').read_bytes()).hexdigest()
items = [(digest, pin['base_sha256'], pin['patched_sha256'], args.xnu, 'MiniAppleKeyStore.patch'),
         (loader_digest, pin['loader_base_sha256'], pin['loader_patched_sha256'], args.loader, 'loader-allow.patch')]
for actual, before, after, tree, patch in items:
    if actual not in (before, after):
        raise SystemExit(f'Unexpected prerequisite for {patch}; nothing changed')
    if actual == before:
        subprocess.run(['git', 'apply', '--check', str(root/patch)], cwd=tree, check=True)
if args.check:
    print('Independent keybag patch prerequisites passed')
else:
    for actual, before, after, tree, patch in items:
        if actual == before:
            subprocess.run(['git', 'apply', str(root/patch)], cwd=tree, check=True)
    for name in ('UmiAksLab.h', 'umi_aks_crypto.h'):
        shutil.copyfile(root/name, target.parent/name)
    print('Staged independent keybags; requires umi_aks_lab=1 and explicit root client opt-in')
