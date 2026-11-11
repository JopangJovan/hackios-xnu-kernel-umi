#!/bin/bash
# Compile a standalone baseband capabilities diagnostic for the private test ramdisk.
set -euo pipefail
device=$(cd "$(dirname "$0")/.." && pwd)
repo=$(cd "$device/../.." && pwd)
out=${1:-$repo/out/xiaomi-mi-10/sdx55-corelocation-20F66/umi_baseband_check}
sdk=${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk
cc=${XNU_CLANG_DIR:-$HOME/tools/bin18}/clang
mkdir -p "$(dirname "$out")"
tmpdir=$(mktemp -d "$(dirname "$out")/.umi_baseband_check.XXXXXX")
tmp="$tmpdir/umi_baseband_check"
trap 'rm -f -- "$tmp"; rmdir -- "$tmpdir"' EXIT
"$cc" -target arm64-apple-ios16.5 -isysroot "$sdk" --ld-path="$repo/xnu-deps/host/bin/ld64" \
    -O2 -Wall -Wextra -Werror -fobjc-arc -fblocks -framework Foundation -framework CoreFoundation -framework IOKit -framework CoreTelephony -lMobileGestalt \
    "$device/overlays/userland/src/umi_baseband_check.m" -o "$tmp"
chmod 755 "$tmp"
mv -f -- "$tmp" "$out"
