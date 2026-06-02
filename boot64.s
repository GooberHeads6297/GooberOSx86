; -----------------------------------------------------------------------------
; boot64.s - Phase 1 32->64 long-mode trampoline for GooberOSx86.
;
; This is the x86_64 counterpart to boot.s. It is assembled with `nasm -f elf64`
; only by scripts/build-x64.sh; the x86 build keeps using the original boot.s.
;
; Boot flow (multiboot enters us in 32-bit protected mode regardless of whether
; GRUB ran on legacy BIOS or UEFI x86_64-efi):
;
;   1. Save the multiboot magic (EAX) and info pointer (EBX) into .data.
;   2. Set up a temporary 32-bit stack (the same stack reservation we'll reuse
;      in 64-bit mode).
;   3. Verify the CPU supports CPUID and long mode; halt-with-message via the
;      0xE9 QEMU debug port + COM1 if either is missing.
;   4. Build PML4 + PDPT + four PDs identity-mapping the low 4 GiB with 2 MiB
;      huge pages (PS|RW|P). Tables live in .bss with 4 KiB alignment.
;   5. Load PML4 into CR3, set CR4.PAE | CR4.PGE, set EFER.LME via WRMSR
;      (0xC0000080), set CR0.PG | CR0.PE | CR0.WP | CR0.MP, clear CR0.EM.
;   6. lgdt the 64-bit GDT, far-jump to the 64-bit code segment.
;   7. In 64-bit mode: zero data segment selectors, set up the 64-bit stack,
;      pass mb_magic in RDI and mb_info in RSI (System V AMD64 ABI),
;      call kernel_main, then cli/hlt loop.
;
; Both multiboot1 and multiboot2 headers are kept verbatim so the existing
; grub.cfg menu entries (multiboot1 VGA-compat path, multiboot2 graphics path)
; keep finding a valid header during ISO boot.
; -----------------------------------------------------------------------------

; ---- Multiboot1 + Multiboot2 headers ----------------------------------------
section .multiboot
align 4
mb1_header:
    dd 0x1BADB002              ; multiboot1 magic
    dd 0x0                     ; flags (none)
    dd -(0x1BADB002 + 0x0)     ; checksum

align 8
mb2_header_start:
    dd 0xE85250D6              ; multiboot2 magic
    dd 0                       ; architecture: i386 protected mode (always)
    dd mb2_header_end - mb2_header_start
    dd -(0xE85250D6 + 0 + (mb2_header_end - mb2_header_start))

; framebuffer tag (type=5, optional). 0/0/0 means firmware-preferred mode.
; Required for GRUB to honour gfxpayload=keep + GOP under UEFI.
align 8
    dw 5                       ; tag type: framebuffer
    dw 1                       ; flags: optional
    dd 20                      ; size
    dd 0                       ; preferred width
    dd 0                       ; preferred height
    dd 0                       ; preferred depth

; end tag
align 8
    dw 0                       ; tag type: end
    dw 0                       ; flags
    dd 8                       ; size
mb2_header_end:

; ---- 32-bit entry from GRUB --------------------------------------------------
section .text
BITS 32

global start
extern kernel_main
extern gdt64_pointer

start:
    cli

    ; Save multiboot magic + info before clobbering EAX/EBX.
    mov [multiboot_magic], eax
    mov [multiboot_info],  ebx

    ; Establish a temporary 32-bit stack (same reservation reused in 64-bit
    ; mode after the trampoline). PUSHFD/POPFD for the CPUID test below need
    ; a working stack.
    mov esp, stack_top

    ; ---- CPUID support test --------------------------------------------------
    pushfd
    pop  eax
    mov  ecx, eax
    xor  eax, 1 << 21          ; toggle ID flag
    push eax
    popfd
    pushfd
    pop  eax
    push ecx                   ; restore original EFLAGS
    popfd
    cmp  eax, ecx
    je   .no_cpuid

    ; ---- Long-mode support test ----------------------------------------------
    mov  eax, 0x80000000
    cpuid
    cmp  eax, 0x80000001
    jb   .no_lm
    mov  eax, 0x80000001
    cpuid
    test edx, 1 << 29          ; LM bit in CPUID(0x80000001).EDX
    jz   .no_lm

    ; ---- Build identity page tables (low 4 GiB, 2 MiB pages) -----------------
    ;
    ; PML4[0]   -> PDPT
    ; PDPT[0..3]-> PD0..PD3
    ; PD*[i]    -> 2 MiB huge page at i * 2 MiB
    ;
    ; Each PD covers 1 GiB; four PDs cover the full low 4 GiB. Identity
    ; mapping (virt == phys) keeps every existing physical-pointer cast in
    ; the driver stack working post-port without a virt_to_phys rewrite.
    ;
    ; Zero all six tables first (defensive; .bss is already zero on load).
    mov  edi, pml4
    mov  ecx, (4096 * 6) / 4
    xor  eax, eax
    rep  stosd

    ; PML4[0] = PDPT | RW | P
    mov  dword [pml4 + 0], pdpt + 0x3
    mov  dword [pml4 + 4], 0

    ; PDPT[0..3] = PD0..PD3 | RW | P
    mov  dword [pdpt + 0*8 + 0], pd0 + 0x3
    mov  dword [pdpt + 0*8 + 4], 0
    mov  dword [pdpt + 1*8 + 0], pd1 + 0x3
    mov  dword [pdpt + 1*8 + 4], 0
    mov  dword [pdpt + 2*8 + 0], pd2 + 0x3
    mov  dword [pdpt + 2*8 + 4], 0
    mov  dword [pdpt + 3*8 + 0], pd3 + 0x3
    mov  dword [pdpt + 3*8 + 4], 0

    ; Fill PD entries: PA[i] = i * 2 MiB; flags PS | RW | P = 0x83.
    ; All four PDs are contiguous in .bss, so a single linear walk fills them.
    mov  edi, pd0
    mov  eax, 0x83             ; entry low: PA=0, flags=PS|RW|P
    mov  edx, 0                ; entry high
    mov  ecx, 2048             ; 4 PDs * 512 entries
