#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
metal=${HAKIOS_METAL:-$root/work/upstream-hakios-metal}
sdk=${SDK:-${THEOS:-$root/work/theos}/sdks/iPhoneOS16.5.sdk}
[ -d "$metal/.git" ] || python3 "$root/devices/xiaomi-mi-10/sync-dependencies.py"
make -C "$metal" test
# Upstream invokes the host ar directly; GNU's archive index is not understood
# by ld64. Reindex Mach-O members before linking the iOS probe.
make -C "$metal" out/ios/libhakios-metal.a HAKIOS="$root" SDK="$sdk" \
  IOS_CXX="clang++ -target arm64-apple-ios16.0 -isysroot $sdk --ld-path=$root/xnu-deps/host/bin/ld64 -stdlib=libc++ -Wno-unused-command-line-argument"
llvm-ar --format=darwin rcs "$metal/out/ios/libhakios-metal.darwin.a" "$metal"/out/ios/obj/*.o
mv "$metal/out/ios/libhakios-metal.darwin.a" "$metal/out/ios/libhakios-metal.a"
make -C "$metal" ios HAKIOS="$root" SDK="$sdk" \
  IOS_CXX="clang++ -target arm64-apple-ios16.0 -isysroot $sdk --ld-path=$root/xnu-deps/host/bin/ld64 -stdlib=libc++ -Wno-unused-command-line-argument"
