; userspace/enter_user64.s — enter ring-3 at (rip, rsp) with user CS/SS.
BITS 64

global enter_user64
; void enter_user64(uint64_t rip, uint64_t rsp);
; SysV: rdi=rip, rsi=rsp

USER_CS equ 0x1B   ; 0x18 | 3
USER_SS equ 0x23   ; 0x20 | 3

section .text
align 16
enter_user64:
    cli
    mov ax, USER_SS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push USER_SS          ; SS
    push rsi              ; RSP
    pushfq
    pop rax
    or rax, 0x200         ; IF
    push rax              ; RFLAGS
    push USER_CS          ; CS
    push rdi              ; RIP
    iretq
