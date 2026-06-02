; -----------------------------------------------------------------------------
; irq12_wrapper64.s - 64-bit PS/2 mouse ISR (IRQ12 -> remapped vector 0x2C).
;
; Long-mode counterpart to irq12_wrapper.s. Same shape as irq1_wrapper64.s;
; the only meaningful difference is the EOI sequence: IRQ12 lives on the
; slave PIC, so we acknowledge BOTH the slave (0xA0) and the master (0x20)
; before iretq. The 32-bit version does the same.
; -----------------------------------------------------------------------------

BITS 64

global irq12_handler_asm
extern mouse_handler_main

section .text

irq12_handler_asm:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    sub rsp, 8
    call mouse_handler_main
    add rsp, 8

    ; IRQ12 EOI: slave first, then master.
    mov al, 0x20
    out 0xA0, al
    out 0x20, al

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    iretq
