#include "basic_display.h"
#include "bios_vbe.h"
#include "connector.h"
#include "display.h"
#include "intel_gfx.h"
#include "native_fb.h"
#include "drivers/diagnostics/driver_log.h"
#include "drivers/io/io.h"
#include "font_8x16.h"
#include <stddef.h>

static int g_scanout_preserved = 0;

static void bd_serial(const char* s) {
    while (s && *s) outb(0xE9, (uint8_t)*s++);
}

static void bd_serial_hex32(uint32_t v) {
    const char* hex = "0123456789ABCDEF";
    char buf[11];
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 8; i++)
        buf[2 + i] = hex[(v >> ((7 - i) * 4)) & 0xF];
    buf[10] = '\0';
    bd_serial(buf);
}

static int bd_bpp_bytes(uint8_t bpp, uint32_t* bpx_out) {
    if (bpp == 32) { if (bpx_out) *bpx_out = 4; return 1; }
    if (bpp == 24) { if (bpx_out) *bpx_out = 3; return 1; }
    if (bpp == 16 || bpp == 15) { if (bpx_out) *bpx_out = 2; return 1; }
    return 0;
}

static int bd_pitch_ok(uintptr_t addr, uint32_t w, uint32_t h,
                       uint32_t pitch, uint8_t bpp) {
    uint32_t bpx;
    if (!bd_bpp_bytes(bpp, &bpx)) return 0;
    (void)h;
    (void)addr;
    /* Geometry-only: never write the LFB here. Bay Trail BIOS often hands a
     * multiboot tag that is not the scanned surface; a stride probe would
     * scribble the wrong buffer and can hang on bad phys addresses. */
    return pitch >= w * bpx;
}

static void bd_log_fb(const char* tag, const kernel_loader_fb_t* fb) {
    bd_serial(tag);
    if (!fb) { bd_serial(" (null)\n"); return; }
    bd_serial(" LFB=0x");
    bd_serial_hex32((uint32_t)fb->addr);
    bd_serial(" ");
    { char n[12]; uint32_t v = fb->w; int p = 0;
      if (v == 0) n[p++] = '0';
      else { char t[12]; int q = 0; while (v) { t[q++] = (char)('0' + (v % 10)); v /= 10; }
             while (q) n[p++] = t[--q]; }
      n[p] = '\0'; bd_serial(n); }
    bd_serial("x");
    { char n[12]; uint32_t v = fb->h; int p = 0;
      if (v == 0) n[p++] = '0';
      else { char t[12]; int q = 0; while (v) { t[q++] = (char)('0' + (v % 10)); v /= 10; }
             while (q) n[p++] = t[--q]; }
      n[p] = '\0'; bd_serial(n); }
    bd_serial("x");
    { char n[8]; uint32_t v = fb->bpp; int p = 0;
      if (v == 0) n[p++] = '0';
      else { char t[8]; int q = 0; while (v) { t[q++] = (char)('0' + (v % 10)); v /= 10; }
             while (q) n[p++] = t[--q]; }
      n[p] = '\0'; bd_serial(n); }
    bd_serial(" pitch=");
    bd_serial_hex32(fb->pitch);
    bd_serial("\n");
}

typedef struct {
    uint32_t w, h;
    uint8_t  bpp;
    uint32_t pitches[10];
    uint8_t  n;
} msbd_pitch_row_t;

static const msbd_pitch_row_t msbd_pitch_catalog[] = {
    { 640,  480,  32, { 2560, 2816, 3072, 4096 }, 4 },
    { 640,  480,  24, { 1920, 2048, 2560 }, 3 },
    { 800,  600,  32, { 3200, 3328, 4096, 8192 }, 4 },
    { 800,  600,  24, { 2400, 2560, 3200 }, 3 },
    { 1024, 768,  32, { 4096, 4160, 4352, 8192 }, 4 },
    { 1024, 768,  24, { 3072, 4096, 8192 }, 3 },
    { 1280, 720,  32, { 5120, 5632, 6144, 8192 }, 4 },
    { 1280, 800,  32, { 5120, 5280, 6144, 8192 }, 4 },
    { 1360, 768,  32, { 5440, 5504, 5632, 6144 }, 4 },
    { 1366, 768,  32, { 5504, 5472, 5464, 5632, 6144, 8192 }, 6 },
    { 1366, 768,  24, { 4128, 4096, 5504, 5632 }, 4 },
    { 1440, 900,  32, { 5760, 5888, 6144, 8192 }, 4 },
    { 1600, 900,  32, { 6400, 6656, 7168, 8192 }, 4 },
    { 1680, 1050, 32, { 6720, 7168, 8192 }, 3 },
    { 1280, 1024, 32, { 5120, 5632, 6144, 8192 }, 4 },
    { 1280, 1024, 24, { 3840, 4096, 5120 }, 3 },
    { 1920, 1080, 32, { 7680, 8192, 8448, 8704 }, 4 },
    { 1920, 1200, 32, { 7680, 8192, 9600 }, 3 },
};

