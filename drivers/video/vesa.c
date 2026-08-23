#include "vesa.h"
#include "font.h"
#include "display.h"
#include "../../lib/string.h"
#include "../../gooberos_arch.h"

static volatile uint8_t* framebuffer = NULL;
static uint8_t* backbuffer = NULL;
static uint32_t fb_width = 0;
static uint32_t fb_height = 0;
static uint32_t fb_pitch = 0;
static uint32_t fb_addr32 = 0;
static uint32_t backbuffer_bytes = 0;
static uint8_t  fb_bpp = 0;
static int vesa_initialized = 0;
static int use_backbuffer = 0;
static int clip_enabled = 0;
static int clip_x1 = 0, clip_y1 = 0, clip_x2 = 0, clip_y2 = 0;

static int vesa_clip_rect(int* x, int* y, int* w, int* h) {
    int x2 = *x + *w;
    int y2 = *y + *h;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if ((uint32_t)x2 > fb_width) x2 = (int)fb_width;
    if ((uint32_t)y2 > fb_height) y2 = (int)fb_height;
    if (clip_enabled) {
        if (*x < clip_x1) *x = clip_x1;
        if (*y < clip_y1) *y = clip_y1;
        if (x2 > clip_x2) x2 = clip_x2;
        if (y2 > clip_y2) y2 = clip_y2;
    }
    *w = x2 - *x;
    *h = y2 - *y;
    return *w > 0 && *h > 0;
}

static int vesa_point_visible(int x, int y) {
    if (x < 0 || y < 0 || (uint32_t)x >= fb_width || (uint32_t)y >= fb_height)
        return 0;
    if (!clip_enabled) return 1;
    return x >= clip_x1 && x < clip_x2 && y >= clip_y1 && y < clip_y2;
}

static inline uint32_t rgb888_to_rgb565(uint32_t color) {
    uint32_t r = (color >> 16) & 0xFF;
    uint32_t g = (color >> 8) & 0xFF;
    uint32_t b = color & 0xFF;
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static inline void vesa_write_pixel_raw(uint8_t* base, int x, int y, uint32_t color) {
    if (!base || x < 0 || y < 0 || (uint32_t)x >= fb_width || (uint32_t)y >= fb_height)
        return;
    uint8_t* row = base + (uint32_t)y * fb_pitch;
    if (fb_bpp == 32) {
        ((uint32_t*)row)[x] = color;
    } else if (fb_bpp == 24) {
        uint8_t* p = row + (uint32_t)x * 3;
        /* VBE direct-color 24 bpp is almost always byte-order B,G,R. */
        p[0] = (uint8_t)(color & 0xFF);
        p[1] = (uint8_t)((color >> 8) & 0xFF);
        p[2] = (uint8_t)((color >> 16) & 0xFF);
    } else if (fb_bpp == 16 || fb_bpp == 15) {
        ((uint16_t*)row)[x] = (uint16_t)rgb888_to_rgb565(color);
    }
}

/* Pointer to the active draw target: backbuffer when armed, else the LFB
 * directly. Phase 4 generalizes this past the 32-bpp-only restriction the
 * earlier static backbuffer code carried, so 15/16/24 bpp framebuffers
 * also get tear-free presents. */
static inline uint8_t* vesa_draw_target(void) {
    return use_backbuffer ? backbuffer : (uint8_t*)framebuffer;
}

void vesa_init(uint64_t fb_addr, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp) {
    /* Phase 3b audit: keep the full address as `uintptr_t` so the framework's
     * descriptor (display_register_framebuffer) carries the unmodified base on
     * x86_64. `fb_addr32` is retained for the splash hex dump (which always
     * prints 8 hex chars regardless of arch); the implicit low-32 truncation
     * there is diagnostic-only. */
    framebuffer = (volatile uint8_t*)(uintptr_t)fb_addr;
    fb_addr32 = (uint32_t)fb_addr;
    fb_width = width;
    fb_height = height;
    fb_pitch = pitch;
    fb_bpp = bpp;
    backbuffer = NULL;
    backbuffer_bytes = 0;
    use_backbuffer = 0;
    clip_enabled = 0;
    vesa_initialized = 1;
    display_pixel_format_t fmt = DISPLAY_FORMAT_UNKNOWN;
    if (bpp == 32) fmt = DISPLAY_FORMAT_XRGB8888;
    else if (bpp == 24) fmt = DISPLAY_FORMAT_BGR888;
    else if (bpp == 16 || bpp == 15) fmt = DISPLAY_FORMAT_RGB565;
    display_register_framebuffer(DISPLAY_DRIVER_VESA_LFB, fmt,
                                 (uintptr_t)fb_addr,
                                 width, height, pitch, bpp);
}

void vesa_set_backbuffer(uint32_t* buffer) {
    vesa_set_backbuffer_bytes(buffer, fb_pitch * fb_height);
}

void vesa_set_backbuffer_bytes(uint32_t* buffer, uint32_t bytes) {
    backbuffer = (uint8_t*)buffer;
    backbuffer_bytes = buffer ? bytes : 0;
    /*
     * Phase 4 (display polish, item 2): the back-buffer carries one row of
     * `fb_pitch` bytes per scanline, matching the LFB layout EXACTLY. That
     * makes the present step a literal memcpy(LFB, BB, pitch * height)
     * regardless of bpp -- 15/16/24/32 bpp panels all benefit from tear-free
     * whole-screen compositing. The earlier 32bpp-only restriction is
     * dropped; the only requirement is that the supplied buffer is at
     * least pitch x height bytes.
     */
    use_backbuffer = (buffer != NULL && bytes >= fb_pitch * fb_height);
}

/*
 * Whole-screen present. Copies the back-buffer to the LFB in one pass.
 * memcpy() is the fastest path on modest hardware (single rep-mov on x86;
 * the toolchain may select an unrolled version for x64) and avoids the
 * tearing visible on the earlier dirty-rect path when multiple subsystems
 * (window drag + cursor + taskbar) touched overlapping regions in a frame.
 *
 * No-op when no back-buffer is armed (kmalloc failed at desktop init): all
 * drawing went straight to the LFB so the panel is already up to date.
 */
void vesa_swap(void) {
    if (!use_backbuffer || !framebuffer || !backbuffer) return;
    memcpy((void*)framebuffer, backbuffer, fb_pitch * fb_height);
    /* Flush WC stores so the panel sees this frame (PAT WC / ioremap_wc). */
    __asm__ volatile("sfence" ::: "memory");
}

/*
 * Phase 4: tear-free whole-screen present. Identical to vesa_swap() today
 * but exposed as a separate name so the per-frame call site reads as
 * "present this frame" rather than "swap buffers"; future implementations
 * (vsync wait, async dma blit) can hook here without touching call sites.
 */
void vesa_present(void) {
    vesa_swap();
}

void vesa_swap_rect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    (void)vesa_swap_rect_rows(x, y, w, h, h);
}

