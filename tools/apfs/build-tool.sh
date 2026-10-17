#!/bin/bash
# Build the kernel's APFS reading code as a program for this machine, so it can be run
# against a real image before it goes anywhere near the kernel.  The .c files are taken from
# the kernel tree unchanged; only apfs_lib.h's few allocation and logging macros differ
# when KERNEL is not defined.  apfs_deflate.c is libdeflate's decompressor; host-shim/ stands
# in for its x86 code, which is not in the kernel tree, so the host runs the kernel's decoder.
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
# TOOL_CFLAGS: more for the compiler, e.g. "-fsanitize=thread -g" with APFS_TOOL_THREADS
TOOL_CFLAGS=${TOOL_CFLAGS:-}
OBJ=$(mktemp -d)
trap 'rm -rf "$OBJ"' EXIT
# (libdeflate as released compares signed with unsigned in a few places; -Wextra says so)
cc -std=c11 -O2 -Wall -Wextra -Wno-sign-compare -Wno-unused-parameter $TOOL_CFLAGS -I"$HERE/host-shim" -c -o "$OBJ/apfs_deflate.o" \
	"$SRC/apfs_deflate.c"
cc -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Wno-unused-parameter -I"$SRC" -I"$HERE" \
	$TOOL_CFLAGS -o "$OUT" "$HERE/apfs-tool.c" "$SRC/apfs_lib.c" "$OBJ/apfs_deflate.o" -lz -pthread
echo "build-tool.sh: $OUT (from $SRC)"