.fill_pd:
    mov  [edi + 0], eax
    mov  [edi + 4], edx
    add  eax, 0x00200000       ; +2 MiB
    adc  edx, 0
    add  edi, 8
    loop .fill_pd

    ; ---- CR3 / CR4 / EFER / CR0 sequence -------------------------------------
    mov  eax, pml4
    mov  cr3, eax              ; load page table root

    mov  eax, cr4
    or   eax, (1 << 5) | (1 << 7)   ; PAE + PGE
    mov  cr4, eax

    mov  ecx, 0xC0000080       ; IA32_EFER
    rdmsr
    or   eax, (1 << 8)         ; LME
    wrmsr

    mov  eax, cr0
    or   eax, (1 << 31) | (1 << 16) | (1 << 0) | (1 << 1)   ; PG | WP | PE | MP
    and  eax, ~(1 << 2)                                     ; clear EM
    mov  cr0, eax

    ; CR0.PG | EFER.LME | CR4.PAE => IA-32e mode active. CS still has L=0
    ; (legacy 32-bit), so we are in compatibility mode until the far jump.

    ; ---- Load 64-bit GDT and far-jump to 64-bit code -------------------------
    lgdt [gdt64_pointer]
    jmp  0x08:long_mode_start

; ---- Halt-with-message helpers (32-bit) -------------------------------------
.no_cpuid:
    mov  esi, msg_no_cpuid
    jmp  .die_print
.no_lm:
    mov  esi, msg_no_lm
    jmp  .die_print

; Print an ASCIZ string at ESI to both COM1 (0x3F8) and the QEMU debug port
; (0xE9), then halt forever. Used before paging/long-mode are up so we have
; visible output if either CPUID check fails.
.die_print:
    ; Init COM1 (8N1, 38400) so a -serial file:... capture sees the message.
    mov  dx, 0x3FB             ; LCR
    mov  al, 0x80              ; DLAB
    out  dx, al
    mov  dx, 0x3F8             ; divisor low
    mov  al, 3                 ; 38400 baud
    out  dx, al
    mov  dx, 0x3F9             ; divisor high
    xor  al, al
    out  dx, al
    mov  dx, 0x3FB             ; 8N1, DLAB clear
    mov  al, 0x03
    out  dx, al
.dp_loop:
    mov  al, [esi]
    test al, al
    jz   .dp_done
    mov  dx, 0x3F8             ; THR
    out  dx, al
    mov  dx, 0xE9              ; QEMU debug console
    out  dx, al
    inc  esi
    jmp  .dp_loop
.dp_done:
    cli
.die32:
    hlt
    jmp .die32

; ---- 64-bit code -------------------------------------------------------------
BITS 64
default rel                    ; RIP-relative addressing for memory operands
long_mode_start:
    ; Zero data segment selectors. In long mode DS/ES/FS/GS/SS are largely
    ; vestigial (base ignored), but we still want clean values.
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax

    ; 64-bit stack reuse the same 64 KiB reservation.
    lea  rsp, [stack_top]
    xor  rbp, rbp

    ; System V AMD64: first arg in RDI, second in RSI.
    ; multiboot_magic / multiboot_info are 32-bit values; zero-extend them.
    xor  rdi, rdi
    mov  edi, dword [multiboot_magic]
    xor  rsi, rsi
    mov  esi, dword [multiboot_info]

    call kernel_main

.hang64:
    cli
    hlt
    jmp .hang64

; ---- Data + page tables ------------------------------------------------------
section .data
align 4
multiboot_magic: dd 0
multiboot_info:  dd 0

msg_no_cpuid: db "GooberOSx86 boot64: FATAL: CPUID not supported.",10,0
msg_no_lm:    db "GooberOSx86 boot64: FATAL: long mode (CPUID.80000001:EDX.LM) absent.",10,0

section .bss
; 4 KiB-aligned page tables for the low-4-GiB identity map. Each table is
; one full 4 KiB frame so it is naturally aligned and trivially indexable.
align 4096
pml4: resb 4096
pdpt: resb 4096
pd0:  resb 4096
pd1:  resb 4096
pd2:  resb 4096
pd3:  resb 4096

; 64 KiB stack matching boot.s. 64 KiB has historically been needed for the
; deeper init paths the kernel runs through, so we keep the same reservation.
align 16
stack_bottom:
    resb 65536
stack_top:
