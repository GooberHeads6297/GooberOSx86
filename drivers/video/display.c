#include "display.h"
#include "font.h"
#include "vesa.h"
#include "intel_gfx.h"
#include "../diagnostics/driver_log.h"
#include "../io/io.h"
#include <stddef.h>

static display_mode_info_t active_display = {
    DISPLAY_DRIVER_NONE,
    DISPLAY_FORMAT_UNKNOWN,
    0, 0, 0, 0, 0, 0
};

static const display_driver_ops_t* driver_registry[DISPLAY_MAX_DRIVERS];
static const display_driver_ops_t* active_driver_ops = NULL;
static int driver_count = 0;
static const char* display_error = NULL;
static display_present_stats_t present_stats;
static display_present_mode_t last_logged_present_mode = DISPLAY_PRESENT_NONE;
static uint32_t last_present_log_count = 0;
/* Firmware GOP LFB without write-combining (Bay Trail / Braswell inherit):
 * a single multi-megabyte memcpy into scanout can bus-stall the CPU mid-copy
 * (top of panel updates, then total freeze -- no IRQs, no input). */
static int g_scanout_uncached = 0;
static int pending_present_h = 0;
static int pending_present_x = 0;
static int pending_present_y = 0;
static int pending_present_w = 0;

void display_set_scanout_uncached(int uncached) {
    g_scanout_uncached = uncached ? 1 : 0;
}

int display_scanout_uncached(void) {
    return g_scanout_uncached;
}

/* Per-call LFB write budget: keep 64 KiB on UC GOP; 512 KiB when WC/cached. */
uint32_t display_present_budget_bytes(void) {
    return g_scanout_uncached ? 65536u : (512u * 1024u);
}

static int g_present_oneshot = 0;

void display_present_set_oneshot(int oneshot) {
    g_present_oneshot = oneshot ? 1 : 0;
}

int display_present_has_pending(void) {
    return pending_present_h > 0;
}

void display_present_consume_pending(int* x, int* y, int* w, int* h) {
    if (!pending_present_h) return;
    if (x) *x = pending_present_x;
    if (y) *y = pending_present_y;
    if (w) *w = pending_present_w;
    if (h) *h = pending_present_h;
    pending_present_h = 0;
}

/* Local case-sensitive string compare; we have no libc in this layer. */
static int display_streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int g_display_register_quiet = 0;

void display_register_set_quiet(int quiet) {
    g_display_register_quiet = quiet ? 1 : 0;
}

void display_register_framebuffer(display_driver_t driver,
                                  display_pixel_format_t format,
                                  uintptr_t framebuffer_addr,
                                  uint32_t width,
                                  uint32_t height,
                                  uint32_t pitch,
                                  uint8_t bpp) {
    active_display.driver = driver;
    active_display.format = format;
    active_display.framebuffer_addr = framebuffer_addr;
    active_display.width = width;
    active_display.height = height;
    active_display.pitch = pitch;
    active_display.bpp = bpp;
    active_display.available = 1;
    if (g_display_register_quiet) return;
    driver_log("[display] active framebuffer driver=");
    driver_log(display_driver_name(driver));
    driver_log(" mode=");
    driver_log_u32(width);
    driver_log("x");
    driver_log_u32(height);
    driver_log("x");
    driver_log_u32(bpp);
    driver_log(" pitch=");
    driver_log_u32(pitch);
    driver_log("\n");
}

void display_register_text_mode(void) {
    active_display.driver = DISPLAY_DRIVER_VGA_TEXT;
    active_display.format = DISPLAY_FORMAT_UNKNOWN;
    active_display.framebuffer_addr = (uintptr_t)0xB8000u;
    active_display.width = 80;
    active_display.height = 25;
    active_display.pitch = 160;
    active_display.bpp = 16;
    active_display.available = 1;
    driver_log_line("[display] active mode: VGA text 80x25.");
}

const display_mode_info_t* display_get_mode(void) {
    return &active_display;
}

/* ---- VGA text-mode (mode 0x03) restore ---- *
 *
 * Standard register dump for 80x25 colour text. Layout: 1 MISC byte, 5
 * sequencer, 25 CRTC, 9 graphics-controller, 21 attribute-controller. These
 * are the long-standing, widely-used mode-0x03 values; they reprogram only the
 * VGA-compatible block, never the PLL/panel-power, so reverting here cannot
 * brick the panel -- worst case it is a no-op on hardware that ignores legacy
 * VGA. */
