/* linker64.ld - ELF64 linker script for the GooberOSx86 x86_64 kernel.
 *
 * The x86 build keeps using linker.ld; this script is referenced ONLY by
 * scripts/build-x64.sh. The kernel still loads at 1 MiB (identity-mapped by
 * the boot64.s trampoline) so multiboot1+2 spec compliance is preserved.
 *
 * Section layout matches linker.ld so the build doesn't need to relocate
 * symbols; .multiboot stays at the very front of .text so GRUB's header
 * search (limited to the first 32 KiB on multiboot1, 32 KiB on multiboot2)
 * always finds both signatures. */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(start)

SECTIONS {
    . = 1M;

    _kernel_start = .;

    .text : ALIGN(4K) {
        KEEP(*(.multiboot))
        *(.text*)
    }

    .rodata : ALIGN(4K) { *(.rodata*) }
    .data   : ALIGN(4K) { *(.data*)   }

    /* IDT before .bss so the 8 MiB heap arena cannot overrun it.
     * (Heap-before-IDT caused Acer #GP when kmalloc smashed g_idt.) */
    .idt : ALIGN(4K) {
        *(.idt)
    }

    .bss    : ALIGN(4K) {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    . = ALIGN(4K);
    _kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note*)
        *(.comment*)
    }
}

