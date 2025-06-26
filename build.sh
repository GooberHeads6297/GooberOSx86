#!/bin/bash
set -e

BUILD_DIR=build
ISO_DIR=iso

mkdir -p ${BUILD_DIR}

# Assemble bootloader, GDT loader, and interrupt handlers
nasm -f elf32 boot.s         -o ${BUILD_DIR}/boot.o
nasm -f elf32 gdt.s          -o ${BUILD_DIR}/gdt.o
nasm -f elf32 irq1_wrapper.s -o ${BUILD_DIR}/irq1_wrapper.o
nasm -f elf32 idt_load.s     -o ${BUILD_DIR}/idt_load.o
nasm -f elf32 isr32_stub.s   -o ${BUILD_DIR}/isr32_stub.o

# Compile kernel and drivers
i686-elf-gcc -ffreestanding -m32 -O0 -I. -c kernel.c -o ${BUILD_DIR}/kernel.o
i686-elf-gcc -ffreestanding -m32 -O0 -I. -c drivers/keyboard/keyboard.c -o ${BUILD_DIR}/keyboard.o

# Link all objects into kernel binary
i686-elf-ld -m elf_i386 -T linker.ld -o ${BUILD_DIR}/kernel.bin \
    ${BUILD_DIR}/boot.o \
    ${BUILD_DIR}/gdt.o \
    ${BUILD_DIR}/irq1_wrapper.o \
    ${BUILD_DIR}/idt_load.o \
    ${BUILD_DIR}/isr32_stub.o \
    ${BUILD_DIR}/keyboard.o \
    ${BUILD_DIR}/kernel.o

echo "[+] Kernel built: ${BUILD_DIR}/kernel.bin"

# Prepare ISO
mkdir -p ${ISO_DIR}/boot/grub
cp ${BUILD_DIR}/kernel.bin ${ISO_DIR}/boot/
cp grub/grub.cfg ${ISO_DIR}/boot/grub/

# Create ISO
grub-mkrescue -o GooberOSx86.iso ${ISO_DIR}/ --modules="biosdisk part_msdos"
echo "[+] ISO created: GooberOSx86.iso"
