#ifndef KERNEL_H
#define KERNEL_H

#include <stddef.h>
#include <stdint.h>

#include "gooberos_arch.h"
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

/*
 * Pure-serial diagnostic sink (COM1 + port 0xE9), bypassing the framebuffer/
 * panel entirely. Use for low-level oracles where the on-screen console cannot
 * be trusted (e.g. verifying the desktop pump keeps running after a keypress on
 * VirtualBox). Host captures via VBox COM1 -> file or QEMU -serial file:. */
void kserial_note(const char* s);

/* Enable/disable COM1 (0x3F8) transmit at runtime. On x64 COM1 TX starts
 * disabled (real laptops lack a usable UART); the debug path enables it so the
 * kserial_note oracle reaches a VM's COM1 raw-file capture. */
void kernel_serial_com1_enable(int on);

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
    char usb[16];       /* gooberos.usb=     ("" if unset; "on"|"full" enables
                         * Braswell xHCI which is skipped by default) */
    char i2c[16];       /* gooberos.i2c=     (""/"full", "off", "safe") */
    char touchpad[16];  /* gooberos.touchpad=(""|"auto" probe if ACPI match;
                         * "on"|"poll"|"irq" force; "off" disable) */
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
    /*
     * gooberos.usb.stack= ("new"|"legacy"; default "new")
     *
     * new    -> redesigned HCD registry + class drivers (usb_core_*)
     * legacy -> singleton host fallback / enumeration.c path
     */
    char usb_stack[16];
    /*
     * gooberos.usb.byt.phy= ("on"|"off"; default "on")
     * Bay Trail PHY/MMIO/clock-gating scripts (needed for EP0 after XUSB2PR).
     */
    int  usb_byt_phy;
    char root[24];      /* gooberos.root= ("auto", "live", or "dev:part") */
    /*
     * gooberos.storage= ("ata"|"sdhci"|"all"|"off"; default "")
     *
     * Controls which storage backends run MMIO/bring-up probes:
     *   ata   -> ATA PIO only; PCI controllers are inventoried, not probed
     *   sdhci -> ATA + SDHCI/eMMC probe
     *   all   -> ATA + SDHCI + USB-MSC scaffold probes
     *   off   -> ATA only, skip PCI storage inventory
     *   ""    -> live root: ata; persistent root (auto/dev:part): sdhci
     */
    char storage[16];
    /*
     * gooberos.vbe= ("bios"|"loader"|"off"; default depends on display=)
     *   bios   -> INT 10h VBE modeset via real-mode trampoline before adopting LFB
     *   loader -> use GRUB/multiboot framebuffer only (retry BIOS on confirm fail)
     *   off    -> never call the BIOS VBE trampoline
     */
    char vbe[16];
} boot_config_t;

/*
 * Smart boot profile: filled when gooberos.boot=smart resolves hardware
 * signals (loader FB, Bay Trail, Bochs, SDHCI) into concrete display/storage
 * settings before framebuffer_bringup(). Explicit cmdline fields win.
 */
typedef struct {
    int  active;            /* 1 if this boot used Smart resolution */
    char display[24];       /* resolved gooberos.display= value */
    char storage[16];       /* resolved gooberos.storage= value */
    char reason[96];        /* human-readable decision for diagnostics */
    boot_display_confirm_t confirm;
} boot_smart_profile_t;

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

/*
 * x64 VGA compatibility: returns 1 when the display stage committed to
 * the 80x25 text console (either explicitly via gooberos.boot=vga /
 * gooberos.display=vga-text, or as the last-resort fallback when every
 * graphical rung was rejected). The x64 kernel_main runs the full
 * interactive text shell in that case instead of the VESA desktop.
 */
int kernel_display_is_text_console(void);

/* Mute/enable the x64 framebuffer print overlay (boot log glyphs). Desktop
 * mode disables this so print() cannot scribble over the VESA compositor. */
void kernel_set_fb_console_echo(int enabled);

const boot_config_t* boot_get_config(void);
int boot_safe_mode(void);
const boot_smart_profile_t* boot_smart_profile(void);

/*
 * FAT partition template loaded by GRUB as a multiboot2 module
 * (module2 /boot/install/FAT_PART.IMG). Used by install fat32 on USB live
 * boots where no ATAPI optical drive is present. Returns NULL if absent.
 */
const uint8_t* boot_fat_template_module(size_t* size_out);

#endif
