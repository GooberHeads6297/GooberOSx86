; CPU exception stubs (vectors 0-31).
;
; Each stub pushes a dummy error code (for vectors that don't push one), then
; the vector number, then jumps to a common dispatcher which calls the C
; handler with (vector, error_code). The handler halts the CPU rather than
; allowing a triple-fault on real hardware where there is no debugger.

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

%macro ISR_NOERR 1
isr%1_stub:
    cli
    push dword 0           ; dummy error code
    push dword %1          ; vector number
    jmp isr_common
%endmacro

%macro ISR_ERR 1
isr%1_stub:
    cli
    push dword %1          ; vector number (error code already on stack)
    jmp isr_common
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

isr_common:
    pushad
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Stack layout after segment pushes:
    ;   gs, fs, es, ds, edi, esi, ebp, esp, ebx, edx, ecx, eax,
    ;   vector, errcode, eip, cs, eflags
    ; Pass the useful fault frame fields to the C handler.
    mov ecx, [esp + 64]    ; eflags
    mov edx, [esp + 60]    ; cs
    mov esi, [esp + 56]    ; eip
    mov eax, [esp + 48]    ; vector (4 segs * 4 + 8 gprs * 4 = 48)
    mov ebx, [esp + 52]    ; error code
    push ecx
    push edx
    push esi
    push ebx
    push eax
    call cpu_exception_handler
    add esp, 20

    ; If the handler returns, just hang. Don't try to resume.
.halt:
    cli
    hlt
    jmp .halt
