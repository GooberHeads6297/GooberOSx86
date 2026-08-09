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

# Never block on mtools "overwrite / rename?" prompts (looks like a hang in CI/IDEs).
# mmd/mcopy open /dev/tty for clash questions even when stderr is redirected.
export MTOOLS_SKIP_CHECK=1
mcopy_fat() {
  # -o: overwrite DOS files without confirmation.
  mcopy -o -i "${PART_IMG}" "$@" </dev/null >/dev/null
}
mmd_fat() {
  # -D s: skip if the name already exists (do not prompt on /dev/tty).
  mmd -D s -i "${PART_IMG}" "$@" </dev/null >/dev/null 2>&1 || true
}

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

# Create every directory once up front. Re-running mmd on an existing dir
# prompts on /dev/tty and hangs the build with no visible output.
mmd_fat ::boot ::boot/grub ::Desktop ::home ::tmp \
  ::usr ::usr/bin ::usr/share ::usr/share/gooberc ::usr/share/gooberdos \
  ::Config ::Apps ::Apps/src ::lib ::lib/goober ::Dos ::Dos/Apps \
  ::EFI ::EFI/BOOT ::EFI/GooberOS
echo "[*] Seeding boot/kernel.bin into FAT template…"
mcopy_fat "${KERNEL}" ::boot/kernel.bin
mcopy_fat "${GRUB_CFG}" ::boot/grub/grub.cfg

# Boot cfg (HideGRUB) + GRUB-sourceable sibling for installed disks.
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ -f "${REPO_ROOT}/dosemu/fixtures/HELLO.COM" ]; then
  mcopy_fat "${REPO_ROOT}/dosemu/fixtures/HELLO.COM" ::Dos/Apps/HELLO.COM
  echo "[+] Seeded Dos/Apps/HELLO.COM (GooberDOS)"
fi
if [ -f "${REPO_ROOT}/dosemu/fixtures/VER.COM" ]; then
  mcopy_fat "${REPO_ROOT}/dosemu/fixtures/VER.COM" ::Dos/Apps/VER.COM
  echo "[+] Seeded Dos/Apps/VER.COM (GooberDOS)"
fi

# DOOM shareware (optional — run scripts/fetch-doom-shareware.sh first)
DOOM_FIX="${REPO_ROOT}/dosemu/fixtures/doom"
if [ ! -f "${DOOM_FIX}/DOOM.EXE" ] || [ ! -f "${DOOM_FIX}/DOOM1.WAD" ]; then
  if [ "${GOOBER_FETCH_DOOM:-1}" != "0" ] && [ -x "${REPO_ROOT}/scripts/fetch-doom-shareware.sh" ]; then
    echo "[*] Doom shareware fixtures missing — fetching…"
    "${REPO_ROOT}/scripts/fetch-doom-shareware.sh" || echo "[!] Doom fetch skipped/failed (ISO builds without it)"
  fi
fi
if [ -f "${DOOM_FIX}/DOOM.EXE" ] && [ -f "${DOOM_FIX}/DOOM1.WAD" ]; then
  mcopy_fat "${DOOM_FIX}/DOOM.EXE" ::Dos/Apps/DOOM.EXE
  mcopy_fat "${DOOM_FIX}/DOOM1.WAD" ::Dos/Apps/DOOM1.WAD
  # Alias some loaders expect
  mcopy_fat "${DOOM_FIX}/DOOM1.WAD" ::Dos/Apps/DOOM.WAD
  # 8.3-friendly note (avoid LFN debris in DIR listings)
  if [ -f "${DOOM_FIX}/README.txt" ]; then
    mcopy_fat "${DOOM_FIX}/README.txt" ::Dos/Apps/DOOM.TXT
  fi
  echo "[+] Seeded Dos/Apps/DOOM.EXE + DOOM1.WAD (shareware)"
fi
if [ -f "${REPO_ROOT}/config/boot.cfg.template" ]; then
  mcopy_fat "${REPO_ROOT}/config/boot.cfg.template" ::Config/boot.cfg
fi
if [ -f "${REPO_ROOT}/config/grub-boot.cfg.template" ]; then
  mcopy_fat "${REPO_ROOT}/config/grub-boot.cfg.template" ::Config/grub-boot.cfg
fi

