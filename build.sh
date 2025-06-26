#!/bin/bash
set -e

mkdir -p build

# Compile boot.s with Multiboot header
nasm -f elf32 boot.s -o build/boot.o

# Compile kernel.c
i686-elf-gcc -ffreestanding -m32 -c kernel.c -o build/kernel.o

# Link kernel
i686-elf-ld -m elf_i386 -T linker.ld -o build/kernel.bin build/boot.o build/kernel.o

echo "[+] Kernel built successfully: build/kernel.bin"

# Prepare ISO folder structure
mkdir -p iso/boot/grub
cp build/kernel.bin iso/boot/
cp grub/grub.cfg iso/boot/grub/

# Create ISO using grub-mkrescue
grub-mkrescue -o GooberOSx86.iso iso/

echo "[+] Bootable ISO created: GooberOSx86.iso"
