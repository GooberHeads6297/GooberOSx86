; userspace/syscall64.s — int 0x80 entry (DPL=3).
BITS 64

global syscall80_stub
extern syscall_dispatch

section .text
align 16
syscall80_stub:
    ; Save GPRs. User args: rax=num, rdi=a0, rsi=a1, rdx=a2, r10=a3
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax

    mov rdi, rax              ; num
    ; a0..a3 still in rdi/rsi/rdx/r10 — but rdi overwritten.
    ; Recover from stack: after 15 pushes, original rdi is at rsp+10*8
    ; order: rax,rbx,rcx,rdx,rsi,rdi,rbp,r8,r9,r10,r11,r12,r13,r14,r15
    mov rsi, [rsp + 40]       ; saved rdi = a0
    mov rdx, [rsp + 32]       ; saved rsi = a1
    mov rcx, [rsp + 24]       ; saved rdx = a2
    mov r8,  [rsp + 72]       ; saved r10 = a3  (rax0 rbx8 rcx16 rdx24 rsi32 rdi40 rbp48 r8 56 r9 64 r10 72)

    call syscall_dispatch

    mov [rsp], rax            ; return value
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    iretq
