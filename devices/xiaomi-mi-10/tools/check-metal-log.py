#!/usr/bin/env python3
"""Require both public MTL and native GPU probe completion in a UMI boot log."""
import argparse
from pathlib import Path
import re
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('log', type=Path)
args = parser.parse_args()
text = args.log.read_bytes().replace(b'\0', b'').decode(errors='replace')
checks = {
    'iOS Metal copy/fill': bool(re.search(r'metal_test:\s+blits WORK\b', text)),
    'Adreno 650 native transport': bool(re.search(r'probe: GPU (?:0|650), chip id 0x(?:[0-9a-f]*0)?605000[0-9a-f]\b', text)),
    'native probe completed': 'probe: 0 failures' in text,
    'no public Metal test crash': not re.search(r'metal_test:\s+signal \d+', text),
    'no native probe failure': 'probe: FAIL' not in text,
}
for name, ok in checks.items():
    print(('PASS: ' if ok else 'FAIL: ') + name)
raise SystemExit(0 if all(checks.values()) else 1)
