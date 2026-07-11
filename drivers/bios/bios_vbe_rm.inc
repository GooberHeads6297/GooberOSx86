; bios_vbe_rm.inc -- 16-bit INT 10h VBE workers (included in rm_blob).

%define VBE_INFO_PHYS    0x8000
%define VBE_MODE_PHYS    0x8200
%define VBE_PARAMS_PHYS  0x8100
%define VBE_SCRATCH_MODE 0x8300
%define VBE_SCRATCH_SCORE 0x8302
%define VBE_SCRATCH_PASS 0x8304

%define VBE_OK           0x004F

%macro rm_log 1
    push ax
    push dx
    mov dx, 0xE9
    mov al, %1
    out dx, al
    pop dx
    pop ax
%endmacro

%macro set_es_di_phys 1
    mov di, %1
    mov ax, di
    mov cl, 4
    shr ax, cl
    mov es, ax
    and di, 0x000F
%endmacro

%macro zero_mode_info 0
    push di
    push es
    push ax
    push cx
    set_es_di_phys VBE_MODE_PHYS
    xor ax, ax
    mov cx, 128
    rep stosw
    pop cx
    pop ax
    pop es
    pop di
%endmacro

; Fill BIOS_VBE_PARAMS_PHYS from VBE_MODE_PHYS for the mode in VBE_SCRATCH_MODE.
%macro vbe_fill_params_ok 0
    mov byte [VBE_PARAMS_PHYS + 4], 1
    mov ax, [VBE_SCRATCH_MODE]
    mov [VBE_PARAMS_PHYS + 6], ax

    mov ax, [VBE_MODE_PHYS + 0x28]
    mov [VBE_PARAMS_PHYS + 8], ax
    mov ax, [VBE_MODE_PHYS + 0x2A]
    mov [VBE_PARAMS_PHYS + 10], ax

    mov ax, [VBE_MODE_PHYS + 0x12]
    mov [VBE_PARAMS_PHYS + 12], ax
    mov word [VBE_PARAMS_PHYS + 14], 0

    mov ax, [VBE_MODE_PHYS + 0x14]
    mov [VBE_PARAMS_PHYS + 16], ax
    mov word [VBE_PARAMS_PHYS + 18], 0

    mov ax, [VBE_MODE_PHYS + 0x10]
    mov [VBE_PARAMS_PHYS + 20], ax
    mov word [VBE_PARAMS_PHYS + 22], 0

    mov al, [VBE_MODE_PHYS + 0x19]
    mov [VBE_PARAMS_PHYS + 24], al

    mov word [0x703E], 0
%endmacro

; Microsoft Basic Display style: query the firmware's CURRENT mode (no 4F02).
rm16_vbe_query_current:
    push ds
    push es
    push si
    push bx
    push cx
    push dx

    rm_log 'Q'
    mov byte [VBE_PARAMS_PHYS + 4], 0

    mov ax, 0x4F03
    int 0x10
    cmp ax, VBE_OK
    jne .query_fail
    mov ax, bx
    and ax, 0xBFFF
    test ax, ax
    jz  .query_fail
    mov [VBE_SCRATCH_MODE], ax

    zero_mode_info
    mov cx, [VBE_SCRATCH_MODE]
    mov ax, 0x4F01
    set_es_di_phys VBE_MODE_PHYS
    int 0x10
    cmp ax, VBE_OK
    jne .query_fail

    test byte [VBE_MODE_PHYS], 0x80
    jz  .query_fail

    mov al, [VBE_MODE_PHYS + 0x19]
    cmp al, 32
    je  .bpp_ok
    cmp al, 24
    je  .bpp_ok
    jmp .query_fail

.bpp_ok:
    mov ax, [VBE_MODE_PHYS + 0x28]
    or  ax, [VBE_MODE_PHYS + 0x2A]
    jz  .query_fail

    vbe_fill_params_ok
    rm_log 'O'
    jmp .query_exit

.query_fail:
    mov word [0x703E], 1
    rm_log 'X'

.query_exit:
    pop dx
    pop cx
    pop bx
    pop si
    pop es
    pop ds
    jmp rm16_reenter_pm

