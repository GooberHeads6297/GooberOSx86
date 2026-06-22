#include "kernel.h"
#include <stdint.h>
#include <stddef.h>
#include "drivers/timer/timer.h"
#include "drivers/io/io.h"
#include "drivers/video/vga.h"
#include "drivers/video/vesa.h"
#include "drivers/video/display.h"
#include "drivers/video/native_fb.h"
#include "drivers/video/intel_gfx.h"
#include "drivers/video/textcon.h"
#include "drivers/video/font.h"
#include "drivers/pci/pci.h"
#include "lib/string.h"
#include "boot_safety.h"
#include "cpu_exception_names.h"

/*
 * Phase 3b/3c: kernel.c is the unified staged-boot orchestrator for BOTH the
 * x86 (i386) and x86_64 (long-mode) builds. The two builds share:
 *   - boot_config_parse / boot_config_parse_multiboot
 *   - record_loader_framebuffer / kernel_fb_* accessors
 *   - framebuffer_bringup / display_confirm_visible / report_display_diagnostics
 *   - display_on_panel_confirm (Phase 3c: now compiled for both arches; the
 *     x64 keyboard driver is live)
 *   - input_init / keyboard_init / mouse_init (Phase 3c)
 *   - print() (routed through the active print sink to VGA + COM1)
 *   - the staged-boot orchestrator (boot_run_stages_table)
 *   - the CPU-exception handler entry point (the body differs by ABI;
 *     gated by __i386__ / __x86_64__)
 *
 * The driver stack that is still x86-only at this round (Phase 3e/3f):
 *   drivers/storage, fs, shell/shell.c, taskmgr, gui, editor, games.
 *
 * Phase 3d brings drivers/pci/pci.c (real PCI scan) and the entire USB host
 * stack (uhci/ohci/ehci/xhci + host.c + enumeration.c + hid.c + usb.c) into
 * the unified link. The x64 boot stage `USB host stack` invokes usb_init()
 * under boot_guarded_run() with a 12-second watchdog (WD_USB = 1200 ticks
 * at 100 Hz), so a wedged controller is contained and the boot continues
 * to Display + REPL.
 */
#include "drivers/keyboard/keyboard.h"
#include "drivers/mouse/mouse.h"
#include "drivers/input/input.h"
#include "drivers/usb/usb.h"
#include "drivers/acpi/acpi.h"
#include "drivers/input/touchpad.h"
#ifdef __i386__
#include "drivers/storage/storage.h"
#include "taskmgr/process.h"
#endif
/* Phase 3e brings the heap online on x64 too -- the new Filesystem and
 * Shell / desktop boot stages run fs_init() / vesa_desktop_init() which
 * both kmalloc() small structures. The include needs to be unconditional
 * (the bump allocator is arch-agnostic). */
#include "lib/memory.h"

#define IRQ0 32
#define IRQ1 33

#define KERNEL_HEAP_SIZE (64 * 1024)  // 64KB heap, adjust as needed
#define VESA_STATIC_BACKBUFFER_BYTES (8 * 1024 * 1024)

#ifdef __i386__
volatile int keyboard_interrupt_flag = 0;
#endif

extern unsigned char _kernel_start;
extern unsigned char _kernel_end;



#ifdef __i386__
static int kernel_pid = -1;
static uint8_t vesa_static_backbuffer[VESA_STATIC_BACKBUFFER_BYTES] __attribute__((aligned(4096)));
#endif

/* Boot mode: 0 = VGA text mode, 1 = VESA framebuffer mode */
static int boot_mode_vesa = 0;
static const char* vesa_reject_reason = NULL;

/* Last framebuffer info reported by the loader, even when VESA is rejected.
 *
 * `last_fb_addr` is `uintptr_t` (Phase 3b pointer-width audit) so the
 * captured base survives 64-bit kernels end-to-end. On x86 it is
 * byte-equivalent to the previous `uint32_t` field. */
static uint8_t last_fb_type = 0xFF;
static uint8_t last_fb_bpp = 0;
static uint32_t last_fb_pitch = 0;
static uint32_t last_fb_width = 0;
static uint32_t last_fb_height = 0;
static uintptr_t last_fb_addr = 0;

/*
 * Unified boot configuration, parsed once in boot_config_parse(). Every
 * gooberos.* switch lives here so drivers can consult one struct instead of
 * re-walking the cmdline. gooberos.display= values: "auto" (try every driver
 * in priority order), "vesa" (inherited GRUB LFB only), "bochs" (Bochs/QEMU
 * dispi only), or "off" (force VGA text). gooberos.boot= values: "vesa-auto",
 * "vga", or "default".
 */
static boot_config_t g_boot_config = {
    .cmdline = "",
    .boot    = "default",
    .display = "auto",
    .usb     = "",
    .i2c     = "",
    .touchpad = "",
    .theme   = "",
    .native  = "",
    .safe    = 0,
    .display_confirm = BOOT_DISPLAY_CONFIRM_DEFAULT,
    .display_fps = 0,
    .usb_hotplug = 1,  /* default: hot-plug enabled when USB stack is up */
};

const boot_config_t* boot_get_config(void) { return &g_boot_config; }
int boot_safe_mode(void) { return g_boot_config.safe; }

int kernel_display_target_fps(void) {
    int fps = g_boot_config.display_fps;
    if (fps <= 0) return 60;
    return fps;
}

/*
 * Phase 4 VGA-graphics fallback flag. Toggled inside framebuffer_bringup()
 * when the display framework commits to the mode-13h "vga-graphics" rung
 * (see drivers/video/vga.c). Defaults to 0 (VESA/native LFB path).
 */
static int boot_mode_vga_graphics = 0;
int kernel_display_is_vga_graphics(void) { return boot_mode_vga_graphics; }

/*
 * Text-console boot flag (x64 VGA compatibility). Set inside
 * framebuffer_bringup() when the display stage commits to the 80x25 text
 * console rather than any graphical surface -- either explicitly
 * (gooberos.boot=vga / gooberos.display=vga-text) or as the last-resort
 * fallback when every other rung was rejected. When set, the x64 main
 * loop runs the full interactive text shell instead of the VESA desktop;
 * the x86 path is unaffected (it always has a working VGA text mode under
 * BIOS so the flag is set but the shell/desktop dispatch in kernel_main
 * stays the same).
 */
static int boot_mode_text_console = 0;
int kernel_display_is_text_console(void) { return boot_mode_text_console; }

int is_vesa_mode(void) {
    return boot_mode_vesa;
}

const char* vesa_boot_status(void) {
    if (boot_mode_vesa) return "VESA framebuffer accepted";
    return vesa_reject_reason ? vesa_reject_reason : "VESA not requested";
}

const char* kernel_boot_request(void) { return g_boot_config.boot; }
const char* kernel_boot_cmdline(void) { return g_boot_config.cmdline; }
uint8_t kernel_fb_type(void)   { return last_fb_type; }
uint8_t kernel_fb_bpp(void)    { return last_fb_bpp; }
uint32_t kernel_fb_pitch(void) { return last_fb_pitch; }
uint32_t kernel_fb_width(void) { return last_fb_width; }
uint32_t kernel_fb_height(void){ return last_fb_height; }
uintptr_t kernel_fb_addr(void) { return last_fb_addr; }

/*
 * Record the framebuffer the bootloader reported for diagnostics, even when we
 * later decide not to use it. Validation/acceptance now happens inside the
 * display framework's "vesa" (simple-framebuffer) driver; see
 * drivers/video/native_fb.c.
 */
static void record_loader_framebuffer(uint64_t addr, uint32_t width, uint32_t height,
                                      uint32_t pitch, uint8_t bpp, uint8_t type) {
    last_fb_type = type;
    last_fb_bpp = bpp;
    last_fb_pitch = pitch;
    last_fb_width = width;
    last_fb_height = height;
    /*
     * Phase 3b widening: on x86_64 `uintptr_t` is 64 bits, so the full GOP
     * base survives the capture. On x86 (32-bit) `uintptr_t` is 32 bits and
     * a > 4 GiB framebuffer is unreachable, so we surface 0 in that case to
     * make diagnostics obvious.
     */
    if (sizeof(uintptr_t) < sizeof(uint64_t) && ((addr >> 32) != 0)) {
        last_fb_addr = 0;
    } else {
        last_fb_addr = (uintptr_t)addr;
    }
}

/* Local string helpers (we can't pull in libc here). */
static int kstr_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int kstr_starts(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++; prefix++;
    }
    return 1;
}

/* Check whether the parsed kernel cmdline contains a given token (whole-
 * token match against whitespace-separated arguments). Used by the x64
 * boot orchestrator to honor `gooberos.selftest=1` without having to
 * widen boot_config_t for a one-off cmdline switch. */
static int kcmdline_contains(const char* tok) {
    const char* s = g_boot_config.cmdline;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (kstr_starts(s, tok)) {
            const char* end = s;
            const char* t = tok;
            while (*t) { end++; t++; }
            if (*end == '\0' || *end == ' ' || *end == '\t') return 1;
        }
        while (*s && *s != ' ' && *s != '\t') s++;
    }
    return 0;
}

