#include "keyboard.h"
#include "../io/io.h"
#include <stddef.h>
#include <stdbool.h>

#define BUFFER_SIZE 128
static char buffer[BUFFER_SIZE];
static volatile int head = 0;
static volatile int tail = 0;

// State tracking
static bool key_states[256]; // Track pressed state of raw scancodes
static bool shift_pressed = false;
static bool ctrl_pressed = false;
static bool alt_pressed = false;
static bool caps_lock = false;
static uint8_t extended = 0;

static char scancode_to_ascii[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8',
    '9', '0', '-', '=', '\b', '\t', 'q', 'w', 'e', 'r',
    't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, // 29 = Ctrl
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    '\'', '`', 0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', // 42 = Shift
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
    shift_pressed = false;
    ctrl_pressed = false;
    alt_pressed = false;
    extended = 0;
    for (int i = 0; i < 256; i++) key_states[i] = false;
}

void keyboard_interrupt_handler(void) {
    uint8_t scancode = inb(0x60);

    if (scancode == 0xE0) {
        extended = 1;
        outb(0x20, 0x20); // EOI
        return;
    }

    bool released = (scancode & 0x80) != 0;
    uint8_t code = scancode & 0x7F;

    // Update state
    if (code < 128) {
        key_states[code] = !released;
    }

    // Handle modifiers
    if (code == 0x2A || code == 0x36) shift_pressed = !released;
    if (code == 0x1D) ctrl_pressed = !released;
    if (code == 0x38) alt_pressed = !released;
    if (code == 0x3A && !released) caps_lock = !caps_lock;

    if (!released) {
        char c = 0;

        if (extended) {
            switch (code) {
                case 0x48: c = KEY_UP; break;
                case 0x50: c = KEY_DOWN; break;
                case 0x4B: c = KEY_LEFT; break;
                case 0x4D: c = KEY_RIGHT; break;
                // Add more extended keys if needed
            }
        } else {
            // Standard mapping
            if (code < 128) {
                // F-keys
                if (code >= 0x3B && code <= 0x44) c = KEY_F1 + (code - 0x3B);
                else if (code == 0x57) c = KEY_F1 + 10; // F11
                else if (code == 0x58) c = KEY_F1 + 11; // F12
                else {
                    bool upper = shift_pressed ^ caps_lock;
                    // Note: caps lock only affects letters usually, but for simplicity here we use shift map
                    // A better way is to check if it's a letter.
                    // For now, let's stick to shift map for simplicity, or refine:
                    if (caps_lock && scancode_to_ascii[code] >= 'a' && scancode_to_ascii[code] <= 'z') {
                        c = scancode_to_ascii_shift[code];
                        if (shift_pressed) c = scancode_to_ascii[code]; // Shift cancels caps
                    } else {
                        c = shift_pressed ? scancode_to_ascii_shift[code] : scancode_to_ascii[code];
                    }
                }
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

    extended = 0;
    outb(0x20, 0x20); // EOI
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

bool keyboard_is_pressed(uint8_t scancode) {
    return key_states[scancode & 0x7F];
}

bool keyboard_is_shift_active(void) { return shift_pressed; }
bool keyboard_is_ctrl_active(void) { return ctrl_pressed; }
bool keyboard_is_alt_active(void) { return alt_pressed; }
