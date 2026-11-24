#!/usr/bin/env python3
"""Generate an ignored C include from UMI's locally extracted SDX55 SBL1."""

import argparse
import hashlib
from pathlib import Path


EXPECTED_SIZE = 548056
EXPECTED_SHA256 = "7d3897eb5d7097db94600bcdc1ac8cb51e4aa2addaf2e4329fa5ab0d63f0b8d6"

parser = argparse.ArgumentParser()
parser.add_argument("--input", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()

data = args.input.read_bytes()
digest = hashlib.sha256(data).hexdigest()
if len(data) != EXPECTED_SIZE or digest != EXPECTED_SHA256:
    raise SystemExit(
        f"{args.input}: expected UMI SDX55 SBL1 {EXPECTED_SIZE} bytes / "
        f"{EXPECTED_SHA256}, got {len(data)} bytes / {digest}"
    )

lines = ["static const uint8_t umi_sdx55_sbl1[] = {"]
for offset in range(0, len(data), 16):
    lines.append("    " + ",".join(f"0x{byte:02x}" for byte in data[offset : offset + 16]) + ",")
lines.extend(
    [
        "};",
        f"static const size_t umi_sdx55_sbl1_size = {len(data)}u;",
        "",
    ]
)
contents = "\n".join(lines)
args.output.parent.mkdir(parents=True, exist_ok=True)
if not args.output.exists() or args.output.read_text(encoding="utf-8") != contents:
    args.output.write_text(contents, encoding="utf-8")
print(f"embedded local SDX55 SBL1: {len(data)} bytes sha256={digest}")
