#!/bin/bash
# Build the kernel's APFS reading code as a program for this machine, so it can be run
# against a real image before it goes anywhere near the kernel.  The .c file is taken from
# the kernel tree unchanged; only apfs_lib.h's few allocation and logging macros differ
# when KERNEL is not defined.
#
# usage: tools/apfs/build-tool.sh [OUT]       (default: apfs-work/apfs-tool)
set -eu
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${1:-$ROOT/apfs-work/apfs-tool}

SRC=""
for d in "$ROOT/xnu-src/xnu-8796.121.2/bsd/miscfs/apfs" "$ROOT/xnu/files/bsd/miscfs/apfs"; do
	[ -f "$d/apfs_lib.c" ] && { SRC=$d; break; }
done
[ -n "$SRC" ] || { echo "build-tool.sh: apfs_lib.c is in neither the kernel tree nor xnu/files" >&2; exit 1; }

mkdir -p "$(dirname "$OUT")"
cc -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Wno-unused-parameter -I"$SRC" -I"$HERE" \
	-o "$OUT" "$HERE/apfs-tool.c" "$SRC/apfs_lib.c" -lz
echo "build-tool.sh: $OUT (from $SRC)"
