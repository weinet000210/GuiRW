#!/usr/bin/env bash
# Boot dist/GuiRw.efi directly in QEMU + OVMF (q35). Needs qemu-system-x86 and ovmf.
#   OVMF_CODE / OVMF_VARS can override the firmware paths.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OVMF_CODE="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
OVMF_VARS="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
TMP="$(mktemp -d)"
mkdir -p "$TMP/esp/EFI/BOOT"
cp "$REPO/dist/GuiRw.efi" "$TMP/esp/EFI/BOOT/BOOTX64.EFI"
cp "$OVMF_VARS" "$TMP/vars.fd"
exec qemu-system-x86_64 -machine q35 -m 512 -cpu max \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$TMP/vars.fd" \
  -drive file=fat:rw:"$TMP/esp",format=raw \
  -device e1000 -net none "$@"