rm16_vbe_set_mode:
    push ds
    push es
    push si
    push bx
    push cx
    push dx

    rm_log 'V'
    mov byte [VBE_PARAMS_PHYS + 4], 0
    mov word [VBE_SCRATCH_MODE], 0xFFFF
    mov byte [VBE_SCRATCH_SCORE], 0
    mov byte [VBE_SCRATCH_PASS], 0

    push di
    push es
    push ax
    push cx
    set_es_di_phys VBE_INFO_PHYS
    xor ax, ax
    mov cx, 256
    rep stosw
    pop cx
    pop ax
    pop es
    pop di

    mov ax, 0x4F00
    set_es_di_phys VBE_INFO_PHYS
    int 0x10
    cmp ax, VBE_OK
    jne .fail

    cmp word [VBE_INFO_PHYS + 0], 0x4553
    jne .fail
    cmp word [VBE_INFO_PHYS + 2], 0x4156
    jne .fail

    mov ax, [VBE_INFO_PHYS + 0x16]
    cmp ax, 0x0200
    jb  .fail

    rm_log 'I'

    mov si, [VBE_INFO_PHYS + 0x0E]
    mov ax, [VBE_INFO_PHYS + 0x10]
    mov ds, ax

.mode_loop:
    mov cx, [si]
    add si, 2
    cmp cx, 0xFFFF
    je  .pass_done
    test cx, cx
    jz  .mode_loop

    push cx
    zero_mode_info
    mov ax, 0x4F01
    set_es_di_phys VBE_MODE_PHYS
    pop cx
    push cx
    int 0x10
    cmp ax, VBE_OK
    jne .next_mode

    mov al, [VBE_MODE_PHYS]
    test al, 0x01
    jz  .next_mode
    test al, 0x10
    jz  .next_mode
    test al, 0x80
    jz  .next_mode

    mov al, [VBE_MODE_PHYS + 0x19]
    cmp al, 32
    je  .bpp_ok2
    cmp byte [VBE_SCRATCH_PASS], 0
    je  .next_mode
    cmp al, 24
    je  .bpp_ok2
    jmp .next_mode

.bpp_ok2:
    mov ax, [VBE_MODE_PHYS + 0x12]
    mov cx, [VBE_MODE_PHYS + 0x10]
    mov dl, [VBE_MODE_PHYS + 0x19]
    cmp dl, 32
    jne .chk24pitch
    shl ax, 1
    shl ax, 1
    jmp .cmp_pitch
.chk24pitch:
    mov di, ax
    shl ax, 1
    add ax, di
.cmp_pitch:
    cmp cx, ax
    jb  .next_mode

    mov ax, [VBE_MODE_PHYS + 0x12]
    mov bx, [VBE_MODE_PHYS + 0x14]
    xor dl, dl
    cmp ax, 1024
    jne .s800
    cmp bx, 768
    je  .sc100
.s800:
    cmp ax, 800
    jne .s640
    cmp bx, 600
    je  .sc90
.s640:
    cmp ax, 640
    jne .sc40
    cmp bx, 480
    je  .sc80
.sc40:
    mov dl, 40
    jmp .bpp_add
.sc80:
    mov dl, 80
    jmp .bpp_add
.sc90:
    mov dl, 90
    jmp .bpp_add
.sc100:
    mov dl, 100

.bpp_add:
    mov al, [VBE_MODE_PHYS + 0x19]
    cmp al, 32
    jne .add10
    add dl, 30
    jmp .best
.add10:
    add dl, 10

.best:
    cmp dl, [VBE_SCRATCH_SCORE]
    jbe .next_mode
    mov [VBE_SCRATCH_SCORE], dl
    pop cx
    push cx
    mov [VBE_SCRATCH_MODE], cx
    jmp .next_mode

.next_mode:
    pop cx
    jmp .mode_loop

.pass_done:
    mov ax, [VBE_SCRATCH_MODE]
    cmp ax, 0xFFFF
    jne .have_mode
    cmp byte [VBE_SCRATCH_PASS], 0
    jne .fail
    mov byte [VBE_SCRATCH_PASS], 1
    mov byte [VBE_SCRATCH_SCORE], 0
    mov si, [VBE_INFO_PHYS + 0x0E]
    mov ax, [VBE_INFO_PHYS + 0x10]
    mov ds, ax
    jmp .mode_loop

.have_mode:
    rm_log 'S'
    mov bx, [VBE_SCRATCH_MODE]
    or  bx, 0x4000
    mov ax, 0x4F02
    xor cx, cx
    xor dx, dx
    int 0x10
    cmp ax, VBE_OK
    jne .fail

    mov cx, [VBE_SCRATCH_MODE]
    zero_mode_info
    mov ax, 0x4F01
    set_es_di_phys VBE_MODE_PHYS
    int 0x10
    cmp ax, VBE_OK
    jne .fail

    mov ax, [VBE_MODE_PHYS + 0x28]
    or  ax, [VBE_MODE_PHYS + 0x2A]
    jz  .fail

    vbe_fill_params_ok
    rm_log 'O'
    jmp .exit

.fail:
    mov word [0x703E], 1
    rm_log 'X'

.exit:
    pop dx
    pop cx
    pop bx
    pop si
    pop es
    pop ds
    jmp rm16_reenter_pm
