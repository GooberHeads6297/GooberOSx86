; -----------------------------------------------------------------------------
; isr32_stub64.s - 64-bit IRQ0 (PIT) timer ISR.
;
; Long-mode counterpart to the inline `__attribute__((naked)) irq0_handler_asm`
; in kernel.c. A real assembly file is safer in long mode because gcc's `naked`
; attribute is only well-supported on i386/arm; on x86_64 it can still emit
; prologue/epilogue or stack-canary code in some configs.
;
; Saves the 16 GPRs, calls irq0_handler_main(), restores GPRs, iretq's.
; Symmetric with irq1_wrapper64.s.
;
; Phase 3b.0 note (THIS REVISION): the previous Phase 2/3a stub emitted an
; explicit `outb 0x20, 0x20` master-PIC EOI here because the local
; irq0_handler_main() in kernel_x64.c was a one-liner that just bumped a
; counter. With Phase 3b.0 wiring in the real drivers/timer/timer.c::
; timer_interrupt_handler() (called from the now-real irq0_handler_main()),
; the EOI is emitted by timer_interrupt_handler() itself. Acking the master
; PIC twice would mask the next tick and break IRQ0 entirely, so the ASM
; EOI is dropped. The 16-GPR save/restore is unchanged.
; -----------------------------------------------------------------------------

BITS 64

global isr32_stub
extern irq0_handler_main

section .text

isr32_stub:
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

    sub rsp, 8
    call irq0_handler_main
    add rsp, 8

    ; NOTE: NO PIC EOI here in Phase 3b.0+. timer_interrupt_handler() in
    ; drivers/timer/timer.c emits `outb 0x20, 0x20` itself (matching the
    ; 32-bit kernel.c path). Adding an EOI here would double-ack the
    ; master PIC and silently mask the next tick.

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
    iretq
