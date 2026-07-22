#include "vga_passthrough.h"
#include "vesa_window.h"
#include "../drivers/video/vesa.h"
#include "../drivers/video/font.h"
#include "../drivers/video/textcon.h"
#include "../lib/string.h"

/*
 * Phase 4 (item 5): VGA-text application passthrough shim. See the
 * header for the design summary; this file is the implementation.
 *
 * Storage layout:
 *   - g_cells[80*25]   one byte per character cell
 *   - g_attrs[80*25]   matching attribute byte (4-bit fg | 4-bit bg << 4)
 *
 * The CGA-style palette below maps the 4-bit colour indices to the
 * 32-bit XRGB8888 values the VESA primitives expect.
 */

static char         g_cells[VGA_PT_CELL_BYTES];
static unsigned char g_attrs[VGA_PT_CELL_BYTES];
static int          g_armed = 0;

/* Forward declaration of the hook installer in drivers/video/vga.c. We
 * intentionally do NOT include vga.h here (it would pull in VGA_WIDTH
 * macros that would collide with the shim's own VGA_PT_COLS); using
 * the forward decl matches what vga.c::vga_text_passthrough_install
 * declares with extern linkage. */
typedef void (*vga_passthrough_writechar_fn)(int x, int y, char c, unsigned char attr);
extern void vga_text_passthrough_install(vga_passthrough_writechar_fn fn);

/* 16-entry CGA-style palette, used to expand 4-bit attribute fg/bg
 * indices to the 32-bit colours the VESA primitives consume. Matches
 * the same colour table the mode-13h fallback uses (vga.c). */
static const uint32_t cga32[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

void vga_passthrough_clear(void) {
    for (int i = 0; i < VGA_PT_CELL_BYTES; i++) {
        g_cells[i] = ' ';
        g_attrs[i] = 0x07; /* light grey on black */
    }
}

static void redirect_writechar(int x, int y, char c, unsigned char attr) {
    vga_passthrough_writechar(x, y, c, attr);
}

void vga_passthrough_arm(void) {
    if (g_armed) return;
    g_armed = 1;
    vga_text_passthrough_install(redirect_writechar);
}

void vga_passthrough_disarm(void) {
    if (!g_armed) return;
    g_armed = 0;
    vga_text_passthrough_install(0);
}

int vga_passthrough_active(void) { return g_armed; }

void vga_passthrough_writechar(int x, int y, char c, unsigned char attr) {
    if (x < 0 || y < 0 || x >= VGA_PT_COLS || y >= VGA_PT_ROWS) return;
    int idx = y * VGA_PT_COLS + x;
    g_cells[idx] = c;
    g_attrs[idx] = attr;
}

void vga_passthrough_readcell(int x, int y, char* c, unsigned char* attr) {
    if (x < 0 || y < 0 || x >= VGA_PT_COLS || y >= VGA_PT_ROWS) {
        if (c) *c = ' ';
        if (attr) *attr = 0x07;
        return;
    }
    int idx = y * VGA_PT_COLS + x;
    if (c) *c = g_cells[idx];
    if (attr) *attr = g_attrs[idx];
}

/*
 * Composite the 80x25 cell grid into a VESA window's client area via the
 * 8x16 font. Each cell is FONT_WIDTH x FONT_HEIGHT pixels (8x16 = 640x400
 * for the full 80x25), so a typical 480x320 console window shows a clipped
 * portion (60x20 cells). The clip is at cell granularity so we never
 * stamp a half-glyph onto the panel.
 */
void vga_passthrough_present_into_window(int cx, int cy, int cw, int ch) {
    int max_cols = cw / FONT_WIDTH;
    int max_rows = ch / FONT_HEIGHT;
    if (max_cols > VGA_PT_COLS) max_cols = VGA_PT_COLS;
    if (max_rows > VGA_PT_ROWS) max_rows = VGA_PT_ROWS;

    for (int row = 0; row < max_rows; row++) {
        for (int col = 0; col < max_cols; col++) {
            int idx = row * VGA_PT_COLS + col;
            char c = g_cells[idx];
            unsigned char attr = g_attrs[idx];
            uint32_t fg = cga32[attr & 0x0F];
            uint32_t bg = cga32[(attr >> 4) & 0x0F];
            vesa_draw_char(cx + col * FONT_WIDTH,
                           cy + row * FONT_HEIGHT,
                           c, fg, bg);
        }
    }
}

void vga_passthrough_sync_from_textcon(void) {
    int row, col;
    if (!con_ready()) return;
    for (row = 0; row < VGA_PT_ROWS && row < CON_ROWS; row++) {
        for (col = 0; col < VGA_PT_COLS && col < CON_COLS; col++) {
            uint16_t cell = con_get_cell(row, col);
            g_cells[row * VGA_PT_COLS + col] = (char)(cell & 0xFF);
            g_attrs[row * VGA_PT_COLS + col] = (unsigned char)(cell >> 8);
        }
    }
}

void vga_passthrough_run_body(vga_passthrough_body_fn body, void* user) {
    if (!body) return;
    int was_armed = g_armed;
    if (!was_armed) vga_passthrough_arm();
    body(user);
    if (!was_armed) vga_passthrough_disarm();
}