static int bd_pitch_from_catalog(kernel_loader_fb_t* fb) {
    uintptr_t addr;
    uint32_t bpx, min_pitch;
    size_t i, j;

    if (!fb || !fb->have || fb->type != 1 || fb->addr == 0) return 0;
    if (fb->w < 320 || fb->h < 200) return 0;
    if (!bd_bpp_bytes(fb->bpp, &bpx)) return 0;

    addr = (uintptr_t)fb->addr;
    min_pitch = fb->w * bpx;

    for (i = 0; i < sizeof(msbd_pitch_catalog) / sizeof(msbd_pitch_catalog[0]); i++) {
        const msbd_pitch_row_t* row = &msbd_pitch_catalog[i];
        if (row->w != fb->w || row->h != fb->h || row->bpp != fb->bpp)
            continue;
        bd_serial("[msbd] catalog match ");
        bd_serial_hex32(row->w);
        bd_serial("x");
        bd_serial_hex32(row->h);
        bd_serial("\n");
        for (j = 0; j < row->n; j++) {
            uint32_t pitch = row->pitches[j];
            if (pitch < min_pitch) continue;
            if (!bd_pitch_ok(addr, fb->w, fb->h, pitch, fb->bpp))
                continue;
            fb->pitch = pitch;
            kernel_loader_fb_set(fb);
            bd_serial("[msbd] catalog pitch adopted: ");
            bd_serial_hex32(pitch);
            bd_serial("\n");
            return 1;
        }
    }
    return 0;
}

static int bd_pitch_alignment_sweep(kernel_loader_fb_t* fb) {
    uintptr_t addr;
    uint32_t bpx, min_pitch;
    uint32_t candidates[12];
    int n = 0, i;

    if (!fb || !fb->have || fb->type != 1 || fb->addr == 0) return 0;
    if (!bd_bpp_bytes(fb->bpp, &bpx)) return 0;
    addr = (uintptr_t)fb->addr;
    min_pitch = fb->w * bpx;

    if (fb->pitch >= min_pitch &&
        bd_pitch_ok(addr, fb->w, fb->h, fb->pitch, fb->bpp))
        return 0;

    candidates[n++] = (min_pitch + 15U) & ~15U;
    candidates[n++] = (min_pitch + 31U) & ~31U;
    candidates[n++] = (min_pitch + 63U) & ~63U;
    candidates[n++] = ((min_pitch + 127U) / 128U) * 128U;
    if ((min_pitch & 63U) != 0)
        candidates[n++] = (min_pitch + 63U) & ~63U;

    for (i = 0; i < n; i++) {
        uint32_t pitch = candidates[i];
        if (pitch <= min_pitch) continue;
        if (!bd_pitch_ok(addr, fb->w, fb->h, pitch, fb->bpp))
            continue;
        fb->pitch = pitch;
        kernel_loader_fb_set(fb);
        bd_serial("[msbd] alignment pitch adopted: ");
        bd_serial_hex32(pitch);
        bd_serial("\n");
        return 1;
    }
    return 0;
}

#define MSBD_BANNER_FG 0xFFFFFFU
#define MSBD_BANNER_BG 0x1A1A2EU

static void bd_fb_putc(volatile uint8_t* fb, uint32_t pitch, uint8_t bpp,
                       int cx, int cy, char ch, uint32_t fg, uint32_t bg) {
    const uint8_t* glyph;
    int row, col;
    unsigned c = (unsigned char)ch;

    if (c >= 128U) c = '?';
    glyph = goober_font_8x16[c];
    for (row = 0; row < 16; row++) {
        volatile uint8_t* line = fb + (uint32_t)(cy + row) * pitch;
        uint8_t bits = glyph[row];
        for (col = 0; col < 8; col++) {
            uint32_t color = (bits & (0x80U >> col)) ? fg : bg;
            int px = cx + col;
            if (bpp == 32) {
                ((volatile uint32_t*)line)[px] = color;
            } else if (bpp == 24) {
                volatile uint8_t* p = line + (uint32_t)px * 3U;
                p[0] = (uint8_t)(color & 0xFF);
                p[1] = (uint8_t)((color >> 8) & 0xFF);
                p[2] = (uint8_t)((color >> 16) & 0xFF);
            }
        }
    }
}