int vesa_swap_rect_rows(int x, int y, int w, int h, int max_rows) {
    if (!use_backbuffer || !framebuffer || !backbuffer) return 0;
    if (w <= 0 || h <= 0 || max_rows <= 0) return 0;

    int x2 = x + w;
    int y2 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if ((uint32_t)x2 > fb_width) x2 = fb_width;
    if ((uint32_t)y2 > fb_height) y2 = fb_height;
    if (x >= x2 || y >= y2) return 0;

    uint32_t bytes_per_px;
    if (fb_bpp == 32) bytes_per_px = 4;
    else if (fb_bpp == 24) bytes_per_px = 3;
    else if (fb_bpp == 16 || fb_bpp == 15) bytes_per_px = 2;
    else return 0;

    uint32_t bytes = (uint32_t)(x2 - x) * bytes_per_px;
    int rows_left = y2 - y;
    if (rows_left > max_rows) rows_left = max_rows;

    for (int i = 0; i < rows_left; i++) {
        int row = y + i;
        uint8_t* dst = (uint8_t*)framebuffer + row * fb_pitch + x * bytes_per_px;
        uint8_t* src = backbuffer + row * fb_pitch + x * bytes_per_px;
        memcpy(dst, src, bytes);
    }
    __asm__ volatile("sfence" ::: "memory");
    return rows_left;
}

uint32_t vesa_get_width(void) { return fb_width; }
uint32_t vesa_get_height(void) { return fb_height; }
uint32_t vesa_get_pitch(void) { return fb_pitch; }
uint32_t vesa_get_framebuffer_addr(void) { return fb_addr32; }
uint32_t vesa_get_backbuffer_bytes(void) { return backbuffer_bytes; }
uint8_t vesa_get_bpp(void) { return fb_bpp; }
int vesa_has_backbuffer(void) { return use_backbuffer; }
int vesa_is_initialized(void) { return vesa_initialized; }

void vesa_put_pixel(int x, int y, uint32_t color) {
    if (!vesa_point_visible(x, y)) return;
    vesa_write_pixel_raw(vesa_draw_target(), x, y, color);
}

void vesa_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!vesa_clip_rect(&x, &y, &w, &h)) return;

    uint8_t* base = vesa_draw_target();
    if (!base) return;

    if (fb_bpp == 32) {
        for (int row = y; row < y + h; row++) {
            uint32_t* ptr = (uint32_t*)(base + row * fb_pitch) + x;
            for (int col = 0; col < w; col++) ptr[col] = color;
        }
    } else {
        for (int row = y; row < y + h; row++) {
            for (int col = 0; col < w; col++) {
                vesa_write_pixel_raw(base, x + col, row, color);
            }
        }
    }
}

void vesa_set_clip(int x, int y, int w, int h) {
    clip_enabled = 0;
    if (!vesa_clip_rect(&x, &y, &w, &h)) {
        clip_enabled = 1;
        clip_x1 = clip_y1 = clip_x2 = clip_y2 = 0;
        return;
    }
    clip_enabled = 1;
    clip_x1 = x;
    clip_y1 = y;
    clip_x2 = x + w;
    clip_y2 = y + h;
}

