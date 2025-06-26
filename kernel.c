#include <stdint.h>
#include <stddef.h>

static uint16_t* const VIDEO_MEMORY = (uint16_t*)0xb8000;
static uint8_t cursor_row = 0;
static uint8_t cursor_col = 0;

static void clear_screen() {
    for (size_t row = 0; row < 25; row++) {
        for (size_t col = 0; col < 80; col++) {
            const size_t index = row * 80 + col;
            VIDEO_MEMORY[index] = ((uint16_t)0x0F << 8) | ' ';
        }
    }
    cursor_row = 0;
    cursor_col = 0;
}

static void print_char(char c) {
    if (c == '\n') {
        cursor_row++;
        cursor_col = 0;
        return;
    }

    const uint8_t attribute = 0x0F;
    const uint16_t index = cursor_row * 80 + cursor_col;
    VIDEO_MEMORY[index] = ((uint16_t)attribute << 8) | c;

    cursor_col++;
    if (cursor_col >= 80) {
        cursor_col = 0;
        cursor_row++;
    }

    if (cursor_row >= 25) {
        clear_screen();
    }
}

static void print(const char* str) {
    for (size_t i = 0; str[i] != '\0'; i++) {
        print_char(str[i]);
    }
}

void kernel_main() {
    clear_screen();
    print("GooberOS Kernel -- X86\n");
    print("This is VGA text mode output.\n");

    while (1) { __asm__("hlt"); }
}
