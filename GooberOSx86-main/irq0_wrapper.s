global irq0_handler_asm
extern irq0_handler_main

irq0_handler_asm:
    pusha
    call irq0_handler_main
    popa
    iretd