void vesa_clear_clip(void) {
    clip_enabled = 0;
}

void vesa_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg) {
    if (!framebuffer || (unsigned char)c < 0x20 || (unsigned char)c > 0x7E)
        return;

    for (int glyph_y = 0; glyph_y < FONT_HEIGHT; glyph_y++) {
        uint8_t line = font8x16[(unsigned char)c][glyph_y];
        for (int glyph_x = 0; glyph_x < FONT_WIDTH; glyph_x++) {
            int px = x + glyph_x;
            int py = y + glyph_y;
            vesa_put_pixel(px, py, (line & (0x80 >> glyph_x)) ? fg : bg);
        }
    }
}

void vesa_draw_string(int x, int y, const char* str, uint32_t fg, uint32_t bg) {
    int cur_x = x;
    int cur_y = y;
    if (!str) return;
    for (int i = 0; str[i] != '\0'; i++) {
        if (str[i] == '\n') {
            cur_x = x;
            cur_y += FONT_HEIGHT;
            continue;
        }
        vesa_draw_char(cur_x, cur_y, str[i], fg, bg);
        cur_x += FONT_WIDTH;
        if ((uint32_t)(cur_x + FONT_WIDTH) > fb_width) {
            cur_x = x;
            cur_y += FONT_HEIGHT;
        }
    }
}

void vesa_clear(uint32_t color) {
    vesa_fill_rect(0, 0, fb_width, fb_height, color);
}

void vesa_update(void) {
    /* No-op; use vesa_swap() for double buffering */
}

static void splash_u32(char* dst, uint32_t v) {
    /* Write v as decimal into dst, NUL-terminated; assumes dst has 12 bytes. */
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = '0' + (v % 10); v /= 10; }
    int i = 0;
    while (n) dst[i++] = tmp[--n];
    dst[i] = '\0';
}

static void splash_hex32(char* dst, uint32_t v) {
    /* Write v as 0xHHHHHHHH into dst, assumes 11 bytes available. */
    const char* h = "0123456789ABCDEF";
    dst[0] = '0'; dst[1] = 'x';
    for (int i = 0; i < 8; i++) dst[2 + i] = h[(v >> ((7 - i) * 4)) & 0xF];
    dst[10] = '\0';
}

void vesa_boot_splash(const char* status) {
    if (!vesa_initialized) return;

    static uint32_t splash_step = 0;
    const uint32_t bg       = 0x050608;
    const uint32_t panel    = 0x111827;
    const uint32_t border   = 0x263244;
    const uint32_t accent   = 0x4C8BF5;
    const uint32_t accent2  = 0x8AB4F8;
    const uint32_t text     = 0xE8EAED;
    const uint32_t muted    = 0x9AA0A6;
    int w = (int)fb_width;
    int h = (int)fb_height;
    int card_w = w > 560 ? 520 : w - 40;
    int card_h = 188;
    int card_x = (w - card_w) / 2;
    int card_y = (h - card_h) / 2;
    int logo_x = card_x + 28;
    int logo_y = card_y + 30;
    int bar_x = card_x + 28;
    int bar_y = card_y + card_h - 54;
    int bar_w = card_w - 56;
    int fill_w;

    if (card_w < 260) card_w = w - 16;
    if (card_w < 180) card_w = 180;
    card_x = (w - card_w) / 2;
    logo_x = card_x + 28;
    bar_x = card_x + 28;
    bar_w = card_w - 56;

    if (splash_step < 8) splash_step++;
    fill_w = (bar_w * (int)(splash_step + 2)) / 10;
    if (fill_w > bar_w) fill_w = bar_w;

    vesa_clear(bg);
    vesa_fill_rect(card_x, card_y, card_w, card_h, panel);
    vesa_fill_rect(card_x, card_y, card_w, 2, border);
    vesa_fill_rect(card_x, card_y + card_h - 2, card_w, 2, border);
    vesa_fill_rect(card_x, card_y, 2, card_h, border);
    vesa_fill_rect(card_x + card_w - 2, card_y, 2, card_h, border);

    vesa_fill_rect(logo_x, logo_y, 44, 44, accent);
    vesa_fill_rect(logo_x + 8, logo_y + 8, 28, 28, panel);
    vesa_fill_rect(logo_x + 16, logo_y + 16, 12, 12, accent2);

    vesa_draw_string(logo_x + 62, logo_y + 2, GOOBEROS_MENU_LABEL, text, panel);
    vesa_draw_string(logo_x + 62, logo_y + 24, "Starting graphical desktop", muted, panel);
    vesa_draw_string(bar_x, bar_y - 28, status ? status : "Loading...", text, panel);

    vesa_fill_rect(bar_x, bar_y, bar_w, 14, bg);
    vesa_fill_rect(bar_x, bar_y, fill_w, 14, accent);
    vesa_fill_rect(bar_x, bar_y, bar_w, 1, border);
    vesa_fill_rect(bar_x, bar_y + 13, bar_w, 1, border);

    vesa_swap();
}
