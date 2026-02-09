bits 32
global bios_int13_ext

; Placeholder BIOS INT 13h thunk.
; Returns non-zero in EAX to indicate failure.
bios_int13_ext:
    mov eax, 1
    ret
