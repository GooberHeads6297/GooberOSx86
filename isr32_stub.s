global isr32_stub

section .text
isr32_stub:
    cli
    pushad
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10          ; Kernel data segment selector (make sure matches your GDT)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; No special C handler call here, just print to screen (or nop for now)
    ; You could add a C handler call if desired, but here we just acknowledge the interrupt

    mov al, 0x20
    out 0x20, al          ; Send EOI to PIC

    pop gs
    pop fs
    pop es
    pop ds
    popad

    sti
    iret