if [ -n "${BOOTX64_EFI}" ] && [ -s "${BOOTX64_EFI}" ]; then
  mcopy_fat "${BOOTX64_EFI}" ::EFI/BOOT/BOOTX64.EFI
  mcopy_fat "${BOOTX64_EFI}" ::EFI/GooberOS/BOOTX64.EFI
  echo "[+] Embedded UEFI GRUB -> EFI/BOOT/BOOTX64.EFI + EFI/GooberOS/BOOTX64.EFI"
fi
if [ -n "${BOOTIA32_EFI}" ] && [ -s "${BOOTIA32_EFI}" ]; then
  mcopy_fat "${BOOTIA32_EFI}" ::EFI/BOOT/BOOTIA32.EFI
  mcopy_fat "${BOOTIA32_EFI}" ::EFI/GooberOS/BOOTIA32.EFI
  echo "[+] Embedded IA32 UEFI GRUB -> EFI/BOOT/BOOTIA32.EFI + EFI/GooberOS/BOOTIA32.EFI"
fi

# Seed Desktop so an installed volume is never an empty `ls` surprise.
echo "[*] Seeding Desktop + GooberC sources…"
printf '%s\n' \
  "Welcome to GooberOS." \
  "" \
  "This is the Desktop folder on the installed FAT32 volume." \
  "Use 'cd /' then 'ls' to see boot/, EFI/, and other system folders." \
  > "${PAYLOAD_DIR}/Desktop-README.txt"
mcopy_fat "${PAYLOAD_DIR}/Desktop-README.txt" ::Desktop/README.txt
rm -f "${PAYLOAD_DIR}/Desktop-README.txt"

# GooberC examples + prebuilt Welcome.gob (install-only payload).
if [ -d "${REPO_ROOT}/gooberc/examples" ]; then
  for f in "${REPO_ROOT}/gooberc/examples/"*.gc; do
    [ -f "$f" ] || continue
    mcopy_fat "$f" "::Apps/src/$(basename "$f")"
  done
  echo "[+] Seeded Apps/src/*.gc"
fi
if [ -f "${REPO_ROOT}/gooberc/README.md" ]; then
  mcopy_fat "${REPO_ROOT}/gooberc/README.md" ::usr/share/gooberc/README.md
  if [ -f "${REPO_ROOT}/gooberc/SPEC.md" ]; then
    mcopy_fat "${REPO_ROOT}/gooberc/SPEC.md" ::usr/share/gooberc/SPEC.md
  fi
fi
if command -v python3 >/dev/null 2>&1 && [ -f "${REPO_ROOT}/gooberc/gooberc.py" ]; then
  echo "[*] Compiling GooberC apps with gooberc.py…"
  python3 -u "${REPO_ROOT}/gooberc/gooberc.py" \
    "${REPO_ROOT}/gooberc/examples/Welcome.gc" \
    -o "${PAYLOAD_DIR}/Welcome.gob"
  mcopy_fat "${PAYLOAD_DIR}/Welcome.gob" ::Apps/Welcome.gob
  rm -f "${PAYLOAD_DIR}/Welcome.gob"
  echo "[+] Seeded Apps/Welcome.gob (GooberC v2)"
  for game in Minesweeper CubeDip SnakeGame; do
    if [ -f "${REPO_ROOT}/gooberc/examples/${game}.gc" ]; then
      python3 -u "${REPO_ROOT}/gooberc/gooberc.py" \
        "${REPO_ROOT}/gooberc/examples/${game}.gc" \
        -o "${PAYLOAD_DIR}/${game}.gob"
      mcopy_fat "${PAYLOAD_DIR}/${game}.gob" "::Apps/${game}.gob"
      rm -f "${PAYLOAD_DIR}/${game}.gob"
      echo "[+] Seeded Apps/${game}.gob (GooberC game)"
    fi
  done
else
  echo "[!] python3/gooberc.py missing — Apps/*.gob not seeded into FAT template"
fi
# Library notes for agents / users on installed disk
if [ -d "${REPO_ROOT}/gooberc/libs" ]; then
  for f in "${REPO_ROOT}/gooberc/libs/"*.md; do
    [ -f "$f" ] || continue
    mcopy_fat "$f" "::lib/goober/$(basename "$f")"
  done
fi
if [ -f "${REPO_ROOT}/dosemu/README.md" ]; then
  mcopy_fat "${REPO_ROOT}/dosemu/README.md" ::usr/share/gooberdos/README.md
fi

echo "[+] FAT partition template (${TEMPLATE_MIB} MiB) -> ${PART_IMG}"
