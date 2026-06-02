#!/bin/bash
# -----------------------------------------------------------------------------
# scripts/build-common.sh
#
# Shared helpers used by both scripts/build-x86.sh and scripts/build-x64.sh.
#
# Source this file; do not run it directly. The sourcing script is expected
# to define:
#   BUILD_DIR        e.g. "build"
#   ISO_DIR          e.g. "iso"
#   ISO_OUTPUT       e.g. "GooberOSx86.iso"
#   REPO_ROOT        absolute path to the repo root
# -----------------------------------------------------------------------------

# Format a byte count as a human-readable size (B/KiB/MiB/GiB/TiB).
format_bytes() {
  local bytes="$1"
  local unit=0
  local units=(B KiB MiB GiB TiB)

  while [ "${bytes}" -ge 1024 ] && [ "${unit}" -lt 4 ]; do
    bytes=$((bytes / 1024))
    unit=$((unit + 1))
  done

  printf "%s%s" "${bytes}" "${units[${unit}]}"
}

# Read a model/vendor string from a /sys/block/* device, falling back politely.
read_block_model() {
  local sysdev="$1"
  local candidate

  for candidate in \
    "${sysdev}/device/model" \
    "${sysdev}/device/name" \
    "${sysdev}/device/vendor"; do
    if [ -r "${candidate}" ]; then
      tr -s ' ' ' ' < "${candidate}" | tr -d '\n'
      return
    fi
  done

  printf "Unknown"
}

# Classify a block device as NVMe / eMMC/SD / USB / SSD / HDD.
classify_block_device() {
  local sysdev="$1"
  local name
  local path
  local rotational=0

  name="$(basename "${sysdev}")"
  path="$(readlink -f "${sysdev}/device" 2>/dev/null || true)"

  if [[ "${name}" == nvme* ]]; then
    printf "NVMe SSD"
  elif [[ "${name}" == mmcblk* ]]; then
    printf "eMMC/SD"
  elif [[ "${path}" == *"/usb"* ]]; then
    printf "USB storage"
  else
    if [ -r "${sysdev}/queue/rotational" ]; then
      rotational="$(cat "${sysdev}/queue/rotational")"
    fi
    if [ "${rotational}" = "1" ]; then
      printf "HDD"
    else
      printf "SSD"
    fi
  fi
}

# Walk /sys/block and print a human-friendly table of installable targets.
list_host_devices() {
  local sysdev
  local name
  local blocks
  local bytes
  local dtype
  local model
  local size_human

  printf "%-14s %-12s %-10s %s\n" "Device" "Type" "Size" "Model"
  for sysdev in /sys/block/*; do
    [ -e "${sysdev}" ] || continue
    name="$(basename "${sysdev}")"
    case "${name}" in
      loop*|ram*|fd*)
        continue
        ;;
    esac

    blocks="$(cat "${sysdev}/size")"
    bytes=$((blocks * 512))
    dtype="$(classify_block_device "${sysdev}")"
    model="$(read_block_model "${sysdev}")"
    size_human="$(format_bytes "${bytes}")"

    printf "%-14s %-12s %-10s %s\n" "/dev/${name}" "${dtype}" "${size_human}" "${model}"
  done
}

# Stage the install root (kernel + grub.cfg) under ${BUILD_DIR}/install-root.
prepare_install_root() {
  local install_root="${BUILD_DIR}/install-root"
  mkdir -p "${install_root}/boot/grub"
  cp "${BUILD_DIR}/kernel.bin" "${install_root}/boot/kernel.bin"
  cp grub/grub.cfg "${install_root}/boot/grub/grub.cfg"
  echo "[+] Install root staged at ${install_root}"
}

# Build a HYBRID BIOS + UEFI ISO via grub-mkrescue. Used by both the x86 and
# x64 builders so that a single ISO boots on legacy BIOS *and* UEFI firmware
# (the latter via grub-efi-amd64 + GOP framebuffer -- this is the Phase 0
# de-risk step of the UEFI/GOP/x64 migration plan).
#
# Notes:
#   - We deliberately do NOT pass --directory=/usr/lib/grub/i386-pc/. With both
#     grub-pc-bin and grub-efi-amd64-bin installed, grub-mkrescue auto-emits
#     an El Torito + GPT/UEFI hybrid image bootable from either firmware.
#   - --modules is applied to ALL targets, so it can only list modules that
#     exist on both i386-pc and x86_64-efi. `biosdisk` is BIOS-only and is
#     already embedded in GRUB's BIOS core image regardless, so it does not
#     belong here.
create_iso_hybrid() {
  mkdir -p "${ISO_DIR}/boot/grub"
  cp "${BUILD_DIR}/kernel.bin" "${ISO_DIR}/boot/"
  cp grub/grub.cfg "${ISO_DIR}/boot/grub/"
  prepare_install_root

  grub-mkrescue \
    -o "${ISO_OUTPUT}" \
    "${ISO_DIR}/" \
    --modules="part_msdos iso9660 multiboot multiboot2 all_video gfxterm font"

  echo "[+] Hybrid BIOS+UEFI ISO created: ${ISO_OUTPUT}"
}

# Install the freshly built kernel + grub config onto a mounted target device.
# Shared between x86 and x64 builds; arch-specific grub-install target is
# passed in as $1 (e.g. "i386-pc" or "x86_64-efi").
install_to_device_with_target() {
  local grub_target="$1"; shift
  local device=""
  local mountpoint=""

  while [ "$#" -gt 0 ]; do
    case "$1" in
      --device)
        device="${2:-}"
        shift 2
        ;;
      --mount)
        mountpoint="${2:-}"
        shift 2
        ;;
      *)
        echo "Unknown install option: $1"
        return 1
        ;;
    esac
  done

  if [ -z "${device}" ] || [ -z "${mountpoint}" ]; then
    echo "install requires --device and --mount"
    return 1
  fi

  if [ ! -b "${device}" ]; then
    echo "Target device does not exist: ${device}"
    return 1
  fi

  if [ ! -d "${mountpoint}" ]; then
    echo "Mount point does not exist: ${mountpoint}"
    return 1
  fi

  if ! command -v grub-install >/dev/null 2>&1; then
    echo "grub-install is not available on this host."
    echo "Use the in-kernel 'install write <target-id> YES' flow for VirtualBox HDD tests."
    return 1
  fi

  mkdir -p "${mountpoint}/boot/grub"
  cp "${BUILD_DIR}/kernel.bin" "${mountpoint}/boot/kernel.bin"
  cp grub/grub.cfg "${mountpoint}/boot/grub/grub.cfg"

  grub-install --target="${grub_target}" --boot-directory="${mountpoint}/boot" "${device}"

  echo "[+] Installed GooberOS boot files to ${mountpoint}/boot"
  echo "[+] GRUB (${grub_target}) installed to ${device}"
}
