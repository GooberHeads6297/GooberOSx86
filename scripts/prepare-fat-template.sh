#!/bin/bash
# Build host-formatted FAT32 partition template for in-OS install fat32.
# Called from prepare_install_payload() in build-common.sh.
# Optional 4th arg: BOOTX64.EFI; optional 5th: BOOTIA32.EFI (Bay Trail 32-bit UEFI).
set -euo pipefail

PAYLOAD_DIR="${1:?payload dir required}"
KERNEL="${2:?kernel.bin path required}"
GRUB_CFG="${3:?grub.cfg path required}"
BOOTX64_EFI="${4:-}"
BOOTIA32_EFI="${5:-}"
# 36 MiB + 512-byte clusters yields >=65525 clusters (valid FAT32).
# A 16 MiB "FAT32" with 4K clusters is rejected by OVMF/VirtualBox UEFI
# ("BdsDxe: ... Not Found") even when BOOTX64.EFI is present.
TEMPLATE_MIB="${INSTALL_FAT_TEMPLATE_MIB:-36}"
PART_IMG="${PAYLOAD_DIR}/fat-partition.img"

if ! command -v mkfs.vfat >/dev/null 2>&1; then
  echo "[!] mkfs.vfat not found; cannot build FAT template"
  exit 1
fi
if ! command -v mmd >/dev/null 2>&1 || ! command -v mcopy >/dev/null 2>&1; then
  echo "[!] mtools (mmd/mcopy) not found; cannot build FAT template"
  exit 1
fi

rm -f "${PART_IMG}"
dd if=/dev/zero of="${PART_IMG}" bs=1M count="${TEMPLATE_MIB}" status=none
# -s 1 (512-byte clusters) is required so a ~36 MiB volume meets the FAT32
# minimum cluster count that EDK2/OVMF will mount as an ESP.
mkfs.vfat -F 32 -n GOOBEROS -S 512 -s 1 -R 32 "${PART_IMG}"
if command -v fsck.vfat >/dev/null 2>&1; then
  if fsck.vfat -n "${PART_IMG}" 2>&1 | grep -q 'less than the required minimum'; then
    echo "[!] FAT template is not valid FAT32 (too few clusters); OVMF will not boot it"
    exit 1
  fi
fi

mmd -i "${PART_IMG}" ::boot ::boot/grub ::Desktop ::home ::tmp ::usr ::usr/bin ::usr/share ::Config ::Apps ::Apps/src ::lib ::lib/goober
mcopy -i "${PART_IMG}" "${KERNEL}" ::boot/kernel.bin
mcopy -i "${PART_IMG}" "${GRUB_CFG}" ::boot/grub/grub.cfg

# Boot cfg (HideGRUB) + GRUB-sourceable sibling for installed disks.
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ -f "${REPO_ROOT}/config/boot.cfg.template" ]; then
  mcopy -i "${PART_IMG}" "${REPO_ROOT}/config/boot.cfg.template" ::Config/boot.cfg
fi
if [ -f "${REPO_ROOT}/config/grub-boot.cfg.template" ]; then
  mcopy -i "${PART_IMG}" "${REPO_ROOT}/config/grub-boot.cfg.template" ::Config/grub-boot.cfg
fi

if { [ -n "${BOOTX64_EFI}" ] && [ -s "${BOOTX64_EFI}" ]; } || \
   { [ -n "${BOOTIA32_EFI}" ] && [ -s "${BOOTIA32_EFI}" ]; }; then
  mmd -i "${PART_IMG}" ::EFI ::EFI/BOOT ::EFI/GooberOS
fi
if [ -n "${BOOTX64_EFI}" ] && [ -s "${BOOTX64_EFI}" ]; then
  mcopy -i "${PART_IMG}" "${BOOTX64_EFI}" ::EFI/BOOT/BOOTX64.EFI
  mcopy -i "${PART_IMG}" "${BOOTX64_EFI}" ::EFI/GooberOS/BOOTX64.EFI
  echo "[+] Embedded UEFI GRUB -> EFI/BOOT/BOOTX64.EFI + EFI/GooberOS/BOOTX64.EFI"
fi
if [ -n "${BOOTIA32_EFI}" ] && [ -s "${BOOTIA32_EFI}" ]; then
  mcopy -i "${PART_IMG}" "${BOOTIA32_EFI}" ::EFI/BOOT/BOOTIA32.EFI
  mcopy -i "${PART_IMG}" "${BOOTIA32_EFI}" ::EFI/GooberOS/BOOTIA32.EFI
  echo "[+] Embedded IA32 UEFI GRUB -> EFI/BOOT/BOOTIA32.EFI + EFI/GooberOS/BOOTIA32.EFI"
fi

# Seed Desktop so an installed volume is never an empty `ls` surprise.
printf '%s\n' \
  "Welcome to GooberOS." \
  "" \
  "This is the Desktop folder on the installed FAT32 volume." \
  "Use 'cd /' then 'ls' to see boot/, EFI/, and other system folders." \
  > "${PAYLOAD_DIR}/Desktop-README.txt"
mcopy -i "${PART_IMG}" "${PAYLOAD_DIR}/Desktop-README.txt" ::Desktop/README.txt
rm -f "${PAYLOAD_DIR}/Desktop-README.txt"

# GooberC examples + prebuilt Welcome.gob (install-only payload).
if [ -f "${REPO_ROOT}/gooberc/examples/Welcome.gc" ]; then
  mcopy -i "${PART_IMG}" "${REPO_ROOT}/gooberc/examples/Welcome.gc" ::Apps/src/Welcome.gc
fi
if [ -f "${REPO_ROOT}/gooberc/README.md" ]; then
  mmd -i "${PART_IMG}" ::usr/share/gooberc 2>/dev/null || true
  mcopy -i "${PART_IMG}" "${REPO_ROOT}/gooberc/README.md" ::usr/share/gooberc/README.md
fi
if command -v python3 >/dev/null 2>&1 && [ -f "${REPO_ROOT}/gooberc/gooberc.py" ]; then
  python3 "${REPO_ROOT}/gooberc/gooberc.py" \
    "${REPO_ROOT}/gooberc/examples/Welcome.gc" \
    -o "${PAYLOAD_DIR}/Welcome.gob"
  mcopy -i "${PART_IMG}" "${PAYLOAD_DIR}/Welcome.gob" ::Apps/Welcome.gob
  rm -f "${PAYLOAD_DIR}/Welcome.gob"
  echo "[+] Seeded Apps/Welcome.gob (GooberC)"
fi
# Library notes for agents / users on installed disk
if [ -d "${REPO_ROOT}/gooberc/libs" ]; then
  for f in "${REPO_ROOT}/gooberc/libs/"*.md; do
    [ -f "$f" ] || continue
    mcopy -i "${PART_IMG}" "$f" "::lib/goober/$(basename "$f")"
  done
fi

echo "[+] FAT partition template (${TEMPLATE_MIB} MiB) -> ${PART_IMG}"
