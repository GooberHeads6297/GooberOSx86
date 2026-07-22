#!/bin/bash
# Register a UEFI NVRAM boot entry labeled "GooberOS" that points at the
# installed ESP loader (\EFI\GooberOS\BOOT*.EFI).
#
# Run from a Linux live USB (or the host) after install fat32, with the
# GooberOS ESP mounted or by passing the block device:
#
#   sudo ./scripts/register-uefi-boot.sh /dev/mmcblk0
#   sudo ./scripts/register-uefi-boot.sh /dev/sda 1
#
# Requires: efibootmgr, and a UEFI boot (sysfs efivars available).
set -euo pipefail

DISK="${1:-}"
PART="${2:-1}"

if [ -z "${DISK}" ]; then
  echo "Usage: $0 <disk> [partition_number]"
  echo "  Example: $0 /dev/mmcblk0"
  echo "  Example: $0 /dev/sda 1"
  exit 1
fi

if [ ! -e /sys/firmware/efi ]; then
  echo "[!] Not booted in UEFI mode (no /sys/firmware/efi); cannot write NVRAM."
  exit 1
fi

if ! command -v efibootmgr >/dev/null 2>&1; then
  echo "[!] efibootmgr not found; install efibootmgr and retry."
  exit 1
fi

# Prefer IA32 on Bay Trail tablets/laptops; fall back to x64.
LOADER='\EFI\GooberOS\BOOTX64.EFI'
if [ -d /sys/firmware/efi/fw_platform_size ]; then
  :
fi
# Probe ESP for which loader exists when a partition node is guessable.
PART_NODE=""
if [[ "${DISK}" == *mmcblk* ]] || [[ "${DISK}" == *nvme* ]]; then
  PART_NODE="${DISK}p${PART}"
else
  PART_NODE="${DISK}${PART}"
fi

TMP_MNT=""
cleanup() {
  if [ -n "${TMP_MNT}" ] && mountpoint -q "${TMP_MNT}" 2>/dev/null; then
    umount "${TMP_MNT}" || true
  fi
  if [ -n "${TMP_MNT}" ]; then
    rmdir "${TMP_MNT}" 2>/dev/null || true
  fi
}
trap cleanup EXIT

if [ -b "${PART_NODE}" ]; then
  TMP_MNT="$(mktemp -d /tmp/gooberos-esp.XXXXXX)"
  mount -o ro "${PART_NODE}" "${TMP_MNT}"
  if [ -f "${TMP_MNT}/EFI/GooberOS/BOOTIA32.EFI" ]; then
    LOADER='\EFI\GooberOS\BOOTIA32.EFI'
  elif [ -f "${TMP_MNT}/EFI/GooberOS/BOOTX64.EFI" ]; then
    LOADER='\EFI\GooberOS\BOOTX64.EFI'
  elif [ -f "${TMP_MNT}/EFI/BOOT/BOOTIA32.EFI" ]; then
    LOADER='\EFI\BOOT\BOOTIA32.EFI'
  elif [ -f "${TMP_MNT}/EFI/BOOT/BOOTX64.EFI" ]; then
    LOADER='\EFI\BOOT\BOOTX64.EFI'
  else
    echo "[!] No GooberOS EFI loader found on ${PART_NODE}"
    exit 1
  fi
  umount "${TMP_MNT}"
  rmdir "${TMP_MNT}"
  TMP_MNT=""
  trap - EXIT
fi

# Remove any prior GooberOS-labeled entries to avoid duplicates.
while read -r bootnum; do
  [ -n "${bootnum}" ] || continue
  efibootmgr -b "${bootnum}" -B >/dev/null 2>&1 || true
done < <(efibootmgr 2>/dev/null | sed -n 's/^Boot\([0-9A-Fa-f][0-9A-Fa-f]*\).*GooberOS.*/\1/p')

efibootmgr --create \
  --disk "${DISK}" \
  --part "${PART}" \
  --label "GooberOS" \
  --loader "${LOADER}"

echo "[+] UEFI boot entry created: GooberOS -> ${LOADER}"
echo "    Reboot and pick GooberOS from the firmware boot menu (F12 / Boot Menu)."
efibootmgr | sed -n '1,20p'
