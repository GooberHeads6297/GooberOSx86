#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>

typedef enum {
    DISPLAY_DRIVER_NONE = 0,
    DISPLAY_DRIVER_VGA_TEXT,
    DISPLAY_DRIVER_VESA_LFB,
    /*
     * Basic Display Adapter: firmware/GRUB LFB only, no modeset, no vblank.
     * Prefer this on real Intel laptops (e.g. Lenovo 80M4) where aggressive
     * display probes leave a wedged or invisible panel.
     */
    DISPLAY_DRIVER_BASIC_LFB,
    DISPLAY_DRIVER_NATIVE_INTEL,
    DISPLAY_DRIVER_NATIVE_GENERIC,
    /*
     * Phase 4 (display polish, item 3): VGA mode-13h fallback. Rung sits
     * BETWEEN the LFB-class rungs (VESA / Bochs / Intel) and the VGA-text
     * floor, so a panel that can't drive a usable framebuffer still gets a
     * 320x200x8 graphical surface (vs. the 80x25 text floor).
     */
    DISPLAY_DRIVER_VGA_GRAPHICS
} display_driver_t;

typedef enum {
    DISPLAY_FORMAT_UNKNOWN = 0,
    DISPLAY_FORMAT_RGB565,
    DISPLAY_FORMAT_BGR888,
    DISPLAY_FORMAT_XRGB8888
} display_pixel_format_t;

typedef struct {
    display_driver_t driver;
    display_pixel_format_t format;
    /* Framebuffer base is `uintptr_t` so the same descriptor can carry an
     * x86 (32-bit) or x86_64 (64-bit) physical address without truncation.
     * Phase 3b widening: the Lenovo S21e-20 GOP base is sub-4GiB today, but
     * future UEFI firmware may hand back a > 4 GiB address. */
    uintptr_t framebuffer_addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t bpp;
    uint8_t available;
} display_mode_info_t;

/*
 * A linear framebuffer descriptor produced by a probe driver. This is the
 * common currency of the display framework: a driver's init() fills one of
 * these out and the caller (kernel.c) hands it to vesa_init().
 *
 * `framebuffer_addr` is `uintptr_t` so the descriptor survives a 64-bit
 * physical address end-to-end (Phase 3b pointer-width audit).
 */
typedef struct {
    uintptr_t framebuffer_addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t  bpp;
    display_pixel_format_t format;
} display_framebuffer_t;

typedef enum {
    DISPLAY_PRESENT_NONE = 0,
    DISPLAY_PRESENT_COPY_RECT,
    DISPLAY_PRESENT_COPY_FRAME,
    DISPLAY_PRESENT_VBLANK_COPY_RECT,
    DISPLAY_PRESENT_VBLANK_COPY_FRAME,
    DISPLAY_PRESENT_PAGE_FLIP
} display_present_mode_t;

typedef struct {
    uint32_t present_count;
    uint32_t vblank_waits;
    uint32_t vblank_misses;
    uint32_t promoted_frames;
    uint32_t last_dirty_area;
    display_present_mode_t last_mode;
} display_present_stats_t;

/*
 * A candidate display driver. Drivers are registered into a fixed-size,
 * priority-ordered registry (no dynamic allocation) and tried in order until
 * one produces a usable linear framebuffer.
 *
 *   name  - short selector token used by the gooberos.display= switch
 *           (e.g. "vesa", "bochs", "intel").
 *   id    - display_driver_t reported through display_get_mode().
 *   probe - returns non-zero if the backing source/hardware is present. May be
 *           NULL, in which case the driver is always considered present.
 *   init  - performs any hardware mode-set and fills *out with the resulting
 *           linear framebuffer. req_w/req_h/req_bpp are hints; a value of 0
 *           means "driver default". Returns non-zero on success.
 *
 * Both probe() and init() MUST be bounded (no unbounded hardware spins) and
 * MUST fail cleanly so the framework can fall through to the next candidate.
 */
