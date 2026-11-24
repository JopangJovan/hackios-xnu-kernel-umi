#!/usr/bin/env python3
"""Embed the verified local DDR partition, never the 40-byte firmware placeholder."""
import argparse
import hashlib
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--input', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--msadp', type=Path, required=True)
args = parser.parse_args()
data = args.input.read_bytes()
digest = hashlib.sha256(data).hexdigest()
if len(data) != 1048576 or digest != '08a3cc690da248f80665405ea3e5d011764eeff5d7dad8d1462479af94c47fc8':
    raise SystemExit('SDX55 DDR partition size/hash mismatch; refusing placeholder or foreign data')
lines = ['static const uint8_t umi_sdx55_ddr[] = {']
for offset in range(0, len(data), 16):
    lines.append('    ' + ','.join(f'0x{x:02x}' for x in data[offset:offset + 16]) + ',')
lines.extend(['};', ''])
msadp = args.msadp.read_bytes()
if len(msadp) != 262144 or hashlib.sha256(msadp).hexdigest() != 'b42cbf5baa6a027dafba645873fa693555459fe3cc656ac735da1ddd8c617c0c':
    raise SystemExit('SDX55 MSADP partition size/hash mismatch')
lines.append('static const uint8_t umi_sdx55_msadp[] = {')
for offset in range(0, len(msadp), 16):
    lines.append('    ' + ','.join(f'0x{x:02x}' for x in msadp[offset:offset + 16]) + ',')
lines.extend(['};', ''])
contents = '\n'.join(lines)
args.output.parent.mkdir(parents=True, exist_ok=True)
if not args.output.exists() or args.output.read_text() != contents:
    args.output.write_text(contents)
print(f'embedded verified local SDX55 DDR partition: {len(data)} bytes sha256={digest}')
