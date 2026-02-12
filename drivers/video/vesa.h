#ifndef VESA_H
#define VESA_H

#include <stdint.h>
#include "../../include/multiboot.h"

void vesa_init(multiboot_info_t* mb_info);
void vesa_put_pixel(int x, int y, uint32_t color);
void vesa_fill_rect(int x, int y, int w, int h, uint32_t color);
void vesa_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);
void vesa_draw_string(int x, int y, const char* str, uint32_t fg, uint32_t bg);
void vesa_clear(uint32_t color);
void vesa_update(void); // If double buffered

uint32_t vesa_get_width(void);
uint32_t vesa_get_height(void);

#endif
