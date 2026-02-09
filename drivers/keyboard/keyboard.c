#include "keyboard.h"
#include "../io/io.h"
#include <stddef.h>
#include "../video/vga.h"

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

void keyboard_init(void) {
    head = 0;
    tail = 0;
    shift = 0;
    extended = 0;
}

void keyboard_interrupt_handler(void) {
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
                case 0x48: c = 0x80; break; /* Arrow up */
                case 0x50: c = 0x81; break; /* Arrow down */
                case 0x4B: c = 0x82; break; /* Arrow left */
                case 0x4D: c = 0x83; break; /* Arrow right */
                case 0x4A: c = '-'; break;
                case 0x4E: c = '+'; break;
                case 0x47: c = '7'; break;
                case 0x49: c = '9'; break;
                case 0x4F: c = '1'; break;
                case 0x51: c = '3'; break;
                case 0x52: c = '0'; break;
                case 0x53: c = '.'; break;
                default: c = 0; break;
            }
        } else {
            switch (scancode) {
                case 0x48: c = 0x80; break; /* Arrow up (non-extended) */
                case 0x50: c = 0x81; break; /* Arrow down (non-extended) */
                case 0x4B: c = 0x82; break; /* Arrow left (non-extended) */
                case 0x4D: c = 0x83; break; /* Arrow right (non-extended) */
                case 0x3B: c = 0x8B; break; // F1
                case 0x3C: c = 0x8C; break; // F2
                case 0x3D: c = 0x8D; break; // F3
                case 0x3E: c = 0x8E; break; // F4
                case 0x3F: c = 0x8F; break; // F5
                case 0x40: c = 0x90; break; // F6
                case 0x41: c = 0x91; break; // F7
                case 0x42: c = 0x92; break; // F8
                case 0x43: c = 0x93; break; // F9
                case 0x44: c = 0x94; break; // F10
                case 0x57: c = 0x95; break; // F11
                case 0x58: c = 0x96; break; // F12
                default:
                    c = shift ? scancode_to_ascii_shift[scancode] : scancode_to_ascii[scancode];
                    break;
            }
        }

        if (c) {
            int next = (head + 1) % BUFFER_SIZE;
            if (next != tail) {
                buffer[head] = c;
                head = next;
            }
        }
    }

    if (scancode != 0xE0) {
        extended = 0;
    }
    outb(0x20, 0x20);
}

int keyboard_has_char(void) {
    return head != tail;
}

char keyboard_read_char(void) {
    if (head == tail) return 0;
    char c = buffer[tail];
    tail = (tail + 1) % BUFFER_SIZE;
    return c;
}