static void kstr_copy(char* dst, const char* src, uint32_t max) {
    uint32_t i = 0;
    if (!dst || !src || max == 0) return;
    for (; i < max - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* Copy the value following a "key=" token into dst (stops at whitespace). */
static void kcopy_token_value(const char* src, char* dst, uint32_t max) {
    uint32_t i = 0;
    while (src[i] && src[i] != ' ' && src[i] != '\t' && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/*
 * Parse the kernel command line once into the unified boot_config. Recognizes
 * every gooberos.* switch including the master gooberos.safe= compatibility
 * switch. Called from boot_config_parse_multiboot() during early bootstrap,
 * before any staged init runs, so boot_safe_mode() is valid for the
 * orchestrator.
 */
static void boot_config_parse(const char* cmdline) {
    /* Reset to defaults so a re-parse is idempotent. */
    kstr_copy(g_boot_config.boot, "default", sizeof(g_boot_config.boot));
    kstr_copy(g_boot_config.display, "auto", sizeof(g_boot_config.display));
    g_boot_config.usb[0] = '\0';
    g_boot_config.i2c[0] = '\0';
    g_boot_config.touchpad[0] = '\0';
    g_boot_config.theme[0] = '\0';
    g_boot_config.native[0] = '\0';
    g_boot_config.safe = 0;
    g_boot_config.display_confirm = BOOT_DISPLAY_CONFIRM_DEFAULT;
    g_boot_config.display_fps = 0;
    g_boot_config.usb_hotplug = 1;  /* default: hot-plug enabled */
    g_boot_config.cmdline[0] = '\0';

    if (!cmdline) return;
    kstr_copy(g_boot_config.cmdline, cmdline, sizeof(g_boot_config.cmdline));

    const char* p = cmdline;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (kstr_starts(p, "gooberos.boot=")) {
            kcopy_token_value(p + 14, g_boot_config.boot, sizeof(g_boot_config.boot));
        } else if (kstr_starts(p, "gooberos.display.confirm=")) {
            /*
             * Phase 3f: explicit override for the on-panel confirm-or-revert
             * gate. `skip` -> never run the prompt this boot (CI / unattended
             * QEMU); `force` -> always run it (useful for x86 boots where it
             * would otherwise be skipped); any other value -> default.
             */
            char v[16];
            kcopy_token_value(p + 25, v, sizeof(v));
            if (kstr_eq(v, "skip"))
                g_boot_config.display_confirm = BOOT_DISPLAY_CONFIRM_SKIP;
            else if (kstr_eq(v, "force"))
                g_boot_config.display_confirm = BOOT_DISPLAY_CONFIRM_FORCE;
            else
                g_boot_config.display_confirm = BOOT_DISPLAY_CONFIRM_DEFAULT;
        } else if (kstr_starts(p, "gooberos.display.fps=")) {
            /*
             * Phase 4 (display polish): user-tunable target frame rate for
             * the back-buffer + whole-screen present loop. Clamp to a sane
             * 10..120 Hz so a malformed value can't disable the desktop
             * pump entirely. The default (0 / unset) means "60 Hz".
             */
            char v[8];
            kcopy_token_value(p + 21, v, sizeof(v));
            int fps = 0;
            for (int i = 0; v[i] >= '0' && v[i] <= '9'; i++)
                fps = fps * 10 + (v[i] - '0');
            if (fps < 10)  fps = 10;
            if (fps > 120) fps = 120;
            g_boot_config.display_fps = fps;
        } else if (kstr_starts(p, "gooberos.display=")) {
            kcopy_token_value(p + 17, g_boot_config.display, sizeof(g_boot_config.display));
        } else if (kstr_starts(p, "gooberos.usb.hotplug=")) {
            /*
             * USB-mouse + keyboard hot-plug toggle. Default = on; set to
             * "off" to disable the post-boot port-status-change polling
             * (boot-time enumeration is unaffected). Useful as a
             * diagnostic switch on chipsets where touching PORTSC every
             * 50 ticks itself triggers hardware quirks.
             */
            char v[8];
            kcopy_token_value(p + 21, v, sizeof(v));
            g_boot_config.usb_hotplug = (kstr_eq(v, "off") || kstr_eq(v, "0") ||
                                         kstr_eq(v, "false") || kstr_eq(v, "no"))
                                            ? 0 : 1;
        } else if (kstr_starts(p, "gooberos.usb=")) {
            kcopy_token_value(p + 13, g_boot_config.usb, sizeof(g_boot_config.usb));
        } else if (kstr_starts(p, "gooberos.i2c=")) {
            kcopy_token_value(p + 13, g_boot_config.i2c, sizeof(g_boot_config.i2c));
        } else if (kstr_starts(p, "gooberos.touchpad=")) {
            kcopy_token_value(p + 18, g_boot_config.touchpad, sizeof(g_boot_config.touchpad));
        } else if (kstr_starts(p, "gooberos.theme=")) {
            kcopy_token_value(p + 15, g_boot_config.theme, sizeof(g_boot_config.theme));
        } else if (kstr_starts(p, "gooberos.native=")) {
            kcopy_token_value(p + 16, g_boot_config.native, sizeof(g_boot_config.native));
        } else if (kstr_starts(p, "gooberos.safe=")) {
            char v[8];
            kcopy_token_value(p + 14, v, sizeof(v));
            g_boot_config.safe = (kstr_eq(v, "1") || kstr_eq(v, "on") ||
                                  kstr_eq(v, "true") || kstr_eq(v, "yes")) ? 1 : 0;
        }
        while (*p && *p != ' ' && *p != '\t') p++;
    }
}

/*
 * Framebuffer info the bootloader described, captured during bootstrap by
 * boot_config_parse_multiboot() and consumed later by the (guarded)
 * framebuffer_bringup() display stage. Held in statics so the cmdline parse
 * (which decides safe mode) can run before any risky hardware bring-up.
 */
static uint64_t saved_fb_addr = 0;
static uint32_t saved_fb_w = 0, saved_fb_h = 0, saved_fb_pitch = 0;
static uint8_t  saved_fb_bpp = 0, saved_fb_type = 0xFF;
static int      saved_have_loader_fb = 0;

/*
 * Bootstrap step (NOT guarded): walk multiboot1/2 info, extract the cmdline +
 * any inherited framebuffer, and parse the unified boot config. This is pure
 * memory walking over the loader-provided info block and must complete before
 * the staged orchestrator runs so boot_safe_mode() is known.
 */
static void boot_config_parse_multiboot(uint32_t magic, multiboot_info_t* mb_info) {
    multiboot2_tag_framebuffer_t* mb2_fb = NULL;
    const char* mb_cmdline = NULL;

    if (magic == MULTIBOOT2_MAGIC && mb_info) {
        /* Phase 3b widening: walk the multiboot2 tag chain using `uintptr_t`
         * so the same code is correct on x86_64 (where the multiboot info
         * pointer is 64-bit, identity-mapped in the low 4 GiB by boot64.s). */
        uint32_t total_size = *((uint32_t*)mb_info);
        uintptr_t addr = (uintptr_t)mb_info + 8;
        uintptr_t end = (uintptr_t)mb_info + total_size;
        while (addr + sizeof(multiboot2_tag_t) <= end) {
            multiboot2_tag_t* tag = (multiboot2_tag_t*)addr;
            if (tag->type == MULTIBOOT2_TAG_END) break;
            if (tag->size < sizeof(multiboot2_tag_t) || addr + tag->size > end) {
                vesa_reject_reason = "VESA disabled: malformed multiboot2 tag chain.\n";
                break;
            }
            if (tag->type == MULTIBOOT2_TAG_CMDLINE) {
                multiboot2_tag_cmdline_t* c = (multiboot2_tag_cmdline_t*)tag;
                mb_cmdline = c->string;
            } else if (tag->type == MULTIBOOT2_TAG_FRAMEBUFFER) {
                mb2_fb = (multiboot2_tag_framebuffer_t*)tag;
            }
            addr += (tag->size + 7) & ~7u;
        }
    } else if (magic == MULTIBOOT_MAGIC && mb_info) {
        if (mb_info->flags & MULTIBOOT_INFO_CMDLINE) {
            mb_cmdline = (const char*)(uintptr_t)mb_info->cmdline;
        }
    }

    boot_config_parse(mb_cmdline);

    /* Collect whatever framebuffer the bootloader described (if any). */
    saved_fb_addr = 0;
    saved_fb_w = saved_fb_h = saved_fb_pitch = 0;
    saved_fb_bpp = 0; saved_fb_type = 0xFF;
    saved_have_loader_fb = 0;

    if (magic == MULTIBOOT2_MAGIC) {
        if (mb2_fb) {
            saved_fb_addr = mb2_fb->framebuffer_addr;
            saved_fb_w = mb2_fb->framebuffer_width;
            saved_fb_h = mb2_fb->framebuffer_height;
            saved_fb_pitch = mb2_fb->framebuffer_pitch;
            saved_fb_bpp = mb2_fb->framebuffer_bpp;
            saved_fb_type = mb2_fb->framebuffer_type;
            saved_have_loader_fb = 1;
        }
    } else if (magic == MULTIBOOT_MAGIC && mb_info) {
        if (mb_info->flags & MULTIBOOT_INFO_FRAMEBUFFER) {
            saved_fb_addr = mb_info->framebuffer_addr;
            saved_fb_w = mb_info->framebuffer_width;
            saved_fb_h = mb_info->framebuffer_height;
            saved_fb_pitch = mb_info->framebuffer_pitch;
            saved_fb_bpp = mb_info->framebuffer_bpp;
            saved_fb_type = mb_info->framebuffer_type;
            saved_have_loader_fb = 1;
        }
    } else {
        vesa_reject_reason = "VESA disabled: unrecognized bootloader magic.\n";
    }

    if (saved_have_loader_fb) {
        record_loader_framebuffer(saved_fb_addr, saved_fb_w, saved_fb_h,
                                  saved_fb_pitch, saved_fb_bpp, saved_fb_type);
    }
}

/* Forward declarations for the serial boot log (defined later in this file). */
static void serial_out(const char* s);
static void serial_out_hex(uint32_t v);
static void serial_out_hex64(uint64_t v);

/* Parse a "WxH" geometry string (e.g. "1366x768"). Returns 1 and fills the w
 * and h out-params on success, 0 on a malformed/empty string. */
static int parse_wxh(const char* s, uint32_t* w, uint32_t* h) {
    uint32_t a = 0, b = 0;
    int seen = 0;
    if (!s || !*s) return 0;
    while (*s >= '0' && *s <= '9') { a = a * 10u + (uint32_t)(*s - '0'); s++; seen = 1; }
    if (!seen || (*s != 'x' && *s != 'X')) return 0;
    s++;
    seen = 0;
    while (*s >= '0' && *s <= '9') { b = b * 10u + (uint32_t)(*s - '0'); s++; seen = 1; }
    if (!seen) return 0;
    if (w) *w = a;
    if (h) *h = b;
    return 1;
}

/*
 * Heuristic (a): write a known pattern to a handful of scattered pixels of the
 * REAL framebuffer and read them back. If any read-back mismatches, the
 * physical framebuffer address is wrong/unmapped (or write-only), so the buffer
 * is not safely usable. Returns 1 if every sample round-trips, 0 otherwise.
 *
 * This writes through the framebuffer's physical (identity-mapped) address
 * directly; the subsequent splash clears the screen anyway.
 */
static int fb_readback_ok(uintptr_t addr, uint32_t w, uint32_t h,
                          uint32_t pitch, uint8_t bpp) {
    uint32_t bpx;
    if (!addr || !w || !h || !pitch) return 0;
    if (bpp == 32) bpx = 4;
    else if (bpp == 24) bpx = 3;
    else if (bpp == 16 || bpp == 15) bpx = 2;
    else if (bpp == 8) bpx = 1;            /* Phase 4 (item 3): VGA mode-13h */
    else return 0;

    volatile uint8_t* fb = (volatile uint8_t*)addr;
    const uint32_t patt[5] = { 0xA5A5A5A5u, 0x5A5A5A5Au, 0xDEADBEEFu,
                               0x12345678u, 0xCAFEBABEu };
    const uint32_t xs[5] = { 0, w / 2u, w - 1u, w - 1u, 0 };
    const uint32_t ys[5] = { 0, h / 2u, h - 1u, 0, h - 1u };

    for (int i = 0; i < 5; i++) {
        volatile uint8_t* p = fb + (uint32_t)ys[i] * pitch + (uint32_t)xs[i] * bpx;
        if (bpp == 32) {
            *(volatile uint32_t*)p = patt[i];
            if (*(volatile uint32_t*)p != patt[i]) return 0;
        } else if (bpp == 16 || bpp == 15) {
            uint16_t v = (uint16_t)patt[i];
            *(volatile uint16_t*)p = v;
            if (*(volatile uint16_t*)p != v) return 0;
        } else if (bpp == 8) {
            /*
             * Phase 4 (item 3): 8bpp indexed (VGA mode-13h). A single
             * palette-index byte; the round-trip proves 0xA0000 is
             * actually backed by the VGA scanout aperture (vs. mapped
             * to nothing or a stale legacy region).
             */
            uint8_t v = (uint8_t)patt[i];
            *p = v;
            if (*p != v) return 0;
        } else { /* 24 bpp */
            uint8_t b0 = (uint8_t)patt[i];
            uint8_t b1 = (uint8_t)(patt[i] >> 8);
            uint8_t b2 = (uint8_t)(patt[i] >> 16);
            p[0] = b0; p[1] = b1; p[2] = b2;
            if (p[0] != b0 || p[1] != b1 || p[2] != b2) return 0;
        }
    }
    return 1;
}

/*
 * display_confirm_visible(): the per-rung visibility gate. Called by the
 * display framework (as a display_confirm_fn) AFTER a driver's init() produced
 * a candidate framebuffer but BEFORE we commit to it. Combines:
 *
 *   (a) a scattered-pixel write/read-back of the real framebuffer (proves the
 *       physical address is actually backed by the scanout buffer),
 *   (b) when an Intel GPU is present: PIPECONF_ENABLE on the pipe whose source
 *       size matches the FB, plus a PIPEFRAMECOUNT delta across a bounded settle
 *       (proves a pipe is actively scanning out -- a dead pipe is a hard reject),
 *   (c) read-only PP_STATUS/PP_CONTROL + backlight PWM enable: if panel power
 *       AND backlight both clearly report OFF, the panel would be dark even
 *       though the FB is valid, so we prefer fallback (loudly logged).
 *
 * Returns 1 to accept the framebuffer, 0 to reject it (framework then abandons
 * this rung and continues down the ladder; the ultimate floor is VGA text).
 * Every reason is logged to the serial boot log via the shared serial plumbing.
 */
static int display_confirm_visible(const display_driver_ops_t* drv,
                                   const display_framebuffer_t* fb, void* ctx) {
    char b[16];
    (void)ctx;
    if (!fb) return 0;

    serial_out("[display] confirm: driver '");
    serial_out(drv ? drv->name : "?");
    serial_out("' fb=");
    /* `framebuffer_addr` is `uintptr_t` (Phase 3b widening); on x86 that's
     * 32 bits and serial_out_hex64 zero-extends, on x86_64 the full 64-bit
     * GOP base survives. The Lenovo / OVMF GOP bases tested are sub-4 GiB
     * but a > 4 GiB FB would now be visible in the diagnostic. */
    serial_out_hex64((uint64_t)fb->framebuffer_addr);
    serial_out(" ");
    itoa((int)fb->width, b, 10);  serial_out(b); serial_out("x");
    itoa((int)fb->height, b, 10); serial_out(b); serial_out("x");
    itoa((int)fb->bpp, b, 10);    serial_out(b); serial_out("\n");

    /* (a) real-framebuffer pixel round-trip. */
    if (!fb_readback_ok(fb->framebuffer_addr, fb->width, fb->height,
                        fb->pitch, fb->bpp)) {
        serial_out("[display] confirm REJECT: framebuffer pixel read-back mismatch "
                   "(physical FB address wrong/unmapped).\n");
        display_set_error("VESA disabled: framebuffer failed pixel read-back probe.\n");
        return 0;
    }

    /* (b)+(c) Intel scanout health + panel power (read-only). */
    intel_scanout_probe_t sc;
    if (intel_gfx_probe_scanout(fb->width, fb->height, &sc) && sc.present) {
        serial_out("[display] intel scanout: pipe=");
        itoa(sc.pipe, b, 10); serial_out(b);
        serial_out(" enabled="); serial_out(sc.pipe_enabled ? "1" : "0");
        serial_out(" frame "); serial_out_hex(sc.frame_before);
        serial_out("->"); serial_out_hex(sc.frame_after);
        serial_out(" adv="); serial_out(sc.frame_advanced ? "1" : "0");
        serial_out("\n[display] intel panel: pp_status="); serial_out_hex(sc.pp_status);
        serial_out(" pp_ctl="); serial_out_hex(sc.pp_control);
        serial_out(" pp_on="); serial_out(sc.panel_power_known ?
                                          (sc.panel_power_on ? "1" : "0") : "?");
        serial_out(" blc_cpu="); serial_out_hex(sc.blc_cpu);
        serial_out(" blc_pch="); serial_out_hex(sc.blc_pch);
        serial_out(" bl_on="); serial_out(sc.backlight_known ?
                                          (sc.backlight_on ? "1" : "0") : "?");
        serial_out("\n");

        if (sc.pipe >= 0 && !sc.pipe_enabled) {
            serial_out("[display] confirm REJECT: pipe matching this resolution is "
                       "DISABLED (pipe dead).\n");
            display_set_error("VESA disabled: Intel pipe for this mode is disabled.\n");
            return 0;
        }
        if (sc.pipe >= 0 && sc.pipe_enabled && !sc.frame_advanced) {
            serial_out("[display] confirm REJECT: pipe enabled but frame counter frozen "
                       "(pipe is not scanning out).\n");
            display_set_error("VESA disabled: Intel pipe is not scanning out (frozen frame counter).\n");
            return 0;
        }
        if (sc.pipe < 0) {
            serial_out("[display] confirm WARN: no Intel pipe matches the FB resolution; "
                       "cannot positively confirm scanout (on-panel gate will catch a dark panel).\n");
        }
        /* (c) Only treat panel power as decisive when BOTH power and backlight
         * read trustworthy AND both report OFF; otherwise warn and rely on the
         * on-panel confirm-or-revert gate so we never wrongly reject a panel
         * whose status registers we simply could not read. */
        if (sc.panel_power_known && !sc.panel_power_on &&
            sc.backlight_known && !sc.backlight_on) {
            serial_out("[display] confirm REJECT: panel power AND backlight report OFF "
                       "while committing to graphics -- preferring fallback (panel would be dark).\n");
            print("[display] Panel power + backlight report OFF; preferring VGA text.\n");
            display_set_error("VESA disabled: panel power/backlight off; framebuffer would scan out dark.\n");
            return 0;
        }
        if ((sc.panel_power_known && !sc.panel_power_on) ||
            (sc.backlight_known && !sc.backlight_on)) {
            serial_out("[display] confirm WARN: panel power or backlight reports OFF; "
                       "committing but arming the on-panel confirm-or-revert gate.\n");
        }
    } else {
        serial_out("[display] confirm: no Intel GPU present; skipping pipe/panel "
                   "heuristics (pixel read-back passed).\n");
    }

    serial_out("[display] confirm OK: framebuffer accepted as a visible candidate.\n");
    return 1;
}

/*
 * Optional on-panel confirm-or-revert gate. The default boot skips this now
 * that the framebuffer path is validated; safe/diagnostic boots can still force
 * it with gooberos.display.confirm=force.
 *
 * The PS/2 keyboard and the 100Hz PIT are already live here (the staged boot
 * enabled interrupts after the kernel-heap floor stage, before this display
 * stage), so the wait is interrupt-driven and bounded by the tick counter with
 * a hard spin cap as a backstop. Returns 1 if confirmed, 0 if it reverted.
 */
#define DISPLAY_CONFIRM_TIMEOUT_MS 3000u

/* Interactive on-panel confirmation. Requires the PS/2 keyboard driver to be
 * online; the boot orchestrator runs the input stage (input_init +
 * keyboard_init + mouse_init) BEFORE the display stage on both x86 and x64
 * (Phase 3c lifted the x64 keyboard driver into the link), so this function
 * is now compiled for both arches. */
static int display_on_panel_confirm(void) {
    vesa_boot_splash("Press ENTER if you can see this. "
                     "Reverting to VGA text in 3s...");
    serial_out("[display] on-panel confirm: waiting up to 3s for a keypress...\n");

    /* Drain stale keystrokes so a pre-buffered key cannot auto-confirm. */
    while (keyboard_has_char()) (void)keyboard_read_char();

    uint32_t start = timer_ticks();
    /* stage_timer programs the PIT at 100Hz, so 1 tick == 10ms. */
    uint32_t deadline = (DISPLAY_CONFIRM_TIMEOUT_MS * 100u) / 1000u;
    uint32_t spin_cap = 0;

    while ((timer_ticks() - start) < deadline) {
        if (keyboard_has_char()) {
            (void)keyboard_read_char();   /* any key means "I can see this" */
            serial_out("[display] on-panel confirm: keypress received; staying graphical.\n");
            return 1;
        }
        __asm__ volatile("hlt");
        if (++spin_cap >= 100000000u) break;   /* belt-and-suspenders bound */
    }

    serial_out("[display] on-panel confirm: TIMEOUT (no keypress); reverting to VGA text.\n");
    return 0;
}

/*
 * Risky display stage (run under the boot guard): formalize the graceful
 * fallback ladder and never commit to a framebuffer we have not proven usable.
 *
 *   1. Honor an explicit VGA / display=off request (never override it).
 *   2. gooberos.display=safe adopts ONLY the inherited firmware framebuffer
 *      ("vesa" driver), arms the visibility probe, and forces the on-panel
 *      auto-revert gate -- no aggressive driver is tried.
 *   3. Register candidates in priority order and run the visibility-gated
 *      ladder: vesa (inherited LFB) -> bochs (dispi) -> intel (plane repoint).
 *      Each rung must init AND pass display_confirm_visible() or it is cleanly
 *      abandoned (serial-logged) and the next rung is tried.
 *   4. On a confirmed rung: vesa_init() + commit. If an Intel GPU is present
 *      (the at-risk hardware) or safe mode is set, run the on-panel
 *      confirm-or-revert gate; on timeout, revert to the VGA text floor.
 *
 * If a modeset faults on real hardware, the guard contains it and the
 * orchestrator forces boot_mode_vesa back to 0 (minimal VGA text floor).
 */
static int g_display_native_w = 0, g_display_native_h = 0;

/*
 * Commit to the 80x25 text console. Two paths:
 *
 *   1. GRUB handed us an inherited graphics LFB (type 1) -- e.g. UEFI x64
 *      VGA-compat boot, where the firmware never gives back the legacy text
 *      plane after a graphics handoff. In that case the textcon framebuffer
 *      backend renders the cell grid via the 8x16 font directly into the
 *      top-left 640x400 region of the GOP framebuffer. This is the only
 *      visible text path under UEFI.
 *
 *   2. GRUB left us in legacy VGA text mode (no graphics FB inherited).
 *      That is the classic x86 BIOS path: textcon binds the 0xB8000 plane
 *      and mirrors cell writes verbatim. On the rare UEFI legacy-BIOS
 *      reverting case (graphics LFB inherited but no longer wanted),
 *      display_restore_vga_text() reprograms the VGA controller back to
 *      text first.
 *
 * Always sets boot_mode_text_console (so the x64 main loop runs the full
 * shell instead of the VESA desktop) and clears boot_mode_vesa.
 */
static void revert_to_text_floor(void) {
    boot_mode_vesa = 0;
    boot_mode_text_console = 1;

    /* Path 1: inherited LFB -- render the text console into the
     * firmware framebuffer. This is the visible path on UEFI x64. */
    if (saved_have_loader_fb && saved_fb_type == 1) {
        uintptr_t fb_addr;
        if (sizeof(uintptr_t) < sizeof(uint64_t) && ((saved_fb_addr >> 32) != 0)) {
            fb_addr = 0;
        } else {
            fb_addr = (uintptr_t)saved_fb_addr;
        }
        if (fb_addr && con_init_fb(fb_addr, saved_fb_w, saved_fb_h,
                                   saved_fb_pitch, saved_fb_bpp)) {
            /* Reflect the FB as the visible console in diagnostics. */
            last_fb_type   = saved_fb_type;
            last_fb_bpp    = saved_fb_bpp;
            last_fb_pitch  = saved_fb_pitch;
            last_fb_width  = saved_fb_w;
            last_fb_height = saved_fb_h;
            last_fb_addr   = fb_addr;
            serial_out("[display] bound text console to inherited "
                       "framebuffer (textcon FB backend).\n");
            return;
        }
        /* FB bind failed (too small, weird bpp): try to hard-revert the
         * VGA controller back to text and use 0xB8000 instead. On UEFI
         * this almost certainly won't light the panel either, but on a
         * legacy-BIOS boot it will. */
        display_restore_vga_text();
        vga_set_text_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        serial_out("[display] textcon FB bind declined; falling back "
                   "to 0xB8000 (legacy BIOS only).\n");
    }

    /* Path 2: legacy VGA text plane. clear_screen() writes 0xB8000 and
     * con_init_vga then snapshots that into the cell grid. */
    clear_screen();
    con_init_vga();
    serial_out("[display] bound text console to 0xB8000 "
               "(textcon VGA backend).\n");
}

static void framebuffer_bringup(void) {
    /* Respect an explicit VGA / off request; never override the user's choice. */
    if (kstr_eq(g_boot_config.boot, "vga")) {
        vesa_reject_reason = "VESA disabled: VGA Compatibility Mode requested by GRUB.\n";
        revert_to_text_floor();
        return;
    }
    if (kstr_eq(g_boot_config.display, "off")) {
        vesa_reject_reason = "VESA disabled: graphics disabled by gooberos.display=off.\n";
        revert_to_text_floor();
        return;
    }
    /* gooberos.display=vga-text: explicit 80x25 text floor, even when
     * GRUB inherited a graphics LFB. Goes through the same textcon path
     * as the VGA-Compatibility GRUB entry. */
    if (kstr_eq(g_boot_config.display, "vga-text")) {
        vesa_reject_reason = "VESA disabled: text-only floor requested "
                             "by gooberos.display=vga-text.\n";
        revert_to_text_floor();
        return;
    }

    /* gooberos.display=safe: firmware FB only + probe + forced auto-revert. */
    int safe_display = kstr_eq(g_boot_config.display, "safe");
    const char* force_name = safe_display ? "vesa" : g_boot_config.display;
    if (safe_display)
        serial_out("[display] gooberos.display=safe: adopting firmware FB only, "
                   "probe + on-panel auto-revert armed.\n");

    /* Pick the recommended geometry that programmable drivers should try first.
     * Inherited framebuffers ignore this and keep the loader's exact mode. */
    uint32_t nat_w = 0, nat_h = 0;
    uint32_t req_w = 0, req_h = 0;
    if (parse_wxh(g_boot_config.native, &nat_w, &nat_h)) {
        g_display_native_w = (int)nat_w;
        g_display_native_h = (int)nat_h;
        req_w = nat_w;
        req_h = nat_h;
        serial_out("[display] native panel hint: ");
        { char b[16]; itoa((int)nat_w, b, 10); serial_out(b); serial_out("x");
          itoa((int)nat_h, b, 10); serial_out(b); serial_out("\n"); }
    } else if (saved_have_loader_fb && saved_fb_type == 1 &&
               saved_fb_w >= 320 && saved_fb_h >= 200 &&
               saved_fb_w <= 1920 && saved_fb_h <= 1200) {
        req_w = saved_fb_w;
        req_h = saved_fb_h;
        serial_out("[display] recommended mode from loader framebuffer: ");
        { char b[16]; itoa((int)req_w, b, 10); serial_out(b); serial_out("x");
          itoa((int)req_h, b, 10); serial_out(b); serial_out("\n"); }
    } else {
        serial_out("[display] recommended mode: driver default "
                   "(no safe native/loader geometry).\n");
    }

    /*
     * Feed the inherited framebuffer to the simple-framebuffer driver. On
     * x86_64 the kernel identity-maps the low 4 GiB and `uintptr_t` is 64
     * bits, so the full GOP base survives. On x86 (32-bit) a > 4 GiB
     * address is unreachable, so we surface 0 in that case to make the
     * "vesa" driver decline cleanly and let the bochs/intel rungs try.
     */
    uintptr_t inh_addr;
    if (sizeof(uintptr_t) < sizeof(uint64_t) && ((saved_fb_addr >> 32) != 0)) {
        inh_addr = 0;
    } else {
        inh_addr = (uintptr_t)saved_fb_addr;
    }
    native_fb_set_inherited(saved_have_loader_fb ? inh_addr : (uintptr_t)0,
                            saved_fb_w, saved_fb_h, saved_fb_pitch, saved_fb_bpp,
                            saved_have_loader_fb ? saved_fb_type : 0xFF);

    /* Build the candidate registry in priority order. Phase 4 (item 3) adds
     * the VGA mode-13h rung BETWEEN the LFB-class rungs (vesa / bochs /
     * intel) and the final VGA-text floor: when no LFB candidate confirms
     * we still get a 320x200x8 indexed-colour surface, which is far
     * better than a bare 80x25 text floor for the "visibility" goal. */
    display_reset_drivers();
    native_fb_register_drivers();    /* "vesa" then "bochs"       */
    intel_gfx_register_driver();     /* "intel" (plane-repoint)   */
    vga_graphics_register_driver();  /* "vga-graphics" (mode-13h) */

    /* Run the visibility-gated ladder. The generic driver gets the native hint
     * (the inherited "vesa" driver ignores hints and uses GRUB's geometry). */
    display_framebuffer_t fb;
    const display_driver_ops_t* drv =
        display_probe_drivers(force_name, req_w, req_h, 0, &fb,
                              display_confirm_visible, NULL);

    if (!drv) {
        const char* err = display_last_error();
        vesa_reject_reason = err ? err
            : "VESA disabled: no display driver could bring up a confirmed framebuffer.\n";
        /* Nothing confirmed: if GRUB left us in a graphics mode, get back to a
         * visible text console rather than leaving a dark panel. */
        revert_to_text_floor();
        return;
    }

    /*
     * Phase 4 (item 3): if the chosen rung is the VGA-graphics fallback,
     * the 320x200x8 surface is NOT compatible with vesa_init's direct-RGB
     * pixel writers. Record the FB descriptor in the framework + flip the
     * vga_graphics flag, but do NOT call vesa_init. The desktop layer
     * (gui/desktop_vesa.c) reads kernel_display_is_vga_graphics() and
     * paints through the mode-13h primitives instead.
     */
    if (drv->id == DISPLAY_DRIVER_VGA_GRAPHICS) {
        display_register_framebuffer(drv->id, fb.format,
                                     fb.framebuffer_addr, fb.width, fb.height,
                                     fb.pitch, fb.bpp);
        boot_mode_vga_graphics = 1;
        boot_mode_vesa = 1;          /* still "graphical mode" for downstream */
        last_fb_type = 0;            /* indexed, not direct-RGB              */
        last_fb_bpp = fb.bpp;
        last_fb_pitch = fb.pitch;
        last_fb_width = fb.width;
        last_fb_height = fb.height;
        last_fb_addr = fb.framebuffer_addr;
        serial_out("[display] committed to VGA mode-13h (320x200x8).\n");
        return;
    }

    /* Commit the confirmed framebuffer. */
    vesa_init((uint64_t)fb.framebuffer_addr, fb.width, fb.height, fb.pitch, fb.bpp);
    display_register_framebuffer(drv->id, fb.format,
                                 fb.framebuffer_addr, fb.width, fb.height,
                                 fb.pitch, fb.bpp);
    boot_mode_vesa = 1;
    last_fb_type = 1;
    last_fb_bpp = fb.bpp;
    last_fb_pitch = fb.pitch;
    last_fb_width = fb.width;
    last_fb_height = fb.height;
    last_fb_addr = fb.framebuffer_addr;

    /*
     * The normal path now trusts the validated framebuffer and goes directly
     * to the desktop. Safe mode and gooberos.display.confirm=force keep the
     * bounded fallback prompt available for diagnostics.
     */
    int intel_present = intel_gfx_detect(NULL);
    int run_confirm = safe_display;
    /*
     * gooberos.display.confirm=skip|force|default lets the cmdline override
     * the default. Normal boot passes skip; the safe GRUB entry passes force.
     */
    switch (g_boot_config.display_confirm) {
        case BOOT_DISPLAY_CONFIRM_SKIP:
            if (run_confirm) {
                serial_out("[display] gooberos.display.confirm=skip: "
                           "skipping on-panel confirm gate.\n");
            }
            run_confirm = 0;
            break;
        case BOOT_DISPLAY_CONFIRM_FORCE:
            if (!run_confirm) {
                serial_out("[display] gooberos.display.confirm=force: "
                           "arming on-panel confirm gate even though arch default "
                           "would skip it.\n");
            }
            run_confirm = 1;
            break;
        case BOOT_DISPLAY_CONFIRM_DEFAULT:
        default:
            break;
    }
    (void)intel_present;
    if (run_confirm) {
        if (!display_on_panel_confirm()) {
            /* Reverting: restore the VGA text floor so we are never stranded on
             * a black panel, and drop back to text-mode boot. */
            revert_to_text_floor();
            vesa_reject_reason =
                "VESA reverted: no on-panel confirmation; restored VGA text floor.\n";
            print("Display: no confirmation within 3s; reverted to VGA text.\n");
            serial_out("[display] reverting to VGA text floor after on-panel timeout.\n");
            /* Reflect the revert through the diagnostic accessors. */
            last_fb_type = 0xFF;
        } else {
            serial_out("[display] on-panel confirmation OK; keeping graphical mode.\n");
        }
    } else {
        (void)safe_display;
    }
}

static void serial_out(const char* s);
static void serial_out_hex(uint32_t v);
static void print_hex32(uint32_t v);

/*
 * Report the selected display driver, the resulting mode, and any detected
 * Intel GPU to both the VGA console and the COM1 serial boot log.
 */
static void report_display_diagnostics(void) {
    char buf[16];
    const display_mode_info_t* mode = display_get_mode();

    print("Display driver: ");
    print(display_driver_name(mode->driver));
    print("\n");
    serial_out("Display driver: ");
    serial_out(display_driver_name(mode->driver));
    serial_out("\n");

    if (mode->driver == DISPLAY_DRIVER_VESA_LFB ||
        mode->driver == DISPLAY_DRIVER_NATIVE_GENERIC ||
        mode->driver == DISPLAY_DRIVER_NATIVE_INTEL) {
        print("Display mode: ");
        itoa((int)mode->width, buf, 10);  print(buf); print("x");
        itoa((int)mode->height, buf, 10); print(buf); print("x");
        itoa((int)mode->bpp, buf, 10);    print(buf); print(" (");
        print(display_format_name(mode->format)); print(")\n");

        serial_out("Display mode: ");
        itoa((int)mode->width, buf, 10);  serial_out(buf); serial_out("x");
        itoa((int)mode->height, buf, 10); serial_out(buf); serial_out("x");
        itoa((int)mode->bpp, buf, 10);    serial_out(buf); serial_out(" ");
        serial_out(display_format_name(mode->format)); serial_out("\n");

        /* gooberos.native=WxH: warn (don't fail) when we did not land on the
         * requested native panel mode -- a non-native mode is the usual cause
         * of letterboxing / blurry scaling on a fixed-resolution LCD. */
        if (g_display_native_w && g_display_native_h &&
            ((int)mode->width != g_display_native_w ||
             (int)mode->height != g_display_native_h)) {
            print("WARNING: committed mode != requested native (");
            itoa(g_display_native_w, buf, 10); print(buf); print("x");
            itoa(g_display_native_h, buf, 10); print(buf); print(")\n");
            serial_out("WARNING: committed display mode != gooberos.native (");
            itoa(g_display_native_w, buf, 10); serial_out(buf); serial_out("x");
            itoa(g_display_native_h, buf, 10); serial_out(buf); serial_out(")\n");
        }
    }

    /* Always report Intel GPU presence for triage, regardless of which driver
     * actually drives the panel. */
    intel_gfx_info_t intel;
    if (intel_gfx_detect(&intel)) {
        print("Intel GPU: detected (device ");
        itoa((int)intel.device_id, buf, 16); print(buf);
        print(", aperture "); print_hex32(intel.aperture_base);
        print(", mmio "); print_hex32(intel.mmio_base);
        print(")\n");

        serial_out("Intel GPU detected; device=");
        itoa((int)intel.device_id, buf, 16); serial_out(buf);
        serial_out(" aperture="); serial_out_hex(intel.aperture_base);
        serial_out(" mmio="); serial_out_hex(intel.mmio_base);
        serial_out("\n");
    }

    /* If the Intel plane-repoint path ran (gooberos.display=intel, or it was
     * the only candidate that succeeded), dump the BIOS-programmed registers we
     * read and exactly what we changed. This is the primary triage tool for the
     * "VGA text visible but LFB black" Lenovo case. */
    const intel_plane_report_t* pr = intel_gfx_get_plane_report();
    if (pr->attempted) {
        serial_out("Intel plane-repoint: ");
        serial_out(pr->succeeded ? "REPOINTED\n" : "declined\n");
        if (pr->reason) serial_out(pr->reason);
        serial_out("  mmio="); serial_out_hex(pr->mmio_base);
        serial_out(" aperture="); serial_out_hex(pr->aperture_base);
        serial_out("\n  pipe="); itoa(pr->pipe, buf, 10); serial_out(buf);
        serial_out(" pipeconf="); serial_out_hex(pr->pipeconf);
        serial_out(" pipe_src="); itoa((int)pr->pipe_w, buf, 10); serial_out(buf);
        serial_out("x"); itoa((int)pr->pipe_h, buf, 10); serial_out(buf);
        serial_out(" frame="); serial_out_hex(pr->frame_count);
        serial_out("->"); serial_out_hex(pr->frame_count2);
        serial_out(" advanced="); serial_out(pr->frame_advanced ? "1" : "0");
        serial_out("\n  fb_addr="); serial_out_hex(pr->fb_addr);
        serial_out(" gfx_off="); serial_out_hex(pr->gfx_offset);
        serial_out(" pitch="); itoa((int)pr->fb_pitch, buf, 10); serial_out(buf);
        serial_out(" bpp="); itoa((int)pr->fb_bpp, buf, 10); serial_out(buf);
        serial_out("\n  DSPCNTR "); serial_out_hex(pr->old_dspcntr);
        serial_out(" -> "); serial_out_hex(pr->new_dspcntr);
        serial_out(" (old DSPSURF="); serial_out_hex(pr->old_dspsurf);
        serial_out(" DSPSTRIDE="); serial_out_hex(pr->old_dspstride);
        serial_out(")\n");

        print("Intel plane-repoint: ");
        print(pr->succeeded ? "REPOINTED pipe " : "declined (see serial)\n");
        if (pr->succeeded) {
            itoa(pr->pipe, buf, 10); print(buf);
            print(", DSPCNTR "); print_hex32(pr->old_dspcntr);
            print(" -> "); print_hex32(pr->new_dspcntr); print("\n");
        } else if (pr->reason) {
            print(pr->reason);
        }
    }
}

#ifdef __i386__
static void update_kernel_process_memory();

static void register_kernel_process() {
    size_t kernel_size_bytes = (size_t)(&_kernel_end) - (size_t)(&_kernel_start);
    size_t kernel_size_kb = (kernel_size_bytes + 1023) / 1024;
    create_process("kernel.bin", kernel_size_kb);
}

struct IDTEntry {
    uint16_t base_low;
    uint16_t selector;
    uint8_t zero;
    uint8_t type_attr;
    uint16_t base_high;
} __attribute__((packed));

struct IDTPointer {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct IDTEntry idt[256];
static struct IDTPointer idt_ptr;

extern void load_idt(struct IDTPointer*);
extern void irq1_handler_asm();
extern void irq12_handler_asm();
extern void isr32_stub();

extern void isr0_stub();  extern void isr1_stub();  extern void isr2_stub();  extern void isr3_stub();
extern void isr4_stub();  extern void isr5_stub();  extern void isr6_stub();  extern void isr7_stub();
extern void isr8_stub();  extern void isr9_stub();  extern void isr10_stub(); extern void isr11_stub();
extern void isr12_stub(); extern void isr13_stub(); extern void isr14_stub(); extern void isr15_stub();
extern void isr16_stub(); extern void isr17_stub(); extern void isr18_stub(); extern void isr19_stub();
extern void isr20_stub(); extern void isr21_stub(); extern void isr22_stub(); extern void isr23_stub();
extern void isr24_stub(); extern void isr25_stub(); extern void isr26_stub(); extern void isr27_stub();
extern void isr28_stub(); extern void isr29_stub(); extern void isr30_stub(); extern void isr31_stub();

static void (*const exception_stubs[32])(void) = {
    isr0_stub,  isr1_stub,  isr2_stub,  isr3_stub,
    isr4_stub,  isr5_stub,  isr6_stub,  isr7_stub,
    isr8_stub,  isr9_stub,  isr10_stub, isr11_stub,
    isr12_stub, isr13_stub, isr14_stub, isr15_stub,
    isr16_stub, isr17_stub, isr18_stub, isr19_stub,
    isr20_stub, isr21_stub, isr22_stub, isr23_stub,
    isr24_stub, isr25_stub, isr26_stub, isr27_stub,
    isr28_stub, isr29_stub, isr30_stub, isr31_stub,
};
#endif /* __i386__ */

/* exception_names[32] now lives in cpu_exception_names.h, shared with the
 * 64-bit cpu_exception_handler in kernel_x64.c. Phase 3b.0 lift. */

static void serial_out(const char* s);
static void serial_out_hex(uint32_t v) {
    const char* hex = "0123456789ABCDEF";
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) buf[2 + i] = hex[(v >> ((7 - i) * 4)) & 0xF];
    buf[10] = '\0';
    serial_out(buf);
}

/*
 * 64-bit serial hex printer. Emits a single "0x" prefix followed by 16 hex
 * digits. Used by the x64 cpu_exception_handler (so RIP/CS/RFLAGS survive
 * end-to-end without the doubled-prefix glitch from stitching two
 * serial_out_hex(uint32_t) calls together) and by display-diagnostic paths
 * that report `framebuffer_addr` as a uintptr_t (forward-compat for a
 * > 4 GiB GOP base; the Lenovo / OVMF GOP bases tested so far are sub-4 GiB
 * but we widened the pointer in Phase 3b so the diagnostic should follow).
 */
static void serial_out_hex64(uint64_t v) {
    const char* hex = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        buf[2 + i] = hex[(v >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\0';
    serial_out(buf);
}

static void print_hex32(uint32_t v) {
    const char* hex = "0123456789ABCDEF";
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) buf[2 + i] = hex[(v >> ((7 - i) * 4)) & 0xF];
    buf[10] = '\0';
    print(buf);
}

/*
 * Called by isr_common in cpu_exceptions.s (32-bit) or cpu_exceptions64.s
 * (64-bit). Two builds with slightly different ABIs share the same body
 * shape (log fault, longjmp via boot_guard if armed, else cli/hlt) but the
 * frame-field types differ -- gated below by __i386__ / __x86_64__ so each
 * kernel.o links against the matching ASM dispatcher.
 *
 * Two behaviors, depending on whether the boot fault guard is armed:
 *
 *   - Guard ACTIVE (a stage is running under boot_guarded_run): this is the
 *     "boot safety floor" path. We log the fault frame + the faulting stage
 *     name to serial (and the text console), then longjmp back into
 *     boot_guarded_run, which reports the stage as FAILED and lets the
 *     orchestrator continue with the next stage. The abandoned exception frame
 *     on the stack is discarded by the longjmp's esp restore, and the
 *     pre-stage interrupt state is restored there too. This is how a faulting
 *     driver probe is contained instead of killing the machine.
 *
 *   - Guard INACTIVE (fault outside guarded init, e.g. in the running shell):
 *     keep the original behavior -- report and halt forever rather than
 *     letting the CPU triple-fault to a reboot.
 */
#ifdef __i386__
void cpu_exception_handler(uint32_t vector, uint32_t error_code,
                           uint32_t eip, uint32_t cs, uint32_t eflags) {
    const char* name = (vector < 32) ? exception_names[vector] : "Unknown";

    /* Always log the fault frame to serial. */
    serial_out("\n[CPU exception] vector=");
    serial_out_hex(vector);
    serial_out(" (");
    serial_out(name);
    serial_out("), errcode=");
    serial_out_hex(error_code);
    serial_out(", eip=");
    serial_out_hex(eip);
    serial_out(", cs=");
    serial_out_hex(cs);
    serial_out(", eflags=");
    serial_out_hex(eflags);
    serial_out("\n");

    if (boot_guard_active()) {
        /* Contained fault: report which stage faulted and resume the boot. */
        serial_out("[guard] fault during stage '");
        serial_out(boot_guard_stage_name());
        serial_out("' -- containing; stage marked FAILED, boot continues.\n");

        if (!boot_mode_vesa) {
            vga_set_text_color(VGA_COLOR_BLACK, VGA_COLOR_LIGHT_BROWN);
            print("[guard] CPU exception in stage '");
            print(boot_guard_stage_name());
            print("' (");
            print(name);
            print(") -- contained, continuing boot\n");
            vga_set_text_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        }

        boot_guard_longjmp();   /* does not return */
    }

    /* No guard armed: original panic-and-halt behavior. */
    serial_out("[KERNEL PANIC] no guard active; halting.\n");
    if (!boot_mode_vesa) {
        vga_set_text_color(VGA_COLOR_WHITE, VGA_COLOR_RED);
        print("\n*** GooberOS kernel panic: CPU exception ***\n");
        print("Vector: ");
        char buf[16];
        itoa((int)vector, buf, 10);
        print(buf);
        print(" (");
        print(name);
        print(")\n");
        print("EIP: ");
        print_hex32(eip);
        print("  ERR: ");
        print_hex32(error_code);
        print("\n");
    }

    /* Disable interrupts and halt forever; do NOT try to recover. */
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}
#endif /* __i386__ cpu_exception_handler */

#ifdef __x86_64__
/*
 * x86_64 ABI port of cpu_exception_handler above. The cpu_exceptions64.s
 * stubs pass the fault frame in System V AMD64 registers:
 *   RDI = vector, RSI = error_code, RDX = rip, RCX = cs, R8 = rflags.
 * Each field is uint64_t so 64-bit RIP / RFLAGS survive intact. The body
 * mirrors the 32-bit handler: log the frame on serial, then either longjmp
 * via boot_guard (if armed) or cli/hlt forever.
 *
 * We do NOT call print() / vga_set_text_color() on the contained path here
 * because the x64 build's "VGA console" is a framebuffer renderer mapped
 * through the print sink, and we are in interrupt context. Serial-only.
 */
void cpu_exception_handler(uint64_t vector, uint64_t error_code,
                           uint64_t rip, uint64_t cs, uint64_t rflags) {
    const char* name = (vector < 32) ? exception_names[vector] : "Unknown";

    serial_out("\n[CPU exception] vector=");
    serial_out_hex((uint32_t)vector);
    serial_out(" (");
    serial_out(name);
    serial_out("), errcode=");
    serial_out_hex((uint32_t)error_code);
    serial_out(", rip=");
    serial_out_hex64(rip);
    serial_out(", cs=");
    serial_out_hex64(cs);
    serial_out(", rflags=");
    serial_out_hex64(rflags);
    serial_out("\n");

    if (boot_guard_active()) {
        serial_out("[guard] fault during stage '");
        serial_out(boot_guard_stage_name());
        serial_out("' (");
        serial_out(name);
        serial_out(") -- containing; stage marked FAILED, boot continues.\n");
        boot_guard_longjmp();   /* does not return */
    }

    serial_out("[KERNEL PANIC] no guard active; halting.\n");
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}
#endif /* __x86_64__ cpu_exception_handler */

static kernel_print_sink_t print_sink = NULL;
static void* print_sink_ctx = NULL;

#ifdef __i386__
static unsigned int update_counter = 0;

void pic_remap() {
    uint8_t a1 = inb(0x21);
    uint8_t a2 = inb(0xA1);

    outb(0x20, 0x11);
    outb(0xA0, 0x11);

    outb(0x21, 0x20);
    outb(0xA1, 0x28);

    outb(0x21, 4);
    outb(0xA1, 2);

    outb(0x21, 0x01);
    outb(0xA1, 0x01);

    outb(0x21, a1);
    outb(0xA1, a2);
}

void set_idt_entry(int index, uint32_t base, uint16_t selector, uint8_t type_attr) {
    idt[index].base_low  = base & 0xFFFF;
    idt[index].selector  = selector;
    idt[index].zero      = 0;
    idt[index].type_attr = type_attr;
    idt[index].base_high = (base >> 16) & 0xFFFF;
}

void irq0_handler_main() {
    // Increment counter and update kernel process memory every ~2 seconds
    update_counter++;
    if (update_counter >= 200) {
        update_kernel_process_memory();
        update_counter = 0;
    }

    timer_interrupt_handler();

    /* Last-resort stage watchdog. timer_interrupt_handler() already sent the
     * PIC EOI, so it is safe for this to longjmp out of a wedged guarded stage
     * without returning through the IRQ wrapper. No-op unless a stage overran. */
    boot_watchdog_tick();
}

__attribute__((naked)) void irq0_handler_asm() {
    __asm__ volatile (
        "pusha\n"
        "call irq0_handler_main\n"
        "popa\n"
        "iret\n"
    );
}

void irq1_handler_main() {
    keyboard_interrupt_handler();
}

void idt_init() {
    pic_remap();

    /*
     * Install CPU exception handlers (vectors 0-31) FIRST so that any fault
     * (page fault, GPF, etc.) during the rest of boot prints a panic message
     * instead of triple-faulting the box. This is critical on real hardware
     * where chipset quirks can fault unexpectedly during PCI/USB probing.
     */
    for (int v = 0; v < 32; v++) {
        set_idt_entry(v, (uint32_t)exception_stubs[v], 0x08, 0x8E);
    }

    set_idt_entry(IRQ0, (uint32_t)irq0_handler_asm, 0x08, 0x8E);
    set_idt_entry(IRQ1, (uint32_t)irq1_handler_asm, 0x08, 0x8E);
    set_idt_entry(44, (uint32_t)irq12_handler_asm, 0x08, 0x8E);
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint32_t)&idt;
    load_idt(&idt_ptr);
}
#endif /* __i386__ */

void kernel_set_print_sink(kernel_print_sink_t sink, void* ctx) {
    print_sink = sink;
    print_sink_ctx = ctx;
}

void kernel_clear_print_sink(void) {
    print_sink = NULL;
    print_sink_ctx = NULL;
}

void print(const char* str) {
    if (print_sink) {
        print_sink(str, print_sink_ctx);
        return;
    }
    /*
     * Fall-through sink. On x86 we land on vga_put_char (the legacy 0xB8000
     * text plane). On x64 the legacy text plane is dead on UEFI panels, so
     * the orchestrator sets a print sink early in kernel_main that mirrors
     * to COM1 + the framebuffer text console; this fall-through still runs
     * for any early print() that fires before that sink is installed.
     */
    for (size_t i = 0; str[i] != '\0'; i++) {
        vga_put_char(str[i]);
    }
}

#ifdef __i386__
extern void fs_init();
extern void vesa_desktop_run();
#endif

/* shell.c is linked on both arches (Phase 3e), so the shell entry points
 * are available regardless of __i386__ / __x86_64__. The x64 main loop
 * dispatches the full shell when the VGA-Compatibility boot path commits
 * the 80x25 text console; the x86 path uses the same shell_init/shell_run
 * pair after vesa_desktop_run returns or is skipped. usb_poll() is already
 * declared via drivers/usb/usb.h included above. */
extern void shell_init(void);
extern void shell_run(void);

#ifdef __i386__
static void update_kernel_process_memory() {
    if (kernel_pid < 0) return;
    size_t kernel_size_bytes = (size_t)(&_kernel_end) - (size_t)(&_kernel_start);
    size_t kernel_size_kb = (kernel_size_bytes + 1023) / 1024;

    process_entry_t *table = get_kernel_process_table();
    int total = get_kernel_process_count();
    for (int i = 0; i < total; i++) {
        if (table[i].pid == kernel_pid && table[i].active) {
            table[i].memory_kb = kernel_size_kb;
            break;
        }
    }
}
#endif /* __i386__ */

/*
 * Serial output. Writes every char to BOTH the legacy Bochs/QEMU debug port
 * (0xE9) AND the COM1 transmit register. The 0xE9 mirror preserves the
 * `-debugcon file:` capture path the x86 build has always used; the COM1
 * mirror lets `-serial file:` capture every kernel-emitted line as well,
 * which is what the Phase 3b verification harness uses.
 *
 * COM1 must be initialized before the first call (every kernel_main below
 * does this as its first action). The wait-on-LSR.THRE keeps us from
 * dropping characters when the THR is full.
 */
static void serial_out(const char* s) {
    while (*s) {
        uint8_t c = (uint8_t)*s++;
        outb(0xE9, c);
        while ((inb(0x3F8 + 5) & 0x20) == 0) { /* spin on LSR.THRE */ }
        outb(0x3F8, c);
    }
}

/* ===================== Staged boot orchestrator =========================
 *
 * Each init entry point is wrapped in a named stage and run through the boot
 * fault guard. The first stages establish the always-works MINIMAL floor (VGA
 * text + PS/2 keyboard/mouse + heap); the risky stages (framebuffer/display,
 * PCI scan, storage, USB) run AFTER the floor so that a fault in any of them is
 * contained and the system still reaches the shell/desktop on the minimal
 * console. In safe mode (gooberos.safe=1) the risky stages are SKIPPED
 * entirely, which is how compatibility mode works without touching the
 * USB/video drivers themselves.
 *
 * These wrappers only order + guard the existing entry points; they do not
 * reimplement them.
 *
 * The orchestrator runner `boot_run_stages_table()` is arch-agnostic; the
 * stage tables (k_boot_stages on x86, k_boot_stages_x64 on x86_64) differ
 * because the x64 build does not link the full driver stack yet (Phase 3c/3d/
 * 3e bring those in). Both arches use the same boot_guarded_run + watchdog +
 * results-log plumbing from boot_safety.c.
 */
typedef struct {
    const char* name;
    void (*fn)(void);
    int  risky;              /* 1 => skipped when boot_safe_mode() is set */
    int  enable_ints_after;  /* 1 => sti after this stage (floor/risky boundary) */
    const char* splash;      /* vesa boot splash on success, or NULL */
    int  is_display;         /* 1 => force VGA text floor if this stage fails */
    uint32_t watchdog_ticks; /* 0 => none; else force-abort after N*10ms */
} boot_stage_def_t;

/*
 * Watchdog budgets (100 Hz ticks; 1 tick = 10 ms). These are deliberately
 * generous last-resort ceilings, NOT the normal-path timeouts (those live in
 * the drivers). They only fire if a stage truly wedges, guaranteeing the boot
 * always reaches the desktop/shell. The display stage has its own bounded
 * on-panel confirm gate, so it is left unguarded to avoid clipping that.
 */
#define WD_HWSUMMARY  300u   /* 3 s  */
#define WD_PCI        400u   /* 4 s  */
#define WD_STORAGE    500u   /* 5 s  */
#define WD_USB        500u   /* 5 s -- bounded so bad USB does not dominate boot */
#define WD_TOUCHPAD   250u   /* 2.5 s -- ACPI/I2C touchpad probe ceiling */
/*
 * Phase 3f doubles the desktop init budget to 60 s. Storage-driven icon
 * enumeration (Recent Files, mounted volumes, /Desktop on a slow USB
 * stick) plus the 12-second USB watchdog earlier in the boot can push
 * past 30 s on a wedged USB MSC; the 60-s ceiling keeps the wedge
 * contained while still letting the orchestrator fall through to the
 * Phase 3c REPL on a real hardware fault.
 */
#define WD_DESKTOP   6000u   /* 60 s -- generous ceiling for desktop init  */

/* Arch-agnostic stage runner. Pass the stage table + count from each arch's
 * kernel_main below. Records every stage's result (OK/FAILED/SKIPPED) into
 * boot_safety's per-stage log so boot_print_results_summary() works. */
static void boot_run_stages_table(const boot_stage_def_t* stages, int n) {
    const int safe = boot_safe_mode();
    boot_results_reset();

    for (int i = 0; i < n; i++) {
        const boot_stage_def_t* s = &stages[i];

        if (s->risky && safe) {
            boot_record_stage(s->name, BOOT_STAGE_SKIPPED);
            serial_out("[boot] SKIP (safe mode): ");
            serial_out(s->name);
            serial_out("\n");
            if (s->enable_ints_after) __asm__ volatile("sti");
            continue;
        }

        serial_out("[boot] stage: ");
        serial_out(s->name);
        serial_out("\n");

        /* Arm the watchdog so a wedged stage can't hang the boot. Interrupts
         * are enabled for every risky stage (sti at the floor->risky
         * boundary), so IRQ0 keeps advancing the tick and the watchdog can
         * force-abort an overrun via the existing guard. */
        if (s->watchdog_ticks) boot_watchdog_arm(s->watchdog_ticks);
        int rc = boot_guarded_run(s->name, s->fn);
        boot_watchdog_disarm();
        int status = (rc == BOOT_GUARD_OK) ? BOOT_STAGE_OK : BOOT_STAGE_FAILED;
        boot_record_stage(s->name, status);

        if (s->is_display && status != BOOT_STAGE_OK) {
            /* Display bring-up faulted: drop to the minimal VGA text floor. If
             * GRUB had set a graphics mode, reprogram VGA text so the contained-
             * fault console is actually visible (never a dark, stranded panel). */
            revert_to_text_floor();
            if (!vesa_reject_reason) {
                vesa_reject_reason =
                    "VESA disabled: display stage faulted; using VGA text.\n";
            }
        }

        /* Enable interrupts at the floor->risky boundary (and even if the
         * boundary stage was skipped) so the text console + timer work. */
        if (s->enable_ints_after) __asm__ volatile("sti");

        if (status == BOOT_STAGE_OK && s->splash && boot_mode_vesa) {
            vesa_boot_splash(s->splash);
        }
    }
}

#ifdef __i386__
static void stage_timer(void)    { timer_init(100); }
static void stage_input(void)    { input_init(); keyboard_init(); mouse_init(); }
static void stage_heap(void)     { memory_init((void*)(&_kernel_end), KERNEL_HEAP_SIZE); }
static void stage_hwsummary(void){ boot_print_hardware_summary(); }
static void stage_display(void)  { framebuffer_bringup(); report_display_diagnostics(); }
static void stage_pci(void)      { pci_init(); }
static void stage_storage(void)  { storage_init(); }
static void stage_usb(void)      { usb_init(); }
static void stage_fs(void)       { fs_init(); }
static void stage_acpi(void)     { acpi_init(); }
static void stage_touchpad(void) {
    if (kstr_eq(g_boot_config.i2c, "off") ||
        kstr_eq(g_boot_config.touchpad, "off")) {
        print("[touchpad] disabled by cmdline.\n");
        return;
    }
    touchpad_init();
}

static const boot_stage_def_t k_boot_stages[] = {
    /* --- Minimal floor: always runs, even in safe mode --- */
    { "Timer (PIT 100Hz)",           stage_timer,     0, 0, NULL, 0, 0 },
    { "PS/2 input + keyboard/mouse", stage_input,     0, 0, NULL, 0, 0 },
    { "Kernel heap",                 stage_heap,      0, 1, NULL, 0, 0 },
    /* --- Risky stages: guarded, skipped in safe mode --- */
    { "Hardware summary (PCI scan)", stage_hwsummary, 1, 0, NULL, 0, WD_HWSUMMARY },
    { "ACPI tables",                 stage_acpi,      1, 0, NULL, 0, WD_HWSUMMARY },
    { "Display / framebuffer",       stage_display,   1, 0,
      "Framebuffer OK. Initializing hardware...", 1, 0 },
    { "PCI init",                    stage_pci,       1, 0,
      "PCI initialized. Scanning storage...", 0, WD_PCI },
    { "I2C HID touchpad",            stage_touchpad,  1, 0,
      "Touchpad probe complete. Scanning storage...", 0, WD_TOUCHPAD },
    { "Storage controllers",         stage_storage,   1, 0,
      "Storage initialized. Initializing USB...", 0, WD_STORAGE },
    { "USB host stack",              stage_usb,       1, 0,
      "USB initialized. Loading filesystem...", 0, WD_USB },
    /* --- Back to the floor: filesystem powers the text shell --- */
    { "Filesystem",                  stage_fs,        0, 0,
      "Filesystem ready. Starting desktop...", 0, 0 },
};

static void boot_run_stages(void) {
    boot_run_stages_table(k_boot_stages,
                          (int)(sizeof(k_boot_stages) / sizeof(k_boot_stages[0])));
}
#endif /* __i386__ */

#ifdef __i386__
void kernel_main(uint32_t magic, multiboot_info_t* mb_info) {
    /* Init serial port COM1 for debug */
    outb(0x3F8 + 1, 0x00); /* Disable interrupts */
    outb(0x3F8 + 3, 0x80); /* Enable DLAB */
    outb(0x3F8 + 0, 0x01); /* Set divisor low */
    outb(0x3F8 + 1, 0x00); /* Set divisor high */
    outb(0x3F8 + 3, 0x03); /* 8N1 */
    outb(0x3F8 + 2, 0xC7); /* Enable FIFO */

    serial_out("GooberOS boot starting...\n");

    vga_set_text_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    clear_screen();
    display_register_text_mode();
    print("GooberOS -- x86 Kernel\n");
    print("\n");

    /*
     * Bootstrap (NOT guarded -- these must succeed for the guard to function):
     *   1. Parse the unified boot config from the multiboot cmdline. This must
     *      happen before the staged boot so boot_safe_mode() is known.
     *   2. Install the IDT so the CPU-exception handlers (and IRQ handlers)
     *      exist; the boot fault guard relies on cpu_exception_handler being
     *      wired here. A fault before this point would triple-fault, so the
     *      bootstrap is intentionally kept tiny.
     */
    boot_config_parse_multiboot(magic, mb_info);
    idt_init();

    print("Boot request: ");
    print(g_boot_config.boot);
    print("\n");
    serial_out("Boot request: ");
    serial_out(g_boot_config.boot);
    serial_out("\n");

    if (boot_safe_mode()) {
        print("Boot config: COMPATIBILITY (safe) mode -- minimal VGA + PS/2 floor only.\n");
        serial_out("Boot config: gooberos.safe=1 (compatibility mode); skipping risky stages.\n");
    }

    /*
     * Run the staged boot: the minimal floor first, then the risky graphical /
     * USB stages under the fault guard. If a risky stage faults it is contained
     * and the next stage still runs; in safe mode the risky stages are skipped.
     */
    boot_run_stages();

    /* Report the resulting display mode (text vs framebuffer). */
    if (!boot_mode_vesa) {
        print("Boot: VGA text mode\n");
        if (vesa_reject_reason) {
            print(vesa_reject_reason);
            serial_out(vesa_reject_reason);
        }
        if (last_fb_width || last_fb_height || last_fb_type != 0xFF) {
            char buf[16];
            print("FB info: ");
            itoa((int)last_fb_width, buf, 10); print(buf); print("x");
            itoa((int)last_fb_height, buf, 10); print(buf); print(" @");
            itoa((int)last_fb_bpp, buf, 10); print(buf); print(" bpp, type=");
            itoa((int)last_fb_type, buf, 10); print(buf); print("\n");
        }
        serial_out("Boot: VGA text mode\n");
    } else {
        print("Boot: VESA graphics mode\n");
        serial_out("Boot: VESA graphics mode\n");
    }

    /* Per-stage OK / FAILED / SKIPPED summary to VGA + serial. */
    boot_print_results_summary();

    if (boot_mode_vesa) {
        uint32_t fb_size = vesa_get_pitch() * vesa_get_height();
        if (fb_size <= VESA_STATIC_BACKBUFFER_BYTES) {
            vesa_set_backbuffer_bytes((uint32_t*)vesa_static_backbuffer, fb_size);
            serial_out("VESA static backbuffer enabled\n");
        } else {
            vesa_set_backbuffer_bytes(NULL, 0);
            serial_out("VESA backbuffer too large; using direct framebuffer\n");
        }
    }

    kernel_pid = create_process("kernel.bin", 0);
    update_kernel_process_memory();

    shell_init();

    if (boot_mode_vesa) {
        vesa_desktop_run();
    }

    while (1) {
        usb_poll();
        touchpad_poll();
        shell_run();
        __asm__("hlt");
    }
}
#endif /* __i386__ kernel_main */

#ifdef __x86_64__
/* ===================================================================== *
 *  x86_64 long-mode kernel_main                                          *
 *                                                                       *
 *  Phase 3b deliverable: the unified staged-boot orchestrator runs on   *
 *  x64. We deliberately only link the EARLY stages this round (IDT,     *
 *  PIC, timer, framebuffer bring-up + display confirm). The PS/2 input, *
 *  PCI, USB, FS, shell, GUI stages stay x86-only until Phase 3c/3d/3e   *
 *  port the corresponding driver translation units to -m64.             *
 *                                                                       *
 *  Boot flow on x64:                                                    *
 *    1. COM1 init -> "GooberOS boot starting..." serial banner.         *
 *    2. Preserved Phase 1/2/3a/3a.1/3b.0 milestone serial: trampoline   *
 *       hand-off magic + multiboot info + cmdline; 64-bit IDT install;  *
 *       PIC remap; multiboot2 framebuffer tag walk + on-panel RGB       *
 *       bands + 8x16 proof-of-life lines (the bits the user already saw *
 *       on the Lenovo S21e-20 -- preserved verbatim).                   *
 *    3. boot_config_parse_multiboot(magic, info) captures the cmdline   *
 *       and the inherited framebuffer geometry into saved_fb_*.         *
 *    4. boot_run_stages_table(k_boot_stages_x64) executes:              *
 *         timer_init (PIT @100Hz, TSC calibrate),                       *
 *         hardware summary (PCI scan -- stubbed; reports 0 controllers),*
 *         display / framebuffer bring-up + confirm,                     *
 *         a deliberate-fault stage that triggers #BP via int3 (proves   *
 *           the orchestrator's boot_guarded_run + cpu_exception_handler *
 *           still contain a fault end-to-end), and                      *
 *         a watchdog overrun stage (proves the timer ISR pumps the      *
 *           watchdog tick under the orchestrator).                      *
 *    5. boot_print_results_summary() -- per-stage OK/FAILED/SKIPPED.    *
 *    6. Final "Phase 3b complete" banner; cli/hlt forever.              *
 * ===================================================================== */

/* x64-only helpers exported by kernel_x64.c (the tiny long-mode helper TU). */
extern void x64_arch_serial_init(void);
extern void x64_arch_dump_multiboot(uint32_t magic, uintptr_t info);
extern void x64_arch_idt_install(void);
extern void x64_arch_pic_remap(void);
extern void x64_arch_walk_and_draw_framebuffer(uintptr_t info);
extern void x64_arch_legacy_vga_text_line(const char* s);

/*
 * Framebuffer text console for x64. Renders 8x16 white-on-black glyphs via
 * vesa_draw_char starting at row 4 (so the preserved 3-line proof-of-life
 * drawn at rows 0..2 by x64_arch_walk_and_draw_framebuffer() stays visible).
 * If vesa hasn't been initialized yet the FB part is a no-op; the COM1 +
 * 0xE9 mirrors still fire.
 *
 * No scrolling: once we hit the bottom of the panel, further characters are
 * dropped (serial still carries them, which is the canonical log for x64).
 * Phase 3c will replace this with a real framebuffer console driver.
 */
/* The first 4 8x16 rows are reserved for the Phase 3a.1 proof-of-life
 * lines drawn by x64_arch_walk_and_draw_framebuffer(). Everything from
 * this row down is the framebuffer console managed below. */
#define X64_FB_POL_RESERVED_ROWS 4

static int x64_fb_glyph_row = X64_FB_POL_RESERVED_ROWS;
static int x64_fb_glyph_col = 0;

static void x64_fb_putc(char c) {
    if (!vesa_is_initialized()) return;
    int max_cols = (int)(vesa_get_width()  / (uint32_t)FONT_WIDTH);
    int max_rows = (int)(vesa_get_height() / (uint32_t)FONT_HEIGHT);
    if (c == '\n') {
        x64_fb_glyph_col = 0;
        if (x64_fb_glyph_row + 1 < max_rows) x64_fb_glyph_row++;
        return;
    }
    if (c == '\r') { x64_fb_glyph_col = 0; return; }
    if (c == '\t') { c = ' '; }
    if (c == '\b' || c == 0x7F) {
        if (x64_fb_glyph_col > 0) {
            x64_fb_glyph_col--;
            vesa_draw_char(x64_fb_glyph_col * FONT_WIDTH,
                           x64_fb_glyph_row * FONT_HEIGHT,
                           ' ', 0x00FFFFFFu, 0x00000000u);
        }
        return;
    }
    if (x64_fb_glyph_col >= max_cols) {
        x64_fb_glyph_col = 0;
        if (x64_fb_glyph_row + 1 < max_rows) x64_fb_glyph_row++;
        else return;
    }
    if (x64_fb_glyph_row >= max_rows) return;
    vesa_draw_char(x64_fb_glyph_col * FONT_WIDTH,
                   x64_fb_glyph_row * FONT_HEIGHT,
                   c, 0x00FFFFFFu, 0x00000000u);
    x64_fb_glyph_col++;
}

/* Clear the framebuffer console below the proof-of-life lines and reset
 * the glyph cursor. No-op if vesa is not initialized (e.g. on UEFI
 * after the on-panel confirm timed out and reverted to VGA text floor). */
static void x64_fb_clear_below_pol(void) {
    if (!vesa_is_initialized()) {
        x64_fb_glyph_row = X64_FB_POL_RESERVED_ROWS;
        x64_fb_glyph_col = 0;
        return;
    }
    int top_y = X64_FB_POL_RESERVED_ROWS * FONT_HEIGHT;
    int h = (int)vesa_get_height() - top_y;
    if (h > 0) {
        vesa_fill_rect(0, top_y, (int)vesa_get_width(), h, 0x00000000u);
    }
    x64_fb_glyph_row = X64_FB_POL_RESERVED_ROWS;
    x64_fb_glyph_col = 0;
}

/*
 * Textcon mirror for the x64 print sink. Active when the display stage
 * bound the 80x25 text console (the VGA-compat boot path -- either x64
 * UEFI rendering into the GOP framebuffer, or x64 legacy BIOS writing
 * to 0xB8000). Maintains its own row/col cursor so the boot log scrolls
 * cleanly; on `\n` we move to the next row and scroll when we run off
 * the bottom. Handles `\r`, `\t` -> space, and `\b`/DEL.
 *
 * After the boot log finishes printing, x64_con_sync_shell_cursor()
 * copies the row/col into the shell's `cursor_row`/`cursor_col` so
 * shell_init() starts its prompt immediately below the last log line.
 */
static int x64_con_row = 0;
static int x64_con_col = 0;
#define X64_CON_ATTR 0x07u   /* light grey on black: a calm boot-log tone */

static void x64_con_putc(char c) {
    if (!con_ready()) return;
    if (c == '\n') {
        x64_con_col = 0;
        if (x64_con_row + 1 < CON_ROWS) {
            x64_con_row++;
        } else {
            con_scroll_up(1, X64_CON_ATTR);
            /* stay on last row */
        }
        return;
    }
    if (c == '\r') { x64_con_col = 0; return; }
    if (c == '\t') c = ' ';
    if (c == '\b' || c == 0x7F) {
        if (x64_con_col > 0) {
            x64_con_col--;
            con_put_cell(x64_con_row, x64_con_col, ' ', X64_CON_ATTR);
        }
        return;
    }
    if (x64_con_col >= CON_COLS) {
        x64_con_col = 0;
        if (x64_con_row + 1 < CON_ROWS) {
            x64_con_row++;
        } else {
            con_scroll_up(1, X64_CON_ATTR);
        }
    }
    con_put_cell(x64_con_row, x64_con_col, c, X64_CON_ATTR);
    x64_con_col++;
}

/* Hand off the boot-log cursor to the shell. Called from the x64 main
 * loop just before shell_init() so the prompt starts on a fresh line
 * immediately below the boot-stage results summary, not at row 0
 * overwriting it. */
static void x64_con_sync_shell_cursor(void) {
    if (!con_ready()) return;
    /* If the boot log ended mid-line, drop down to the next row so the
     * prompt does not concatenate onto the partial line. */
    int row = x64_con_row;
    if (x64_con_col != 0 && row + 1 < CON_ROWS) row++;
    if (row >= CON_ROWS) row = CON_ROWS - 1;
    cursor_row = (uint8_t)row;
    cursor_col = 0;
}

static void x64_print_sink(const char* str, void* ctx) {
    (void)ctx;
    while (*str) {
        char c = *str++;
        /* 0xE9 + COM1 mirror (same as serial_out). */
        outb(0xE9, (uint8_t)c);
        while ((inb(0x3F8 + 5) & 0x20) == 0) {}
        outb(0x3F8, (uint8_t)c);
        /* On-panel mirror. The graphical (VESA desktop) path is the
         * common one; the textcon path lights up when the user picked
         * the VGA-Compatibility GRUB entry. They are mutually exclusive
         * because the display stage commits to exactly one. */
        if (vesa_is_initialized()) {
            x64_fb_putc(c);
        } else {
            x64_con_putc(c);
        }
    }
}

/* --- x64 stage wrappers --- */
static void stage_x64_idt(void)        { x64_arch_idt_install(); }
static void stage_x64_pic(void)        { x64_arch_pic_remap(); }
static void stage_x64_timer(void)      { timer_init(100); timer_calibrate_tsc(); }
static void stage_x64_input(void)      { input_init(); keyboard_init(); mouse_init(); }
static void stage_x64_hwsummary(void)  { boot_print_hardware_summary(); }
static void stage_x64_display(void)    { framebuffer_bringup(); report_display_diagnostics(); }
/*
 * Phase 3d: USB host stack. Brings up the unified usb_init() shared with
 * the x86 build, which:
 *   - calibrates the IRQ-independent TSC clock (already alive after Phase
 *     3b.0 timer_calibrate_tsc; usb_init re-checks),
 *   - reads gooberos.usb= from the unified boot config and either skips
 *     entirely (USB_OFF), refuses xHCI (safe), goes UHCI/OHCI-only
 *     (minimal), or tries every controller in priority order (full),
 *   - performs the BIOS legacy handoff for each candidate (UHCI USBLEGSUP,
 *     OHCI HcControl.IR, EHCI extended-cap, xHCI USBLEGCTLSTS),
 *   - on Intel xHCI gates the XUSB2PR / USB3_PSSEN port-routing writes on
 *     EHCI-companion presence (skipped on Bay Trail's xHCI-only SoC),
 *   - enumerates a single boot HID device under per-port + per-controller
 *     + per-scan time budgets backed by timer_deadline_ms.
 *
 * Wrapped in boot_guarded_run() with a 12-second watchdog (WD_USB) by
 * the orchestrator so a wedged controller is contained.
 */
static void stage_x64_usb(void)        { usb_init(); }

/*
 * Phase 3f: storage controllers (SATA AHCI, NVMe, SD-host, USB-MSC)
 * discovered via the PCI scan brought up in stage_x64_hwsummary. The
 * scan adds one storage_device_info_t per controller; the SDHCI rung
 * and USB-MSC rung get their own probe routines (sdhci_probe_pci_
 * controller / usb_msc_probe_pci_controller) so the install state is
 * filled in correctly. ATA PIO devices are also enumerated through
 * the legacy 0x1F0/0x170 channels. Bounded by the WD_STORAGE 5 s
 * watchdog so a wedged AHCI/NVMe initialization can't hang the boot.
 */
extern void storage_init(void);
static void stage_x64_storage(void)    { storage_init(); }
static void stage_x64_acpi(void)       { acpi_init(); }
static void stage_x64_touchpad(void) {
    if (kstr_eq(g_boot_config.i2c, "off") ||
        kstr_eq(g_boot_config.touchpad, "off")) {
        print("[touchpad] disabled by cmdline.\n");
        return;
    }
    touchpad_init();
}

/*
 * Phase 3e: in-memory filesystem and VESA desktop bring-up.
 *
 * stage_x64_fs() runs fs_init() (the same routine the x86 build uses) to
 * stand up the in-memory tree before the desktop starts enumerating
 * /Desktop. It is fast and side-effect-free apart from kmalloc()ing the
 * root directory, but we still slot it as a regular stage so a failure
 * shows up in the results table instead of crashing the desktop later.
 *
 * stage_x64_desktop() runs vesa_desktop_init() under a generous 30-second
 * watchdog (WD_DESKTOP). Only the *init* half of the desktop runs here --
 * geometry query, vdesk_init (theme + backbuffer alloc), icon
 * registration, /Desktop enumeration, initial Shell window creation. The
 * unbounded event pump (vesa_desktop_main_loop) is dispatched by
 * kernel_main below AFTER boot_print_results_summary so the results
 * table prints before the kernel sits forever in vdesk_run().
 *
 * The static flag g_desktop_init_ok lets kernel_main decide whether to
 * enter the event pump or fall back to the Phase 3c line editor. A
 * contained fault inside vesa_desktop_init() simply leaves the flag at 0
 * and the orchestrator drops back to the REPL -- the smallest possible
 * diff vs. polling boot_record_stage's log.
 */
static int g_desktop_init_ok = 0;
extern void fs_init(void);
extern void vesa_desktop_init(void);
extern void vesa_desktop_main_loop(void);
/*
 * Phase 3e gave the x64 build its own kernel heap (a 1 MiB BSS-backed
 * bump allocator). Phase 3f bumps the budget to 4 MiB and swaps the
 * underlying allocator for the free-list implementation in
 * lib/memory.c (kfree + krealloc with coalescing). Storage drivers,
 * the install path, the editor, taskmgr and games all allocate and
 * release non-trivial amounts during a long session; the bump
 * allocator would have run the heap dry. The free-list keeps the same
 * memory_init / kmalloc / kfree / krealloc surface so callers don't
 * branch on arch.
 *
 * Phase 4 (display polish) bumps the budget to 8 MiB so the desktop
 * back-buffer alloc (pitch x height; ~4 MiB at 1366x768x32) fits in
 * the heap with room left for the rest of the kernel's allocations.
 * The fallback path (kmalloc returns NULL) drops the back-buffer and
 * renders direct to the LFB, but the 8 MiB ceiling is sized so that
 * never happens on the panels we target. See gui/desktop_vesa.c.
 *
 * 8 MiB is still BSS-backed; the identity-mapped low 4 GiB makes
 * direct access trivial, no MMIO mapping needed. The arena is sandbox-
 * safe: no syscalls, no mmap, no anything other than the static array
 * + free-list bookkeeping.
 */
#define X64_KERNEL_HEAP_SIZE (8u * 1024u * 1024u)
static uint8_t g_x64_kernel_heap[X64_KERNEL_HEAP_SIZE];
static void stage_x64_heap(void) {
    memory_init(g_x64_kernel_heap, X64_KERNEL_HEAP_SIZE);
    char buf[24];
    print("[heap] init: size=");
    itoa((int)memory_total_bytes(), buf, 10); print(buf);
    print(" free=");
    itoa((int)memory_free_bytes(), buf, 10); print(buf);
    print(" (free-list, coalescing, BSS-backed)\n");
    serial_out("[heap] init: size=");
    itoa((int)memory_total_bytes(), buf, 10); serial_out(buf);
    serial_out(" free=");
    itoa((int)memory_free_bytes(), buf, 10); serial_out(buf);
    serial_out("\n");
}
static void stage_x64_fs(void) {
    fs_init();
    print("[boot] fs_init complete (in-memory tree ready).\n");
}

/*
 * Phase 3f: with taskmgr/process.c lifted into the x64 link the registry
 * is empty until something calls create_process. Mirror the x86 path
 * (kernel.c::register_kernel_process) and seed pid=1="kernel.bin" so
 * process_is_protected(1) keeps the kernel entry unkillable. Memory
 * footprint is approximated by the BSS heap budget (best the orchestrator
 * has at this point); the desktop's per-frame metrics update is fed by
 * the desktop itself.
 */
extern int create_process(const char* name, size_t memory_kb);
static void register_kernel_process_x64(void) {
    /* heap budget is 4 MiB on x64; report in KB so taskmgr column is sane */
    create_process("kernel.bin", X64_KERNEL_HEAP_SIZE / 1024u);
}

static void stage_x64_desktop(void) {
    register_kernel_process_x64();
    vesa_desktop_init();
    g_desktop_init_ok = 1;
}

/* Forward declaration of the Phase 3c interactive line editor. The body
 * lives near kernel_main_x64 below so it has direct access to the
 * x64_fb_putc / x64 print sink helpers. Run by kernel_main_x64 outside
 * the productive stage table so the boot-stage results summary prints
 * BEFORE the read loop blocks for input. Kept as a fallback surface in
 * Phase 3e when the Shell / desktop stage fails. */
static void x64_line_editor_run(void);

/*
 * Deliberate-fault self-test. Triggers #BP via int3, which dispatches
 * through the long-mode IDT to isr3_stub -> cpu_exception_handler. With a
 * guard armed (boot_guarded_run), the handler longjmps back here and the
 * stage is recorded as FAILED -- which is the *expected* outcome, hence
 * the "(self-test)" suffix and the gooberos.selftest=1 gate in
 * kernel_main_x64. Preserves the 3b.0 milestone behavior so a regression
 * in the unified orchestrator's fault-containment path is immediately
 * visible when the user opts in.
 */
static void stage_x64_fault_probe(void) {
    serial_out("[stage:fault-probe] deliberately triggering #BP (int3) -- "
               "boot_guarded_run should contain it.\n");
    __asm__ volatile ("int3");
    serial_out("[stage:fault-probe] BUG: int3 returned (guard did NOT catch).\n");
}

static volatile int x64_watchdog_probe_observed = 0;

/*
 * Watchdog overrun self-test. Re-enables interrupts inside the stage and
 * busy-loops with HLTs. timer_interrupt_handler() pumps
 * boot_watchdog_tick() every 10 ms; once the stage budget elapses,
 * boot_watchdog_tick() longjmps out via the guard. A wedged timer ISR or
 * a broken EOI would leave us spinning until the firmware/QEMU killed
 * the machine, so a clean FAILED return here is the test. Like
 * stage_x64_fault_probe this is gated behind gooberos.selftest=1 so the
 * default boot-stage results table is all-green.
 */
static void stage_x64_watchdog_probe(void) {
    serial_out("[stage:watchdog-probe] entering bounded busy-loop with IRQ0 "
               "enabled; boot_watchdog_tick should longjmp out.\n");
    x64_watchdog_probe_observed = 1;
    __asm__ volatile ("sti");
    for (;;) __asm__ volatile ("hlt");
}

/*
 * Phase 3c productive boot stage table.
 *
 * The minimal floor (IDT/PIC/Timer/PS/2 input) always runs. Interrupts
 * are enabled after the PS/2 input stage so the watchdog can tick under
 * the risky stages. The display stage now runs the on-panel confirm-or-
 * revert gate (PS/2 keyboard is live above it).
 *
 * The Phase 3c interactive line editor is run separately, AFTER the
 * boot-stage results summary (and the optional self-test summary)
 * prints. That way the "Boot stage results" table stays all-green on a
 * healthy boot AND prints to serial+panel before the line editor blocks
 * the orchestrator forever waiting for input. See kernel_main below.
 *
 * The fault-probe + watchdog-probe self-tests live in a separate table
 * (k_boot_selftests_x64) and only run when gooberos.selftest=1 is on
 * the kernel cmdline -- approach (b) from the Phase 3c brief, with
 * stage names suffixed "(self-test, expected to fail)" and the results
 * printed in a dedicated `=== Self-test results ===` section so the
 * main table stays unambiguously all-green when the user did not opt
 * in.
 */
static const boot_stage_def_t k_boot_stages_x64[] = {
    /* --- Minimal floor: always runs, even in safe mode --- */
    { "IDT (64-bit, 256 gates)",     stage_x64_idt,       0, 0, NULL, 0, 0 },
    { "PIC remap",                   stage_x64_pic,       0, 0, NULL, 0, 0 },
    { "Timer (PIT 100Hz + TSC)",     stage_x64_timer,     0, 0, NULL, 0, 0 },
    { "PS/2 input + keyboard/mouse", stage_x64_input,     0, 1, NULL, 0, 0 },
    /* --- Risky stages: guarded, skipped in safe mode --- */
    { "Hardware summary (PCI scan)", stage_x64_hwsummary, 1, 0, NULL, 0, WD_HWSUMMARY },
    { "ACPI tables",                 stage_x64_acpi,      1, 0, NULL, 0, WD_HWSUMMARY },
    /*
     * Phase 3d: USB host stack between the PCI scan and the display stage,
     * matching the x86 ordering in k_boot_stages above. The 12-second
     * watchdog (WD_USB = 1200 ticks @ 100 Hz) is what saved the boot on
     * Bay Trail when xHCI deadlocked -- a wedged controller MUST be
     * contained so the orchestrator still reaches Display + REPL.
     */
    { "USB host stack",              stage_x64_usb,       1, 0, NULL, 0, WD_USB },
    /* No splash on the display stage: vesa_boot_splash() inside
     * framebuffer_bringup() draws the on-panel confirm prompt itself, so
     * a stage-level splash here would clobber it. */
    { "Display / framebuffer",       stage_x64_display,   1, 0, NULL, 1, 0 },
    /*
     * Phase 3e: in-memory filesystem + VESA desktop bring-up. Both run
     * AFTER the framebuffer is up. The desktop's first full-screen paint
     * (inside vdesk_init -> render_desktop) clears any RGB-test-pattern
     * residue from the splash hand-off, so by the time vesa_desktop_main_loop
     * runs the panel shows the actual desktop background, not the splash.
     *
     * The Filesystem stage carries a small watchdog budget out of an
     * abundance of caution -- the only failure mode is kmalloc returning
     * NULL, but if that ever happens we don't want a hung boot.
     *
     * The Shell / desktop stage runs vesa_desktop_init() under a 30-s
     * ceiling. The actual desktop event pump is dispatched outside the
     * stage table by kernel_main below.
     */
    { "Kernel heap",                 stage_x64_heap,      0, 0, NULL, 0, 0 },
    { "I2C HID touchpad",            stage_x64_touchpad,  1, 0, NULL, 0, WD_TOUCHPAD },
    { "Filesystem",                  stage_x64_fs,        1, 0, NULL, 0, 200 },
    /*
     * Phase 3f: Storage stage runs after Filesystem + before the desktop
     * so the install path / `storage` shell command can enumerate
     * AHCI / NVMe / SDHCI / USB-MSC controllers via the PCI scan that
     * Hardware summary already kicked off. Bounded by WD_STORAGE so a
     * stuck AHCI/NVMe initialization is contained.
     */
    { "Storage",                     stage_x64_storage,   1, 0, NULL, 0, WD_STORAGE },
    { "Shell / desktop",             stage_x64_desktop,   1, 0, NULL, 0, WD_DESKTOP },
};

/*
 * Optional self-test table. Each entry is *expected* to fail under the
 * boot guard -- they prove the fault-containment + watchdog plumbing
 * still works. Gated by gooberos.selftest=1 so a healthy default boot
 * does not produce confusing [FAILED] rows in the main results.
 */
static const boot_stage_def_t k_boot_selftests_x64[] = {
    { "Fault probe (self-test, expected to fail)",     stage_x64_fault_probe,    0, 0, NULL, 0, 0 },
    { "Watchdog probe (self-test, expected to fail)",  stage_x64_watchdog_probe, 0, 0, NULL, 0, 50 },
};

void kernel_main(uint32_t magic, uintptr_t info) {
    /*
     * Step 1. Initialize COM1 so every serial_out / print() call below
     *         shows up in QEMU's -serial file: capture. Both the 0xE9 and
     *         COM1 sinks are written; serial_out spins on LSR.THRE.
     */
    x64_arch_serial_init();

    serial_out("\n");
    serial_out("=========================================================\n");
    serial_out(" GooberOSx86 x86_64 long-mode kernel: Phase 1 milestone\n");
    serial_out("=========================================================\n");
    serial_out("[boot64] long mode operational; serial print confirmed.\n");

    /*
     * Step 2. Preserved Phase 1 hand-off log: multiboot magic + info +
     *         cmdline. The kernel_x64.c helper handles the dec/hex
     *         formatting and the multiboot2 cmdline walk so the lines
     *         match the 3b.0 log byte-for-byte.
     */
    x64_arch_dump_multiboot(magic, info);

    serial_out("[boot64] sizeof(void*) = ");
    { char b[2] = { '0' + (char)sizeof(void*), 0 }; serial_out(b); }
    serial_out(" (expecting 8 in long mode)\n");

    /*
     * Step 3. Parse the boot config and capture the inherited framebuffer
     *         geometry into saved_fb_*. After this, kernel_fb_* accessors
     *         and framebuffer_bringup() see the correct GOP description.
     *         (Phase 3b widening: this now uses uintptr_t internally.)
     */
    boot_config_parse_multiboot(magic, (multiboot_info_t*)info);
    serial_out("Boot request: ");
    serial_out(g_boot_config.boot);
    serial_out("\n");
    if (boot_safe_mode()) {
        serial_out("Boot config: gooberos.safe=1 (compatibility mode); "
                   "skipping risky stages.\n");
    }

    /*
     * Step 4. Capture framebuffer diagnostics early. The visible boot surface
     *         is now the dark logo/progress splash in vesa_boot_splash().
     */
    x64_arch_walk_and_draw_framebuffer(info);

    /* Legacy 0xB8000 fallback for legacy-BIOS x64 boots. Invisible on UEFI
     * (the firmware does not map the legacy text plane), harmless either way.
     * Kept for parity with the 3b.0 log. */
    x64_arch_legacy_vga_text_line("GooberOSx86 x64 Phase 3b\n");

    /*
     * Step 5. Install the print sink so every subsequent print() call
     *         mirrors to COM1 + the framebuffer text console. Must happen
     *         AFTER the proof-of-life draws, so the print sink's row
     *         counter starts on a fresh line below them.
     */
    kernel_set_print_sink(x64_print_sink, NULL);

    serial_out("\n");
    serial_out("=========================================================\n");
    serial_out(" Phase 3b milestone: unified staged-boot orchestrator on x64\n");
    serial_out("=========================================================\n");
    print("GooberOS boot starting (x86_64, Phase 3b orchestrator)...\n");

    /*
     * Step 6. Run the productive x64 stages through the same
     *         boot_run_stages_table the x86 path uses. boot_safety.c
     *         owns the per-stage results log and the fault guard. The
     *         line editor runs separately below, AFTER we print the
     *         results summary, because it blocks waiting for input.
     */
    boot_run_stages_table(k_boot_stages_x64,
                          (int)(sizeof(k_boot_stages_x64) /
                                sizeof(k_boot_stages_x64[0])));

    /* Step 7. Per-stage OK/FAILED/SKIPPED summary to COM1 + framebuffer. */
    boot_print_results_summary();

    /*
     * Step 8. Optional self-test pass. The fault-probe (#BP via int3)
     *         and watchdog overrun probe deliberately trigger the
     *         contain-and-continue plumbing, so they ALWAYS report
     *         FAILED. We keep them as a smoke test for boot_safety.c
     *         but gate them behind gooberos.selftest=1 so a healthy
     *         default boot's main results table is unambiguously
     *         all-green. Results print into a dedicated `=== Self-test
     *         results ===` section.
     */
    if (kcmdline_contains("gooberos.selftest=1")) {
        serial_out("[boot] gooberos.selftest=1: running fault + watchdog "
                   "self-tests.\n");
        boot_results_reset();
        boot_run_stages_table(k_boot_selftests_x64,
                              (int)(sizeof(k_boot_selftests_x64) /
                                    sizeof(k_boot_selftests_x64[0])));
        boot_print_results_summary_titled("Self-test results");
    } else {
        serial_out("[boot] gooberos.selftest not set; skipping fault + "
                   "watchdog self-tests.\n");
    }

    serial_out("\n");
    serial_out("=========================================================\n");
    serial_out(" Phase 3e: VESA desktop boot stage executed; entering\n");
    serial_out("           desktop event pump (fallback: Phase 3c REPL).\n");
    serial_out("=========================================================\n");

    /*
     * Step 9. Phase 3e final surface.
     *
     * The unified boot orchestrator just ran the Filesystem and
     * Shell / desktop stages. If vesa_desktop_init() returned cleanly,
     * g_desktop_init_ok is 1 and we hand control to the event pump
     * (vesa_desktop_main_loop never returns; it pumps usb_poll at the
     * desktop frame rate which is well above 60 Hz, so HID input keeps
     * draining).
     *
     * If the Shell / desktop stage faulted or overran its watchdog, the
     * stage row in the results table is FAILED, g_desktop_init_ok is 0
     * and we fall through to the existing Phase 3c interactive line
     * editor as a minimal floor. The REPL itself runs under the same
     * boot_guarded_run wrapper that contained any prior fault, so a
     * contained fault inside the REPL also lands us in the cli/hlt loop
     * below. This is the "smallest diff" version of the
     * desktop_started_ok hand-off discussed in the Phase 3e brief.
     */
    /*
     * VGA-Compatibility path takes precedence: when the display stage
     * committed to the 80x25 text console (gooberos.boot=vga,
     * gooberos.display=vga-text, or the all-rungs-rejected fallback in
     * revert_to_text_floor), run the full interactive text shell instead
     * of the VESA desktop. The shell renders through textcon, which
     * mirrors cell writes to either the inherited GOP framebuffer (UEFI)
     * or 0xB8000 (legacy BIOS).
     */
    if (kernel_display_is_text_console()) {
        print("\nGooberOSx86 x64 VGA-Compatibility text shell\n");
        print("Type `help` for the command list.\n\n");
        /* Sync the shell's cell-grid cursor to where the boot log left
         * off so the prompt does not overwrite the results summary. */
        x64_con_sync_shell_cursor();
        shell_init();
        while (1) {
            usb_poll();
            touchpad_poll();
            shell_run();
            __asm__ volatile ("hlt");
        }
        /* unreachable */
    }

    if (g_desktop_init_ok) {
        print("Phase 3e online: VESA desktop event pump engaged.\n");
        vesa_desktop_main_loop();
        /* Should never return. If it does, treat it as the same contained-
         * fault scenario and fall through to the REPL fallback below. */
        serial_out("[boot] vesa_desktop_main_loop returned; falling back to REPL.\n");
    } else {
        serial_out("[boot] desktop init did not complete; "
                   "falling back to Phase 3c REPL.\n");
        print("Desktop unavailable; falling back to line editor.\n");
    }

    print("Phase 3c online: keyboard + mouse + line editor.\n");
    boot_guarded_run("Interactive line editor (Phase 3c fallback)",
                     x64_line_editor_run);

    serial_out("[boot] line editor returned (contained fault?); halting.\n");
    print("Line editor halted; system idle.\n");

    /* Belt-and-suspenders: ensure interrupts are off before the final halt. */
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("cli; hlt");
}

/* ===================================================================== *
 *  Phase 3c x86_64 line editor                                          *
 *                                                                       *
 *  Tiny on-panel REPL with three built-in commands (help / clear /     *
 *  halt). Echoes typed characters via x64_fb_putc + serial_out.        *
 *  Backspace erases the previous cell; ENTER dispatches the buffer.    *
 *                                                                      *
 *  Mouse coordinate pump: while waiting on keyboard input we drain     *
 *  the input event queue. Every 32 events we emit a single "[mouse]   *
 *  dx=N dy=N buttons=0xXX" line on serial; the accumulated (x, y)     *
 *  coordinate prints to the framebuffer console + serial about once   *
 *  per second based on timer_ticks(). The cursor is NOT rendered to   *
 *  the panel -- that's Phase 3e.                                       *
 * ===================================================================== */

#define X64_REPL_LINE_MAX  96

static void x64_repl_putc(char c) {
    /* Mirror to COM1 + framebuffer text console (the print sink path). */
    outb(0xE9, (uint8_t)c);
    while ((inb(0x3F8 + 5) & 0x20) == 0) {}
    outb(0x3F8, (uint8_t)c);
    x64_fb_putc(c);
}

static void x64_repl_puts(const char* s) {
    while (*s) x64_repl_putc(*s++);
}

static void x64_repl_print_prompt(void) {
    x64_repl_puts("goober> ");
}

static int x64_repl_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* Drain any queued input events. Periodically prints a one-line mouse
 * delta trace to serial (every 32 events) and an accumulated coordinate
 * line to panel + serial (~once per second). Also pumps the USB host
 * controller (Phase 3d): usb_poll() drains HID interrupt-IN reports off
 * whichever host controller came up, which is what feeds USB keyboard
 * keystrokes into keyboard_inject_char() so the REPL can read them out
 * of the same buffer the PS/2 IRQ1 path uses. Called from the line
 * editor's read loop while waiting on keyboard input. */
static void x64_repl_pump_mouse(void) {
    static uint32_t s_event_count = 0;
    static uint32_t s_last_coord_print_tick = 0;

    /* Phase 3d: pump the USB host stack. No-op on a healthy boot when no
     * USB controller initialized (usb_init() set a flag); on a successful
     * USB enumeration this drains HID reports off the active host. The
     * call is structurally safe even when usb_init() failed because
     * usb_poll() short-circuits on usb_initialized=0 / unhealthy host. */
    usb_poll();
    touchpad_poll();

    input_event_t ev;
    while (input_poll_event(&ev)) {
        if (ev.type == INPUT_EVENT_POINTER_MOVE ||
            ev.type == INPUT_EVENT_BUTTON_DOWN  ||
            ev.type == INPUT_EVENT_BUTTON_UP) {
            s_event_count++;
            if ((s_event_count & 0x1Fu) == 0) {
                char buf[16];
                serial_out("[mouse] dx=");
                itoa((int)ev.dx, buf, 10); serial_out(buf);
                serial_out(" dy=");
                itoa((int)ev.dy, buf, 10); serial_out(buf);
                serial_out(" buttons=");
                serial_out_hex((uint32_t)ev.buttons);
                serial_out("\n");
            }
        }
    }

    uint32_t now = timer_ticks();
    /* timer_init(100): 1 tick = 10 ms, so 100 ticks = 1 second. */
    if ((uint32_t)(now - s_last_coord_print_tick) >= 100u) {
        s_last_coord_print_tick = now;
        int mx = input_get_pointer_x();
        int my = input_get_pointer_y();
        uint8_t mb = input_get_pointer_buttons();
        char buf[16];
        serial_out("[mouse] x=");
        itoa(mx, buf, 10); serial_out(buf);
        serial_out(" y=");
        itoa(my, buf, 10); serial_out(buf);
        serial_out(" buttons=");
        serial_out_hex((uint32_t)mb);
        serial_out("\n");
    }
}

static void x64_repl_dispatch(const char* line) {
    if (line[0] == '\0') {
        return;
    }
    if (x64_repl_streq(line, "help")) {
        x64_repl_puts("Available commands:\n");
        x64_repl_puts("  help   -- print this help text\n");
        x64_repl_puts("  clear  -- clear the framebuffer console\n");
        x64_repl_puts("  halt   -- stop the CPU (cli; hlt forever)\n");
        return;
    }
    if (x64_repl_streq(line, "clear")) {
        x64_fb_clear_below_pol();
        x64_repl_puts("[clear] framebuffer console reset\n");
        return;
    }
    if (x64_repl_streq(line, "halt")) {
        x64_repl_puts("halting...\n");
        serial_out("[repl] halt: entering cli/hlt loop forever.\n");
        __asm__ volatile ("cli");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    x64_repl_puts("unknown command: ");
    x64_repl_puts(line);
    x64_repl_putc('\n');
}

static void x64_line_editor_run(void) {
    char line[X64_REPL_LINE_MAX];
    uint32_t len = 0;

    /* Make sure interrupts are enabled so the keyboard IRQ can fire.
     * boot_guarded_run() will restore the pre-stage IF on return; the
     * boot orchestrator already enabled interrupts after the PS/2 input
     * stage, but belt-and-suspenders. */
    __asm__ volatile ("sti");

    /* Drain any pre-buffered keystrokes left over from the on-panel
     * confirm gate so a stray ENTER doesn't auto-dispatch an empty line. */
    while (keyboard_has_char()) (void)keyboard_read_char();

    serial_out("[repl] interactive line editor up; type 'help' for commands.\n");
    x64_repl_print_prompt();

    for (;;) {
        if (keyboard_has_char()) {
            char c = keyboard_read_char();
            if (c == 0) {
                /* keyboard_read_char returns 0 when buffer empty; the
                 * has_char check above guards against this, but be safe. */
                continue;
            }
            serial_out("[kbd] keychar=");
            if (c == '\n' || c == '\r') {
                serial_out("\\n\n");
            } else if (c == '\b' || c == 0x7F) {
                serial_out("\\b\n");
            } else if (c >= 0x20 && c < 0x7F) {
                char buf[2] = { c, 0 };
                serial_out(buf);
                serial_out("\n");
            } else {
                serial_out("0x");
                serial_out_hex((uint32_t)(uint8_t)c);
                serial_out("\n");
            }

            if (c == '\n' || c == '\r') {
                x64_repl_putc('\n');
                line[len] = '\0';
                x64_repl_dispatch(line);
                len = 0;
                x64_repl_print_prompt();
                continue;
            }
            if (c == '\b' || c == 0x7F) {
                if (len > 0) {
                    len--;
                    x64_repl_putc('\b');
                }
                continue;
            }
            if (c >= 0x20 && c < 0x7F) {
                if (len + 1 < sizeof(line)) {
                    line[len++] = c;
                    x64_repl_putc(c);
                } else {
                    /* Line full: ignore further chars until ENTER.
                     * Beep on serial only. */
                    serial_out("[repl] line buffer full; press ENTER\n");
                }
                continue;
            }
            /* Anything else (extended keys, F-keys, arrows): drop. */
        } else {
            /* No keyboard byte: drain mouse events + tick coord print. */
            x64_repl_pump_mouse();
            __asm__ volatile ("hlt");
        }
    }
}
#endif /* __x86_64__ kernel_main */
