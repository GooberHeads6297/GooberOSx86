; -----------------------------------------------------------------------------
; setjmp64.s - Freestanding 64-bit setjmp/longjmp for the boot fault guard.
;
; Long-mode counterpart to setjmp.s. The 32-bit version saves [ebx, esi, edi,
; ebp, esp, eip]; on x86_64 the System V ABI's callee-saved set is different,
; so we save [rbx, rbp, rsp, r12, r13, r14, r15, rip] = 8 qwords = 64 bytes.
;
; Like the 32-bit version these are namespaced gj_* so the compiler doesn't
; recognize them as libc setjmp/longjmp and apply special "may return twice"
; codegen. The boot guard (boot_safety.c on x86; the Phase-2-local stub in
; kernel_x64.c on x64) is written so only callee-saved registers and stack
; state survive the second return, which keeps this correct without compiler
; setjmp semantics.
;
; goober_jmp_buf layout on x86_64 (matches boot_safety.h):
;   buf[0] = rbx
;   buf[1] = rbp
;   buf[2] = rsp        (caller's, i.e. after the return address is popped)
;   buf[3] = r12
;   buf[4] = r13
;   buf[5] = r14
;   buf[6] = r15
;   buf[7] = rip        (return address into the caller of gj_setjmp)
; -----------------------------------------------------------------------------

BITS 64

global gj_setjmp
global gj_longjmp

section .text

; int gj_setjmp(goober_jmp_buf env)
;   System V AMD64: env in RDI, return value in RAX.
;   Saves callee-saved registers + the caller's stack pointer (the value RSP
;   will hold immediately after this function returns) + the return address.
;   Returns 0.
gj_setjmp:
    mov     [rdi + 0],  rbx
    mov     [rdi + 8],  rbp
    lea     rax, [rsp + 8]      ; caller's RSP (after the return address is popped)
    mov     [rdi + 16], rax
    mov     [rdi + 24], r12
    mov     [rdi + 32], r13
    mov     [rdi + 40], r14
    mov     [rdi + 48], r15
    mov     rax, [rsp]          ; return address == resume RIP
    mov     [rdi + 56], rax
    xor     eax, eax            ; first return: 0 (zero-extends to RAX)
    ret

; void gj_longjmp(goober_jmp_buf env, int val)
;   System V AMD64: env in RDI, val in ESI.
;   Restores callee-saved registers and the stack pointer, then jumps to the
;   saved RIP so the matching gj_setjmp appears to return `val` (or 1 if
;   val == 0; libc-style "longjmp(env, 0) is forbidden" surface).
gj_longjmp:
    mov     eax, esi            ; eax = requested return value (zero-extends)
    test    eax, eax
    jnz     .have_val
    mov     eax, 1              ; longjmp(env, 0) must surface as 1
.have_val:
    mov     rbx, [rdi + 0]
    mov     rbp, [rdi + 8]
    mov     rsp, [rdi + 16]     ; restore stack: discards the abandoned frame(s)
    mov     r12, [rdi + 24]
    mov     r13, [rdi + 32]
    mov     r14, [rdi + 40]
    mov     r15, [rdi + 48]
    mov     rcx, [rdi + 56]     ; resume RIP (env lives in .bss, safe to read)
    jmp     rcx
