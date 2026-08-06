/* kernel_x64.c - x86_64-only helpers for the unified kernel.c orchestrator.
 *
 * Phase 3b deliverable: the unified staged-boot orchestrator now lives in
 * kernel.c (the same translation unit that owns the x86 build), and kernel.c
 * itself is the long-mode kernel_main. This file shrinks to the x64-only
 * helpers that the orchestrator calls into:
 *
 *   - x64_arch_serial_init()                  -- COM1 8N1 38400 + 0xE9 mirror
 *   - x64_arch_dump_multiboot(magic, info)    -- Phase 1 hand-off log
 *   - x64_arch_idt_install()                  -- 64-bit IDT (256 gates,
 *                                                exception_stubs + IRQ0/1/12)
 *   - x64_arch_pic_remap()                    -- legacy 8259 remap
 *   - x64_arch_walk_and_draw_framebuffer(info) -- mb2 framebuffer tag walk
 *                                                  and serial diagnostics
 *   - x64_arch_legacy_vga_text_line(s)        -- one line of 0xB8000 text
 *                                                (visible on legacy BIOS;
 *                                                 silently dropped on UEFI)
 *
 * It also provides the long-mode IRQ handler bodies the ASM wrappers call
 * (irq0_handler_main / irq1_handler_main / mouse_handler_main) and the x64
 * stubs for the kernel.c-only-on-x86 symbols boot_safety.c references on
 * the boot-summary path (pci_find_display_controllers / pci_find_usb_controllers).
 *
 * What this file NO LONGER owns (Phase 3b lift):
 *   - kernel_main(): in kernel.c under `#ifdef __x86_64__`.
 *   - print(): in kernel.c (the real one routed through the active print
 *              sink + vga_put_char fall-through).
 *   - vga_set_text_color(): in drivers/video/vga.c (linked under -m64 now).
 *   - cpu_exception_handler(): in kernel.c under `#ifdef __x86_64__`,
 *                              alongside the x86 sibling.
 *   - Phase 2/3a.1/3b.0 demo stages (clean / fault / watchdog): the
 *              orchestrator's k_boot_stages_x64[] replaces them.
 */

#include <stdint.h>
#include <stddef.h>
#include "drivers/io/io.h"
#include "include/multiboot.h"
#include "drivers/pci/pci.h"
#include "boot_safety.h"
#include "drivers/timer/timer.h"
#include "cpu_exception_names.h"
#include "font_8x16.h"

/* ---- COM1 (0x3F8) serial init ------------------------------------------- */

#define COM1 0x3F8

void x64_arch_serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);  /* 38400 baud divisor low */
    outb(COM1 + 1, 0x00);  /* divisor high */
    outb(COM1 + 3, 0x03);  /* 8N1 */
    outb(COM1 + 2, 0xC7);  /* enable + clear FIFOs, 14-byte trigger */
    outb(COM1 + 4, 0x0B);  /* DTR + RTS + OUT2 */
}

/* Local serial helpers used only inside this file. The unified kernel.c
 * serial_out() is the canonical sink for everything orchestrator-driven;
 * we keep these here because they are needed before kernel.c::serial_out
 * is reachable (the Phase 1 hand-off log runs while x64_arch_serial_init
 * is still the only thing that has touched COM1). */

/*
 * Bounded COM1 wait. Real laptops (Acer R3-131T) often have no usable
 * UART at 0x3F8; an unbounded LSR.THRE spin hangs boot mid-string and
 * floating-bus reads make it intermittent. After one timeout, skip COM1
 * for the rest of this file's early log; 0xE9 still works under QEMU.
 */
static int g_x64_com1_tx_ok = 1;

static void x64_putc(char c) {
    outb(0xE9, (uint8_t)c);
    if (g_x64_com1_tx_ok) {
        uint32_t i;
        for (i = 0; i < 100000u; i++) {
            if (inb(COM1 + 5) & 0x20) {
                outb(COM1, (uint8_t)c);
                return;
            }
        }
        g_x64_com1_tx_ok = 0;
    }
}

static void x64_out(const char* s) { while (*s) x64_putc(*s++); }

static void x64_out_hex64(uint64_t v) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) buf[2 + i] = hex[(v >> ((15 - i) * 4)) & 0xF];
    buf[18] = '\0';
    x64_out(buf);
}

static void x64_out_hex32(uint32_t v) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) buf[2 + i] = hex[(v >> ((7 - i) * 4)) & 0xF];
    buf[10] = '\0';
    x64_out(buf);
}

static void x64_out_hex8(uint8_t v) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[5];
    buf[0] = '0'; buf[1] = 'x';
    buf[2] = hex[(v >> 4) & 0xF];
    buf[3] = hex[v & 0xF];
    buf[4] = '\0';
    x64_out(buf);
}

static void x64_out_dec(uint64_t v) {
    char buf[24];
    int  i = 0;
    if (v == 0) { x64_out("0"); return; }
    while (v > 0 && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i--) x64_putc(buf[i]);
}

/* ---- Multiboot info quick-glance ---------------------------------------- */

static void x64_dump_multiboot2_cmdline(uintptr_t info) {
    if (!info) return;
    const uint8_t* base = (const uint8_t*)info;
    uint32_t total_size = *(const uint32_t*)base;
    if (total_size < 8 || total_size > 0x100000) return;

    uintptr_t addr = info + 8;
    uintptr_t end  = info + total_size;
    while (addr + 8 <= end) {
        uint32_t type = *(const uint32_t*)addr;
        uint32_t size = *(const uint32_t*)(addr + 4);
        if (type == 0) break;
        if (size < 8 || addr + size > end) break;
        if (type == MULTIBOOT2_TAG_CMDLINE) {
            x64_out("multiboot2 cmdline: \"");
            x64_out((const char*)(addr + 8));
            x64_out("\"\n");
            return;
        }
        addr += (size + 7u) & ~7u;
    }
    x64_out("multiboot2 cmdline: (none)\n");
}

void x64_arch_dump_multiboot(uint32_t magic, uintptr_t info) {
    x64_out("[boot64] multiboot magic = ");
    x64_out_hex32(magic);
    if (magic == MULTIBOOT2_MAGIC)      x64_out(" (multiboot2)\n");
    else if (magic == MULTIBOOT_MAGIC)  x64_out(" (multiboot1)\n");
    else                                x64_out(" (UNKNOWN)\n");

    x64_out("[boot64] multiboot info  = ");
    x64_out_hex64((uint64_t)info);
    x64_out("\n");

    if (magic == MULTIBOOT2_MAGIC) {
        x64_dump_multiboot2_cmdline(info);
    }
}

/* ========================================================================
 * Multiboot2 framebuffer tag walk + serial diagnostics.
 *
 * Draw a tiny proof-of-life immediately after the trampoline hands control to
 * the kernel. On some laptops the firmware logo remains on screen until the
 * first GOP/LFB write; waiting for later driver stages makes a slow or wedged
 * hardware probe look like a display failure.
 * ======================================================================== */

struct goober_fb {
    uintptr_t addr;
    uint32_t  pitch;
    uint32_t  width;
    uint32_t  height;
    uint8_t   bpp;
    uint8_t   type;
    uint8_t   red_pos, red_size;
    uint8_t   green_pos, green_size;
    uint8_t   blue_pos, blue_size;
    int       have_color_info;
    int       present;
};

static struct goober_fb g_fb;

static void mb2_walk_framebuffer(uintptr_t info, struct goober_fb* out) {
    out->present = 0;
    out->have_color_info = 0;
    if (!info) return;
    const uint8_t* base = (const uint8_t*)info;
    uint32_t total_size = *(const uint32_t*)base;
    if (total_size < 8 || total_size > 0x100000) return;

    uintptr_t addr = info + 8;
    uintptr_t end  = info + total_size;
    while (addr + 8 <= end) {
        uint32_t type = *(const uint32_t*)addr;
        uint32_t size = *(const uint32_t*)(addr + 4);
        if (type == 0) break;
        if (size < 8 || addr + size > end) break;
        if (type == MULTIBOOT2_TAG_FRAMEBUFFER) {
            const uint8_t* body = (const uint8_t*)(addr + 8);
            uint32_t body_size = size - 8;
            uint64_t fb_addr = 0;
            for (int i = 0; i < 8; i++)
                fb_addr |= ((uint64_t)body[i]) << (i * 8);
            uint32_t fb_pitch  = (uint32_t)body[8]  | ((uint32_t)body[9]  << 8)
                               | ((uint32_t)body[10] << 16) | ((uint32_t)body[11] << 24);
            uint32_t fb_width  = (uint32_t)body[12] | ((uint32_t)body[13] << 8)
                               | ((uint32_t)body[14] << 16) | ((uint32_t)body[15] << 24);
            uint32_t fb_height = (uint32_t)body[16] | ((uint32_t)body[17] << 8)
                               | ((uint32_t)body[18] << 16) | ((uint32_t)body[19] << 24);
            uint8_t  fb_bpp    = body[20];
            uint8_t  fb_type   = body[21];

            out->addr    = (uintptr_t)fb_addr;
            out->pitch   = fb_pitch;
            out->width   = fb_width;
            out->height  = fb_height;
            out->bpp     = fb_bpp;
            out->type    = fb_type;
            out->present = 1;

            if (fb_type == 1 && body_size >= 30) {
                out->red_pos     = body[24];
                out->red_size    = body[25];
                out->green_pos   = body[26];
                out->green_size  = body[27];
                out->blue_pos    = body[28];
                out->blue_size   = body[29];
                out->have_color_info = 1;
            }
            return;
        }
        addr += (size + 7u) & ~7u;
    }
}

static void print_framebuffer_tag(const struct goober_fb* fb) {
    if (!fb->present) {
        x64_out("[fb] no multiboot2 framebuffer tag; skipping\n");
        return;
    }
    x64_out("[fb] mb2 framebuffer tag:\n");
    x64_out("[fb]   addr   = "); x64_out_hex64((uint64_t)fb->addr); x64_out("\n");
    x64_out("[fb]   pitch  = "); x64_out_hex32(fb->pitch);
    x64_out(" ("); x64_out_dec(fb->pitch); x64_out(" bytes/row)\n");
    x64_out("[fb]   width  = "); x64_out_hex32(fb->width);
    x64_out(" ("); x64_out_dec(fb->width); x64_out(" px)\n");
    x64_out("[fb]   height = "); x64_out_hex32(fb->height);
    x64_out(" ("); x64_out_dec(fb->height); x64_out(" px)\n");
    x64_out("[fb]   bpp    = "); x64_out_hex8(fb->bpp);
    x64_out(" ("); x64_out_dec(fb->bpp); x64_out(" bits/px)\n");
    x64_out("[fb]   type   = "); x64_out_hex8(fb->type);
    x64_out(" ("); x64_out(fb->type == 0 ? "indexed" :
                            fb->type == 1 ? "RGB" :
                            fb->type == 2 ? "EGA text" : "unknown");
    x64_out(")\n");
    if (fb->have_color_info) {
        x64_out("[fb]   color_info: R(pos=");
        x64_out_dec(fb->red_pos);   x64_out(",size="); x64_out_dec(fb->red_size);
        x64_out(") G(pos=");
        x64_out_dec(fb->green_pos); x64_out(",size="); x64_out_dec(fb->green_size);
        x64_out(") B(pos=");
        x64_out_dec(fb->blue_pos);  x64_out(",size="); x64_out_dec(fb->blue_size);
        x64_out(")\n");
    }
}

/* ---- channel-aware pixel packer + 8x16 console renderer ---------------- */

