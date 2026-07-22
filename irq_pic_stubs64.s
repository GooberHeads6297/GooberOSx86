; -----------------------------------------------------------------------------
; irq_pic_stubs64.s - 64-bit catch-all stubs for remapped legacy PIC vectors.
;
; Real handlers exist for IRQ0 (0x20), IRQ1 (0x21), and IRQ12 (0x2C). The
; remaining PIC vectors must still have present IDT gates: real hardware can
; deliver masked/spurious IRQ7 (0x27) or IRQ15 (0x2F), and a not-present gate
; turns that external interrupt into #GP before the kernel can acknowledge it.
; -----------------------------------------------------------------------------

BITS 64

global irq2_spurious_asm
global irq3_spurious_asm
global irq4_spurious_asm
global irq5_spurious_asm
global irq6_spurious_asm
global irq7_spurious_asm
global irq8_spurious_asm
global irq9_spurious_asm
global irq10_spurious_asm
global irq11_spurious_asm
global irq13_spurious_asm
global irq14_spurious_asm
global irq15_spurious_asm

section .text

%macro MASTER_EOI_STUB 1
%1:
    push rax
    mov al, 0x20
    out 0x20, al
    pop rax
    iretq
%endmacro

%macro SLAVE_EOI_STUB 1
%1:
    push rax
    mov al, 0x20
    out 0xA0, al
    out 0x20, al
    pop rax
    iretq
%endmacro

MASTER_EOI_STUB irq2_spurious_asm
MASTER_EOI_STUB irq3_spurious_asm
MASTER_EOI_STUB irq4_spurious_asm
MASTER_EOI_STUB irq5_spurious_asm
MASTER_EOI_STUB irq6_spurious_asm

; IRQ7 is the master PIC's spurious-interrupt vector. If ISR bit 7 is clear,
; the PIC did not actually accept IRQ7, so no EOI is required.
irq7_spurious_asm:
    push rax
    mov al, 0x0B            ; OCW3: read ISR
    out 0x20, al
    in al, 0x20
    test al, 0x80
    jz .done
    mov al, 0x20
    out 0x20, al
.done:
    pop rax
    iretq

SLAVE_EOI_STUB irq8_spurious_asm
SLAVE_EOI_STUB irq9_spurious_asm
SLAVE_EOI_STUB irq10_spurious_asm
SLAVE_EOI_STUB irq11_spurious_asm
SLAVE_EOI_STUB irq13_spurious_asm
SLAVE_EOI_STUB irq14_spurious_asm

; IRQ15 is the slave PIC's spurious-interrupt vector. For a true spurious IRQ15
; the slave ISR bit is clear: do not EOI the slave, but still EOI the master
; cascade because IRQ2 was asserted.
irq15_spurious_asm:
    push rax
    mov al, 0x0B            ; OCW3: read slave ISR
    out 0xA0, al
    in al, 0xA0
    test al, 0x80
    jz .master_only
    mov al, 0x20
    out 0xA0, al
    out 0x20, al
    pop rax
    iretq
.master_only:
    mov al, 0x20
    out 0x20, al
    pop rax
    iretq
