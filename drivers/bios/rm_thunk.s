; rm_thunk.s -- protected-mode <-> real-mode gateway for BIOS INT calls.

BITS 32

section .text

global bios_rm_init
global bios_rm_call
global bios_rm_pm_resume
global rm_blob_start
global rm_blob_end

extern memcpy

%define RM_STATE_PHYS  0x7000
%define RM_CODE_PHYS   0x9000
%define RM_CODE_MAX    0x1000

section .text.rm_blob align=1
rm_blob_start:
    BITS 16

%macro rm_log_entry 1
    push ax
    push dx
    mov dx, 0xE9
    mov al, %1
    out dx, al
    pop dx
    pop ax
%endmacro

rm16_entry:
    cli
    rm_log_entry 'E'
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov sp, 0x9F00

    mov ax, [0x7034]
    cmp ax, 1
    je  rm16_vbe_set_mode
    cmp ax, 2
    je  rm16_vbe_query_current
    mov word [0x703E], 0x00FF
    jmp rm16_reenter_pm

%include "bios_vbe_rm.inc"

rm16_reenter_pm:
    cli
    o32 lgdt [0x7004]
    mov eax, cr0
    or  eax, 1
    mov cr0, eax
    ; 32-bit far jump back to bios_rm_pm_resume (offset patched by linker).
    db 0x66
    db 0xEA
    dd bios_rm_pm_resume
    dw 0x08

rm_blob_end:

BITS 32

section .text

bios_rm_init:
    push ebx
    mov eax, rm_blob_end
    sub eax, rm_blob_start
    cmp eax, RM_CODE_MAX
    ja  .fail
    push eax
    push dword RM_CODE_PHYS
    push dword rm_blob_start
    call memcpy
    add esp, 12
    mov eax, 1
    pop ebx
    ret
.fail:
    xor eax, eax
    pop ebx
    ret

; int bios_rm_call(uint32_t fn, void* params)
bios_rm_call:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi

    mov eax, edi
    mov edi, RM_STATE_PHYS
    mov [edi + 0x1C], eax
    mov [edi + 0x14], ebx
    mov [edi + 0x18], esi
    mov [edi + 0x20], ebp

    mov eax, cr0
    mov [edi + 0x00], eax
    sgdt [edi + 0x04]
    sidt [edi + 0x0C]

    mov eax, esp
    add eax, 16
    mov [edi + 0x24], eax

    xor eax, eax
    mov ax, ds
    mov [edi + 0x28], ax
    mov ax, es
    mov [edi + 0x2A], ax
    mov ax, fs
    mov [edi + 0x2C], ax
    mov ax, gs
    mov [edi + 0x2E], ax
    mov ax, ss
    mov [edi + 0x30], ax

    mov eax, [ebp + 8]
    mov [edi + 0x34], eax
    mov word [edi + 0x3E], 0

    ; Real-mode IVT at 0x0000: required so INT 10h reaches the firmware BIOS.
    ; Leaving the protected-mode IDT loaded across PE=0 triple-faults VBox/QEMU.
    mov word [edi + 0x40], 0x03FF
    mov dword [edi + 0x42], 0
    lidt [edi + 0x40]

    cli
    mov eax, cr0
    and eax, 0x7FFFFFFE
    mov cr0, eax

    ; 16-bit far jump to real-mode entry -- NO 32-bit instructions after CR0.PE=0.
    db 0x66
    db 0xEA
    dw RM_CODE_PHYS
    dw 0x0000

; Returned via far jmp from the 16-bit stub above.
bios_rm_pm_resume:
    mov edi, RM_STATE_PHYS
    movzx eax, word [edi + 0x3E]

    mov ebx, [edi + 0x14]
    mov esi, [edi + 0x18]
    mov ecx, [edi + 0x1C]
    mov ebp, [edi + 0x20]
    mov esp, [edi + 0x24]

    mov dx, [edi + 0x28]
    mov ds, dx
    mov dx, [edi + 0x2A]
    mov es, dx
    mov dx, [edi + 0x2C]
    mov fs, dx
    mov dx, [edi + 0x2E]
    mov gs, dx
    mov dx, [edi + 0x30]
    mov ss, dx

    push eax

    mov eax, [edi + 0x00]
    mov cr0, eax
    lgdt [edi + 0x04]
    lidt [edi + 0x0C]

    pop eax
    mov edi, ecx

    ; Reload CS after GDT/IDT restore (required on VBox/QEMU/real HW).
    push 0x08
    push dword .pm_done
    retf
.pm_done:
    sti
    ret