static uint32_t fb_pack_pixel(const struct goober_fb* fb, uint32_t xrgb) {
    uint32_t r = (xrgb >> 16) & 0xFFu;
    uint32_t g = (xrgb >>  8) & 0xFFu;
    uint32_t b = (xrgb      ) & 0xFFu;

    uint8_t r_pos, r_size, g_pos, g_size, b_pos, b_size;
    if (fb->have_color_info) {
        r_pos = fb->red_pos;   r_size = fb->red_size;
        g_pos = fb->green_pos; g_size = fb->green_size;
        b_pos = fb->blue_pos;  b_size = fb->blue_size;
    } else if (fb->bpp == 32 || fb->bpp == 24) {
        r_pos = 16; r_size = 8;
        g_pos = 8;  g_size = 8;
        b_pos = 0;  b_size = 8;
    } else if (fb->bpp == 16) {
        r_pos = 11; r_size = 5;
        g_pos = 5;  g_size = 6;
        b_pos = 0;  b_size = 5;
    } else {
        return 0;
    }
    uint32_t r_out = r_size ? ((r >> (8 - r_size)) << r_pos) : 0u;
    uint32_t g_out = g_size ? ((g >> (8 - g_size)) << g_pos) : 0u;
    uint32_t b_out = b_size ? ((b >> (8 - b_size)) << b_pos) : 0u;
    return r_out | g_out | b_out;
}

static inline void fb_write_pixel(volatile uint8_t* p,
                                  uint32_t pix,
                                  uint32_t bytes_per_pixel) {
    for (uint32_t i = 0; i < bytes_per_pixel; i++)
        p[i] = (uint8_t)((pix >> (i * 8u)) & 0xFFu);
}

static void fb_draw_band(const struct goober_fb* fb,
                         uint32_t x0, uint32_t y0,
                         uint32_t w, uint32_t h,
                         uint32_t xrgb) {
    uint32_t bytes_per_pixel = fb->bpp / 8u;
    if (bytes_per_pixel == 0 || bytes_per_pixel > 4) return;
    if (x0 >= fb->width || y0 >= fb->height) return;
    if (x0 + w > fb->width)  w = fb->width  - x0;
    if (y0 + h > fb->height) h = fb->height - y0;

    uint32_t pix = fb_pack_pixel(fb, xrgb);

    volatile uint8_t* row = (volatile uint8_t*)fb->addr
                          + (uintptr_t)y0 * (uintptr_t)fb->pitch;
    for (uint32_t y = 0; y < h; y++) {
        volatile uint8_t* p = row + (uintptr_t)x0 * (uintptr_t)bytes_per_pixel;
        for (uint32_t x = 0; x < w; x++) {
            fb_write_pixel(p, pix, bytes_per_pixel);
            p += bytes_per_pixel;
        }
        row += fb->pitch;
    }
}

/* ---- 8x16 on-panel proof-of-life (preserved from 3b.0) ------------------ */

static void fb_putc(int x, int y, char c, uint32_t fg, uint32_t bg) {
    if (!g_fb.present || g_fb.type != 1) return;
    uint32_t bpp = g_fb.bpp;
    if (bpp != 16 && bpp != 24 && bpp != 32 && bpp != 15) return;
    uint32_t bytes_per_pixel = bpp / 8u;
    if (bytes_per_pixel == 0) return;

    if (x < 0 || y < 0) return;
    if ((uint32_t)x + GOOBER_FONT_W > g_fb.width)  return;
    if ((uint32_t)y + GOOBER_FONT_H > g_fb.height) return;

    uint32_t fg_pix = fb_pack_pixel(&g_fb, fg);
    uint32_t bg_pix = fb_pack_pixel(&g_fb, bg);

    unsigned char ch = (unsigned char)c;
    const uint8_t* glyph = goober_font_8x16[ch];
    int solid_block = 0;
    if (ch < 0x20 || ch > 0x7E) solid_block = 1;

    volatile uint8_t* row = (volatile uint8_t*)g_fb.addr
                          + (uintptr_t)y * (uintptr_t)g_fb.pitch
                          + (uintptr_t)x * (uintptr_t)bytes_per_pixel;
    for (int gy = 0; gy < GOOBER_FONT_H; gy++) {
        uint8_t bits = solid_block ? 0xFFu : glyph[gy];
        volatile uint8_t* p = row;
        for (int gx = 0; gx < GOOBER_FONT_W; gx++) {
            uint32_t pix = (bits & (uint8_t)(0x80u >> gx)) ? fg_pix : bg_pix;
            fb_write_pixel(p, pix, bytes_per_pixel);
            p += bytes_per_pixel;
        }
        row += g_fb.pitch;
    }
}

