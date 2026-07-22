#include "display.h"
#include "font.h"
#include "../io/io.h"
#include <stddef.h>

static display_mode_info_t active_display = {
    DISPLAY_DRIVER_NONE,
    DISPLAY_FORMAT_UNKNOWN,
    0, 0, 0, 0, 0, 0
};

static const display_driver_ops_t* driver_registry[DISPLAY_MAX_DRIVERS];
static int driver_count = 0;
static const char* display_error = NULL;

/* Local case-sensitive string compare; we have no libc in this layer. */
static int display_streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
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

void display_reset_drivers(void) {
    driver_count = 0;
    for (int i = 0; i < DISPLAY_MAX_DRIVERS; i++) driver_registry[i] = NULL;
}

void display_register_driver(const display_driver_ops_t* ops) {
    if (!ops || !ops->init) return;
    if (driver_count >= DISPLAY_MAX_DRIVERS) return;
    driver_registry[driver_count++] = ops;
}

void display_set_error(const char* msg) {
    display_error = msg;
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

        if (ops->probe && !ops->probe()) continue;

        display_framebuffer_t fb = {0, 0, 0, 0, 0, DISPLAY_FORMAT_UNKNOWN};
        if (!ops->init(req_w, req_h, req_bpp, &fb)) continue;

        /* Rung initialized; gate it on the visibility confirmation (if any).
         * A rejected rung is abandoned cleanly and we fall through to the next
         * candidate -- the caller's confirm() is responsible for logging why. */
        if (confirm && !confirm(ops, &fb, confirm_ctx)) continue;

        *out = fb;
        return ops;
    }

    if (!display_error) display_error = "No display driver could bring up a framebuffer.";
    return NULL;
}