#define VGA_AC_INDEX     0x3C0
#define VGA_AC_WRITE     0x3C0
#define VGA_MISC_WRITE   0x3C2
#define VGA_SEQ_INDEX    0x3C4
#define VGA_SEQ_DATA     0x3C5
#define VGA_GC_INDEX     0x3CE
#define VGA_GC_DATA      0x3CF
#define VGA_CRTC_INDEX   0x3D4
#define VGA_CRTC_DATA    0x3D5
#define VGA_INSTAT_READ  0x3DA

#define VGA_NUM_SEQ_REGS  5
#define VGA_NUM_CRTC_REGS 25
#define VGA_NUM_GC_REGS   9
#define VGA_NUM_AC_REGS   21

static const unsigned char vga_80x25_text[] = {
    /* MISC */
    0x67,
    /* SEQ */
    0x03, 0x00, 0x03, 0x00, 0x02,
    /* CRTC */
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
    0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x50,
    0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
    0xFF,
    /* GC */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF,
    /* AC */
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
    0x0C, 0x00, 0x0F, 0x08, 0x00
};

static void vga_write_regs(const unsigned char* regs) {
    unsigned char crtc[VGA_NUM_CRTC_REGS];
    unsigned i;

    outb(VGA_MISC_WRITE, *regs++);
    for (i = 0; i < VGA_NUM_SEQ_REGS; i++) {
        outb(VGA_SEQ_INDEX, (uint8_t)i);
        outb(VGA_SEQ_DATA, *regs++);
    }

    /* Copy the CRTC block locally so we can force the unlock bits before
     * programming (CRTC[0x11] bit7 locks CRTC[0..7]). */
    for (i = 0; i < VGA_NUM_CRTC_REGS; i++) crtc[i] = regs[i];
    crtc[0x03] |= 0x80;
    crtc[0x11] &= (unsigned char)~0x80;

    outb(VGA_CRTC_INDEX, 0x11);
    outb(VGA_CRTC_DATA, (uint8_t)(inb(VGA_CRTC_DATA) & ~0x80)); /* unlock */
    for (i = 0; i < VGA_NUM_CRTC_REGS; i++) {
        outb(VGA_CRTC_INDEX, (uint8_t)i);
        outb(VGA_CRTC_DATA, crtc[i]);
    }
    regs += VGA_NUM_CRTC_REGS;

    for (i = 0; i < VGA_NUM_GC_REGS; i++) {
        outb(VGA_GC_INDEX, (uint8_t)i);
        outb(VGA_GC_DATA, *regs++);
    }

    for (i = 0; i < VGA_NUM_AC_REGS; i++) {
        (void)inb(VGA_INSTAT_READ);          /* reset AC flip-flop to index */
        outb(VGA_AC_INDEX, (uint8_t)i);
        outb(VGA_AC_WRITE, *regs++);
    }
    /* Lock palette and unblank the display. */
    (void)inb(VGA_INSTAT_READ);
    outb(VGA_AC_INDEX, 0x20);
}

/* Upload the 8x16 boot font into plane 2 so glyphs render after the mode set.
 * Uses deterministic register values (rather than read-back) to flip into and
 * out of font-access addressing at 0xA0000. */