static void fb_puts(int x, int y, const char* s, uint32_t fg, uint32_t bg) {
    if (!s) return;
    int cx = x;
    while (*s) {
        fb_putc(cx, y, *s, fg, bg);
        cx += GOOBER_FONT_W;
        s++;
    }
}

static void fb_print_at(int row, int col, const char* s,
                        uint32_t fg, uint32_t bg) {
    fb_puts(col * GOOBER_FONT_W, row * GOOBER_FONT_H, s, fg, bg);
}

static size_t fmt_u32_dec(char* dst, size_t cap, uint32_t v) {
    char tmp[11];
    int  i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    size_t n = 0;
    while (i > 0 && n + 1 < cap) dst[n++] = tmp[--i];
    if (n < cap) dst[n] = '\0';
    return n;
}

static size_t fmt_str(char* dst, size_t cap, const char* s) {
    /* Explicit early-return on cap==0 so gcc -Wstringop-overflow proves the
     * loop body never writes through a zero-room destination. Without this
     * guard gcc loses the n+1<cap invariant across inlined call chains. */
    if (cap == 0) return 0;
    size_t n = 0;
    while (*s && n + 1 < cap) dst[n++] = *s++;
    if (n < cap) dst[n] = '\0';
    return n;
}

static void fb_render_proof_of_life(const struct goober_fb* fb) {
    if (!fb->present || fb->type != 1) {
        x64_out("[fb] proof-of-life skipped: no usable RGB GOP\n");
        return;
    }
    if (fb->bpp != 16 && fb->bpp != 24 && fb->bpp != 32 && fb->bpp != 15) {
        x64_out("[fb] proof-of-life skipped: unsupported bpp\n");
        return;
    }

    const uint32_t WHITE = 0x00FFFFFFu;
    const uint32_t BLACK = 0x00000000u;

    fb_draw_band(fb, 0, 0, fb->width, 64, BLACK);
    fb_print_at(0, 0, "GooberOSx86 x64  Phase 3a.1", WHITE, BLACK);

    char line1[96];
    size_t n = 0;
    n += fmt_str(line1 + n, sizeof(line1) - n, "  GOP framebuffer: ");
    n += fmt_u32_dec(line1 + n, sizeof(line1) - n, fb->width);
    n += fmt_str(line1 + n, sizeof(line1) - n, "x");
    n += fmt_u32_dec(line1 + n, sizeof(line1) - n, fb->height);
    n += fmt_str(line1 + n, sizeof(line1) - n, " @ ");
    n += fmt_u32_dec(line1 + n, sizeof(line1) - n, fb->bpp);
    n += fmt_str(line1 + n, sizeof(line1) - n, "bpp, pitch=");
    n += fmt_u32_dec(line1 + n, sizeof(line1) - n, fb->pitch);
    if (n < sizeof(line1)) line1[n] = '\0';
    fb_print_at(1, 0, line1, WHITE, BLACK);

    /* Phase 3b: the orchestrator scrolls staged-boot text in below this
     * proof-of-life via the framebuffer console in kernel.c. The "Phase
     * 3b.0 pending." line is preserved verbatim per the worker brief so
     * the user sees a clean before/after when this ISO is flashed. */
    fb_print_at(2, 0, "  Phase 3b.0 pending.", WHITE, BLACK);

    x64_out("[fb] proof-of-life rendered to GOP at row 0..2\n");
}

void x64_arch_walk_and_draw_framebuffer(uintptr_t info) {
    /* Multiboot2 only on x64 (the trampoline rejects multiboot1 with no
     * framebuffer tag, so this is safe). */
    mb2_walk_framebuffer(info, &g_fb);
    print_framebuffer_tag(&g_fb);
    fb_render_proof_of_life(&g_fb);
}

/* ---- Legacy 0xB8000 line (legacy-BIOS only; harmless on UEFI) ----------- */

