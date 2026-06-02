#include "vga.h"
#include "display.h"
#include "font.h"
#include "../io/io.h"
#include <stdint.h>
#include <stdbool.h>

/* Forward-declared hook into gui/vga_passthrough.c. The shim toggles this
 * (1=active, 0=inactive) and provides a redirect target for legacy 0xB8000
 * writes. The redirect is kept as a callback so vga.c does not need to
 * include the passthrough header (which depends on gui/vesa_window.h). */
typedef void (*vga_passthrough_writechar_fn)(int x, int y, char c, unsigned char attr);
static vga_passthrough_writechar_fn vga_passthrough_hook = 0;

void vga_text_passthrough_install(vga_passthrough_writechar_fn fn);

void vga_text_passthrough_install(vga_passthrough_writechar_fn fn) {
    vga_passthrough_hook = fn;
}

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

void clear_screen() {
    for (uint8_t y = 0; y < 25; y++) {
        for (uint8_t x = 0; x < 80; x++) {
            vga_put_char_at(' ', x, y, 0x0F);
        }
    }
    vga_set_cursor(0, 0);
}

/* ---- Phase 4: vga_text_putc indirection ---------------------------------- */
/*
 * Single hook point for code that historically wrote directly to the 80x25
 * text plane at 0xB8000 (editor.c and games). When the VGA-passthrough
 * shim has registered a redirect (gui/vga_passthrough.c::
 * vga_passthrough_arm()), writes are rerouted into the shim's 80x25 virtual
 * cell grid so a VESA-window class renderer can composite them through the
 * 8x16 font onto the panel pixel grid. When the shim is inactive, writes go
 * to the legacy 0xB8000 plane verbatim (preserving x86 BIOS boot behavior).
 *
 * Bounds-checked so a wild caller can't poke MMIO outside the text region.
 */
void vga_text_putc(char c, int x, int y, unsigned char attr) {
    if (x < 0 || y < 0 || x >= VGA_WIDTH || y >= VGA_HEIGHT) return;
    if (vga_passthrough_hook) {
        vga_passthrough_hook(x, y, c, attr);
        return;
    }
    vga_put_char_at(c, x, y, attr);
}

/* ---- Phase 4: VGA mode-13h graphical surface ----------------------------- *
 *
 * Direct register-file programming of the VGA controller to enter mode 13h
 * (320x200x8 indexed colour, linear at physical 0xA0000). The register dump
 * below is the canonical mode-13h table; it touches only the VGA-compatible
 * block (no PLL / panel-power) so it's safe to call from any graphical
 * state. Modern UEFI GPUs in pure GOP land sometimes silently reject the
 * register sequence -- vga_graphics_init() handles that by failing soft so
 * the framework can fall through to the VGA-text floor.
 *
 * The 0xA0000 LFB is the visible buffer in mode-13h, so vga_graphics_
 * present() is a no-op today; it is exposed so a future double-buffered
 * implementation can be dropped in without touching call sites.
 */

#define VGA_AC_INDEX     0x3C0
#define VGA_AC_WRITE     0x3C0
#define VGA_MISC_WRITE   0x3C2
#define VGA_SEQ_INDEX    0x3C4
#define VGA_SEQ_DATA     0x3C5
#define VGA_DAC_WRITE    0x3C8
#define VGA_DAC_DATA     0x3C9
#define VGA_GC_INDEX     0x3CE
#define VGA_GC_DATA      0x3CF
#define VGA_CRTC_INDEX   0x3D4
#define VGA_CRTC_DATA    0x3D5
#define VGA_INSTAT_READ  0x3DA

#define VGA13_NUM_SEQ_REGS  5
#define VGA13_NUM_CRTC_REGS 25
#define VGA13_NUM_GC_REGS   9
#define VGA13_NUM_AC_REGS   21

#define VGA13_FB ((volatile uint8_t*)0xA0000)
#define VGA13_W  320
#define VGA13_H  200

static const unsigned char vga_320x200x256[] = {
    /* MISC */
    0x63,
    /* SEQ */
    0x03, 0x01, 0x0F, 0x00, 0x0E,
    /* CRTC */
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
    0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3,
    0xFF,
    /* GC */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF,
    /* AC */
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41, 0x00, 0x0F, 0x00, 0x00
};

static int vga_graphics_initialized = 0;

/* Classic CGA-style palette for the first 16 indices; values are 6-bit
 * (0..63). The remaining 240 entries are filled with a coarse 6-3-3 RGB
 * ramp so palette-coloured icons land on reasonable shades without us
 * having to invent a real palette. */
