; -----------------------------------------------------------------------------
; irq1_wrapper64.s - 64-bit PS/2 keyboard ISR (IRQ1 -> remapped vector 0x21).
;
; Long-mode counterpart to irq1_wrapper.s. Differences vs. the 32-bit stub:
;   - Saves the 16 GPRs explicitly (no pushad).
;   - Drops the data segment writes; in long mode mov-to-segreg is a no-op
;     for the segment base on DS/ES/FS/GS and our GDT data descriptor is
;     already flat + writable.
;   - EOI to the master PIC is emitted from the C handler (matches the
;     existing 32-bit convention where irq1_handler_main is the place that
;     decides; the 32-bit asm does it because keyboard.c does not).
; -----------------------------------------------------------------------------

BITS 64

global irq1_handler_asm
extern irq1_handler_main

section .text

irq1_handler_asm:
    ; Save 16 GPRs.
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

    ; Align stack for SysV AMD64 (15 pushes => 8 bytes off 16).
    sub rsp, 8
    call irq1_handler_main
    add rsp, 8

    ; EOI to master PIC. (Kept here so the lifetime of the spurious-IRQ
    ; window stays as small as possible; matches the 32-bit convention.)
    mov al, 0x20
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
