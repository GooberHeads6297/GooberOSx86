#!/bin/bash
# -----------------------------------------------------------------------------
# scripts/build-x86.sh
#
# 32-bit x86 build (i686, freestanding, BIOS multiboot1+2 entry).
#
# Produces:
#   - build/kernel.bin      ELF32 multiboot kernel
#   - GooberOSx86.iso       Hybrid BIOS + UEFI ISO. Boots on legacy BIOS via
#                           grub-pc, and -- as the Phase 0 de-risk step of
#                           the UEFI/GOP/x64 migration plan -- on UEFI
#                           firmware via grub-efi-amd64 with the GOP
#                           framebuffer passed through multiboot2.
#
# Sub-commands (positional, defaults to "build"):
#   build           Build kernel + ISO (with optional embedded installer image).
#   list-devices    Print host block devices (for installation targets).
#   install         Install to a mounted target device.
#
# Environment:
#   EMBED_INSTALL_ISO=1   Embed an installer ISO inside the kernel (default).
# -----------------------------------------------------------------------------
set -euo pipefail

# Resolve repo root regardless of CWD.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

BUILD_DIR=build
ISO_DIR=iso
ISO_OUTPUT="GooberOSx86.iso"
EMBED_INSTALL_ISO="${EMBED_INSTALL_ISO:-1}"
CC="gcc"
LD="ld"
NASM="nasm"

# -m32 + ELF32 toolchain. Optimizations match the historical build.sh so a
# cut-over of this script is byte-equivalent for the existing kernel.
CFLAGS_BASE="-ffreestanding -m32 -Os -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables"
EXTRA_DEFS=""

# shellcheck disable=SC1091
source "${SCRIPT_DIR}/build-common.sh"

usage() {
  cat <<'EOF'
Usage:
  ./scripts/build-x86.sh                       Build x86 kernel + hybrid ISO
  ./scripts/build-x86.sh build                 Same as above
  ./scripts/build-x86.sh list-devices          List installable block devices
  ./scripts/build-x86.sh install --device /dev/sdX --mount /mnt/goober

Notes:
  * The produced ISO is a hybrid BIOS + UEFI image. On UEFI firmware the
    kernel still enters in 32-bit protected mode via multiboot2; the EFI
    handover step of the migration plan ports it to long mode.
EOF
}

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

build_image() {
  local embed_requested="${EMBED_INSTALL_ISO}"

  build_kernel 0 ""
  create_iso_hybrid

  if [ "${embed_requested}" = "1" ]; then
    rm -f "${BUILD_DIR}/osimage.o"
    ${LD} -m elf_i386 -r -b binary "${ISO_OUTPUT}" -o "${BUILD_DIR}/osimage.o"
    build_kernel 1 "${BUILD_DIR}/osimage.o"
    create_iso_hybrid
  fi
}

