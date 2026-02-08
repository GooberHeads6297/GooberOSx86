#!/bin/bash
set -e

BUILD_DIR=build
ISO_DIR=iso

mkdir -p ${BUILD_DIR}

# Assemble NASM source files
nasm -f elf32 boot.s         -o ${BUILD_DIR}/boot.o
nasm -f elf32 gdt.s          -o ${BUILD_DIR}/gdt.o
nasm -f elf32 irq1_wrapper.s -o ${BUILD_DIR}/irq1_wrapper.o
nasm -f elf32 idt_load.s     -o ${BUILD_DIR}/idt_load.o
nasm -f elf32 isr32_stub.s   -o ${BUILD_DIR}/isr32_stub.o

# Compile C source files
i686-elf-gcc -ffreestanding -m32 -O0 \
  -I. \
  -Idrivers/keyboard \
  -Idrivers/timer \
  -Idrivers/video \
  -Idrivers/io \
  -Ifs \
  -Ishell \
  -Itaskmgr \
  -c kernel.c -o ${BUILD_DIR}/kernel.o

i686-elf-gcc -ffreestanding -m32 -O0 -I. -Idrivers/io -c lib/string.c                -o ${BUILD_DIR}/string.o
i686-elf-gcc -ffreestanding -m32 -O0 -I. -Idrivers/io -c lib/memory.c                -o ${BUILD_DIR}/memory.o
i686-elf-gcc -ffreestanding -m32 -O0 -I. -Idrivers/io -c drivers/keyboard/keyboard.c -o ${BUILD_DIR}/keyboard.o
i686-elf-gcc -ffreestanding -m32 -O0 -I. -Idrivers/io -c drivers/timer/timer.c       -o ${BUILD_DIR}/timer.o
i686-elf-gcc -ffreestanding -m32 -O0 -I. -Idrivers/io -c drivers/video/vga.c         -o ${BUILD_DIR}/vga.o
i686-elf-gcc -ffreestanding -m32 -O0 -I. -Idrivers/io -c fs/filesystem.c             -o ${BUILD_DIR}/filesystem.o
i686-elf-gcc -ffreestanding -m32 -O0 -I. -Idrivers/io -Itaskmgr -c shell/shell.c     -o ${BUILD_DIR}/shell.o
i686-elf-gcc -ffreestanding -m32 -O0 -I. -Idrivers/io -c games/snake.c               -o ${BUILD_DIR}/snake.o
i686-elf-gcc -ffreestanding -m32 -O0 -I. -Idrivers/io -c games/cubeDip.c             -o ${BUILD_DIR}/cubeDip.o
i686-elf-gcc -ffreestanding -m32 -O0 -I. -Idrivers/io -c games/pong.c                -o ${BUILD_DIR}/pong.o
i686-elf-gcc -ffreestanding -m32 -O0 -I. -Idrivers/io -c games/doom.c                -o ${BUILD_DIR}/doom.o
i686-elf-gcc -ffreestanding -m32 -O0 -I. -Idrivers/io -Itaskmgr -c taskmgr/taskmgr.c -o ${BUILD_DIR}/taskmgr.o
i686-elf-gcc -ffreestanding -m32 -O0 -I. -Idrivers/io -Itaskmgr -c taskmgr/process.c -o ${BUILD_DIR}/process.o
i686-elf-gcc -ffreestanding -m32 -O0 -I. -Idrivers/io -Idrivers/video -Idrivers/timer -Ifs -c editor/editor.c -o ${BUILD_DIR}/editor.o

# Link all objects into a kernel binary
i686-elf-ld -m elf_i386 -T linker.ld -o ${BUILD_DIR}/kernel.bin \
    ${BUILD_DIR}/boot.o \
    ${BUILD_DIR}/gdt.o \
    ${BUILD_DIR}/irq1_wrapper.o \
    ${BUILD_DIR}/idt_load.o \
    ${BUILD_DIR}/isr32_stub.o \
    ${BUILD_DIR}/keyboard.o \
    ${BUILD_DIR}/timer.o \
    ${BUILD_DIR}/vga.o \
    ${BUILD_DIR}/filesystem.o \
    ${BUILD_DIR}/shell.o \
    ${BUILD_DIR}/snake.o \
    ${BUILD_DIR}/cubeDip.o \
    ${BUILD_DIR}/pong.o \
    ${BUILD_DIR}/doom.o \
    ${BUILD_DIR}/editor.o \
    ${BUILD_DIR}/taskmgr.o \
    ${BUILD_DIR}/process.o \
    ${BUILD_DIR}/memory.o \
    ${BUILD_DIR}/string.o \
    ${BUILD_DIR}/kernel.o

echo "[+] Kernel built: ${BUILD_DIR}/kernel.bin"

# Prepare ISO directory structure
mkdir -p ${ISO_DIR}/boot/grub
cp ${BUILD_DIR}/kernel.bin ${ISO_DIR}/boot/
cp grub/grub.cfg ${ISO_DIR}/boot/grub/

# Create ISO image
grub-mkrescue -o GooberOSx86.iso ${ISO_DIR}/ --modules="biosdisk part_msdos" --directory=/usr/lib/grub/i386-pc/
echo "[+] ISO created: GooberOSx86.iso"