static void vga13_program_default_palette(void) {
    static const uint8_t cga[16][3] = {
        {  0,  0,  0 }, {  0,  0, 42 }, {  0, 42,  0 }, {  0, 42, 42 },
        { 42,  0,  0 }, { 42,  0, 42 }, { 42, 21,  0 }, { 42, 42, 42 },
        { 21, 21, 21 }, { 21, 21, 63 }, { 21, 63, 21 }, { 21, 63, 63 },
        { 63, 21, 21 }, { 63, 21, 63 }, { 63, 63, 21 }, { 63, 63, 63 }
    };
    outb(VGA_DAC_WRITE, 0);
    for (int i = 0; i < 16; i++) {
        outb(VGA_DAC_DATA, cga[i][0]);
        outb(VGA_DAC_DATA, cga[i][1]);
        outb(VGA_DAC_DATA, cga[i][2]);
    }
    /* 6-3-3 ramp for the remaining 240 entries: index n in [16,256) maps to
     * (r=2 bits, g=3 bits, b=3 bits) approximating an 8x8x4 cube. */
    for (int n = 16; n < 256; n++) {
        int idx = n - 16;
        int r = (idx >> 6) & 0x3;   /* 2 bits */
        int g = (idx >> 3) & 0x7;   /* 3 bits */
        int b =  idx       & 0x7;   /* 3 bits */
        outb(VGA_DAC_DATA, (uint8_t)((r * 63) / 3));
        outb(VGA_DAC_DATA, (uint8_t)((g * 63) / 7));
        outb(VGA_DAC_DATA, (uint8_t)((b * 63) / 7));
    }
}

static void vga13_write_regs(const unsigned char* regs) {
    unsigned char crtc[VGA13_NUM_CRTC_REGS];
    unsigned i;

    outb(VGA_MISC_WRITE, *regs++);
    for (i = 0; i < VGA13_NUM_SEQ_REGS; i++) {
        outb(VGA_SEQ_INDEX, (uint8_t)i);
        outb(VGA_SEQ_DATA, *regs++);
    }
    for (i = 0; i < VGA13_NUM_CRTC_REGS; i++) crtc[i] = regs[i];
    crtc[0x11] &= (unsigned char)~0x80;     /* unlock CRTC[0..7]               */
    outb(VGA_CRTC_INDEX, 0x11);
    outb(VGA_CRTC_DATA, (uint8_t)(inb(VGA_CRTC_DATA) & ~0x80));
    for (i = 0; i < VGA13_NUM_CRTC_REGS; i++) {
        outb(VGA_CRTC_INDEX, (uint8_t)i);
        outb(VGA_CRTC_DATA, crtc[i]);
    }
    regs += VGA13_NUM_CRTC_REGS;
    for (i = 0; i < VGA13_NUM_GC_REGS; i++) {
        outb(VGA_GC_INDEX, (uint8_t)i);
        outb(VGA_GC_DATA, *regs++);
    }
    for (i = 0; i < VGA13_NUM_AC_REGS; i++) {
        (void)inb(VGA_INSTAT_READ);
        outb(VGA_AC_INDEX, (uint8_t)i);
        outb(VGA_AC_WRITE, *regs++);
    }
    /* Lock palette + unblank. */
    (void)inb(VGA_INSTAT_READ);
    outb(VGA_AC_INDEX, 0x20);
}

/* Verify that a couple of scattered writes to 0xA0000 round-trip. Modern
 * UEFI GPUs may decode 0xA0000 as a VGA-compatibility window only when
 * the firmware enabled the legacy aperture; on a strict GOP-only platform
 * the writes go to nowhere. We treat read-back mismatch as "mode-13h
 * unavailable" and let the framework fall to the text floor. */
static int vga13_writable(void) {
    volatile uint8_t* fb = VGA13_FB;
    uint8_t saved0 = fb[0];
    uint8_t saved1 = fb[VGA13_W * VGA13_H / 2];
    fb[0] = 0xA5;
    fb[VGA13_W * VGA13_H / 2] = 0x5A;
    int ok = (fb[0] == 0xA5) && (fb[VGA13_W * VGA13_H / 2] == 0x5A);
    fb[0] = saved0;
    fb[VGA13_W * VGA13_H / 2] = saved1;
    return ok;
}

int vga_graphics_init(void) {
    __asm__ volatile("cli");
    vga13_write_regs(vga_320x200x256);
    vga13_program_default_palette();
    __asm__ volatile("sti");
    if (!vga13_writable()) {
        vga_graphics_initialized = 0;
        return 0;
    }
    vga_graphics_initialized = 1;
    /* Clear to the canonical "fallback teal" so a healthy mode-13h
     * commit is visually distinct from a blank panel. */
    vga_graphics_clear(VGA13_COLOR_BLUE);
    return 1;
}

int vga_graphics_active(void) { return vga_graphics_initialized; }

