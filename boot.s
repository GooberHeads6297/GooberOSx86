; -----------------------------------------------------------------------------
; Multiboot1 + Multiboot2 headers
;
; The multiboot1 header has no video flag, so GRUB drops to text for any
; menuentry using the `multiboot` command. That's the VGA-text boot path.
;
; The multiboot2 header includes a framebuffer tag with 0/0/0 (let bootloader
; pick the firmware-preferred mode). Empirically GRUB *only* honors
; `gfxpayload=keep` for multiboot2 when this tag is present -- without it
; GRUB falls back to text mode even on hardware where gfxterm draws fine.
; -----------------------------------------------------------------------------

section .multiboot
align 4
    dd 0x1BADB002              ; Multiboot magic number
    dd 0x0                     ; No flags (no video request)
    dd -(0x1BADB002 + 0x0)     ; Checksum

align 8
mb2_header_start:
    dd 0xE85250D6              ; Multiboot2 magic
    dd 0                       ; Architecture (i386 protected mode)
    dd mb2_header_end - mb2_header_start  ; Header length
    dd -(0xE85250D6 + 0 + (mb2_header_end - mb2_header_start))  ; Checksum

; Framebuffer tag (type=5, optional). 0/0/0 means "any usable mode".
align 8
    dw 5                       ; Type: framebuffer
    dw 1                       ; Flags: bit0=1 means optional
    dd 20                      ; Size
    dd 0                       ; Preferred width  (0 = firmware preferred)
    dd 0                       ; Preferred height (0 = firmware preferred)
    dd 0                       ; Preferred depth  (0 = firmware preferred)

align 8
    dw 0                       ; Type: end
    dw 0                       ; Flags
    dd 8                       ; Size
mb2_header_end:

section .text
global start
extern gdt_load
extern kernel_main

start:
    ; Save multiboot magic and info pointer before anything
    mov [multiboot_magic], eax
    mov [multiboot_info], ebx

    ; Load our GDT (sets up flat code/data segments)
    call gdt_load

    ; Initialize stack
    mov esp, stack_top

    ; Pass multiboot info to kernel
    push dword [multiboot_info]
    push dword [multiboot_magic]
    call kernel_main

.hang:
    hlt
    jmp .hang

section .data
align 4
multiboot_magic: dd 0
multiboot_info:  dd 0

section .bss
align 16
stack_bottom:
    resb 65536                 ; 64KB stack (was 4KB; needed for deeper init paths)
stack_top:
