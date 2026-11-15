#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 BUILD_DIRECTORY" >&2
    exit 2
fi
source=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH='' cd -- "$source/../../../../.." && pwd)
output=$1
mkdir -p "$output/classes" "$output/dex"

for helper in gap-view lun4-map wrapped-fbe extent-test; do
    zig cc -target aarch64-linux-musl -static -O2 -Wall -Wextra -Werror \
        "$source/$helper.c" -o "$output/$helper"
done
zig cc -target aarch64-linux-musl -ffreestanding -fno-builtin -fno-stack-protector \
    -nostdlib -static -fno-lto -O1 -Wall -Wl,-e,_start \
    "$repo/tools/android/f2fs-resize.c" -o "$output/f2fs-resize"

javac --release 8 -d "$output/classes" "$source/../DirectStorageProbe.java" \
    "$source/../ReadOnlyWrappedVts.java" "$source/NativeKeys.java"
d8 --min-api 33 --output "$output/dex" "$output/classes"/*.class
