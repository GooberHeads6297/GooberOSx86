#include "keyboard.h"
#include "drivers/io/io.h"

static char scancode_to_ascii[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', /* 9 */
    '9', '0', '-', '=', '\b', '\t', 'q', 'w', 'e', 'r', /* 19 */
    't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,   /* 29 */
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',  /* 39 */
    '\'', '`', 0, '\\', 'z', 'x', 'c', 'v', 'b', 'n',  /* 49 */
    'm', ',', '.', '/', 0, '*', 0, ' ', 0, 0,         /* 59 */
    /* Remaining scancodes (mostly function keys and extended keys) */ 0
};


volatile char last_char = 0;

void keyboard_init() {
    // Already handled by IRQ setup in your IDT init.
    // Could add future config here if needed.
}

void keyboard_interrupt_handler() {
    uint8_t scancode = inb(0x60);
    if (scancode & 0x80) {
        // Key release (ignore for now)
    } else {
        char c = scancode_to_ascii[scancode];
        if (c) last_char = c;
    }

    // Acknowledge interrupt
    outb(0x20, 0x20);
}

char keyboard_read_char() {
    char c = last_char;
    last_char = 0;
    return c;
}
