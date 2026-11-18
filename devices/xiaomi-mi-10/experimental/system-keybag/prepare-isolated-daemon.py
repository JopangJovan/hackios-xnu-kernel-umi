#!/usr/bin/env python3
"""Relocate only the eight data-path strings in the local iOS 16.5 keybagd.

The output still uses the stock transaction code and XPC ABI, but is a modified
diagnostic binary, not an unmodified Apple binary. No Apple binary is checked in.
"""
import argparse
import hashlib
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('source', type=Path)
p.add_argument('output', type=Path)
p.add_argument('--sha256', required=True)
p.add_argument('--root', required=True, help='Fresh /var/tmp/kXX/ path; 13 bytes')
a = p.parse_args()
original = a.source.read_bytes()
if hashlib.sha256(original).hexdigest() != a.sha256:
    raise SystemExit('Input hash mismatch; nothing written')
root = a.root.encode('ascii')
if len(root) != 13 or not root.startswith(b'/var/tmp/k') or root[-1:] != b'/' or not root[10:12].isalnum():
    raise SystemExit('Expected /var/tmp/kXX/')
if original.count(b'/private/var/') != 8 or a.output.exists():
    raise SystemExit('Unexpected path layout or output already exists')
modified = original.replace(b'/private/var/', root)
a.output.write_bytes(modified)
a.output.chmod(0o755)
print('Relocated 8 data-path strings; re-sign the diagnostic output preserving entitlements')
print('source_sha256=' + hashlib.sha256(original).hexdigest())
print('unsigned_output_sha256=' + hashlib.sha256(modified).hexdigest())
