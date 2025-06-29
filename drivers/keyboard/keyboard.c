#include "keyboard.h"
#include "drivers/io/io.h"
#include <stddef.h>

#define BUFFER_SIZE 128
static char buffer[BUFFER_SIZE];
static volatile int head = 0;
static volatile int tail = 0;
static uint8_t shift = 0;
static uint8_t extended = 0;

static char scancode_to_ascii[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8',
    '9', '0', '-', '=', '\b', '\t', 'q', 'w', 'e', 'r',
    't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    '\'', '`', 0, '\\', 'z', 'x', 'c', 'v', 'b', 'n',
    'm', ',', '.', '/', 0, '*', 0, ' ', 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

static char scancode_to_ascii_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*',
    '(', ')', '_', '+', '\b', '\t', 'Q', 'W', 'E', 'R',
    'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    '"', '~', 0, '|', 'Z', 'X', 'C', 'V', 'B', 'N',
    'M', '<', '>', '?', 0, '*', 0, ' ', 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

// Extended scancodes map for numpad keys (after 0xE0 prefix)
// These are commonly numpad keys with Num Lock ON that send extended scancodes.
static char extended_scancode_to_ascii[128] = {
    0,    0, 0, 0, 0, 0, 0, 0, 0, 0,  // padding for indices 0-9
    0,    0, 0, 0, 0, 0, 0, 0, 0, 0,  // padding for indices 10-19
    0,    0, 0, 0, 0, 0, 0, 0, 0, 0,  // padding for indices 20-29
    0,    0, 0, 0, 0, 0, 0, 0, 0, 0,  // padding for indices 30-39
    0,    0, 0, 0, 0, 0, 0, 0, 0, 0,  // padding for indices 40-49
    0,    0, 0, 0, 0, 0, 0, 0, 0, 0,  // padding for indices 50-59
    0,    0, 0, 0, 0, 0, 0, 0, 0, 0,  // padding for indices 60-69
    0,    0, 0, 0, 0, 0, 0, 0, 0, 0,  // padding for indices 70-79
    0,    0, 0, 0, 0, 0, 0, 0, 0, 0,  // padding for indices 80-89
    0,    0, 0, 0, 0, 0, 0, 0, 0, 0,  // padding for indices 90-99
    0,    0, 0, 0, 0, 0, 0, 0, 0, 0,  // padding for indices 100-109
    0,    0, 0, 0, 0, 0, 0, 0, 0, 0,  // padding for indices 110-119
    0,    0, 0, 0, 0, 0, 0, 0          // up to 127
};

// Manually map common extended numpad keys (0xE0 prefixed)
// Scancodes for keypad with Num Lock ON that send extended codes:
// These are common:
// 0x4A = Keypad '-' (minus)
// 0x4E = Keypad '+' (plus)
// 0x47 = Keypad '7' (Home)
// 0x48 = Keypad '8' (Up)
// 0x49 = Keypad '9' (Page Up)
// 0x4B = Keypad '4' (Left)
// 0x4D = Keypad '6' (Right)
// 0x4F = Keypad '1' (End)
// 0x50 = Keypad '2' (Down)
// 0x51 = Keypad '3' (Page Down)
// 0x52 = Keypad '0' (Insert)
// 0x53 = Keypad '.' (Delete)

// We'll handle these in the handler explicitly instead of table lookup.

void keyboard_init() {
    head = 0;
    tail = 0;
    shift = 0;
    extended = 0;
    // IRQ1 handler should be registered here if not already done elsewhere
    // Example: isr_register_handler(IRQ1, keyboard_interrupt_handler);
}

void keyboard_interrupt_handler() {
    uint8_t scancode = inb(0x60);

    if (scancode == 0xE0) {
        extended = 1;
        outb(0x20, 0x20);
        return;
    }

    if (scancode == 0x2A || scancode == 0x36) {
        shift = 1;
    } else if (scancode == 0xAA || scancode == 0xB6) {
        shift = 0;
    } else if (!(scancode & 0x80)) {
        char c = 0;

        if (extended) {
            switch (scancode) {
                case 0x4A: c = '-'; break;  // keypad minus
                case 0x4E: c = '+'; break;  // keypad plus
                case 0x47: c = '7'; break;
                case 0x48: c = '8'; break;
                case 0x49: c = '9'; break;
                case 0x4B: c = '4'; break;
                case 0x4D: c = '6'; break;
                case 0x4F: c = '1'; break;
                case 0x50: c = '2'; break;
                case 0x51: c = '3'; break;
                case 0x52: c = '0'; break;
                case 0x53: c = '.'; break;
                default: c = 0; break;
            }
        } else {
            c = shift ? scancode_to_ascii_shift[scancode] : scancode_to_ascii[scancode];
        }

        if (c) {
            int next = (head + 1) % BUFFER_SIZE;
            if (next != tail) {
                buffer[head] = c;
                head = next;
            }
        }
    }

    extended = 0;
    outb(0x20, 0x20);
}

int keyboard_has_char() {
    return head != tail;
}

char keyboard_read_char() {
    if (head == tail) return 0;
    char c = buffer[tail];
    tail = (tail + 1) % BUFFER_SIZE;
    return c;
}
