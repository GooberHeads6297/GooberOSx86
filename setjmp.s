; Freestanding 32-bit setjmp/longjmp for the boot fault guard.
;
; These are intentionally namespaced (gj_*) so the compiler does NOT recognize
; them as the libc setjmp/longjmp and apply special "may return twice" codegen.
; The boot guard (boot_safety.c) is written so that only callee-saved registers
; (which longjmp restores) and stack-resident state survive the second return,
; which keeps this correct without compiler setjmp semantics.
;
; goober_jmp_buf is uint32_t[6]: [ebx, esi, edi, ebp, esp, eip].

global gj_setjmp
global gj_longjmp

section .text

; int gj_setjmp(goober_jmp_buf env)
;   Saves the callee-saved registers, the caller's stack pointer (as it will be
;   immediately after this function returns), and the return address. Returns 0.
gj_setjmp:
    mov     eax, [esp + 4]      ; eax = env
    mov     [eax + 0],  ebx
    mov     [eax + 4],  esi
    mov     [eax + 8],  edi
    mov     [eax + 12], ebp
    lea     ecx, [esp + 4]      ; caller's esp after the return address is popped
    mov     [eax + 16], ecx
    mov     ecx, [esp]          ; return address == resume EIP
    mov     [eax + 20], ecx
    xor     eax, eax            ; first return: 0
    ret

; void gj_longjmp(goober_jmp_buf env, int val)
;   Restores callee-saved registers and the stack pointer, then resumes
;   execution at the saved return address so the matching gj_setjmp appears to
;   return `val` (or 1 if val == 0).
gj_longjmp:
    mov     edx, [esp + 4]      ; edx = env
    mov     eax, [esp + 8]      ; eax = requested return value
    test    eax, eax
    jnz     .have_val
    mov     eax, 1              ; longjmp(env, 0) must surface as 1
.have_val:
    mov     ebx, [edx + 0]
    mov     esi, [edx + 4]
    mov     edi, [edx + 8]
    mov     ebp, [edx + 12]
    mov     esp, [edx + 16]     ; restore stack: discards the abandoned frame(s)
    mov     ecx, [edx + 20]     ; resume EIP (env lives in .bss, safe to read now)
    jmp     ecx
