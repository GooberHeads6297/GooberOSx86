#ifndef BASIC_DISPLAY_H
#define BASIC_DISPLAY_H

#include <stdint.h>
#include "display.h"

/*
 * Microsoft Basic Display Adapter-style bring-up for GooberOS (x86 BIOS path).
 *
 * Design goals (mirroring Windows ms disp / boot handoff behaviour):
 *   1. Never modeset over a working firmware/GRUB graphics mode when avoidable.
 *   2. Keep the loader's LFB base; fix bytes_per_scanline from a pitch catalog.
 *   3. Try vendor plane repoint (Intel) before assuming the multiboot tag scans out.
 *   4. Walk a broad driver ladder (intel -> basic -> vesa -> bochs) before VGA text.
 *   5. Log every strategy step on serial 0xE9 for post-mortem on real hardware.
 */

typedef struct {
    uint64_t addr;
    uint32_t w, h, pitch;
    uint8_t  bpp, type;
    int      have;
} kernel_loader_fb_t;

void kernel_loader_fb_get(kernel_loader_fb_t* out);
void kernel_loader_fb_set(const kernel_loader_fb_t* fb);
int  kernel_loader_fb_usable(void);

typedef int (*basic_display_confirm_fn)(const display_driver_ops_t* drv,
                                        const display_framebuffer_t* fb,
                                        void* ctx);

/* Returns 1 if scanout was preserved (no real-mode VBE call). */
int basic_display_scanout_preserved(void);

/* MS-style early prep: pitch catalog + alignment heuristics, no modeset. */
void basic_display_prepare_loader_fb(void);

/* Full-screen checkerboard on inherited LFB (proves CPU writes reach scanout buffer). */
void basic_display_paint_probe(void);

/* Try catalog + padded pitches; returns 1 if pitch changed. */
int basic_display_try_all_pitches(void);

/*
 * Probe drivers in MS Basic Display order. Skips names that are NULL-terminated
 * early when a driver confirms. intel_first=0 skips Intel even if listed.
 */
const display_driver_ops_t* basic_display_probe_ladder(
    uint32_t req_w, uint32_t req_h,
    display_framebuffer_t* out,
    basic_display_confirm_fn confirm,
    void* confirm_ctx,
    int intel_first);

#endif
