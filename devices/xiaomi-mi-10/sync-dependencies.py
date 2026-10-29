#!/usr/bin/env python3
"""Fetch the pinned external UMI build dependencies without changing dirty clones."""
import json
from pathlib import Path
import subprocess
root = Path(__file__).resolve().parents[2]
lock = json.loads(Path(__file__).with_name('upstream.lock.json').read_text())
for name in ('metal', 'uefi'):
    item = lock[name]
    target = root / item['path']
    if not target.exists():
        subprocess.run(['git', 'clone', item['url'], str(target)], check=True)
    if subprocess.check_output(['git', '-C', str(target), 'status', '--porcelain'], text=True).strip():
        raise SystemExit(f'{name}: uncommitted work; refusing to update {target}')
    subprocess.run(['git', '-C', str(target), 'fetch', 'origin', item['revision']], check=True)
    subprocess.run(['git', '-C', str(target), 'checkout', '--detach', item['revision']], check=True)
    print(f'{name}: {item["revision"]}')
