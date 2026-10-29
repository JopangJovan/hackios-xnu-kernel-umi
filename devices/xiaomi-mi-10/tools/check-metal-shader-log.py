#!/usr/bin/env python3
"""Verify the isolated clearFragment01 AIR -> IR3 execution on UMI."""
import argparse
import math
from pathlib import Path
import re
import struct
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('log', type=Path)
a = p.parse_args()
text = a.log.read_bytes().replace(b'\0', b'').decode(errors='replace')
m = re.search(r'air_run: the pixel at \(0, 0\) of render target 0 came back as:\s*\n'
              r'((?:[ \t]+[-+0-9.eE]+\n){4})', text)
actual = [float(v) for v in m.group(1).split()] if m else []
# The runner's first constant buffer contains these bytes; unpack them on the
# host independently of the shader compiler or GPU readback.
expected = struct.unpack('<4f', bytes((i * 7 + 13) & 255 for i in range(16)))
checks = {
    'FD650 hardware readback': "air_run: read back from FD650 (the GPU, through the kernel's msm device)" in text,
    'native fragment instructions': 'Native code for unnamed FRAG shader clearFragment01' in text,
    'all four channels match host reference': len(actual) == 4 and all(
        math.isfinite(x) and math.isclose(x, y, rel_tol=1e-8, abs_tol=0) for x, y in zip(actual, expected)),
    'draw completed': 'air_run: drew 4x4 into r32g32b32a32_float, 1 render target' in text,
    'native Metal probe completed': 'probe: 0 failures' in text and 'probe: FAIL' not in text,
}
for name, ok in checks.items():
    print(('PASS: ' if ok else 'FAIL: ') + name)
raise SystemExit(0 if all(checks.values()) else 1)
