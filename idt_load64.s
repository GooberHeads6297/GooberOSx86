; -----------------------------------------------------------------------------
; idt_load64.s - Load a 64-bit IDT pointer with `lidt`.
;
; The 32-bit version (idt_load.s) loads a 6-byte pseudo-descriptor (limit:16,
; base:32). In long mode `lidt` reads a 10-byte pseudo-descriptor (limit:16,
; base:64). The interface is otherwise identical: a single pointer argument.
;
; SysV AMD64: first argument in RDI, no caller cleanup.
; -----------------------------------------------------------------------------

BITS 64

global load_idt64

section .text

load_idt64:
    lidt [rdi]
    ret