static void bd_fb_puts(volatile uint8_t* fb, uint32_t pitch, uint8_t bpp,
                       int x, int y, const char* s) {
    int cx = x;
    while (s && *s) {
        if (*s == '\n') { cx = x; y += 16; s++; continue; }
        bd_fb_putc(fb, pitch, bpp, cx, y, *s, MSBD_BANNER_FG, MSBD_BANNER_BG);
        cx += 8;
        s++;
    }
}

static void bd_show_banner(const char* line1, const char* line2) {
    kernel_loader_fb_t info;
    volatile uint8_t* fb;
    uint32_t y;

    kernel_loader_fb_get(&info);
    if (!kernel_loader_fb_usable()) return;
    fb = (volatile uint8_t*)(uintptr_t)info.addr;
    for (y = 0; y < 48 && y < info.h; y++) {
        volatile uint8_t* row = fb + y * info.pitch;
        uint32_t x;
        for (x = 0; x < info.w; x++) {
            if (info.bpp == 32)
                ((volatile uint32_t*)row)[x] = MSBD_BANNER_BG;
            else if (info.bpp == 24) {
                volatile uint8_t* p = row + x * 3U;
                p[0] = (uint8_t)(MSBD_BANNER_BG & 0xFF);
                p[1] = (uint8_t)((MSBD_BANNER_BG >> 8) & 0xFF);
                p[2] = (uint8_t)((MSBD_BANNER_BG >> 16) & 0xFF);
            }
        }
    }
    if (line1) bd_fb_puts(fb, info.pitch, info.bpp, 8, 8, line1);
    if (line2) bd_fb_puts(fb, info.pitch, info.bpp, 8, 28, line2);
}

static int bd_adopt_intel_scanout(kernel_loader_fb_t* fb) {
    intel_firmware_scanout_t hw;
    uint32_t bpx, min_pitch;

    if (!fb || !fb->have || fb->type != 1) return 0;
    if (!bd_bpp_bytes(fb->bpp, &bpx)) return 0;

    if (!intel_gfx_read_firmware_scanout(fb->w, fb->h, &hw) || !hw.ok)
        return 0;

    min_pitch = fb->w * bpx;
    if (hw.stride < min_pitch)
        return 0;
    if (!bd_pitch_ok((uintptr_t)hw.fb_phys, fb->w, fb->h, hw.stride, fb->bpp))
        return 0;

    if (hw.fb_phys == (uint32_t)(uintptr_t)fb->addr && hw.stride == fb->pitch) {
        bd_serial("[msbd] firmware scanout matches loader tag\n");
        return 0;
    }

    bd_serial("[msbd] adopting firmware plane surface FB=0x");
    bd_serial_hex32(hw.fb_phys);
    bd_serial(" stride=");
    bd_serial_hex32(hw.stride);
    bd_serial(" pipe=");
    { char n[4]; int v = hw.pipe, p = 0;
      if (v == 0) n[p++] = '0';
      else { char t[4]; int q = 0; while (v) { t[q++] = (char)('0' + (v % 10)); v /= 10; }
             while (q) n[p++] = t[--q]; }
      n[p] = '\0'; bd_serial(n); }
    bd_serial("\n");

    fb->addr = hw.fb_phys;
    fb->pitch = hw.stride;
    kernel_loader_fb_set(fb);
    bd_show_banner("GooberOS Basic Display", "firmware scanout adopted");
    return 1;
}

int basic_display_scanout_preserved(void) {
    return g_scanout_preserved;
}

