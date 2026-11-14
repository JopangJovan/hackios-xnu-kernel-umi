#!/bin/bash
# Build the UMI 20F66 user interface library; this command does not edit an image.
set -euo pipefail
device=$(cd "$(dirname "$0")/.." && pwd)
repo=$(cd "$device/../.." && pwd)
out=${1:-$repo/out/xiaomi-mi-10/sdx55-corelocation-20F66/umi-gps-ui.dylib}
sdk=${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk
cc=${XNU_CLANG_DIR:-$HOME/tools/bin18}/clang
mkdir -p "$(dirname "$out")"
tmpdir=$(mktemp -d "$(dirname "$out")/.umi-gps-ui.XXXXXX")
tmp="$tmpdir/umi-gps-ui.dylib"
trap 'rm -f -- "$tmp"; rmdir -- "$tmpdir"' EXIT
"$cc" -target arm64-apple-ios16.5 -isysroot "$sdk" --ld-path="$repo/xnu-deps/host/bin/ld64" \
    -O2 -Wall -Wextra -Werror -dynamiclib -install_name /redwood/lib/umi-gps-ui.dylib \
    -I"$device/overlays/xnu/files/bsd" -framework CoreFoundation -framework Foundation -framework UIKit -lobjc \
    -fobjc-arc "$device/overlays/userland/src/umi_cell_settings.m" \
    "$device/overlays/userland/src/umi_voice_phone.m" \
    "$device/overlays/userland/src/umi_gps_settings.c" -o "$tmp"
chmod 755 "$tmp"
mv -f -- "$tmp" "$out"
printf '%s\n' "$out"
