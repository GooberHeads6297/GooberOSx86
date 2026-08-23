ENTRY(start)

SECTIONS {
    . = 1M;

    _kernel_start = .;

    .text : {
        KEEP(*(.multiboot))
        *(.text*)
    }

    .rodata : { *(.rodata*) }
    .data : { *(.data*) }

    /* IDT before .bss so the kernel heap arena cannot overrun it. */
    .idt : ALIGN(4K) {
        *(.idt)
    }

    .bss : ALIGN(4K) {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    . = ALIGN(4K);
    _kernel_end = .;
}
