; -----------------------------------------------------------------------------
; gdt64.s - 64-bit GDT for GooberOSx86 long mode (+ ring-3 + TSS).
;
; Selectors:
;   0x00  null
;   0x08  ring-0 64-bit code
;   0x10  ring-0 data
;   0x18  ring-3 64-bit code
;   0x20  ring-3 data
;   0x28  64-bit TSS (16-byte descriptor)
; -----------------------------------------------------------------------------

BITS 64

global gdt64_pointer
global gdt64_code_offset
global gdt64_data_offset
global gdt64_user_code_sel
global gdt64_user_data_sel
global gdt64_tss_sel
global tss64
global gdt64_reload
global gdt64_set_tss_rsp0
global gdt64_load_tr

section .bss
align 16
tss64:
    resb 104

section .data
align 16
gdt64_start:
    dq 0x0000000000000000

gdt64_code:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10011010b
    db 10101111b
    db 0x00

gdt64_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b
    db 11001111b
    db 0x00

gdt64_user_code:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 11111010b
    db 10101111b
    db 0x00

gdt64_user_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 11110010b
    db 11001111b
    db 0x00

; 16-byte TSS descriptor; base filled by gdt64_reload()
gdt64_tss:
    dw 103                     ; limit = sizeof(tss64)-1
    dw 0                       ; base 15:0
    db 0                       ; base 23:16
    db 10001001b               ; present, type 9
    db 0
    db 0                       ; base 31:24
    dd 0                       ; base 63:32
    dd 0

gdt64_end:

align 16
gdt64_pointer:
    dw gdt64_end - gdt64_start - 1
    dq gdt64_start

gdt64_code_offset: equ gdt64_code - gdt64_start
gdt64_data_offset: equ gdt64_data - gdt64_start
gdt64_user_code_sel: equ 0x18
gdt64_user_data_sel: equ 0x20
gdt64_tss_sel: equ 0x28

section .text

; void gdt64_reload(void) — patch TSS base, lgdt, reload data segs, ltr
gdt64_reload:
    ; Fill TSS descriptor base from &tss64
    lea rax, [rel tss64]
    mov word [rel gdt64_tss + 2], ax
    shr rax, 16
    mov byte [rel gdt64_tss + 4], al
    mov byte [rel gdt64_tss + 7], ah
    shr rax, 16
    mov dword [rel gdt64_tss + 8], eax

    lgdt [rel gdt64_pointer]

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Far return to reload CS = 0x08 (already in long mode)
    push 0x08
    lea rax, [rel .flush]
    push rax
    retfq
.flush:
    mov ax, 0x28
    ltr ax
    ret

; void gdt64_set_tss_rsp0(uint64_t rsp0)
gdt64_set_tss_rsp0:
    mov qword [rel tss64 + 4], rdi
    ret

gdt64_load_tr:
    mov ax, 0x28
    ltr ax
    ret
