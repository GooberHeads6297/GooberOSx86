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
#   EMBED_INSTALL_PAYLOAD=1  Embed GRUB boot/core blobs for in-OS install (default).
#   EMBED_INSTALL_ISO=1      Legacy: also embed full ISO in kernel (not recommended).
# -----------------------------------------------------------------------------
set -euo pipefail

# Resolve repo root regardless of CWD.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

BUILD_DIR=build
ISO_DIR=iso
ISO_OUTPUT="GooberOSx86.iso"
GOOBEROS_GRUB_ARCH=x86
EMBED_INSTALL_ISO="${EMBED_INSTALL_ISO:-0}"
EMBED_INSTALL_PAYLOAD="${EMBED_INSTALL_PAYLOAD:-1}"
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
    EXTRA_DEFS="-DEMBED_INSTALL_PAYLOAD=1"
    if [ "${EMBED_INSTALL_ISO}" = "1" ]; then
      EXTRA_DEFS="${EXTRA_DEFS} -DEMBED_INSTALL_ISO=1"
    fi
  else
    EXTRA_DEFS=""
  fi
}

compile_c() {
  ${CC} ${CFLAGS_BASE} ${EXTRA_DEFS} "$@"
}

print_migration_banner() {
  cat <<'EOF'

============================================================================
GooberOSx86  i386 protected-mode build (x64 parity backport)
----------------------------------------------------------------------------
Phase B1   (free-list heap + 16 MiB BSS arena)                 -- COMPLETE.
Phase B2   (heap-backed VESA backbuffer; static 8 MiB BSS gone) -- COMPLETE.
Phase B3   (boot stages: userspace + desktop + WD_STORAGE_X64)  -- COMPLETE.
Phase B4   (ACPI reboot + fb console echo + touchpad guard)     -- COMPLETE.
Phase B5   (GooberDOS on x86 — 8 MiB guest arena via kmalloc)   -- COMPLETE.
Phase B6   (verify-x86-smoke.sh + USB gate on x86 ISO)          -- COMPLETE.

x86 keeps BIOS VBE + INT 13h real-mode paths; x64 keeps long-mode + PAT WC.
Both arches share shell, desktop, GooberC, games, storage, USB, and GooberDOS.
============================================================================

EOF
}

build_image() {
  local payload_requested="${EMBED_INSTALL_PAYLOAD}"

  print_migration_banner

  build_kernel 0 ""

  if [ "${payload_requested}" = "1" ]; then
    prepare_install_payload
    link_install_payload_objects
    build_kernel 1 ""
    refresh_fat_template_with_final_kernel
  fi

  if [ "${EMBED_INSTALL_ISO}" = "1" ]; then
    rm -f "${BUILD_DIR}/osimage.o"
    ${LD} -m elf_i386 -r -b binary "${ISO_OUTPUT}" -o "${BUILD_DIR}/osimage.o"
    build_kernel 1 "${BUILD_DIR}/osimage.o"
    refresh_fat_template_with_final_kernel
  fi

  stage_iso_install_files
  create_iso_hybrid
}