static void vga_upload_font(void) {
    volatile uint8_t* seg = (volatile uint8_t*)0xA0000;
    int c, r;

    /* Enter font-access: write plane 2, flat 0xA0000, odd/even off. */
    outb(VGA_SEQ_INDEX, 0x02); outb(VGA_SEQ_DATA, 0x04);
    outb(VGA_SEQ_INDEX, 0x04); outb(VGA_SEQ_DATA, 0x06);
    outb(VGA_GC_INDEX, 0x04);  outb(VGA_GC_DATA, 0x02);
    outb(VGA_GC_INDEX, 0x05);  outb(VGA_GC_DATA, 0x00);
    outb(VGA_GC_INDEX, 0x06);  outb(VGA_GC_DATA, 0x00);

    for (c = 0; c < 256; c++) {
        const uint8_t* glyph = (c >= 0x20 && c <= 0x7E) ? font8x16[c] : 0;
        for (r = 0; r < 16; r++) seg[c * 32 + r] = glyph ? glyph[r] : 0;
        for (r = 16; r < 32; r++) seg[c * 32 + r] = 0;
    }

    /* Return to text-mode addressing (planes 0&1, odd/even, 0xB8000). */
    outb(VGA_SEQ_INDEX, 0x02); outb(VGA_SEQ_DATA, 0x03);
    outb(VGA_SEQ_INDEX, 0x04); outb(VGA_SEQ_DATA, 0x02);
    outb(VGA_GC_INDEX, 0x04);  outb(VGA_GC_DATA, 0x00);
    outb(VGA_GC_INDEX, 0x05);  outb(VGA_GC_DATA, 0x10);
    outb(VGA_GC_INDEX, 0x06);  outb(VGA_GC_DATA, 0x0E);
}

void display_restore_vga_text(void) {
    /* Programming the VGA register file is not interrupt-safe (an IRQ printing
     * to 0xB8000 mid-remap would scribble plane 2); fence it. */
    __asm__ volatile("cli");
    vga_write_regs(vga_80x25_text);
    vga_upload_font();
    __asm__ volatile("sti");

    display_register_text_mode();
}

const char* display_driver_name(display_driver_t driver) {
    switch (driver) {
        case DISPLAY_DRIVER_VGA_TEXT: return "VGA text";
        case DISPLAY_DRIVER_VESA_LFB: return "VESA linear framebuffer";
        case DISPLAY_DRIVER_BASIC_LFB: return "Basic Display (firmware FB)";
        case DISPLAY_DRIVER_NATIVE_INTEL: return "Native Intel display";
        case DISPLAY_DRIVER_NATIVE_GENERIC: return "Native generic framebuffer";
        case DISPLAY_DRIVER_VGA_GRAPHICS: return "VGA mode-13h (320x200x8)";
        default: return "None";
    }
}

const char* display_format_name(display_pixel_format_t format) {
    switch (format) {
        case DISPLAY_FORMAT_RGB565: return "RGB565";
        case DISPLAY_FORMAT_BGR888: return "BGR888";
        case DISPLAY_FORMAT_XRGB8888: return "XRGB8888";
        default: return "Unknown";
    }
}

const char* display_present_mode_name(display_present_mode_t mode) {
    switch (mode) {
        case DISPLAY_PRESENT_COPY_RECT: return "copy-rect";
        case DISPLAY_PRESENT_COPY_FRAME: return "copy-frame";
        case DISPLAY_PRESENT_VBLANK_COPY_RECT: return "vblank-copy-rect";
        case DISPLAY_PRESENT_VBLANK_COPY_FRAME: return "vblank-copy-frame";
        case DISPLAY_PRESENT_PAGE_FLIP: return "page-flip";
        default: return "none";
    }
}

const display_present_stats_t* display_get_present_stats(void) {
    return &present_stats;
}

void display_present_note_promotion(void) {
    present_stats.promoted_frames++;
}

int display_present_vblank_reliable(void) {
    if (!active_driver_ops || !active_driver_ops->wait_vblank) return 0;
    if (present_stats.vblank_waits == 0) return 0;
    if (present_stats.vblank_misses >= present_stats.vblank_waits) return 0;
    if (present_stats.vblank_waits < 8) return 1;
    return present_stats.vblank_misses * 3U < present_stats.vblank_waits;
}

static void display_record_present(display_present_mode_t mode, int x, int y, int w, int h) {
    (void)x;
    (void)y;
    present_stats.present_count++;
    present_stats.last_mode = mode;
    if (w > 0 && h > 0)
        present_stats.last_dirty_area = (uint32_t)w * (uint32_t)h;
    else
        present_stats.last_dirty_area = active_display.width * active_display.height;
    /*
     * Present mode can alternate every frame (small dirty rect vs. promoted
     * frame). Logging every transition made both VM and real hardware visibly
     * slower because Desktop log.txt is filesystem-backed. Keep the current
     * mode in stats/System Info, and log only the first mode plus a sparse
     * heartbeat.
     */
    if (last_logged_present_mode == DISPLAY_PRESENT_NONE ||
        present_stats.present_count - last_present_log_count >= 300U) {
        driver_log("[display] present mode: ");
        driver_log(display_present_mode_name(mode));
        driver_log(" area=");
        driver_log_u32(present_stats.last_dirty_area);
        driver_log("\n");
        last_logged_present_mode = mode;
        last_present_log_count = present_stats.present_count;
    }
}

int display_present_rect(int x, int y, int w, int h) {
    /*
     * Direct-to-LFB when no RAM back-buffer is armed: drawing already landed
     * on scanout; skip the memcpy.
     */
    if (!vesa_has_backbuffer()) return 1;

    int waited = 0;
    if (w <= 0 || h <= 0) return 0;
    /*
     * Never touch Intel display MMIO for vblank on Bay Trail/Braswell — those
     * reads can hard-stall the CPU bus. Generic VESA/basic drivers have a NULL
     * wait_vblank and are fine.
     */
    if (active_driver_ops && active_driver_ops->wait_vblank &&
        !intel_gfx_is_bay_trail_class()) {
        waited = active_driver_ops->wait_vblank(2);
        present_stats.vblank_waits++;
        if (!waited) present_stats.vblank_misses++;
    }
    if (active_driver_ops && active_driver_ops->present_rect) {
        if (active_driver_ops->present_rect(x, y, w, h)) {
            display_record_present(waited ? DISPLAY_PRESENT_VBLANK_COPY_RECT
                                          : DISPLAY_PRESENT_COPY_RECT,
                                   x, y, w, h);
            return 1;
        }
    }
    /*
     * Cap LFB write bandwidth per call. Uncached GOP keeps ~64 KiB so one
     * keypress cannot bus-stall Braswell; WC/cached uses ~512 KiB. Window
     * drag sets oneshot so the dirty AABB finishes in this call (no striped
     * mid-window bands).
     */
    {
        uint32_t row_bytes = (uint32_t)w * 4u;
        uint32_t budget = display_present_budget_bytes();
        int max_rows;
        int copied;
        if (row_bytes == 0) row_bytes = 4u;
        if (g_present_oneshot)
            max_rows = h;
        else {
            max_rows = (int)(budget / row_bytes);
            if (max_rows < 1) max_rows = 1;
            if (max_rows > h) max_rows = h;
            /*
             * Keep partial presents on glyph-row boundaries (8x16 font). Cutting
             * mid-glyph leaves half-drawn characters that look like "cutoff"
             * terminal text until a later pending chunk arrives -- and if that
             * chunk is cancelled, the corruption sticks.
             */
            if (max_rows >= 16 && max_rows < h)
                max_rows -= (max_rows % 16);
            if (max_rows < 1) max_rows = 1;
        }
        copied = vesa_swap_rect_rows(x, y, w, h, max_rows);
        display_record_present(waited ? DISPLAY_PRESENT_VBLANK_COPY_RECT
                                      : DISPLAY_PRESENT_COPY_RECT,
                               x, y, w, copied);
        if (copied < h) {
            /* This blit was truncated: defer the remainder (replaces any
             * previous pending -- caller should drain pending first). */
            pending_present_x = x;
            pending_present_y = y + copied;
            pending_present_w = w;
            pending_present_h = h - copied;
        } else {
            /*
             * This blit finished. Do NOT clear an unrelated pending region.
             * The alive-beacon (and other small oneshots) used to complete here
             * and zero pending_present_h, cancelling the rest of a tall shell
             * present -- the terminal then showed cut-off / stale glyphs on
             * both VirtualBox and real hardware.
             */
            if (pending_present_h > 0 &&
                pending_present_x == x &&
                pending_present_w == w &&
                y <= pending_present_y &&
                (y + copied) >= pending_present_y) {
                int pend_end = pending_present_y + pending_present_h;
                if ((y + copied) >= pend_end) {
                    pending_present_h = 0;
                } else {
                    pending_present_y = y + copied;
                    pending_present_h = pend_end - pending_present_y;
                }
            }
        }
        return 1;
    }
}

