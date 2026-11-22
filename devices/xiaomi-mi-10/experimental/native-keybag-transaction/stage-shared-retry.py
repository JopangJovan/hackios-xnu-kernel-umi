#!/usr/bin/env python3
"""Stage the independently booted retry test in a copy of the qualified v13 tree."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

source = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--xnu', required=True, type=Path)
parser.add_argument('--check', action='store_true')
args = parser.parse_args()

inputs = json.loads((source / 'shared-retry-inputs.json').read_text())
for path, digest in inputs.items():
    if hashlib.sha256((args.xnu / path).read_bytes()).hexdigest() != digest:
        raise SystemExit(f'Unexpected v13 prerequisite {path}; nothing changed')

headers = [args.xnu / 'iokit/Kernel' / name for name in
           ('UmiAksSharedRetry.h', 'UmiAksRetryProtocol.h')]
if any(header.exists() for header in headers):
    raise SystemExit('Unexpected retry header in baseline; nothing changed')
patch = source / 'shared-retry-kernel.patch'
subprocess.run(['git', 'apply', '--check', str(patch)], cwd=args.xnu, check=True)
if not args.check:
    subprocess.run(['git', 'apply', str(patch)], cwd=args.xnu, check=True)
    for header in headers:
        shutil.copyfile(source / header.name, header)
print('Verified shared-retry prerequisites' if args.check else 'Staged opt-in shared retry gate')
