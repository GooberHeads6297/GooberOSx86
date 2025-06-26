global irq1_handler_asm
extern irq1_handler_main

section .text
irq1_handler_asm:
    cli
    pushad
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10         ; Kernel data segment selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    call irq1_handler_main

    mov al, 0x20
    out 0x20, al         ; EOI to Master PIC

    pop gs
    pop fs
    pop es
    pop ds
    popad

    ; no sti here—IF restored by iret
    iret
