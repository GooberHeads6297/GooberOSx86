#!/bin/bash
set -euo pipefail

BUILD_DIR=build
ISO_DIR=iso
EMBED_INSTALL_ISO="${EMBED_INSTALL_ISO:-1}"
CC="i686-elf-gcc"
LD="i686-elf-ld"

CFLAGS_BASE="-ffreestanding -m32 -Os -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables"
EXTRA_DEFS=""

set_embed_defs() {
  if [ "${1}" = "1" ]; then
    EXTRA_DEFS="-DEMBED_INSTALL_ISO=1"
  else
    EXTRA_DEFS=""
  fi
}

compile_c() {
  ${CC} ${CFLAGS_BASE} ${EXTRA_DEFS} "$@"
}

usage() {
  cat <<'EOF'
Usage:
  ./build.sh
  ./build.sh build
  ./build.sh list-devices
  ./build.sh install --device /dev/sdX --mount /mnt/goober

Commands:
  build         Build kernel.bin, refresh the bootable ISO, and embed the direct installer image.
  list-devices  Inspect host block devices and classify them as HDD/SSD/eMMC/USB/NVMe.
  install       Copy kernel.bin + grub.cfg to the mounted target and run grub-install.
EOF
}

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

prepare_install_root() {
  local install_root="${BUILD_DIR}/install-root"
  mkdir -p "${install_root}/boot/grub"
  cp "${BUILD_DIR}/kernel.bin" "${install_root}/boot/kernel.bin"
  cp grub/grub.cfg "${install_root}/boot/grub/grub.cfg"
  echo "[+] Install root staged at ${install_root}"
}

build_image() {
  local embed_requested="${EMBED_INSTALL_ISO}"

  build_kernel 0 ""
  create_iso

  if [ "${embed_requested}" = "1" ]; then
    rm -f "${BUILD_DIR}/osimage.o"
    ${LD} -r -b binary GooberOSx86.iso -o "${BUILD_DIR}/osimage.o"
    build_kernel 1 "${BUILD_DIR}/osimage.o"
    create_iso
  fi
}

build_kernel() {
  local embed_flag="$1"
  local osimage_obj="${2:-}"

  set_embed_defs "${embed_flag}"
  mkdir -p "${BUILD_DIR}"

  nasm -f elf32 boot.s -o "${BUILD_DIR}/boot.o"
  nasm -f elf32 gdt.s -o "${BUILD_DIR}/gdt.o"
  nasm -f elf32 irq1_wrapper.s -o "${BUILD_DIR}/irq1_wrapper.o"
  nasm -f elf32 irq12_wrapper.s -o "${BUILD_DIR}/irq12_wrapper.o"
  nasm -f elf32 idt_load.s -o "${BUILD_DIR}/idt_load.o"
  nasm -f elf32 isr32_stub.s -o "${BUILD_DIR}/isr32_stub.o"
  nasm -f elf32 drivers/storage/bios_int13.s -o "${BUILD_DIR}/bios_int13.o"

  compile_c \
    -I. \
    -Idrivers/keyboard \
    -Idrivers/mouse \
    -Idrivers/timer \
    -Idrivers/video \
    -Idrivers/io \
    -Ifs \
    -Ishell \
    -Igui \
    -Itaskmgr \
    -c kernel.c -o "${BUILD_DIR}/kernel.o"

  compile_c -I. -Idrivers/io -c lib/string.c -o "${BUILD_DIR}/string.o"
  compile_c -I. -Idrivers/io -c lib/memory.c -o "${BUILD_DIR}/memory.o"
  compile_c -I. -Idrivers/io -c drivers/keyboard/keyboard.c -o "${BUILD_DIR}/keyboard.o"
  compile_c -I. -Idrivers/io -c drivers/mouse/mouse.c -o "${BUILD_DIR}/mouse.o"
  compile_c -I. -Idrivers/io -c drivers/timer/timer.c -o "${BUILD_DIR}/timer.o"
  compile_c -I. -Idrivers/io -c drivers/video/vga.c -o "${BUILD_DIR}/vga.o"
  compile_c -I. -Idrivers/io -c drivers/input/input.c -o "${BUILD_DIR}/input.o"
  compile_c -I. -Idrivers/io -Ilib -c drivers/pci/pci.c -o "${BUILD_DIR}/pci.o"
  compile_c -I. -Idrivers/io -c drivers/storage/storage.c -o "${BUILD_DIR}/storage.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -c drivers/storage/sdhci.c -o "${BUILD_DIR}/sdhci.o"
  compile_c -I. -Idrivers/io -Idrivers/input -c drivers/usb/hid/hid.c -o "${BUILD_DIR}/usb_hid.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -c drivers/usb/host/uhci.c -o "${BUILD_DIR}/usb_uhci.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -c drivers/usb/host/host.c -o "${BUILD_DIR}/usb_host.o"
  compile_c -I. -Idrivers/io -c drivers/usb/storage/msc.c -o "${BUILD_DIR}/usb_msc.o"
  compile_c -I. -Idrivers/io -Idrivers/usb -c drivers/usb/core/enumeration.c -o "${BUILD_DIR}/usb_enum.o"
  compile_c -I. -Idrivers/io -Idrivers/usb -c drivers/usb/usb.c -o "${BUILD_DIR}/usb.o"
  compile_c -I. -Idrivers/io -c fs/filesystem.c -o "${BUILD_DIR}/filesystem.o"
  compile_c -I. -Idrivers/io -Itaskmgr -c shell/shell.c -o "${BUILD_DIR}/shell.o"
  compile_c -I. -Idrivers/io -c drivers/storage/bios_disk.c -o "${BUILD_DIR}/bios_disk.o"
  compile_c -I. -Idrivers/io -c games/snake.c -o "${BUILD_DIR}/snake.o"
  compile_c -I. -Idrivers/io -c games/cubeDip.c -o "${BUILD_DIR}/cubeDip.o"
  compile_c -I. -Idrivers/io -c games/pong.c -o "${BUILD_DIR}/pong.o"
  compile_c -I. -Idrivers/io -c games/doom.c -o "${BUILD_DIR}/doom.o"
  compile_c -I. -Idrivers/io -Itaskmgr -c taskmgr/taskmgr.c -o "${BUILD_DIR}/taskmgr.o"
  compile_c -I. -Idrivers/io -Itaskmgr -c taskmgr/process.c -o "${BUILD_DIR}/process.o"
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/mouse -Idrivers/keyboard -Ilib -c gui/window.c -o "${BUILD_DIR}/window.o"
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/timer -Ifs -c editor/editor.c -o "${BUILD_DIR}/editor.o"

  ${LD} -m elf_i386 -T linker.ld --gc-sections -o "${BUILD_DIR}/kernel.bin" \
    "${BUILD_DIR}/boot.o" \
    "${BUILD_DIR}/gdt.o" \
    "${BUILD_DIR}/irq1_wrapper.o" \
    "${BUILD_DIR}/irq12_wrapper.o" \
    "${BUILD_DIR}/idt_load.o" \
    "${BUILD_DIR}/isr32_stub.o" \
    "${BUILD_DIR}/bios_int13.o" \
    "${BUILD_DIR}/keyboard.o" \
    "${BUILD_DIR}/mouse.o" \
    "${BUILD_DIR}/timer.o" \
    "${BUILD_DIR}/vga.o" \
    "${BUILD_DIR}/input.o" \
    "${BUILD_DIR}/pci.o" \
    "${BUILD_DIR}/storage.o" \
    "${BUILD_DIR}/sdhci.o" \
    "${BUILD_DIR}/usb_hid.o" \
    "${BUILD_DIR}/usb_uhci.o" \
    "${BUILD_DIR}/usb_host.o" \
    "${BUILD_DIR}/usb_msc.o" \
    "${BUILD_DIR}/usb_enum.o" \
    "${BUILD_DIR}/usb.o" \
    "${BUILD_DIR}/filesystem.o" \
    "${BUILD_DIR}/shell.o" \
    "${BUILD_DIR}/bios_disk.o" \
    "${BUILD_DIR}/snake.o" \
    "${BUILD_DIR}/cubeDip.o" \
    "${BUILD_DIR}/pong.o" \
    "${BUILD_DIR}/doom.o" \
    "${BUILD_DIR}/editor.o" \
    "${BUILD_DIR}/taskmgr.o" \
    "${BUILD_DIR}/process.o" \
    "${BUILD_DIR}/window.o" \
    ${osimage_obj} \
    "${BUILD_DIR}/memory.o" \
    "${BUILD_DIR}/string.o" \
    "${BUILD_DIR}/kernel.o"

  echo "[+] Kernel built: ${BUILD_DIR}/kernel.bin"
}

