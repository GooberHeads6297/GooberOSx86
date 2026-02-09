#include "vga.h"
#include <stdint.h>
#include <stdbool.h>

#define VGA_WIDTH 80
#define VGA_HEIGHT 25

uint8_t cursor_row = 0;
uint8_t cursor_col = 0;
uint16_t* const VIDEO_MEMORY = (uint16_t*)0xB8000;

static bool cursor_visible = false;
static unsigned char default_attr = (VGA_COLOR_LIGHT_GREY | (VGA_COLOR_BLACK << 4));

int vga_get_cursor_row(void) { return cursor_row; }
int vga_get_cursor_col(void) { return cursor_col; }
void vga_set_cursor_row(int row) { cursor_row = row; }
void vga_set_cursor_col(int col) { cursor_col = col; }


static void vga_scroll() {
    for (int y = 1; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            VIDEO_MEMORY[(y - 1) * VGA_WIDTH + x] = VIDEO_MEMORY[y * VGA_WIDTH + x];
        }
    }
    for (int x = 0; x < VGA_WIDTH; x++) {
        VIDEO_MEMORY[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = (default_attr << 8) | ' ';
    }
    cursor_row = VGA_HEIGHT - 1;
}

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

void vga_put_char_at(char c, int x, int y, unsigned char attr) {
    const int index = y * VGA_WIDTH + x;
    VIDEO_MEMORY[index] = ((uint16_t)attr << 8) | c;
}

void vga_put_char(char c) {
    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
        if (cursor_row >= VGA_HEIGHT) {
            vga_scroll();
        }
        return;
    }

    // Scroll BEFORE writing if we're at the bottom-right
    if (cursor_row >= VGA_HEIGHT) {
        vga_scroll();
    }

    vga_put_char_at(c, cursor_col, cursor_row, default_attr);

    cursor_col++;
    if (cursor_col >= VGA_WIDTH) {
        cursor_col = 0;
        cursor_row++;
        if (cursor_row >= VGA_HEIGHT) {
            vga_scroll();
        }
    }
}

void vga_set_cursor(int row, int col) {
    cursor_row = row;
    cursor_col = col;
}

void move_cursor(uint8_t row, uint8_t col) {
    vga_set_cursor(row, col);
}

void vga_set_text_color(unsigned char fg, unsigned char bg) {
    default_attr = (fg & 0x0F) | ((bg & 0x0F) << 4);
}

void vga_set_default_color(unsigned char color) {
    default_attr = color;
    for (int y = 0; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            uint16_t current = VIDEO_MEMORY[y * VGA_WIDTH + x];
            char c = current & 0xFF;
            VIDEO_MEMORY[y * VGA_WIDTH + x] = ((uint16_t)color << 8) | c;
        }
    }
}
