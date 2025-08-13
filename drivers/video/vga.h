#ifndef VGA_H
#define VGA_H

#include <stdint.h>

#define VGA_WIDTH 80
#define VGA_HEIGHT 25

#define VGA_COLOR_BLACK         0x0
#define VGA_COLOR_BLUE          0x1
#define VGA_COLOR_GREEN         0x2
#define VGA_COLOR_CYAN          0x3
#define VGA_COLOR_RED           0x4
#define VGA_COLOR_MAGENTA       0x5
#define VGA_COLOR_BROWN         0x6
#define VGA_COLOR_LIGHT_GREY    0x7
#define VGA_COLOR_DARK_GREY     0x8
#define VGA_COLOR_LIGHT_BLUE    0x9
#define VGA_COLOR_LIGHT_GREEN   0xA
#define VGA_COLOR_LIGHT_CYAN    0xB
#define VGA_COLOR_LIGHT_RED     0xC
#define VGA_COLOR_LIGHT_MAGENTA 0xD
#define VGA_COLOR_LIGHT_BROWN   0xE
#define VGA_COLOR_WHITE         0xF

void move_cursor(uint8_t row, uint8_t col);

int vga_get_cursor_row(void);
int vga_get_cursor_col(void);
void vga_set_cursor_row(int row);
void vga_set_cursor_col(int col);


void vga_set_cursor(int row, int col);
void vga_toggle_cursor(void);
void vga_put_char_at(char c, int x, int y, unsigned char attr);
void vga_put_char(char c);
void vga_set_text_color(unsigned char fg, unsigned char bg);

#endif
