#!/usr/bin/env python3
"""Apply the USB checkpoint to explicitly supplied, prepared test trees."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xnu', required=True, type=Path)
    parser.add_argument('--loader', required=True, type=Path,
                        help='directory containing the prepared UFS loader.c')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    records = json.loads((ROOT / 'source-lock.json').read_text())['patches']
    pending = {}
    for item in records:
        tree = args.loader if item['patch'] == 'loader-handoff.patch' else args.xnu
        path = tree / item['path']
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest not in (item['before_sha256'], item['after_sha256']):
            raise SystemExit(f'Unexpected prerequisite: {path}; no changes made')
        key = (tree.resolve(), item['patch'])
        pending.setdefault(key, set()).add(digest == item['after_sha256'])
    for (tree, patch), states in pending.items():
        if len(states) != 1:
            raise SystemExit(f'Partly applied patch: {patch}; no changes made')
        if states == {False}:
            subprocess.run(['git', 'apply', '--check', str(ROOT / 'patches' / patch)],
                           cwd=tree, check=True)
    if args.check:
        print('USB prerequisite hashes and patch checks passed')
        return
    for (tree, patch), states in pending.items():
        if states == {False}:
            subprocess.run(['git', 'apply', str(ROOT / 'patches' / patch)],
                           cwd=tree, check=True)
    include = args.xnu / 'iokit/bsddev/umi-native-usb'
    include.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / 'driver/native-usb-load.inc', include / 'native-usb-load.inc')
    print('USB checkpoint staged; no build, boot, or device writes performed')


if __name__ == '__main__':
    main()