#define VGA_TEXT_BASE  ((volatile uint16_t*)0xB8000)
#define VGA_TEXT_COLS  80
#define VGA_TEXT_ROWS  25
#define VGA_TEXT_ATTR  0x0F

static uint32_t vga_text_col = 0;
static uint32_t vga_text_row = 0;

void x64_arch_legacy_vga_text_line(const char* s) {
    if (!s) return;
    while (*s) {
        char c = *s++;
        if (vga_text_row >= VGA_TEXT_ROWS) return;
        if (c == '\n') {
            vga_text_col = 0;
            vga_text_row++;
            continue;
        }
        if (vga_text_col >= VGA_TEXT_COLS) {
            vga_text_col = 0;
            vga_text_row++;
            if (vga_text_row >= VGA_TEXT_ROWS) return;
        }
        uint16_t cell = ((uint16_t)VGA_TEXT_ATTR << 8) | (uint8_t)c;
        VGA_TEXT_BASE[vga_text_row * VGA_TEXT_COLS + vga_text_col] = cell;
        vga_text_col++;
    }
    x64_out("[vga] wrote proof-of-life line to 0xB8000 (legacy BIOS only)\n");
}

/* ========================================================================
 * 64-bit IDT (256 gates).
 * ======================================================================== */

typedef struct __attribute__((packed)) {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t reserved;
} idt64_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} idt64_pointer_t;

/* Placed in .idt (linker64.ld) BEFORE the large .bss heap arena so a
 * kmalloc overrun cannot wipe interrupt gates. */
static idt64_entry_t   g_idt[256]
    __attribute__((section(".idt"), aligned(16)));
static idt64_pointer_t g_idt_ptr
    __attribute__((section(".idt")));

extern void load_idt64(idt64_pointer_t* ptr);

extern void isr0_stub(void);  extern void isr1_stub(void);  extern void isr2_stub(void);  extern void isr3_stub(void);
extern void isr4_stub(void);  extern void isr5_stub(void);  extern void isr6_stub(void);  extern void isr7_stub(void);
extern void isr8_stub(void);  extern void isr9_stub(void);  extern void isr10_stub(void); extern void isr11_stub(void);
extern void isr12_stub(void); extern void isr13_stub(void); extern void isr14_stub(void); extern void isr15_stub(void);
extern void isr16_stub(void); extern void isr17_stub(void); extern void isr18_stub(void); extern void isr19_stub(void);
extern void isr20_stub(void); extern void isr21_stub(void); extern void isr22_stub(void); extern void isr23_stub(void);
extern void isr24_stub(void); extern void isr25_stub(void); extern void isr26_stub(void); extern void isr27_stub(void);
extern void isr28_stub(void); extern void isr29_stub(void); extern void isr30_stub(void); extern void isr31_stub(void);

extern void isr32_stub(void);            /* IRQ0 timer */
extern void irq1_handler_asm(void);      /* IRQ1 keyboard */
extern void irq12_handler_asm(void);     /* IRQ12 mouse */
extern void irq2_spurious_asm(void);
extern void irq3_spurious_asm(void);
extern void irq4_spurious_asm(void);
extern void irq5_spurious_asm(void);
extern void irq6_spurious_asm(void);
extern void irq7_spurious_asm(void);
extern void irq8_spurious_asm(void);
extern void irq9_spurious_asm(void);
extern void irq10_spurious_asm(void);
extern void irq11_spurious_asm(void);
extern void irq13_spurious_asm(void);
extern void irq14_spurious_asm(void);
extern void irq15_spurious_asm(void);

static void (*const exception_stubs64[32])(void) = {
    isr0_stub,  isr1_stub,  isr2_stub,  isr3_stub,
    isr4_stub,  isr5_stub,  isr6_stub,  isr7_stub,
    isr8_stub,  isr9_stub,  isr10_stub, isr11_stub,
    isr12_stub, isr13_stub, isr14_stub, isr15_stub,
    isr16_stub, isr17_stub, isr18_stub, isr19_stub,
    isr20_stub, isr21_stub, isr22_stub, isr23_stub,
    isr24_stub, isr25_stub, isr26_stub, isr27_stub,
    isr28_stub, isr29_stub, isr30_stub, isr31_stub,
};

