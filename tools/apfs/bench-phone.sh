#!/bin/bash
# Readers at once over an APFS volume on the phone (boot-arg apfs_check=5), one line per run.
# The volume has to be the phone's resident image already (`./xl ramdisk IMAGE`); it stays
# there across runs as long as none of them hangs.
#
# usage: tools/apfs/bench-phone.sh "1 2 4 8" [extra boot-args]
# env:   BENCH_KERNEL  a kernel to put in place first (default: whatever is built)
#        BENCH_CPUS    cpus=N for the kernel (default 8)
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
COUNTS=${1:-"1 2 4 8"}
EXTRA=${2:-}
CPUS=${BENCH_CPUS:-8}
cd "$ROOT" || exit 1
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
[ -n "${BENCH_KERNEL:-}" ] && cp -p "$BENCH_KERNEL" "$K"

printf "%-7s %9s %8s %12s %12s %10s %4s %s\n" readers wall_ms MB/s lock_wait_ms lock_held_ms inflate_ms bad sums
for n in $COUNTS; do
	out=$(XL_CMDLINE="cpus=$CPUS apfs_check=5 apfs_threads=$n $EXTRA" XL_SECS=300 \
		timeout 500 ./xl run 2>&1)
	log=$(echo "$out" | grep -o 'logs/[0-9]*-run.log' | tail -1)
	if [ -z "$log" ] || ! grep -aq "readers done in" "$log" 2>/dev/null; then
		printf "%-7s run did not come back: %s\n" "$n" "$(echo "$out" | tail -1)"
		break
	fi
	wall=$(grep -ao "$n readers done in [0-9]*" "$log" | head -1 | awk '{print $5}')
	kbs=$(grep -ao "bytes  [0-9]* KB/s" "$log" | head -1 | awk '{print $2}')
	lock=$(grep -a "apfs: lock" "$log" | head -1 | sed 's/^\[[^]]*\]: //')
	infl=$(grep -a "apfs: inflate" "$log" | head -1 | sed 's/^\[[^]]*\]: //' | awk '{print $5}')
	bad=$(grep -ao "readers that disagreed or failed: [0-9]*" "$log" | head -1 | awk '{print $6}')
	sums=$(grep -ao "sum [0-9a-f]\{16\}" "$log" | sort -u | awk '{print $2}' | tr '\n' ' ')
	printf "%-7s %9s %8s %12s %12s %10s %4s %s  [%s]\n" "$n" "$wall" \
		"$(echo "scale=1; $kbs / 1024" | bc)" "$(echo "$lock" | awk '{print $5}')" \
		"$(echo "$lock" | awk '{print $8}')" "$infl" "$bad" "$sums" "$log"
done
