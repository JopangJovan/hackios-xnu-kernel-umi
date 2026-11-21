#!/usr/bin/env python3
"""Reject large compiled stack frames on the nested fixture pageout path."""
import argparse
from pathlib import Path

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('stack_usage', type=Path, help='hakios.ufs.su from -fstack-usage')
a = ap.parse_args()
frames = {}
for line in a.stack_usage.read_text().splitlines():
    site, size, kind = line.rsplit('\t', 2)
    frames[site.rsplit(':', 1)[1]] = (int(size), kind)
limits = {'uv_io': 512, 'uv_auth': 256, 'uv_tag': 512, 'uv_strategy': 2048}
if 'uv_hmac2' in frames:  # Otherwise included in uv_tag's compiled frame.
    limits['uv_hmac2'] = 512
for name, limit in limits.items():
    size, kind = frames[name]
    if kind != 'static' or size > limit:
        raise SystemExit(f'Excessive or unbounded stack: {name}: {size} {kind}')
    print(f'PASS: {name} frame {size} bytes (limit {limit})')
