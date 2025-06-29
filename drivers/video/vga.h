#ifndef VGA_H
#define VGA_H

void vga_set_cursor(int x, int y);
void vga_toggle_cursor(void);
void vga_put_char_at(char c, int x, int y, unsigned char attr);

#endif