void basic_display_prepare_loader_fb(void) {
    kernel_loader_fb_t fb;
    uint32_t bpx, min_pitch;
    int bay_trail = intel_gfx_is_bay_trail_class();

    g_scanout_preserved = 0;
    kernel_loader_fb_get(&fb);
    if (!fb.have || fb.type != 1 || fb.addr == 0)
        return;
    if (!bd_bpp_bytes(fb.bpp, &bpx)) return;
    if (fb.w < 320 || fb.h < 200) return;

    bd_serial("[msbd] strategy: preserve firmware mode (no VBE modeset)\n");
    if (bay_trail)
        bd_serial("[msbd] Bay Trail/Braswell: skipping Intel display MMIO\n");

    /*
     * Do NOT paint a banner before scanout adopt — on 80M4 the multiboot tag
     * is often not the scanned surface, so early paints leave GRUB blue.
     *
     * Do NOT touch Intel display MMIO (plane/HPD/GMBUS) on Bay Trail while
     * the firmware LFB is live — that path hangs and freezes gfxterm.
     */
    min_pitch = fb.w * bpx;
    if (fb.pitch == 0)
        fb.pitch = min_pitch;
    if (fb.pitch == fb.w && bpx > 1)
        fb.pitch = min_pitch;

    if (!bay_trail)
        bd_adopt_intel_scanout(&fb);

    if (!bd_pitch_from_catalog(&fb))
        bd_pitch_alignment_sweep(&fb);

    if (fb.pitch < min_pitch) {
        bd_serial("[msbd] reject: pitch below minimum\n");
        bd_log_fb("[msbd] rejected", &fb);
        return;
    }

    kernel_loader_fb_set(&fb);
    g_scanout_preserved = 1;
    bd_log_fb("[msbd] prepared loader FB", &fb);
    driver_log_line("[msbd] loader framebuffer prepared (MS Basic Display path).");

    /* Inventory without any Intel MMIO on Bay Trail (kernel fills stubs). */
    if (!bay_trail) {
        display_connectors_scan_ex(0);
        display_connector_add_simplefb(fb.w, fb.h, "Firmware-LFB");
    }
}

int basic_display_try_all_pitches(void) {
    kernel_loader_fb_t fb;
    kernel_loader_fb_get(&fb);
    if (bd_pitch_from_catalog(&fb)) return 1;
    return bd_pitch_alignment_sweep(&fb);
}

void basic_display_paint_probe(void) {
    kernel_loader_fb_t info;
    uintptr_t fb_addr;
    uint32_t w, h, pitch, bpp, bpx, y;
    volatile uint8_t* fb;

    kernel_loader_fb_get(&info);
    if (!kernel_loader_fb_usable()) {
        bd_serial("[msbd] panel probe skipped (no usable loader FB)\n");
        return;
    }

    fb_addr = (uintptr_t)info.addr;
    w = info.w;
    h = info.h;
    pitch = info.pitch;
    bpp = info.bpp;
    (void)bd_bpp_bytes((uint8_t)bpp, &bpx);
    fb = (volatile uint8_t*)fb_addr;

    for (y = 0; y < h; y++) {
        volatile uint8_t* row = fb + y * pitch;
        uint32_t x;
        for (x = 0; x < w; x++) {
            uint32_t color = (((x / 32U) + (y / 32U)) & 1U) ? 0x4C8BF5U : 0xE8EAEDU;
            if (bpp == 32) {
                ((volatile uint32_t*)row)[x] = color;
            } else if (bpp == 24) {
                volatile uint8_t* p = row + x * 3U;
                p[0] = (uint8_t)(color & 0xFF);
                p[1] = (uint8_t)((color >> 8) & 0xFF);
                p[2] = (uint8_t)((color >> 16) & 0xFF);
            } else if (bpp == 16 || bpp == 15) {
                uint16_t r = (uint16_t)((color >> 19) & 0x1F);
                uint16_t g = (uint16_t)((color >> 10) & 0x3F);
                uint16_t b = (uint16_t)((color >> 3) & 0x1F);
                ((volatile uint16_t*)row)[x] = (uint16_t)((r << 11) | (g << 5) | b);
            }
        }
    }
    bd_serial("[msbd] painted checkerboard probe on inherited LFB\n");
}

static const char* const msbd_driver_ladder[] = {
    "intel",
    "basic",
    "vesa",
    "bochs",
    NULL
};

const display_driver_ops_t* basic_display_probe_ladder(
    uint32_t req_w, uint32_t req_h,
    display_framebuffer_t* out,
    basic_display_confirm_fn confirm,
    void* confirm_ctx,
    int intel_first) {

    const display_driver_ops_t* drv = NULL;
    size_t i;

    if (!out) return NULL;

    for (i = 0; msbd_driver_ladder[i]; i++) {
        const char* name = msbd_driver_ladder[i];

        if (!intel_first && name[0] == 'i' && name[1] == 'n')
            continue;

        bd_serial("[msbd] probe driver ");
        bd_serial(name);
        bd_serial("\n");

        drv = display_probe_drivers(name, req_w, req_h, 0, out,
                                    (display_confirm_fn)confirm, confirm_ctx);
        if (drv) {
            bd_serial("[msbd] selected driver ");
            bd_serial(drv->name ? drv->name : "?");
            bd_serial("\n");
            return drv;
        }
    }
    return NULL;
}
