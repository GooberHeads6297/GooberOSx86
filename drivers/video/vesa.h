#ifndef VESA_H
#define VESA_H

#include <stdint.h>

void vesa_init(uint64_t fb_addr, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp);
void vesa_set_backbuffer(uint32_t* buffer);
void vesa_set_backbuffer_bytes(uint32_t* buffer, uint32_t bytes);
void vesa_swap(void);
void vesa_swap_rect(int x, int y, int w, int h);
/*
 * Phase 4 (display polish): single tear-free whole-screen present. Copies
 * the back-buffer into the LFB in one shot using memcpy. No-op when no
 * back-buffer is armed (drawing already went straight to the LFB).
 */
void vesa_present(void);
void vesa_put_pixel(int x, int y, uint32_t color);
void vesa_fill_rect(int x, int y, int w, int h, uint32_t color);
void vesa_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);
void vesa_draw_string(int x, int y, const char* str, uint32_t fg, uint32_t bg);
void vesa_clear(uint32_t color);
void vesa_update(void);
void vesa_boot_splash(const char* status);

uint32_t vesa_get_width(void);
uint32_t vesa_get_height(void);
uint32_t vesa_get_pitch(void);
uint32_t vesa_get_framebuffer_addr(void);
uint32_t vesa_get_backbuffer_bytes(void);
uint8_t vesa_get_bpp(void);
int vesa_has_backbuffer(void);
int vesa_is_initialized(void);

#endif