static void set_idt_entry64(int vec, void (*handler)(void), uint16_t selector,
                            uint8_t ist, uint8_t type_attr) {
    uint64_t addr = (uint64_t)(uintptr_t)handler;
    g_idt[vec].offset_lo  = (uint16_t)(addr & 0xFFFF);
    g_idt[vec].selector   = selector;
    g_idt[vec].ist        = ist & 0x07;
    g_idt[vec].type_attr  = type_attr;
    g_idt[vec].offset_mid = (uint16_t)((addr >> 16) & 0xFFFF);
    g_idt[vec].offset_hi  = (uint32_t)((addr >> 32) & 0xFFFFFFFFu);
    g_idt[vec].reserved   = 0;
}

void x64_arch_idt_install(void) {
    for (int i = 0; i < 256; i++)
        set_idt_entry64(i, (void (*)(void))0, 0x08, 0, 0x00);
    for (int v = 0; v < 32; v++)
        set_idt_entry64(v, exception_stubs64[v], 0x08, 0, 0x8E);
    set_idt_entry64(0x20, isr32_stub,        0x08, 0, 0x8E);  /* IRQ0 timer */
    set_idt_entry64(0x21, irq1_handler_asm,  0x08, 0, 0x8E);  /* IRQ1 kbd */
    set_idt_entry64(0x22, irq2_spurious_asm, 0x08, 0, 0x8E);
    set_idt_entry64(0x23, irq3_spurious_asm, 0x08, 0, 0x8E);
    set_idt_entry64(0x24, irq4_spurious_asm, 0x08, 0, 0x8E);
    set_idt_entry64(0x25, irq5_spurious_asm, 0x08, 0, 0x8E);
    set_idt_entry64(0x26, irq6_spurious_asm, 0x08, 0, 0x8E);
    set_idt_entry64(0x27, irq7_spurious_asm, 0x08, 0, 0x8E);  /* IRQ7 spurious */
    set_idt_entry64(0x28, irq8_spurious_asm, 0x08, 0, 0x8E);
    set_idt_entry64(0x29, irq9_spurious_asm, 0x08, 0, 0x8E);
    set_idt_entry64(0x2A, irq10_spurious_asm, 0x08, 0, 0x8E);
    set_idt_entry64(0x2B, irq11_spurious_asm, 0x08, 0, 0x8E);
    set_idt_entry64(0x2C, irq12_handler_asm, 0x08, 0, 0x8E);  /* IRQ12 mouse */
    set_idt_entry64(0x2D, irq13_spurious_asm, 0x08, 0, 0x8E);
    set_idt_entry64(0x2E, irq14_spurious_asm, 0x08, 0, 0x8E);
    set_idt_entry64(0x2F, irq15_spurious_asm, 0x08, 0, 0x8E); /* IRQ15 spurious */

    g_idt_ptr.limit = (uint16_t)(sizeof(g_idt) - 1);
    g_idt_ptr.base  = (uint64_t)(uintptr_t)&g_idt[0];
    load_idt64(&g_idt_ptr);
    x64_out("[phase3b] x64 IDT installed (256 gates, 32 exception stubs + "
            "full PIC 0x20..0x2F coverage).\n");
}

/* DPL=3 interrupt gate so ring-3 may execute `int 0x80`. */
void x64_idt_set_user_gate(int vec, void (*handler)(void)) {
    if (vec < 0 || vec >= 256 || !handler) return;
    set_idt_entry64(vec, handler, 0x08, 0, 0xEE); /* present, DPL=3, interrupt gate */
}

/* ========================================================================
 * Legacy PIC (8259) remap.
 *
 * Master -> vectors 0x20..0x27, slave -> 0x28..0x2F. After remap we mask
 * everything except IRQ0 (timer, PIT), IRQ1 (keyboard) and IRQ12 (mouse, via
 * the slave cascade IRQ2). This is the same mask state the 3b.0 milestone
 * used, so a regression in IRQ delivery is immediately visible.
 *
 * The 32-bit kernel.c::pic_remap() saves and restores the firmware mask
 * state instead. Both are correct; the x64 build picks the stricter set
 * because nothing in 3b proper expects spurious IRQs from devices not yet
 * brought up.
 * ======================================================================== */
