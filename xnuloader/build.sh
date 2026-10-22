#!/bin/bash
# Build a generic or explicitly selected UMI EFI loader.
set -euo pipefail
cd "$(dirname "$0")"
flags=()
out=.
case "${DEVICE:-}" in
  umi) flags+=(-DLOADER_UMI); out=../out/umi ;;
  ""|redwood|qemu-virt) ;;
  *) echo "unsupported loader device: $DEVICE" >&2; exit 1 ;;
esac
if [ -n "${PAYLOAD_BASE:-}" ]; then
  [[ "$PAYLOAD_BASE" =~ ^0x[0-9a-fA-F]+$ ]] || { echo 'invalid PAYLOAD_BASE' >&2; exit 1; }
  flags+=("-DPAYLOAD_BASE=${PAYLOAD_BASE}ULL")
fi
mkdir -p "$out"
clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter "${flags[@]}" -c loader.c -o "$out/loader.obj"
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 /out:"$out/loader.efi" "$out/loader.obj"
ls -l "$out/loader.efi"
