#!/bin/bash
set -euo pipefail

BUILD_DIR=build
ISO_DIR=iso
EMBED_INSTALL_ISO="${EMBED_INSTALL_ISO:-0}"
CC="i686-elf-gcc"
LD="i686-elf-ld"

CFLAGS_BASE="-ffreestanding -m32 -Os -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables"
EXTRA_DEFS=""

if [ "${EMBED_INSTALL_ISO}" = "1" ]; then
  EXTRA_DEFS="-DEMBED_INSTALL_ISO=1"
fi

compile_c() {
  ${CC} ${CFLAGS_BASE} ${EXTRA_DEFS} "$@"
}

mkdir -p "${BUILD_DIR}"

# Assemble NASM source files
nasm -f elf32 boot.s -o "${BUILD_DIR}/boot.o"
nasm -f elf32 gdt.s -o "${BUILD_DIR}/gdt.o"
nasm -f elf32 irq1_wrapper.s -o "${BUILD_DIR}/irq1_wrapper.o"
nasm -f elf32 irq12_wrapper.s -o "${BUILD_DIR}/irq12_wrapper.o"
nasm -f elf32 idt_load.s -o "${BUILD_DIR}/idt_load.o"
nasm -f elf32 isr32_stub.s -o "${BUILD_DIR}/isr32_stub.o"
nasm -f elf32 drivers/storage/bios_int13.s -o "${BUILD_DIR}/bios_int13.o"

# Compile C source files
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
compile_c -I. -Idrivers/io -Idrivers/input -c drivers/usb/hid/hid.c -o "${BUILD_DIR}/usb_hid.o"
compile_c -I. -Idrivers/io -Idrivers/pci -c drivers/usb/host/uhci.c -o "${BUILD_DIR}/usb_uhci.o"
compile_c -I. -Idrivers/io -Idrivers/pci -c drivers/usb/host/host.c -o "${BUILD_DIR}/usb_host.o"
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

rm -f "${BUILD_DIR}/osimage.o"
if [ "${EMBED_INSTALL_ISO}" = "1" ]; then
  if [ ! -f GooberOSx86.iso ]; then
    echo "[-] GooberOSx86.iso not found for embedding."
    exit 1
  fi
  ${LD} -r -b binary GooberOSx86.iso -o "${BUILD_DIR}/osimage.o"
fi

OSIMAGE_OBJ=""
if [ -f "${BUILD_DIR}/osimage.o" ]; then
  OSIMAGE_OBJ="${BUILD_DIR}/osimage.o"
fi

# Link all objects into a kernel binary
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
  "${BUILD_DIR}/usb_hid.o" \
  "${BUILD_DIR}/usb_uhci.o" \
  "${BUILD_DIR}/usb_host.o" \
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
  ${OSIMAGE_OBJ} \
  "${BUILD_DIR}/memory.o" \
  "${BUILD_DIR}/string.o" \
  "${BUILD_DIR}/kernel.o"

echo "[+] Kernel built: ${BUILD_DIR}/kernel.bin"

# Prepare ISO directory structure
mkdir -p "${ISO_DIR}/boot/grub"
cp "${BUILD_DIR}/kernel.bin" "${ISO_DIR}/boot/"
cp grub/grub.cfg "${ISO_DIR}/boot/grub/"

# Create ISO image
grub-mkrescue -o GooberOSx86.iso "${ISO_DIR}/" --modules="biosdisk part_msdos" --directory=/usr/lib/grub/i386-pc/
echo "[+] ISO created: GooberOSx86.iso"
