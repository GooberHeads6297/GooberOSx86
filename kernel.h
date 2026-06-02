#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>

#include "include/multiboot.h"

typedef void (*kernel_print_sink_t)(const char* str, void* ctx);

extern uint16_t* const VIDEO_MEMORY;
extern uint8_t cursor_row;
extern uint8_t cursor_col;
void update_cursor_visual();
void print(const char* str);
void kernel_set_print_sink(kernel_print_sink_t sink, void* ctx);
void kernel_clear_print_sink(void);
void clear_screen(void);

/* Boot mode detection */
int is_vesa_mode(void);
const char* vesa_boot_status(void);
const char* kernel_boot_request(void);
const char* kernel_boot_cmdline(void);
uint8_t kernel_fb_type(void);
uint8_t kernel_fb_bpp(void);
uint32_t kernel_fb_pitch(void);
uint32_t kernel_fb_width(void);
uint32_t kernel_fb_height(void);
/* Phase 3b widening: framebuffer base survives 64-bit kernels end-to-end.
 * On the 32-bit kernel `uintptr_t` is `uint32_t` so the ABI here is
 * byte-equivalent to the pre-3b prototype. */
uintptr_t kernel_fb_addr(void);

/*
 * Unified boot configuration, parsed once from the multiboot command line.
 *
 * This is the single place every gooberos.* switch lives so drivers do not
 * each re-walk the cmdline. The master safe switch (gooberos.safe=) is the
 * Mint "Compatibility mode" analogue: when set, the staged boot orchestrator
 * skips the risky graphical/USB stages and stays on the minimal VGA + PS/2
 * floor.
 *
 * Later workers (USB / display) should consult this instead of re-parsing:
 *   - const boot_config_t* cfg = boot_get_config();
 *   - if (boot_safe_mode()) { ...stay minimal... }
 *   - cfg->usb / cfg->display / cfg->theme hold the already-parsed values.
 */
/*
 * Phase 3f: gooberos.display.confirm=
 *   default  -> arch default (x64 always-on, x86 conditional)
 *   skip     -> NEVER show the on-panel confirm prompt this boot
 *   force    -> ALWAYS show the on-panel confirm prompt this boot
 *
 * The auto-revert prompt is the cheapest insurance against handing the
 * user a dark panel on UEFI x64, but it makes unattended QEMU CI
 * timeouts always trip and revert to VGA text. `skip` is the override
 * a CI smoke test sets; `force` is the sometimes-useful x86 inverse.
 */
typedef enum {
    BOOT_DISPLAY_CONFIRM_DEFAULT = 0,
    BOOT_DISPLAY_CONFIRM_SKIP,
    BOOT_DISPLAY_CONFIRM_FORCE
} boot_display_confirm_t;

typedef struct {
    char cmdline[128];  /* full raw cmdline (also via kernel_boot_cmdline()) */
    char boot[24];      /* gooberos.boot=    (default "default") */
    char display[24];   /* gooberos.display= (default "auto"; "safe" = adopt
                         * firmware FB only, arm probe + on-panel auto-revert) */
    char usb[16];       /* gooberos.usb=     ("" if unset) */
    char theme[16];     /* gooberos.theme=   ("" if unset) */
    char native[16];    /* gooberos.native=WxH (preferred native panel mode;
                         * "" if unset). Diagnostics warn when committed != native */
    int  safe;          /* gooberos.safe=    (1 = compatibility/safe mode) */
    boot_display_confirm_t display_confirm; /* gooberos.display.confirm= */
    /*
     * Phase 4 (display polish): gooberos.display.fps=N target frame rate for
     * the VESA desktop's back-buffer presentation loop. 0 keeps the built-in
     * default (60 Hz, 16 ms/frame). Clamped to [10, 120] when set.
     */
    int  display_fps;
    /*
     * gooberos.usb.hotplug= ("on"|"off"; default "on")
     *
     * usb_hotplug = 1 -> usb_poll() runs the per-port-status-change scan and
     *                    calls usb_enumerate_port_hotplug + usb_hid_attach
     *                    when a device is plugged in after boot.
     * usb_hotplug = 0 -> the post-boot polling path is a no-op. Useful as a
     *                    diagnostic switch on flaky chipsets where polling
     *                    PORTSC every 50 ticks itself causes problems; the
     *                    boot-time enumeration path is unaffected.
     *
     * gooberos.usb=off implies hotplug=0 because usb_init() returns early
     * before hotplug bookkeeping is initialized.
     */
    int  usb_hotplug;
} boot_config_t;

/*
 * Phase 4: the user enhancement-2 display-polish layer adds a real back-
 * buffer + whole-screen present + frame pacing. The kernel exposes the
 * effective target frame rate so the desktop can configure its event-loop
 * pacing without re-parsing the cmdline. Returns the configured fps if
 * gooberos.display.fps= was provided, else 60 (the panel-tear-free default).
 */
int kernel_display_target_fps(void);

/*
 * Phase 4 VGA-graphics fallback: returns 1 when the display framework
 * committed to the mode-13h driver (vga-graphics rung). The desktop
 * routes into desktop_vga13_render() in that case so the user always sees
 * SOMETHING on the panel, even when the VESA framebuffer is unusable.
 */
int kernel_display_is_vga_graphics(void);

const boot_config_t* boot_get_config(void);
int boot_safe_mode(void);

#endif