build_kernel() {
  local embed_flag="$1"
  local osimage_obj="${2:-}"
  local install_objs=""

  set_embed_defs "${embed_flag}"
  mkdir -p "${BUILD_DIR}"
  if [ "${embed_flag}" = "1" ]; then
    install_objs="${BUILD_DIR}/install_kernel_payload.o ${BUILD_DIR}/install_grub_cfg.o ${BUILD_DIR}/install_boot_img.o ${BUILD_DIR}/install_core_img.o"
  fi

  ${NASM} -f elf32 boot.s              -o "${BUILD_DIR}/boot.o"
  ${NASM} -f elf32 gdt.s               -o "${BUILD_DIR}/gdt.o"
  ${NASM} -f elf32 irq1_wrapper.s      -o "${BUILD_DIR}/irq1_wrapper.o"
  ${NASM} -f elf32 irq12_wrapper.s     -o "${BUILD_DIR}/irq12_wrapper.o"
  ${NASM} -f elf32 idt_load.s          -o "${BUILD_DIR}/idt_load.o"
  ${NASM} -f elf32 isr32_stub.s        -o "${BUILD_DIR}/isr32_stub.o"
  ${NASM} -f elf32 cpu_exceptions.s    -o "${BUILD_DIR}/cpu_exceptions.o"
  ${NASM} -f elf32 setjmp.s            -o "${BUILD_DIR}/setjmp.o"
  ${NASM} -f elf32 -I drivers/bios drivers/bios/rm_thunk.s -o "${BUILD_DIR}/rm_thunk.o"
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
  compile_c -I. -Idrivers/io -Idrivers/timer -c drivers/timer/softclock.c -o "${BUILD_DIR}/softclock.o"
  compile_c -I. -Idrivers/io -c drivers/video/vga.c -o "${BUILD_DIR}/vga.o"
  compile_c -I. -Ifs -c drivers/diagnostics/driver_log.c -o "${BUILD_DIR}/driver_log.o"
  compile_c -I. -Idrivers/diagnostics -c drivers/video/edid.c -o "${BUILD_DIR}/edid.o"
  compile_c -I. -Idrivers/diagnostics -Idrivers/video -Idrivers/io -c drivers/video/connector.c -o "${BUILD_DIR}/connector.o"
  compile_c -I. -Idrivers/diagnostics -c drivers/video/fb_cache.c -o "${BUILD_DIR}/fb_cache.o"
  compile_c -I. -Idrivers/diagnostics -c drivers/video/fb_pat.c -o "${BUILD_DIR}/fb_pat.o"
  compile_c -I. -Idrivers/io -c drivers/video/display.c -o "${BUILD_DIR}/display.o"
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/bios -c drivers/video/bios_vbe.c -o "${BUILD_DIR}/bios_vbe.o"
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/bios -Idrivers/pci -Idrivers/diagnostics -c drivers/video/basic_display.c -o "${BUILD_DIR}/basic_display.o"
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/pci -Idrivers/timer -c drivers/video/native_fb.c -o "${BUILD_DIR}/native_fb.o"
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/pci -Idrivers/diagnostics -c drivers/video/intel_gfx.c -o "${BUILD_DIR}/intel_gfx.o"
  # textcon: 80x25 text-console abstraction (VGA + framebuffer backends).
  # On x86 BIOS the shell uses the VGA backend, which is byte-equivalent to
  # the legacy direct-0xB8000 path.
  compile_c -I. -Idrivers/io -Idrivers/video -c drivers/video/textcon.c -o "${BUILD_DIR}/textcon.o"
  compile_c -I. -Idrivers/io -c drivers/input/input.c -o "${BUILD_DIR}/input.o"
  compile_c -I. -Idrivers/io -Idrivers/input -Idrivers/acpi -Idrivers/hid -Idrivers/i2c -Ilib -c drivers/input/touchpad.c -o "${BUILD_DIR}/touchpad.o"
  compile_c -I. -Idrivers/io -Ilib -c drivers/acpi/acpi.c -o "${BUILD_DIR}/acpi.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -Idrivers/timer -Ilib -c drivers/i2c/designware.c -o "${BUILD_DIR}/i2c_designware.o"
  compile_c -I. -Idrivers/io -Idrivers/i2c -Ilib -c drivers/hid/i2c_hid.c -o "${BUILD_DIR}/i2c_hid.o"
  compile_c -I. -Idrivers/io -Ilib -c drivers/pci/pci.c -o "${BUILD_DIR}/pci.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -Ilib -c drivers/pci/iosf_mbi.c -o "${BUILD_DIR}/iosf_mbi.o"
  compile_c -I. -Idrivers/io -c drivers/storage/storage.c -o "${BUILD_DIR}/storage.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -c drivers/storage/sdhci.c -o "${BUILD_DIR}/sdhci.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -c drivers/storage/ahci.c -o "${BUILD_DIR}/ahci.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -c drivers/storage/partition.c -o "${BUILD_DIR}/partition.o"
  compile_c -I. -Idrivers/io -Idrivers/input -c drivers/usb/hid/hid.c -o "${BUILD_DIR}/usb_hid.o"
  compile_c -I. -Idrivers/io -Idrivers/usb -Idrivers/input -c drivers/usb/hid/hid_boot.c -o "${BUILD_DIR}/usb_hid_boot.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -c drivers/usb/host/uhci.c -o "${BUILD_DIR}/usb_uhci.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -c drivers/usb/host/ohci.c -o "${BUILD_DIR}/usb_ohci.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -c drivers/usb/host/ehci.c -o "${BUILD_DIR}/usb_ehci.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -c drivers/usb/host/xhci_port.c -o "${BUILD_DIR}/usb_xhci_port.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -c drivers/usb/host/xhci_ctx.c -o "${BUILD_DIR}/usb_xhci_ctx.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -c drivers/usb/host/xhci.c -o "${BUILD_DIR}/usb_xhci.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -Idrivers/usb -c drivers/usb/host/xhci_hcd.c -o "${BUILD_DIR}/usb_xhci_hcd.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -c drivers/usb/host/baytrail_usb.c -o "${BUILD_DIR}/usb_baytrail.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -c drivers/usb/host/host.c -o "${BUILD_DIR}/usb_host.o"
  compile_c -I. -Idrivers/io -c drivers/usb/storage/msc.c -o "${BUILD_DIR}/usb_msc.o"
  compile_c -I. -Idrivers/io -Idrivers/usb -Idrivers/pci -c drivers/usb/core/usb_device.c -o "${BUILD_DIR}/usb_device.o"
  compile_c -I. -Idrivers/io -Idrivers/usb -Idrivers/pci -c drivers/usb/core/usb_hcd.c -o "${BUILD_DIR}/usb_hcd.o"
  compile_c -I. -Idrivers/io -Idrivers/usb -c drivers/usb/core/usb_class.c -o "${BUILD_DIR}/usb_class.o"
  compile_c -I. -Idrivers/io -Idrivers/usb -Idrivers/pci -Idrivers/input -c drivers/usb/core/usb_core.c -o "${BUILD_DIR}/usb_core.o"
  compile_c -I. -Idrivers/io -Idrivers/usb -c drivers/usb/core/enumeration.c -o "${BUILD_DIR}/usb_enum.o"
  compile_c -I. -Idrivers/io -Idrivers/usb -c drivers/usb/usb.c -o "${BUILD_DIR}/usb.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -Idrivers/storage -Ilib -Ifs -c fs/vfs.c -o "${BUILD_DIR}/vfs.o"
  compile_c -I. -Idrivers/io -Ilib -Ifs -c fs/memfs.c -o "${BUILD_DIR}/memfs.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -Idrivers/storage -Ilib -Ifs -c fs/fat32.c -o "${BUILD_DIR}/fat32.o"
  compile_c -I. -Idrivers/io -Ilib -Ifs -c fs/config_boot.c -o "${BUILD_DIR}/config_boot.o"
  python3 scripts/embed-gob-games.py
  compile_c -I. -Idrivers/io -Ilib -Ifs -Itaskmgr -Igui -Iuserspace \
    -c userspace/userspace.c -o "${BUILD_DIR}/userspace.o"
  compile_c -I. -Idrivers/io -Ilib -Ifs -Iuserspace \
    -c userspace/gooberc.c -o "${BUILD_DIR}/gooberc.o"
  compile_c -I. -Idrivers/io -Ilib -Ifs -Iuserspace \
    -c userspace/gob_games_embed.c -o "${BUILD_DIR}/gob_games_embed.o"
  compile_c -I. -Idrivers/io -Ilib -Ifs -Itaskmgr -Igui -Idosemu \
    -c dosemu/memory.c -o "${BUILD_DIR}/dosemu_memory.o"
  compile_c -I. -Idrivers/io -Ilib -Ifs -Itaskmgr -Igui -Idosemu \
    -c dosemu/cpu8086.c -o "${BUILD_DIR}/dosemu_cpu.o"
  compile_c -I. -Idrivers/io -Ilib -Ifs -Itaskmgr -Igui -Idosemu \
    -c dosemu/cpu386.c -o "${BUILD_DIR}/dosemu_cpu386.o"
  compile_c -I. -Idrivers/io -Ilib -Ifs -Itaskmgr -Igui -Idosemu \
    -c dosemu/pm_mode.c -o "${BUILD_DIR}/dosemu_pm.o"
  compile_c -I. -Idrivers/io -Ilib -Ifs -Itaskmgr -Igui -Idosemu \
    -c dosemu/dpmi.c -o "${BUILD_DIR}/dosemu_dpmi.o"
  compile_c -I. -Idrivers/io -Ilib -Ifs -Itaskmgr -Igui -Idosemu \
    -c dosemu/video_text.c -o "${BUILD_DIR}/dosemu_video.o"
  compile_c -I. -Idrivers/io -Ilib -Ifs -Itaskmgr -Igui -Idosemu \
    -c dosemu/bios.c -o "${BUILD_DIR}/dosemu_bios.o"
  compile_c -I. -Idrivers/io -Ilib -Ifs -Itaskmgr -Igui -Idosemu \
    -c dosemu/int21.c -o "${BUILD_DIR}/dosemu_int21.o"
  compile_c -I. -Idrivers/io -Ilib -Ifs -Itaskmgr -Igui -Idosemu \
    -c dosemu/dos_shell.c -o "${BUILD_DIR}/dosemu_shell.o"
  compile_c -I. -Idrivers/io -Ilib -Ifs -Itaskmgr -Igui -Idosemu \
    -c dosemu/dosemu.c -o "${BUILD_DIR}/dosemu.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -Idrivers/storage -Iinstall -Ilib -Ifs \
    -c install/install_payload.c -o "${BUILD_DIR}/install_payload.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -Idrivers/storage -Iinstall -Ilib \
    -c install/iso9660.c -o "${BUILD_DIR}/iso9660.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -Idrivers/storage -Iinstall -Ilib \
    -c install/grub_bios_embed.c -o "${BUILD_DIR}/install_grub_bios_embed.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -Idrivers/storage -Iinstall -Ilib \
    -c install/gpt_embed.c -o "${BUILD_DIR}/install_gpt_embed.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -Idrivers/storage -Iinstall -Ilib \
    -c install/fat32_mkfs.c -o "${BUILD_DIR}/install_fat32_mkfs.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -Idrivers/storage -Iinstall -Ilib \
    -c install/install_debug.c -o "${BUILD_DIR}/install_debug.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -Idrivers/storage -Iinstall -Ilib \
    -c install/install.c -o "${BUILD_DIR}/install.o"
  compile_c -I. -Idrivers/io -Itaskmgr -Ifs -Iinstall -Ilib -Iuserspace -c shell/shell.c -o "${BUILD_DIR}/shell.o"
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
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/keyboard -Idrivers/mouse -Ilib -Itaskmgr -Ifs -Iuserspace \
    -c gui/desktop_vesa.c -o "${BUILD_DIR}/desktop_vesa.o"
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
    "${BUILD_DIR}/rm_thunk.o" \
    "${BUILD_DIR}/bios_int13.o" \
    "${BUILD_DIR}/keyboard.o" \
    "${BUILD_DIR}/mouse.o" \
    "${BUILD_DIR}/timer.o" \
    "${BUILD_DIR}/softclock.o" \
    "${BUILD_DIR}/vga.o" \
    "${BUILD_DIR}/driver_log.o" \
    "${BUILD_DIR}/edid.o" \
    "${BUILD_DIR}/connector.o" \
    "${BUILD_DIR}/fb_cache.o" \
    "${BUILD_DIR}/fb_pat.o" \
    "${BUILD_DIR}/display.o" \
    "${BUILD_DIR}/bios_vbe.o" \
    "${BUILD_DIR}/basic_display.o" \
    "${BUILD_DIR}/native_fb.o" \
    "${BUILD_DIR}/intel_gfx.o" \
    "${BUILD_DIR}/textcon.o" \
    "${BUILD_DIR}/input.o" \
    "${BUILD_DIR}/touchpad.o" \
    "${BUILD_DIR}/acpi.o" \
    "${BUILD_DIR}/i2c_designware.o" \
    "${BUILD_DIR}/i2c_hid.o" \
    "${BUILD_DIR}/pci.o" \
    "${BUILD_DIR}/iosf_mbi.o" \
    "${BUILD_DIR}/storage.o" \
    "${BUILD_DIR}/sdhci.o" \
    "${BUILD_DIR}/ahci.o" \
    "${BUILD_DIR}/partition.o" \
    "${BUILD_DIR}/usb_hid.o" \
    "${BUILD_DIR}/usb_hid_boot.o" \
    "${BUILD_DIR}/usb_uhci.o" \
    "${BUILD_DIR}/usb_ohci.o" \
    "${BUILD_DIR}/usb_ehci.o" \
    "${BUILD_DIR}/usb_xhci_port.o" \
    "${BUILD_DIR}/usb_xhci_ctx.o" \
    "${BUILD_DIR}/usb_xhci.o" \
    "${BUILD_DIR}/usb_xhci_hcd.o" \
    "${BUILD_DIR}/usb_baytrail.o" \
    "${BUILD_DIR}/usb_host.o" \
    "${BUILD_DIR}/usb_msc.o" \
    "${BUILD_DIR}/usb_device.o" \
    "${BUILD_DIR}/usb_hcd.o" \
    "${BUILD_DIR}/usb_class.o" \
    "${BUILD_DIR}/usb_core.o" \
    "${BUILD_DIR}/usb_enum.o" \
    "${BUILD_DIR}/usb.o" \
    "${BUILD_DIR}/vfs.o" \
    "${BUILD_DIR}/memfs.o" \
    "${BUILD_DIR}/fat32.o" \
    "${BUILD_DIR}/config_boot.o" \
    "${BUILD_DIR}/userspace.o" \
    "${BUILD_DIR}/gooberc.o" \
    "${BUILD_DIR}/gob_games_embed.o" \
    "${BUILD_DIR}/dosemu_memory.o" \
    "${BUILD_DIR}/dosemu_cpu.o" \
    "${BUILD_DIR}/dosemu_cpu386.o" \
    "${BUILD_DIR}/dosemu_pm.o" \
    "${BUILD_DIR}/dosemu_dpmi.o" \
    "${BUILD_DIR}/dosemu_video.o" \
    "${BUILD_DIR}/dosemu_bios.o" \
    "${BUILD_DIR}/dosemu_int21.o" \
    "${BUILD_DIR}/dosemu_shell.o" \
    "${BUILD_DIR}/dosemu.o" \
    "${BUILD_DIR}/install_payload.o" \
    "${BUILD_DIR}/iso9660.o" \
    "${BUILD_DIR}/install_grub_bios_embed.o" \
    "${BUILD_DIR}/install_gpt_embed.o" \
    "${BUILD_DIR}/install_fat32_mkfs.o" \
    "${BUILD_DIR}/install_debug.o" \
    "${BUILD_DIR}/install.o" \
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
    ${install_objs} \
    "${BUILD_DIR}/memory.o" \
    "${BUILD_DIR}/string.o" \
    "${BUILD_DIR}/boot_safety.o" \
    "${BUILD_DIR}/kernel.o" \
    "$(gcc -m32 -print-libgcc-file-name)"

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
