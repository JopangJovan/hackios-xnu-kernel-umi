#!/bin/bash
# Run only after building UMI_METAL_TEST=1 build-userland.sh.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
export XL_RAMDISK="$root/out/xiaomi-mi-10/metal-test.apfs"
[ -f "$XL_RAMDISK" ] || { echo 'Build the test image with UMI_METAL_TEST=1 build-userland.sh first' >&2; exit 1; }
args=$(cat "$here/boot.args")
# Keep a firmware core available for live logs during this diagnostic run.
args=${args/bootcpu=0/bootcpu=7}
args=${args/cpus=8/cpus=7}
export XL_CMDLINE="$args"
export XL_SECS=${XL_SECS:-1800}
export XL_FOLLOW=${XL_FOLLOW:-5}
export XL_FOLLOW_KEEP=1
exec "$here/run.sh" run
