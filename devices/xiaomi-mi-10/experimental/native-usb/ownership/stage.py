#!/usr/bin/env python3
"""Fence UEFI charger-policy calls in a disposable prepared native USB loader."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--loader', type=Path, required=True)
parser.add_argument('--check', action='store_true')
args = parser.parse_args()
path = args.loader / 'loader.c'
pins = json.loads((root / 'source-lock.json').read_text())['loader.c']
original = path.read_bytes()
digest = hashlib.sha256(original).hexdigest()
if digest not in pins.values():
    raise SystemExit('Unknown prepared loader; no changes made')
if digest == pins['base']:
    with tempfile.TemporaryDirectory() as temp:
        candidate = Path(temp) / 'loader.c'
        candidate.write_bytes(original)
        subprocess.run(['git', 'apply', str(root / 'charger-owner.patch')],
                       cwd=temp, check=True)
        updated = candidate.read_bytes()
        if hashlib.sha256(updated).hexdigest() != pins['patched']:
            raise SystemExit('Reconstructed loader hash mismatch; no changes made')
    if not args.check:
        if path.read_bytes() != original:
            raise SystemExit('Loader changed during validation; no changes made')
        path.write_bytes(updated)
print('USB ownership fence prerequisites passed' if args.check else
      'USB ownership fence staged; no build, boot or device writes performed')