create_iso() {
  mkdir -p "${ISO_DIR}/boot/grub"
  cp "${BUILD_DIR}/kernel.bin" "${ISO_DIR}/boot/"
  cp grub/grub.cfg "${ISO_DIR}/boot/grub/"
  prepare_install_root

  grub-mkrescue -o GooberOSx86.iso "${ISO_DIR}/" --modules="biosdisk part_msdos" --directory=/usr/lib/grub/i386-pc/
  echo "[+] ISO created: GooberOSx86.iso"
}

install_to_device() {
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
        usage
        exit 1
        ;;
    esac
  done

  if [ -z "${device}" ] || [ -z "${mountpoint}" ]; then
    echo "install requires --device and --mount"
    usage
    exit 1
  fi

  if [ ! -b "${device}" ]; then
    echo "Target device does not exist: ${device}"
    exit 1
  fi

  if [ ! -d "${mountpoint}" ]; then
    echo "Mount point does not exist: ${mountpoint}"
    exit 1
  fi
  if ! command -v grub-install >/dev/null 2>&1; then
    echo "grub-install is not available on this host."
    echo "Use the in-kernel 'install write <target-id> YES' flow for VirtualBox HDD tests."
    exit 1
  fi

  build_image

  mkdir -p "${mountpoint}/boot/grub"
  cp "${BUILD_DIR}/kernel.bin" "${mountpoint}/boot/kernel.bin"
  cp grub/grub.cfg "${mountpoint}/boot/grub/grub.cfg"

  grub-install --target=i386-pc --boot-directory="${mountpoint}/boot" "${device}"

  echo "[+] Installed GooberOS boot files to ${mountpoint}/boot"
  echo "[+] GRUB installed to ${device}"
}

command="${1:-build}"

case "${command}" in
  build)
    if [ "$#" -gt 0 ]; then
      shift
    fi
    if [ "$#" -ne 0 ]; then
      usage
      exit 1
    fi
    build_image
    ;;
  list-devices)
    if [ "$#" -gt 0 ]; then
      shift
    fi
    list_host_devices
    ;;
  install)
    shift
    install_to_device "$@"
    ;;
  help|-h|--help)
    usage
    ;;
  *)
    usage
    exit 1
    ;;
esac
