; -----------------------------------------------------------------------------
; cpu_exceptions64.s - 64-bit CPU exception stubs for vectors 0..31.
;
; Long-mode counterpart to cpu_exceptions.s. Assembled only by the x64 build
; (`nasm -f elf64`); the x86 build keeps using the original 32-bit stubs.
;
; Differences vs. the 32-bit version:
;   - The 16 GPRs (rax..r15) are pushed/popped explicitly (no pushad in long
;     mode). All pushes are 8 bytes wide.
;   - Data segment writes are dropped (DS/ES/FS/GS are largely vestigial in
;     long mode and mov-to-segreg from a 64-bit register is a no-op for the
;     base; the GDT entries for ds/es are flat and writable already).
;   - For no-error-code vectors we still push a fake QWORD 0 so the resulting
;     stack frame layout is uniform across vectors with and without error
;     codes.
;   - We pass arguments to the C handler via the System V AMD64 ABI:
;       RDI = vector
;       RSI = error_code
;       RDX = rip
;       RCX = cs
;       R8  = rflags
;
; The C side prototype (kernel_x64.c) is:
;   void cpu_exception_handler(uint64_t vector, uint64_t error_code,
;                              uint64_t rip, uint64_t cs, uint64_t rflags);
; -----------------------------------------------------------------------------

BITS 64

global isr0_stub
global isr1_stub
global isr2_stub
global isr3_stub
global isr4_stub
global isr5_stub
global isr6_stub
global isr7_stub
global isr8_stub
global isr9_stub
global isr10_stub
global isr11_stub
global isr12_stub
global isr13_stub
global isr14_stub
global isr15_stub
global isr16_stub
global isr17_stub
global isr18_stub
global isr19_stub
global isr20_stub
global isr21_stub
global isr22_stub
global isr23_stub
global isr24_stub
global isr25_stub
global isr26_stub
global isr27_stub
global isr28_stub
global isr29_stub
global isr30_stub
global isr31_stub

extern cpu_exception_handler

section .text

; Save 16 GPRs in canonical order (rax first, r15 last). Restore in reverse.
%macro PUSH_GPRS 0
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
%endmacro

%macro POP_GPRS 0
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
%endmacro

; ISR_NOERR <vec>: vectors that do NOT push a CPU error code. We push a
; fake qword 0 so the frame layout is identical to ISR_ERR.
%macro ISR_NOERR 1
isr%1_stub:
    push qword 0          ; fake error code
    push qword %1         ; vector number
    jmp  isr_common
%endmacro

; ISR_ERR <vec>: vectors that push an 8-byte error code. We just push the
; vector number on top of it.
%macro ISR_ERR 1
isr%1_stub:
    push qword %1         ; vector number (error code already on stack)
    jmp  isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

; Common dispatcher. Stack layout on entry:
;   [rsp + 0]  vector
;   [rsp + 8]  error_code
;   [rsp + 16] rip       (CPU-pushed interrupt frame)
;   [rsp + 24] cs
;   [rsp + 32] rflags
;   [rsp + 40] rsp       (CPU-pushed; long-mode iretq frame is always 5 qwords)
;   [rsp + 48] ss
isr_common:
    PUSH_GPRS
    ; After PUSH_GPRS the original frame is at +120 (15 * 8 = 120).
    mov rdi, qword [rsp + 120 + 0]    ; vector
    mov rsi, qword [rsp + 120 + 8]    ; error_code
    mov rdx, qword [rsp + 120 + 16]   ; rip
    mov rcx, qword [rsp + 120 + 24]   ; cs
    mov r8,  qword [rsp + 120 + 32]   ; rflags

    ; Align RSP to 16 bytes for the System V ABI before the call. The 15-GPR
    ; push leaves RSP off by 8, and we have two qword pushes (vector + err)
    ; below that. Use a sub/add pair instead of relying on the count to keep
    ; future maintenance simple.
    sub rsp, 8
    call cpu_exception_handler
    add rsp, 8

    POP_GPRS
    add rsp, 16            ; drop pushed vector + error code
    iretq
