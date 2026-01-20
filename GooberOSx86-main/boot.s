section .multiboot
align 4
    dd 0x1BADB002              ; Multiboot magic number
    dd 0x0                     ; No flags
    dd -(0x1BADB002 + 0x0)     ; Checksum

section .text
global start
extern gdt_load
extern kernel_main

start:
    ; Load our GDT (sets up flat code/data segments)
    call gdt_load

    ; Initialize stack
    mov esp, stack_top

    ; Enter kernel
    call kernel_main

.hang:
    hlt
    jmp .hang

section .bss
align 16
stack_bottom:
    resb 4096                  ; 4KB stack
stack_top:
