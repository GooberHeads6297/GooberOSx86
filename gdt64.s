; -----------------------------------------------------------------------------
; gdt64.s - 64-bit GDT for GooberOSx86 long mode.
;
; Three descriptors:
;   0x00  null
;   0x08  ring-0 64-bit code (L=1, D=0, executable, readable)
;   0x10  ring-0 data (writable; base/limit ignored in long mode)
;
; A 10-byte pseudo-descriptor (gdt64_pointer) is exported so boot64.s can
; lgdt it before the far-jump to 64-bit code. We perform that lgdt while
; still in compatibility mode (CS.D=1, CS.L=0) so the CPU reads only the
; 16-bit limit + 32-bit base; that is fine because the GDT lives in low
; memory (well below 4 GiB), and once we enter 64-bit the upper 32 bits of
; the GDTR base remain zero - which is the truth.
;
; We never go back to 32-bit code after the trampoline, so a separate set of
; legacy 32-bit selectors is unnecessary; the trampoline keeps using the
; legacy selectors GRUB left in place until it executes the far-jump.
; -----------------------------------------------------------------------------

BITS 64

global gdt64_pointer
global gdt64_code_offset
global gdt64_data_offset

section .rodata
align 16
gdt64_start:
    ; 0x00 - null descriptor
    dq 0x0000000000000000

gdt64_code:
    ; 0x08 - 64-bit ring-0 code segment.
    ;   limit_low  = 0xFFFF (ignored in long mode)
    ;   base_low   = 0
    ;   base_mid   = 0
    ;   access     = 1001_1010b (P=1, DPL=00, S=1, type=Exec/Read non-conforming)
    ;   flags+lim  = 1010_1111b (G=1, D=0, L=1, AVL=0, limit_high=1111)
    ;   base_high  = 0
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10011010b
    db 10101111b
    db 0x00

gdt64_data:
    ; 0x10 - ring-0 data segment. In long mode base/limit are ignored for
    ; non-FS/GS data, but the descriptor must still be present and writable.
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b               ; P=1, DPL=00, S=1, type=Read/Write data
    db 11001111b               ; G=1, D=1, L=0, limit_high=1111 (all ignored)
    db 0x00

gdt64_end:

; 10-byte pseudo-descriptor: 16-bit limit + 64-bit base.
align 16
gdt64_pointer:
    dw gdt64_end - gdt64_start - 1
    dq gdt64_start

; Convenience selector offsets for any C/asm code that wants to refer by name.
gdt64_code_offset: equ gdt64_code - gdt64_start
gdt64_data_offset: equ gdt64_data - gdt64_start