build_kernel() {
  local embed_flag="$1"
  local osimage_obj="${2:-}"

  set_embed_defs "${embed_flag}"
  mkdir -p "${BUILD_DIR}"

  ${NASM} -f elf32 boot.s              -o "${BUILD_DIR}/boot.o"
  ${NASM} -f elf32 gdt.s               -o "${BUILD_DIR}/gdt.o"
  ${NASM} -f elf32 irq1_wrapper.s      -o "${BUILD_DIR}/irq1_wrapper.o"
  ${NASM} -f elf32 irq12_wrapper.s     -o "${BUILD_DIR}/irq12_wrapper.o"
  ${NASM} -f elf32 idt_load.s          -o "${BUILD_DIR}/idt_load.o"
  ${NASM} -f elf32 isr32_stub.s        -o "${BUILD_DIR}/isr32_stub.o"
  ${NASM} -f elf32 cpu_exceptions.s    -o "${BUILD_DIR}/cpu_exceptions.o"
  ${NASM} -f elf32 setjmp.s            -o "${BUILD_DIR}/setjmp.o"
  ${NASM} -f elf32 drivers/storage/bios_int13.s -o "${BUILD_DIR}/bios_int13.o"

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

  compile_c -I. -Idrivers/io -Idrivers/pci -Ilib -c boot_safety.c -o "${BUILD_DIR}/boot_safety.o"
  compile_c -I. -Idrivers/io -c lib/string.c -o "${BUILD_DIR}/string.o"
  compile_c -I. -Idrivers/io -c lib/memory.c -o "${BUILD_DIR}/memory.o"
  compile_c -I. -Idrivers/io -c drivers/keyboard/keyboard.c -o "${BUILD_DIR}/keyboard.o"
  compile_c -I. -Idrivers/io -c drivers/mouse/mouse.c -o "${BUILD_DIR}/mouse.o"
  compile_c -I. -Idrivers/io -c drivers/timer/timer.c -o "${BUILD_DIR}/timer.o"
  compile_c -I. -Idrivers/io -c drivers/video/vga.c -o "${BUILD_DIR}/vga.o"
  compile_c -I. -Idrivers/io -c drivers/video/display.c -o "${BUILD_DIR}/display.o"
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/pci -Idrivers/timer -c drivers/video/native_fb.c -o "${BUILD_DIR}/native_fb.o"
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/pci -c drivers/video/intel_gfx.c -o "${BUILD_DIR}/intel_gfx.o"
  compile_c -I. -Idrivers/io -c drivers/input/input.c -o "${BUILD_DIR}/input.o"
  compile_c -I. -Idrivers/io -Ilib -c drivers/pci/pci.c -o "${BUILD_DIR}/pci.o"
  compile_c -I. -Idrivers/io -c drivers/storage/storage.c -o "${BUILD_DIR}/storage.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -c drivers/storage/sdhci.c -o "${BUILD_DIR}/sdhci.o"
  compile_c -I. -Idrivers/io -Idrivers/input -c drivers/usb/hid/hid.c -o "${BUILD_DIR}/usb_hid.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -c drivers/usb/host/uhci.c -o "${BUILD_DIR}/usb_uhci.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -c drivers/usb/host/ohci.c -o "${BUILD_DIR}/usb_ohci.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -c drivers/usb/host/ehci.c -o "${BUILD_DIR}/usb_ehci.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -c drivers/usb/host/xhci.c -o "${BUILD_DIR}/usb_xhci.o"
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
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/keyboard -Idrivers/mouse -Ilib -c drivers/video/vesa.c -o "${BUILD_DIR}/vesa.o"
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/keyboard -Idrivers/mouse -Ilib -c gui/vesa_window.c -o "${BUILD_DIR}/vesa_window.o"
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/keyboard -Idrivers/mouse -Ilib -c gui/desktop_vesa.c -o "${BUILD_DIR}/desktop_vesa.o"
  # Phase 4 (item 5): VGA-text application passthrough shim. Required link
  # symbol for editor + games on both arches (the indirection via
  # vga_text_putc lives in drivers/video/vga.c, but the shim handler is
  # here). x86 BIOS boots leave the shim disarmed by default, preserving
  # the legacy 0xB8000 path verbatim.
  compile_c -I. -Idrivers/video -Ilib -c gui/vga_passthrough.c -o "${BUILD_DIR}/vga_passthrough.o"
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/timer -Ifs -c editor/editor.c -o "${BUILD_DIR}/editor.o"

  ${LD} -m elf_i386 -T linker.ld --gc-sections -o "${BUILD_DIR}/kernel.bin" \
    "${BUILD_DIR}/boot.o" \
    "${BUILD_DIR}/gdt.o" \
    "${BUILD_DIR}/irq1_wrapper.o" \
    "${BUILD_DIR}/irq12_wrapper.o" \
    "${BUILD_DIR}/idt_load.o" \
    "${BUILD_DIR}/isr32_stub.o" \
    "${BUILD_DIR}/cpu_exceptions.o" \
    "${BUILD_DIR}/setjmp.o" \
    "${BUILD_DIR}/bios_int13.o" \
    "${BUILD_DIR}/keyboard.o" \
    "${BUILD_DIR}/mouse.o" \
    "${BUILD_DIR}/timer.o" \
    "${BUILD_DIR}/vga.o" \
    "${BUILD_DIR}/display.o" \
    "${BUILD_DIR}/native_fb.o" \
    "${BUILD_DIR}/intel_gfx.o" \
    "${BUILD_DIR}/input.o" \
    "${BUILD_DIR}/pci.o" \
    "${BUILD_DIR}/storage.o" \
    "${BUILD_DIR}/sdhci.o" \
    "${BUILD_DIR}/usb_hid.o" \
    "${BUILD_DIR}/usb_uhci.o" \
    "${BUILD_DIR}/usb_ohci.o" \
    "${BUILD_DIR}/usb_ehci.o" \
    "${BUILD_DIR}/usb_xhci.o" \
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
    "${BUILD_DIR}/vesa.o" \
    "${BUILD_DIR}/vesa_window.o" \
    "${BUILD_DIR}/desktop_vesa.o" \
    "${BUILD_DIR}/vga_passthrough.o" \
    ${osimage_obj} \
    "${BUILD_DIR}/memory.o" \
    "${BUILD_DIR}/string.o" \
    "${BUILD_DIR}/boot_safety.o" \
    "${BUILD_DIR}/kernel.o"

  echo "[+] Kernel built: ${BUILD_DIR}/kernel.bin"
}

# ---- Top-level dispatch -----------------------------------------------------

command="${1:-build}"

case "${command}" in
  build)
    if [ "$#" -gt 0 ]; then shift; fi
    if [ "$#" -ne 0 ]; then usage; exit 1; fi
    build_image
    ;;
  list-devices)
    list_host_devices
    ;;
  install)
    shift
    build_image
    install_to_device_with_target "i386-pc" "$@"
    ;;
  help|-h|--help)
    usage
    ;;
  *)
    usage
    exit 1
    ;;
esac
