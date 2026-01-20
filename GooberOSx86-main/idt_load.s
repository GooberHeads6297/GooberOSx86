global load_idt

section .text
load_idt:
    mov eax, [esp + 4]    ; get pointer argument (IDT pointer)
    lidt [eax]            ; load IDT register with IDT pointer
    ret
