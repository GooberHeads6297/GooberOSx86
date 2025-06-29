#include <stdint.h>
#include <stdbool.h>

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VIDEO_MEMORY ((uint16_t*)0xB8000)

extern uint8_t cursor_row;
extern uint8_t cursor_col;

static bool cursor_visible = false;

void vga_toggle_cursor() {
    uint16_t pos = cursor_row * VGA_WIDTH + cursor_col;
    uint16_t current = VIDEO_MEMORY[pos];
    uint8_t attr = current >> 8;

    if (cursor_visible) {
        VIDEO_MEMORY[pos] = (attr << 8) | ' ';
        cursor_visible = false;
    } else {
        VIDEO_MEMORY[pos] = (attr << 8) | '_';
        cursor_visible = true;
    }
}
