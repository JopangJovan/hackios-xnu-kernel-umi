#!/bin/sh
# Native software model only. No ADB, device files, MMIO or firmware required.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$root/tools/umi-mhi-test/shim" -I"$root/xnu/files/bsd" \
    "$root/tools/umi-mhi-test/test_mhi.c" "$root/xnu/files/bsd/dev/umi_mhi.c" \
    -o "$build/test-mhi"
"$build/test-mhi"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$root/tools/umi-mhi-test/shim" -I"$root/xnu/files/bsd" \
    "$root/tools/umi-mhi-test/test_pcie.c" "$root/xnu/files/bsd/dev/umi_pcie.c" \
    -o "$build/test-pcie"
"$build/test-pcie"
