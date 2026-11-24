#!/system/bin/sh
set -eu
unit=/sys/class/block/sde
part=/sys/class/block/sde61
case "$(readlink -f "$unit/device")" in */1d84000.ufshc/host0/target0:0:0/0:0:0:4) ;; *) exit 91;; esac
test "$(cat "$unit/size")" = 2097152
test "$(cat "$part/start")" = 1966080
test "$(cat "$part/size")" = 126976
test "$(cat "$part/partition")" = 61
test "$(readlink -f "$part")" = "$(readlink -f "$unit")/sde61"
test -b /dev/block/sde61
device_id=$(cat "$part/dev")
test "$(stat -L -c %t:%T /dev/block/sde61)" = "$(printf '%x:%x' "${device_id%:*}" "${device_id#*:}")"
test "$(blockdev --getsize64 /dev/block/sde61)" = 65011712
test "$(blockdev --getss /dev/block/sde61)" = 4096