int display_present_frame(void) {
    if (!vesa_has_backbuffer()) return 1;

    int waited = 0;
    if (active_driver_ops && active_driver_ops->wait_vblank &&
        !intel_gfx_is_bay_trail_class()) {
        waited = active_driver_ops->wait_vblank(2);
        present_stats.vblank_waits++;
        if (!waited) present_stats.vblank_misses++;
    }
    if (active_driver_ops && active_driver_ops->page_flip) {
        if (active_driver_ops->page_flip()) {
            display_record_present(DISPLAY_PRESENT_PAGE_FLIP, 0, 0, 0, 0);
            return 1;
        }
    }
    if (active_driver_ops && active_driver_ops->present_frame) {
        if (active_driver_ops->present_frame()) {
            display_record_present(waited ? DISPLAY_PRESENT_VBLANK_COPY_FRAME
                                          : DISPLAY_PRESENT_COPY_FRAME,
                                   0, 0, 0, 0);
            return 1;
        }
    }
    vesa_present();
    display_record_present(waited ? DISPLAY_PRESENT_VBLANK_COPY_FRAME
                                  : DISPLAY_PRESENT_COPY_FRAME,
                           0, 0, 0, 0);
    return 1;
}

void display_reset_drivers(void) {
    driver_count = 0;
    for (int i = 0; i < DISPLAY_MAX_DRIVERS; i++) driver_registry[i] = NULL;
    active_driver_ops = NULL;
    for (uint32_t i = 0; i < sizeof(present_stats); i++) ((uint8_t*)&present_stats)[i] = 0;
    last_logged_present_mode = DISPLAY_PRESENT_NONE;
    last_present_log_count = 0;
}

void display_register_driver(const display_driver_ops_t* ops) {
    if (!ops || !ops->init) return;
    if (driver_count >= DISPLAY_MAX_DRIVERS) return;
    driver_registry[driver_count++] = ops;
    driver_log("[display] registered driver ");
    driver_log(ops->name ? ops->name : "?");
    driver_log("\n");
}

void display_set_error(const char* msg) {
    display_error = msg;
    if (msg) {
        driver_log("[display] error: ");
        driver_log(msg);
        if (msg[0]) {
            uint32_t i = 0;
            while (msg[i]) i++;
            if (i > 0 && msg[i - 1] != '\n') driver_log("\n");
        }
    }
}

const char* display_last_error(void) {
    return display_error;
}

const display_driver_ops_t* display_probe_drivers(const char* force_name,
                                                  uint32_t req_w,
                                                  uint32_t req_h,
                                                  uint8_t req_bpp,
                                                  display_framebuffer_t* out,
                                                  display_confirm_fn confirm,
                                                  void* confirm_ctx) {
    if (!out) return NULL;

    int force = (force_name && !display_streq(force_name, "auto") &&
                 force_name[0] != '\0');

    for (int i = 0; i < driver_count; i++) {
        const display_driver_ops_t* ops = driver_registry[i];
        if (!ops || !ops->init) continue;
        if (force && !display_streq(force_name, ops->name)) continue;

        driver_log("[display] probe driver ");
        driver_log(ops->name ? ops->name : "?");
        driver_log("\n");

        if (ops->probe && !ops->probe()) {
            driver_log("[display] probe declined ");
            driver_log(ops->name ? ops->name : "?");
            driver_log("\n");
            continue;
        }

        display_framebuffer_t fb = {0, 0, 0, 0, 0, DISPLAY_FORMAT_UNKNOWN};
        if (!ops->init(req_w, req_h, req_bpp, &fb)) {
            driver_log("[display] init failed ");
            driver_log(ops->name ? ops->name : "?");
            driver_log("\n");
            continue;
        }

        /* Rung initialized; gate it on the visibility confirmation (if any).
         * A rejected rung is abandoned cleanly and we fall through to the next
         * candidate -- the caller's confirm() is responsible for logging why. */
        if (confirm && !confirm(ops, &fb, confirm_ctx)) {
            driver_log("[display] visibility rejected ");
            driver_log(ops->name ? ops->name : "?");
            driver_log("\n");
            continue;
        }

        *out = fb;
        active_driver_ops = ops;
        driver_log("[display] selected driver ");
        driver_log(ops->name ? ops->name : "?");
        driver_log("\n");
        return ops;
    }

    if (!display_error) display_error = "No display driver could bring up a framebuffer.";
    return NULL;
}
