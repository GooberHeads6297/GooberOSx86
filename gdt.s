; gdt.s — define and load a 3-entry GDT: null, code, data
BITS 32
global gdt_load

section .gdt
gdt_start:
    dq 0x0000000000000000    ;  0: null descriptor

    ; 1: code segment descriptor — base=0, limit=4 GB, exec/read, DPL=0
    dw 0xFFFF                ; limit low
    dw 0x0000                ; base low
    db 0x00                  ; base middle
    db 10011010b             ; access byte: present, ring 0, code, exec+read
    db 11001111b             ; granularity: 4 KB gran, 32-bit, limit high=0xF
    db 0x00                  ; base high

    ; 2: data segment descriptor — base=0, limit=4 GB, read/write, DPL=0
    dw 0xFFFF                ; limit low
    dw 0x0000                ; base low
    db 0x00                  ; base middle
    db 10010010b             ; access: present, ring 0, data, read/write
    db 11001111b             ; granularity
    db 0x00                  ; base high

gdt_end:

; GDT pointer structure (6 bytes)
section .data
gdt_descriptor:
    dw gdt_end - gdt_start - 1   ; limit
    dd gdt_start                 ; base

; Function to load the GDT and reload CS
section .text
gdt_load:
    lgdt [gdt_descriptor]
    ; reload CS via far jump
    jmp 0x08:flush_cs
flush_cs:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret
