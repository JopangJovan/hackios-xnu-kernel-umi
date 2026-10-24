#!/system/bin/sh
# Run beside umi-wifi-read as root. Keep the stock driver in charge of power.
set -eu
pm=/sys/bus/pci/devices/0000:01:00.0/power
original=$(cat "$pm/control")
case "$original" in on|auto) ;; *) echo "Unexpected PCI PM policy" >&2; exit 1;; esac
trap 'printf "%s\n" "$original" > "$pm/control"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
printf 'on\n' > "$pm/control"
if [ "$(cat "$pm/runtime_status")" != active ]; then
    echo 'PCI device did not resume; refusing register reads' >&2
    exit 1
fi
timeout 10 "${0%/*}/umi-wifi-read"