void x64_arch_pic_remap(void) {
    uint8_t a1 = inb(0x21);
    uint8_t a2 = inb(0xA1);
    (void)a1; (void)a2;

    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 4);
    outb(0xA1, 2);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);

    /* master: enable IRQ0,1,2 -> mask = 0xF8 */
    outb(0x21, 0xF8);
    /*
     * Keep the entire slave PIC masked (including IRQ12) until mouse_init().
     * Acer BIOS "Basic" touchpad mode uses PS/2 AUX; mouse_init unmasks IRQ12
     * only after a successful AUX enable. Spurious slave IRQs before that can
     * preempt boot. USB HID remains a separate path.
     */
    outb(0xA1, 0xFF);
    x64_out("[phase3b] PIC remapped: master=0x20..0x27 (IRQ0/1/2 unmasked), "
            "slave=0x28..0x2F (all masked; IRQ12 deferred to mouse_init).\n");
}

/* ========================================================================
 * Long-mode IRQ handler bodies (called from isr32_stub64.s / irq1_wrapper64.s
 * / irq12_wrapper64.s). The IRQ wrappers do their own PIC EOI, so the C
 * bodies can stay narrow.
 *
 *   IRQ0  (timer)    -> timer_interrupt_handler() (drivers/timer/timer.c).
 *                       That helper emits the master PIC EOI itself; the
 *                       isr32_stub64.s wrapper does NOT add an EOI.
 *
 *   IRQ1  (keyboard) -> keyboard_interrupt_handler() (drivers/keyboard/
 *                       keyboard.c). irq1_wrapper64.s acks the master PIC.
 *                       The thin shim irq1_handler_main forwards into the
 *                       real driver and stays compatible with the existing
 *                       irq1_wrapper64.s `extern irq1_handler_main` symbol.
 *
 *   IRQ12 (mouse)    -> mouse_handler_main() (drivers/mouse/mouse.c). The
 *                       linker now resolves that call to the real driver.
 *                       irq12_wrapper64.s acks the slave + master PIC.
 * ======================================================================== */

extern void keyboard_interrupt_handler(void);

void irq0_handler_main(void) {
    timer_interrupt_handler();
    boot_watchdog_tick();
}

/*
 * Phase 3c: forward IRQ1 directly into drivers/keyboard/keyboard.c. Kept as
 * a forwarder rather than renaming the wrapper's extern so the ASM stays
 * stable. The wrapper does the master-PIC EOI; the real keyboard handler
 * only reads scancode + updates the ring buffer, no I/O port acks.
 */
void irq1_handler_main(void) {
    keyboard_interrupt_handler();
}

/* ========================================================================
 * Phase 3d: drivers/pci/pci.c is now linked under -m64, so the placeholder
 * pci_find_display_controllers / pci_find_usb_controllers stubs that lived
 * here for Phase 3b/3c are gone. boot_safety.c::boot_print_hardware_summary,
 * native_fb.c::bochs_lfb_base, and intel_gfx.c::intel_gfx_detect now resolve
 * to the real PCI scan from drivers/pci/pci.c. The xHCI Intel-PCH USB2 port-
 * routing path also uses the real pci_find_usb_controllers() to gate the
 * XUSB2PR / USB3_PSSEN writes on EHCI-companion presence (skipped on Bay
 * Trail's xHCI-only SoC, just like the x86 build).
 * ======================================================================== */

/* ========================================================================
 * Phase 3f: the Phase 3e taskmgr/process.c stub block has been removed.
 * taskmgr/process.c is now linked into the x64 build (see scripts/build-
 * x64.sh) and provides the real registry. kernel.c::register_kernel_
 * process_x64() (called from kernel_main on x64) seeds pid=1="kernel.bin"
 * so process_is_protected(1) keeps the kernel entry unkillable, matching
 * x86 behaviour.
 * ======================================================================== */