typedef struct {
    const char* name;
    display_driver_t id;
    int (*probe)(void);
    int (*init)(uint32_t req_w, uint32_t req_h, uint8_t req_bpp,
                display_framebuffer_t* out);
    int (*wait_vblank)(uint32_t timeout_ticks);
    int (*present_rect)(int x, int y, int w, int h);
    int (*present_frame)(void);
    int (*page_flip)(void);
} display_driver_ops_t;

#define DISPLAY_MAX_DRIVERS 8

void display_register_framebuffer(display_driver_t driver,
                                  display_pixel_format_t format,
                                  uintptr_t framebuffer_addr,
                                  uint32_t width,
                                  uint32_t height,
                                  uint32_t pitch,
                                  uint8_t bpp);
void display_register_text_mode(void);

/*
 * Hard-reprogram the VGA controller back to standard 80x25 colour text mode
 * (mode 0x03) using direct register I/O, then re-upload the 8x16 boot font and
 * register the text mode with the framework. This is the floor the display
 * fallback chain reverts to: it restores the VGA-compatible CRTC/timing the
 * firmware uses for text -- the same state that is known to light the panel on
 * machines where a VBE linear-framebuffer mode scans out dark. It touches only
 * standard VGA registers (no PLL / panel-power), so it is safe to call from any
 * graphical mode. The caller should clear_screen() afterwards.
 */
void display_restore_vga_text(void);

const display_mode_info_t* display_get_mode(void);
const char* display_driver_name(display_driver_t driver);
const char* display_format_name(display_pixel_format_t format);
const char* display_present_mode_name(display_present_mode_t mode);
const display_present_stats_t* display_get_present_stats(void);
int display_present_vblank_reliable(void);
int display_present_rect(int x, int y, int w, int h);
int display_present_frame(void);
void display_present_note_promotion(void);

/* ---- Driver framework ---- */

/* Clear the driver registry (used before (re)registering candidates). */
void display_reset_drivers(void);

/* Append a driver to the registry. Later registrations have lower priority.
 * Ignored once DISPLAY_MAX_DRIVERS have been registered. */
void display_register_driver(const display_driver_ops_t* ops);

/*
 * Optional per-rung visibility confirmation hook. After a driver's init()
 * produces a candidate framebuffer, the framework calls this (if non-NULL)
 * BEFORE accepting the rung. Returning non-zero accepts the framebuffer;
 * returning zero rejects it and the framework cleanly abandons that rung and
 * continues down the ladder to the next candidate driver. This is how the
 * graceful fallback chain proves each framebuffer is actually usable instead
 * of blindly committing to the first one that initializes.
 *
 *   drv - the driver whose init() just succeeded.
 *   fb  - the candidate framebuffer it produced.
 *   ctx - opaque caller context passed through from display_probe_drivers().
 */
typedef int (*display_confirm_fn)(const display_driver_ops_t* drv,
                                  const display_framebuffer_t* fb,
                                  void* ctx);

/*
 * Walk the registry and bring up the first driver that probes present, whose
 * init() succeeds, AND (if confirm != NULL) whose resulting framebuffer passes
 * the confirmation hook. This is the formal firmware-FB -> generic-FB ->
 * native ladder: each rung is attempted in priority order and abandoned
 * cleanly if it cannot be confirmed visible.
 *
 *   force_name - NULL or "auto" tries every driver in priority order; any
 *                other value restricts the search to the driver of that name.
 *   req_*      - resolution/depth hints forwarded to each driver's init().
 *   out        - filled with the chosen framebuffer on success.
 *   confirm    - optional visibility gate (see display_confirm_fn); may be NULL.
 *   confirm_ctx- opaque pointer handed to confirm().
 *
 * Returns the chosen driver ops, or NULL if nothing succeeded/confirmed (call
 * display_last_error() for a human-readable reason).
 */
const display_driver_ops_t* display_probe_drivers(const char* force_name,
                                                  uint32_t req_w,
                                                  uint32_t req_h,
                                                  uint8_t req_bpp,
                                                  display_framebuffer_t* out,
                                                  display_confirm_fn confirm,
                                                  void* confirm_ctx);

/* Diagnostic string set by drivers/framework explaining the last failure. */
void display_set_error(const char* msg);
const char* display_last_error(void);

#endif