void vga_graphics_clear(uint8_t color) {
    if (!vga_graphics_initialized) return;
    volatile uint8_t* fb = VGA13_FB;
    int n = VGA13_W * VGA13_H;
    for (int i = 0; i < n; i++) fb[i] = color;
}

void vga_graphics_set_pixel(int x, int y, uint8_t color) {
    if (!vga_graphics_initialized) return;
    if (x < 0 || y < 0 || x >= VGA13_W || y >= VGA13_H) return;
    VGA13_FB[y * VGA13_W + x] = color;
}

void vga_graphics_blit_8bpp(int x, int y, int w, int h, const uint8_t* src) {
    if (!vga_graphics_initialized || !src) return;
    int x2 = x + w, y2 = y + h;
    if (x < 0) { src += -x; w += x; x = 0; }
    if (y < 0) { src += (-y) * w; h += y; y = 0; }
    if (x2 > VGA13_W) w -= (x2 - VGA13_W);
    if (y2 > VGA13_H) h -= (y2 - VGA13_H);
    if (w <= 0 || h <= 0) return;
    for (int yy = 0; yy < h; yy++) {
        volatile uint8_t* dst = VGA13_FB + (y + yy) * VGA13_W + x;
        for (int xx = 0; xx < w; xx++) dst[xx] = src[yy * w + xx];
    }
}

void vga_graphics_fill_rect(int x, int y, int w, int h, uint8_t color) {
    if (!vga_graphics_initialized) return;
    int x2 = x + w, y2 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > VGA13_W) x2 = VGA13_W;
    if (y2 > VGA13_H) y2 = VGA13_H;
    for (int yy = y; yy < y2; yy++) {
        volatile uint8_t* dst = VGA13_FB + yy * VGA13_W + x;
        for (int xx = 0; xx < (x2 - x); xx++) dst[xx] = color;
    }
}

void vga_graphics_draw_char(int x, int y, char c, uint8_t fg, uint8_t bg) {
    if (!vga_graphics_initialized) return;
    if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) c = ' ';
    for (int gy = 0; gy < FONT_HEIGHT; gy++) {
        uint8_t row = font8x16[(unsigned char)c][gy];
        for (int gx = 0; gx < FONT_WIDTH; gx++) {
            uint8_t col = (row & (0x80 >> gx)) ? fg : bg;
            vga_graphics_set_pixel(x + gx, y + gy, col);
        }
    }
}

void vga_graphics_draw_string(int x, int y, const char* s, uint8_t fg, uint8_t bg) {
    if (!s) return;
    int cx = x;
    int cy = y;
    while (*s) {
        if (*s == '\n') { cx = x; cy += FONT_HEIGHT; s++; continue; }
        vga_graphics_draw_char(cx, cy, *s, fg, bg);
        cx += FONT_WIDTH;
        if (cx + FONT_WIDTH > VGA13_W) { cx = x; cy += FONT_HEIGHT; }
        s++;
    }
}

void vga_graphics_present(void) {
    /* No-op: 0xA0000 is the visible buffer in mode-13h. Reserved so a
     * future double-buffered implementation can hook here without
     * touching call sites. */
}

/* ---- "vga-graphics" driver in the display framework ladder --------------- */

static int vga_graphics_probe(void) {
    /*
     * We do not run vga_graphics_init() at probe time -- entering mode-13h
     * is a hard state change. Approve the rung optimistically and let
     * init() do the real work; the framework's confirm hook will reject
     * the rung if the framebuffer fails the read-back test.
     *
     * (Note: vga_graphics_init() above ALSO does its own writable check
     * before reporting success, so a panel that decodes 0xA0000 to
     * nowhere fails cleanly here as well.)
     */
    return 1;
}

static int vga_graphics_drv_init(uint32_t req_w, uint32_t req_h, uint8_t req_bpp,
                                 display_framebuffer_t* out) {
    (void)req_w; (void)req_h; (void)req_bpp;
    if (!vga_graphics_init()) {
        display_set_error("vga-graphics: mode-13h register programming "
                          "rejected by hardware/firmware.\n");
        return 0;
    }
    out->framebuffer_addr = (uintptr_t)0xA0000u;
    out->width = VGA13_W;
    out->height = VGA13_H;
    out->pitch = VGA13_W;
    out->bpp = 8;
    out->format = DISPLAY_FORMAT_UNKNOWN;     /* indexed; not direct-RGB    */
    return 1;
}

static const display_driver_ops_t vga_graphics_ops = {
    "vga-graphics",
    DISPLAY_DRIVER_VGA_GRAPHICS,
    vga_graphics_probe,
    vga_graphics_drv_init
};

void vga_graphics_register_driver(void) {
    display_register_driver(&vga_graphics_ops);
}
