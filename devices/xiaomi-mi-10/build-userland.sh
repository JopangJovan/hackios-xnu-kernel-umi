#!/bin/bash
set -euo pipefail
DEVICE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$DEVICE_DIR/../.." && pwd)
"$DEVICE_DIR/build-metal.sh"
export HAKIOS_METAL=${HAKIOS_METAL:-$ROOT/work/upstream-hakios-metal}
python3 "$DEVICE_DIR/prepare.py" userland
export DEVICE=umi BLUETOOTH=1 GL=1
export EXTRA="${EXTRA:+$EXTRA }$HAKIOS_METAL/out/ios/hakios-metal-probe"
export RC_JOBS=${RC_JOBS:-rc.jobs.umi-boot}
if [ "${UMI_METAL_TEST:-0}" = 1 ]; then
  export RC_JOBS=rc.jobs.umi-metal
fi
if [ "${UMI_METAL_SHADERS:-0}" = 1 ]; then
  export AIR=1 RC_JOBS=rc.jobs.umi-shaders
  "$ROOT/tools/mk-mesa.sh"
fi
# A cached system image from before Metal integration lacks the plugin link.
# Rebuild it once when this device's system layout changes, then cache it again.
layout="umi-native-metal-v1"
stamp="$ROOT/out/xiaomi-mi-10/system-layout"
if [ "$(cat "$stamp" 2>/dev/null || true)" != "$layout" ]; then
  export SYSTEM=1
fi
export ADSP= REST=${REST:-1}
"$ROOT/out/xiaomi-mi-10/userland-build/userland/build.sh" "$@"
if [ -f "$ROOT/out/xiaomi-mi-10/system.apfs" ]; then
  listing=$(python3 "$ROOT/tools/apfs/apfs.py" ls "$ROOT/out/xiaomi-mi-10/system.apfs" /System/Library/Extensions)
  [[ "$listing" == *HakiosMetal.bundle* ]] || { echo 'System image lacks HakiosMetal.bundle; rebuild with SYSTEM=1' >&2; exit 1; }
  printf '%s\n' "$layout" > "$stamp"
fi
# Keep the validation image separate from the ordinary boot image.
if [ "${UMI_METAL_TEST:-0}" = 1 ]; then
  cp "$ROOT/out/xiaomi-mi-10/root.apfs" "$ROOT/out/xiaomi-mi-10/metal-test.apfs"
fi
if [ "${UMI_METAL_SHADERS:-0}" = 1 ]; then
  cp "$ROOT/out/xiaomi-mi-10/root.apfs" "$ROOT/out/xiaomi-mi-10/metal-shader-test.apfs"
fi
