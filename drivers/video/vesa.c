#include "vesa.h"
#include "font.h"
#include "display.h"
#include "../../lib/string.h"

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
    /* The pitch already includes any per-row padding the firmware chose, so
     * copying `pitch * height` bytes is the canonical full-screen swap. */
    memcpy((void*)framebuffer, backbuffer, fb_pitch * fb_height);
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
    /*
     * Phase 4: kept as a thin compatibility shim. The desktop's per-frame
     * present is now whole-screen (vesa_present), but the existing dirty-
     * rect machinery in gui/vesa_window.c still calls vesa_swap_rect for
     * partial composites. Route it to a partial copy when a back-buffer
     * is armed, no-op otherwise.
     */
    if (!use_backbuffer || !framebuffer || !backbuffer) return;
    if (w <= 0 || h <= 0) return;

    int x2 = x + w;
    int y2 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if ((uint32_t)x2 > fb_width) x2 = fb_width;
    if ((uint32_t)y2 > fb_height) y2 = fb_height;
    if (x >= x2 || y >= y2) return;

    uint32_t bytes_per_px;
    if (fb_bpp == 32) bytes_per_px = 4;
    else if (fb_bpp == 24) bytes_per_px = 3;
    else if (fb_bpp == 16 || fb_bpp == 15) bytes_per_px = 2;
    else return;

    uint32_t bytes = (uint32_t)(x2 - x) * bytes_per_px;
    for (int row = y; row < y2; row++) {
        uint8_t* dst = (uint8_t*)framebuffer + row * fb_pitch + x * bytes_per_px;
        uint8_t* src = backbuffer + row * fb_pitch + x * bytes_per_px;
        memcpy(dst, src, bytes);
    }
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
    vesa_write_pixel_raw(vesa_draw_target(), x, y, color);
}

void vesa_fill_rect(int x, int y, int w, int h, uint32_t color) {
    int x2 = x + w;
    int y2 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if ((uint32_t)x2 > fb_width) x2 = fb_width;
    if ((uint32_t)y2 > fb_height) y2 = fb_height;

    int width = x2 - x;
    if (width <= 0 || y >= y2) return;

    uint8_t* base = vesa_draw_target();
    if (!base) return;

    if (fb_bpp == 32) {
        /* Fast path for the common 32-bpp case: write 4 bytes at a time. */
        for (int row = y; row < y2; row++) {
            uint32_t* ptr = (uint32_t*)(base + row * fb_pitch) + x;
            for (int col = 0; col < width; col++) ptr[col] = color;
        }
    } else {
        for (int row = y; row < y2; row++) {
            for (int col = 0; col < width; col++) {
                vesa_write_pixel_raw(base, x + col, row, color);
            }
        }
    }
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

/*
 * High-contrast boot splash designed to survive marginal LCD sync on real
 * hardware. We deliberately use pure white background + black text + a
 * primary-color test pattern so the user can immediately see whether the
 * panel is locking on to the framebuffer at all. Framebuffer dimensions are
 * printed so we know what mode GRUB actually negotiated.
 */
void vesa_boot_splash(const char* status) {
    if (!vesa_initialized) return;

    const uint32_t white = 0xFFFFFFFF;
    const uint32_t black = 0x00000000;
    const uint32_t red   = 0x00FF0000;
    const uint32_t green = 0x0000C800;
    const uint32_t blue  = 0x002060FF;

    vesa_clear(white);

    /* Outer black border so panel edges are obvious. */
    vesa_fill_rect(0, 0, (int)fb_width, 4, black);
    vesa_fill_rect(0, (int)fb_height - 4, (int)fb_width, 4, black);
    vesa_fill_rect(0, 0, 4, (int)fb_height, black);
    vesa_fill_rect((int)fb_width - 4, 0, 4, (int)fb_height, black);

    /* Color sync bars across the top so we can confirm RGB ordering and the
     * panel is locked to the framebuffer. If colors are swapped we know the
     * driver mis-detected the pixel format. */
    int bar_w = ((int)fb_width - 40) / 3;
    if (bar_w < 30) bar_w = 30;
    int bar_y = 20;
    int bar_h = 24;
    vesa_fill_rect(20,               bar_y, bar_w, bar_h, red);
    vesa_fill_rect(20 + bar_w,       bar_y, bar_w, bar_h, green);
    vesa_fill_rect(20 + bar_w * 2,   bar_y, bar_w, bar_h, blue);

    /* Header. */
    int hx = 30, hy = 60;
    vesa_draw_string(hx, hy, "GooberOS VESA boot", black, white);

    /* Framebuffer info dump. */
    char buf[64];
    int info_y = hy + 24;

    vesa_draw_string(hx, info_y, "Resolution:", black, white);
    splash_u32(buf, fb_width);
    vesa_draw_string(hx + 110, info_y, buf, black, white);
    vesa_draw_string(hx + 110 + 50, info_y, "x", black, white);
    splash_u32(buf, fb_height);
    vesa_draw_string(hx + 110 + 60, info_y, buf, black, white);

    info_y += 16;
    vesa_draw_string(hx, info_y, "Pitch:", black, white);
    splash_u32(buf, fb_pitch);
    vesa_draw_string(hx + 110, info_y, buf, black, white);

    info_y += 16;
    vesa_draw_string(hx, info_y, "BPP:", black, white);
    splash_u32(buf, (uint32_t)fb_bpp);
    vesa_draw_string(hx + 110, info_y, buf, black, white);

    info_y += 16;
    vesa_draw_string(hx, info_y, "FB addr:", black, white);
    splash_hex32(buf, fb_addr32);
    vesa_draw_string(hx + 110, info_y, buf, black, white);

    info_y += 24;
    vesa_draw_string(hx, info_y, "Status:", black, white);
    vesa_draw_string(hx + 80, info_y, status ? status : "Initializing hardware...", blue, white);

    info_y += 32;
    vesa_draw_string(hx, info_y, "If you see this text, the panel is in sync.", black, white);
    info_y += 16;
    vesa_draw_string(hx, info_y, "Otherwise reboot and pick VGA Safe Mode in GRUB.", black, white);

    vesa_swap();
}
