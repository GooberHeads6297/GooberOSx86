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

extern uint16_t* const VIDEO_MEMORY;

void clear_screen(void);
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
void vga_set_default_color(unsigned char color);

/*
 * Phase 4 (display polish, item 5): vga_text_putc(x,y,c,attr) is a single
 * hook point for code that historically wrote directly to the 80x25 text
 * plane at 0xB8000 (editor, games). When the VGA-passthrough shim is
 * armed (see gui/vga_passthrough.h) the write is rerouted into the
 * shim's 80x25 virtual cell grid so the VESA-window class renderer can
 * paint it through the 8x16 font into the panel pixel grid. When the
 * shim is NOT armed, the write goes to the legacy 0xB8000 plane (x86
 * BIOS boot path, preserved verbatim).
 *
 * This is the SAME signature as vga_put_char_at; the editor + games
 * adoption is a literal s/vga_put_char_at/vga_text_putc/.
 */
void vga_text_putc(char c, int x, int y, unsigned char attr);

/*
 * Phase 4 (item 3): VGA mode-13h graphical surface. The driver framework's
 * "vga-graphics" rung calls these to bring up a 320x200x8 indexed-colour
 * framebuffer at physical 0xA0000 with a sane default 256-entry palette.
 * desktop_vga13_render() in gui/desktop_vesa.c uses the blit + clear
 * primitives to paint a minimal fallback surface; the VGA-passthrough
 * shim composites its 80x25 cells via vga_graphics_draw_char.
 *
 * vga_graphics_init() returns 1 on success, 0 if the VGA controller
 * could not be programmed (modern UEFI GPUs in pure GOP land sometimes
 * reject the register sequence -- the driver framework cleanly drops to
 * the VGA-text floor in that case).
 *
 * vga_graphics_present() is a no-op today (0xA0000 is the visible
 * buffer in mode-13h) but is exposed so a future double-buffered
 * implementation can be wired up without touching call sites.
 */
int  vga_graphics_init(void);
void vga_graphics_clear(uint8_t color);
void vga_graphics_set_pixel(int x, int y, uint8_t color);
void vga_graphics_blit_8bpp(int x, int y, int w, int h, const uint8_t* src);
void vga_graphics_draw_char(int x, int y, char c, uint8_t fg, uint8_t bg);
void vga_graphics_draw_string(int x, int y, const char* s, uint8_t fg, uint8_t bg);
void vga_graphics_fill_rect(int x, int y, int w, int h, uint8_t color);
void vga_graphics_present(void);
int  vga_graphics_active(void);

/* Default palette indices for the basic 16-colour subset of mode-13h. The
 * first 16 entries of the 256-entry palette are programmed to match the
 * classic VGA CGA-style palette so existing 4-bit attribute values (used
 * by the VGA-passthrough shim) translate 1:1. */
#define VGA13_COLOR_BLACK         0x00
#define VGA13_COLOR_BLUE          0x01
#define VGA13_COLOR_GREEN         0x02
#define VGA13_COLOR_CYAN          0x03
#define VGA13_COLOR_RED           0x04
#define VGA13_COLOR_MAGENTA       0x05
#define VGA13_COLOR_BROWN         0x06
#define VGA13_COLOR_LIGHT_GREY    0x07
#define VGA13_COLOR_DARK_GREY     0x08
#define VGA13_COLOR_LIGHT_BLUE    0x09
#define VGA13_COLOR_LIGHT_GREEN   0x0A
#define VGA13_COLOR_LIGHT_CYAN    0x0B
#define VGA13_COLOR_LIGHT_RED     0x0C
#define VGA13_COLOR_LIGHT_MAGENTA 0x0D
#define VGA13_COLOR_LIGHT_BROWN   0x0E
#define VGA13_COLOR_WHITE         0x0F

/* Display-framework registration helper. Adds the "vga-graphics" driver
 * to the registry at its priority slot (between native_fb's rungs and
 * the VGA-text floor). Idempotent. */
void vga_graphics_register_driver(void);

#endif
