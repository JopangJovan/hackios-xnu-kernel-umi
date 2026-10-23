#!/bin/bash
# Build the generic or UMI EFI loader with the upstream boot-bundle support.
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
# Preserve an existing owner key; generate an unpinned header when none exists.
[ -f owner_key.h ] || OWNER_KEY_H= python3 ../common/flash/ownerkey.py unpin >/dev/null
identity=
[ -z "${DARWIN_ID:-}" ] || identity=$(readlink -f "$DARWIN_ID")
python3 ../common/flash/darwinid.py header "$identity" darwin_identity.h
if [ -n "${OWNER_KEY_H:-}" ]; then
  flags+=("-DOWNER_KEY_FILE=\"$(readlink -f "$OWNER_KEY_H")\"")
fi
OUT=${OUT:-$out/loader.efi}
mkdir -p "$(dirname "$OUT")"
clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter "${flags[@]}" -c loader.c -o "${OUT%.efi}.obj"
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 /out:"$OUT" "${OUT%.efi}.obj"
ls -l "$OUT"
