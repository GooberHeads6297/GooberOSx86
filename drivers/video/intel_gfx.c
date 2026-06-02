#include "intel_gfx.h"
#include "display.h"
#include "native_fb.h"
#include "../pci/pci.h"
#include <stddef.h>

#define INTEL_VENDOR_ID 0x8086

/* ===================================================================== *
 *  Detection                                                            *
 * ===================================================================== */

int intel_gfx_detect(intel_gfx_info_t* out) {
    pci_display_device_t devs[4];
    int n = pci_find_display_controllers(devs, 4);

    for (int i = 0; i < n && i < 4; i++) {
        if (devs[i].vendor_id != INTEL_VENDOR_ID) continue;

        if (out) {
            out->present = 1;
            out->bus = devs[i].bus;
            out->slot = devs[i].slot;
            out->func = devs[i].func;
            out->vendor_id = devs[i].vendor_id;
            out->device_id = devs[i].device_id;
            /*
             * On Ivy Bridge-era IGPs the BAR layout is:
             *   BAR0 (0x10) = GTTMMADR, 64-bit prefetchable MMIO -> registers
             *   BAR2 (0x18) = GMADR,    64-bit prefetchable      -> LFB aperture
             * The 64-bit BARs consume BAR1/BAR3 as their high dwords, which are
             * zero in our 32-bit physical address space.
             */
            out->mmio_base = devs[i].bar[0] & 0xFFFFFFF0U;
            out->aperture_base = devs[i].bar[2] & 0xFFFFFFF0U;
        }
        return 1;
    }

    if (out) {
        out->present = 0;
        out->bus = out->slot = out->func = 0;
        out->vendor_id = out->device_id = 0;
        out->mmio_base = out->aperture_base = 0;
    }
    return 0;
}

/* ===================================================================== *
 *  Gen7 (Ivy Bridge) display-engine register map                        *
 *                                                                       *
 *  All offsets are relative to GTTMMADR (BAR0). The three display pipes  *
 *  A/B/C live at a fixed 0x1000 stride, both for the pipe/timing block   *
 *  (base 0x60000) and the plane block (base 0x70000). These values are   *
 *  the long-standing i915 definitions for ILK..IVB and are stable.       *
 * ===================================================================== */

#define INTEL_NUM_PIPES        3
#define INTEL_PIPE_STRIDE      0x1000U

/* Pipe configuration / source-size (timing block at 0x60000 / 0x70000). */
#define PIPECONF_A             0x70008U
#define PIPESRC_A              0x6001CU

/* Primary display plane block (pipe A). */
#define DSPCNTR_A              0x70180U
#define DSPLINOFF_A            0x70184U
#define DSPSTRIDE_A            0x70188U
#define DSPSURF_A              0x7019CU
#define DSPTILEOFF_A           0x701A4U

/* Pipe frame counter (advances while the pipe is actively scanning out). */
#define PIPEFRAMECOUNT_A       0x70040U

/*
 * Panel-power and backlight registers (read-only here). On Ivy Bridge the
 * panel-power sequencer lives in the PCH register block. These are well above
 * the display-plane block, so they are guarded by a separate, wider READ-only
 * bounds limit (writes stay clamped to INTEL_MMIO_REG_LIMIT below).
 *
 *   PCH_PP_STATUS  bit31 = panel-on status (sequencer reports the panel lit)
 *   PCH_PP_CONTROL bit0  = power target on
 *   BLC_PWM_*_CTL2 bit31 = backlight PWM enable (CPU- and PCH-side variants)
 */
#define PCH_PP_STATUS          0xC7200U
#define PCH_PP_CONTROL         0xC7204U
#define BLC_PWM_CPU_CTL2       0x48250U
#define BLC_PWM_PCH_CTL1       0xC8250U

#define PP_STATUS_ON           (1U << 31)
#define PP_CONTROL_TARGET_ON   (1U << 0)
#define BLM_PWM_ENABLE         (1U << 31)

/* Highest register offset we are willing to WRITE (display block ends well
 * below this on Gen7). Anything past it is rejected by the write bounds check. */
#define INTEL_MMIO_REG_LIMIT   0x80000U

/* Highest register offset we are willing to READ. Read-only health probes
 * (panel power / backlight) reach into the PCH block, which sits above the
 * write limit but well inside the ~2MB GTTMMADR register window. */
#define INTEL_MMIO_READ_LIMIT  0x100000U

/* PIPECONF bits. */
#define PIPECONF_ENABLE        (1U << 31)
#define PIPECONF_STATE         (1U << 30)   /* read-only: pipe is active */

/* DSPCNTR bits. */
#define DISPPLANE_ENABLE          (1U << 31)
#define DISPPLANE_GAMMA_ENABLE    (1U << 30)
#define DISPPLANE_PIXFORMAT_MASK  (0xFU << 26)
#define DISPPLANE_BGRX555         (0x4U << 26)
#define DISPPLANE_BGRX565         (0x5U << 26)
#define DISPPLANE_BGRX888         (0x6U << 26)  /* 32bpp xRGB (B,G,R,X bytes) */
#define DISPPLANE_TILED           (1U << 10)
#define DISPPLANE_SEL_PIPE_MASK   (0x3U << 24)  /* preserved, never rewritten */

/* ===================================================================== *
 *  Bounds-checked MMIO accessors                                        *
 * ===================================================================== */

static uint32_t g_mmio_base = 0;

static inline int mmio_ok(uint32_t reg) {
    /* Require a plausible high MMIO BAR and an in-range, dword-aligned reg. */
    return (g_mmio_base != 0) && ((g_mmio_base & 0xFFFU) == 0) &&
           (reg < INTEL_MMIO_REG_LIMIT) && ((reg & 0x3U) == 0);
}

static inline uint32_t mmio_rd(uint32_t reg) {
    if (!mmio_ok(reg)) return 0;
    return *(volatile uint32_t*)(uintptr_t)(g_mmio_base + reg);
}

static inline void mmio_wr(uint32_t reg, uint32_t val) {
    if (!mmio_ok(reg)) return;
    *(volatile uint32_t*)(uintptr_t)(g_mmio_base + reg) = val;
}

/* Read-only accessor with the wider read bound (for PCH panel/backlight regs).
 * Never writes; returns 0 if the register/base looks implausible. */
static inline int mmio_rd_ok(uint32_t reg) {
    return (g_mmio_base != 0) && ((g_mmio_base & 0xFFFU) == 0) &&
           (reg < INTEL_MMIO_READ_LIMIT) && ((reg & 0x3U) == 0);
}

static inline uint32_t mmio_rd_ro(uint32_t reg) {
    if (!mmio_rd_ok(reg)) return 0;
    return *(volatile uint32_t*)(uintptr_t)(g_mmio_base + reg);
}

static inline uint32_t pipe_reg(uint32_t base_a, int pipe) {
    return base_a + (uint32_t)pipe * INTEL_PIPE_STRIDE;
}

/*
 * The PIT timer (100Hz) and interrupts are already running by the time the
 * display stage executes (the staged boot enables them after the kernel-heap
 * floor stage), so we can use the tick counter for a real, vblank-spanning
 * settle. timer_ticks() is declared extern to avoid pulling the timer header
 * into this driver's include path. A hard spin cap guarantees we never hang if
 * the timer is somehow not advancing.
 */
extern uint32_t timer_ticks(void);

static void settle_for_frames(void) {
    uint32_t start = timer_ticks();
    uint32_t spins = 0;
    /* ~40ms: at least two 60Hz frames, four 100Hz ticks. */
    while ((timer_ticks() - start) < 4u) {
        __asm__ volatile("inb $0x80, %%al" ::: "al");
        if (++spins >= 5000000u) break;
    }
}

/*
 * Tiny bounded settle delay. NOTE: the display framework runs from
 * detect_framebuffer() BEFORE timer_init()/sti, so timer_ticks() does not yet
 * advance and cannot be used as a timeout here. We therefore use a fixed,
 * capped busy loop with a serializing port read (the classic io_wait trick) so
 * we can never spin unbounded. The surface flip itself is armed by the DSPSURF
 * write and latched by hardware on the next vblank; this delay only lets the
 * write post.
 */
static void short_settle(void) {
    for (volatile int i = 0; i < 4096; i++) {
        __asm__ volatile("inb $0x80, %%al" ::: "al");
    }
}

/* ===================================================================== *
 *  Read-only scanout-health probe (visibility gate helper)             *
 * ===================================================================== */

/* Treat an all-zero or all-ones read as "register not trustworthy" (the BAR
 * is unmapped at that offset, the block is power-gated, or we mis-decoded). */
static int reg_trustworthy(uint32_t v) {
    return (v != 0u && v != 0xFFFFFFFFu);
}

int intel_gfx_probe_scanout(uint32_t fb_w, uint32_t fb_h, intel_scanout_probe_t* out) {
    if (!out) return 0;
    for (uint32_t i = 0; i < sizeof(*out); i++) ((uint8_t*)out)[i] = 0;
    out->pipe = -1;

    intel_gfx_info_t gpu;
    if (!intel_gfx_detect(&gpu) || !gpu.present)
        return 0;                         /* no Intel GPU -> caller skips us */
    if (gpu.mmio_base == 0 || (gpu.mmio_base & 0xFFFU) != 0)
        return 0;                         /* MMIO base unusable; cannot probe */

    g_mmio_base = gpu.mmio_base;
    out->present = 1;

    /* Find the live pipe whose BIOS-programmed source size matches the FB. */
    for (int p = 0; p < INTEL_NUM_PIPES; p++) {
        uint32_t conf = mmio_rd(pipe_reg(PIPECONF_A, p));
        uint32_t src  = mmio_rd(pipe_reg(PIPESRC_A, p));
        uint32_t pw = ((src >> 16) & 0xFFFFU) + 1U;
        uint32_t ph = (src & 0xFFFFU) + 1U;
        if (pw == fb_w && ph == fb_h) {
            out->pipe = p;
            out->pipeconf = conf;
            out->pipe_enabled = (conf & PIPECONF_ENABLE) ? 1 : 0;
            break;
        }
    }

    /* Sample the frame counter across a bounded settle: an advancing counter
     * means a pipe is actively scanning out. If we never matched a pipe by
     * resolution, sample pipe A so we still get a signal. */
    int fc_pipe = (out->pipe >= 0) ? out->pipe : 0;
    out->frame_before = mmio_rd(pipe_reg(PIPEFRAMECOUNT_A, fc_pipe));
    settle_for_frames();
    out->frame_after = mmio_rd(pipe_reg(PIPEFRAMECOUNT_A, fc_pipe));
    out->frame_advanced = (out->frame_after != out->frame_before) ? 1 : 0;

    /* Read-only panel power + backlight (the eDP "is the panel actually lit?"
     * signals). These can legitimately be unreadable on some parts, so we mark
     * them *_known only when the value is trustworthy. */
    out->pp_status  = mmio_rd_ro(PCH_PP_STATUS);
    out->pp_control = mmio_rd_ro(PCH_PP_CONTROL);
    if (reg_trustworthy(out->pp_status)) {
        out->panel_power_known = 1;
        out->panel_power_on = (out->pp_status & PP_STATUS_ON) ? 1 : 0;
    }

    out->blc_cpu = mmio_rd_ro(BLC_PWM_CPU_CTL2);
    out->blc_pch = mmio_rd_ro(BLC_PWM_PCH_CTL1);
    if (reg_trustworthy(out->blc_cpu) || reg_trustworthy(out->blc_pch)) {
        out->backlight_known = 1;
        out->backlight_on = ((out->blc_cpu & BLM_PWM_ENABLE) ||
                             (out->blc_pch & BLM_PWM_ENABLE)) ? 1 : 0;
    }

    return 1;
}

/* ===================================================================== *
 *  Plane-repoint path                                                   *
 * ===================================================================== */

static intel_plane_report_t g_report;

const intel_plane_report_t* intel_gfx_get_plane_report(void) {
    return &g_report;
}

static int fail(const char* reason) {
    g_report.reason = reason;
    display_set_error(reason);
    return 0;
}

/* Map an inherited direct-color depth to a Gen7 plane pixel format. Returns 0
 * (an invalid plane format selector, since 0 == 8bpp-indexed which we never
 * use) if the depth is not supported by the primary plane. */
static uint32_t plane_pixfmt_for_bpp(uint8_t bpp, int* ok) {
    *ok = 1;
    switch (bpp) {
        case 32: return DISPPLANE_BGRX888;
        case 16: return DISPPLANE_BGRX565;
        case 15: return DISPPLANE_BGRX555;
        default: *ok = 0; return 0;
    }
}

static int intel_probe(void) {
    return intel_gfx_detect(NULL);
}

/*
 * "intel" driver init: inherit the BIOS-programmed timings/PLL and re-point the
 * primary display plane at the framebuffer GRUB handed us. See intel_gfx.h for
 * the full rationale and safety model. Returns non-zero only if the plane was
 * repointed; otherwise records a reason and declines so the framework falls
 * back to the inherited VESA LFB / VGA text.
 */
static int intel_init(uint32_t req_w, uint32_t req_h, uint8_t req_bpp,
                      display_framebuffer_t* out) {
    (void)req_w; (void)req_h; (void)req_bpp;

    /* Reset the diagnostic record for this attempt. */
    for (uint32_t i = 0; i < sizeof(g_report); i++) ((uint8_t*)&g_report)[i] = 0;
    g_report.attempted = 1;
    g_report.pipe = -1;

    if (!out) return fail("intel: no output descriptor provided.\n");

    /* 1. Locate the Intel GPU and its MMIO/aperture windows. */
    intel_gfx_info_t gpu;
    if (!intel_gfx_detect(&gpu) || !gpu.present)
        return fail("intel: no Intel display controller present.\n");

    g_report.mmio_base = gpu.mmio_base;
    g_report.aperture_base = gpu.aperture_base;

    if (gpu.mmio_base == 0 || (gpu.mmio_base & 0xFFFU) != 0)
        return fail("intel: GTTMMADR (BAR0) MMIO base looks invalid.\n");
    if (gpu.aperture_base == 0)
        return fail("intel: GMADR (BAR2) aperture base is zero.\n");
    g_mmio_base = gpu.mmio_base;

    /* 2. Discover the framebuffer we should scan out (the inherited GRUB LFB).
     *    We never do a full modeset, so without an inherited buffer there is
     *    nothing to point the plane at -- decline.
     *
     *    `fb_addr` is `uintptr_t` (Phase 3b pointer-width audit). The
     *    diagnostic report stores the low 32 bits because the IVB GMADR
     *    aperture is always sub-4 GiB on this platform; if a future port
     *    moves to a > 4 GiB aperture, widen g_report.fb_addr too. */
    uintptr_t fb_addr_full = 0;
    uint32_t fb_w = 0, fb_h = 0, fb_pitch = 0;
    uint8_t fb_bpp = 0, fb_type = 0;
    if (!native_fb_get_inherited(&fb_addr_full, &fb_w, &fb_h, &fb_pitch, &fb_bpp, &fb_type))
        return fail("intel: no inherited linear framebuffer to repoint to.\n");
    uint32_t fb_addr = (uint32_t)fb_addr_full;

    g_report.fb_addr = fb_addr;
    g_report.fb_w = fb_w;
    g_report.fb_h = fb_h;
    g_report.fb_pitch = fb_pitch;
    g_report.fb_bpp = fb_bpp;

    int fmt_ok = 0;
    uint32_t pixfmt = plane_pixfmt_for_bpp(fb_bpp, &fmt_ok);
    if (!fmt_ok)
        return fail("intel: inherited framebuffer depth not supported by plane (need 32/16/15).\n");

    /* The primary plane requires a 64-byte-aligned linear stride and a
     * 4KB-aligned surface offset. If GRUB's framebuffer does not satisfy this,
     * repointing would skew/garble scanout -- decline rather than risk it. */
    if ((fb_pitch & 0x3FU) != 0)
        return fail("intel: inherited pitch is not 64-byte aligned; cannot repoint plane.\n");

    /* The framebuffer must live inside the GMADR aperture so that DSPSURF (a
     * graphics-address offset) can address it. */
    if (fb_addr < gpu.aperture_base)
        return fail("intel: framebuffer is below the GMADR aperture; not a graphics surface.\n");
    uint32_t gfx_offset = fb_addr - gpu.aperture_base;
    if ((gfx_offset & 0xFFFU) != 0)
        return fail("intel: framebuffer graphics offset is not 4KB aligned.\n");
    /* Reject implausibly large offsets (the IVB aperture is at most 512MB). */
    if (gfx_offset > (512U * 1024U * 1024U))
        return fail("intel: framebuffer graphics offset exceeds plausible aperture size.\n");
    g_report.gfx_offset = gfx_offset;

    /* 3. Find the live pipe whose BIOS-programmed source size matches the
     *    inherited framebuffer. We do NOT touch its timings or PLL. */
    int chosen = -1;
    for (int p = 0; p < INTEL_NUM_PIPES; p++) {
        uint32_t conf = mmio_rd(pipe_reg(PIPECONF_A, p));
        if (!(conf & PIPECONF_ENABLE)) continue;
        uint32_t src = mmio_rd(pipe_reg(PIPESRC_A, p));
        uint32_t pw = ((src >> 16) & 0xFFFFU) + 1U;
        uint32_t ph = (src & 0xFFFFU) + 1U;
        if (pw == fb_w && ph == fb_h) {
            chosen = p;
            g_report.pipeconf = conf;
            g_report.pipe_w = pw;
            g_report.pipe_h = ph;
            break;
        }
    }

    if (chosen < 0)
        return fail("intel: no enabled pipe matches the inherited resolution; "
                    "BIOS timings unknown -- declining (no native modeset).\n");
    g_report.pipe = chosen;

    /* Sample the frame counter BEFORE the flip; we compare against a post-flip
     * sample to prove the pipe is actually scanning out (see below). */
    uint32_t frame_pre = mmio_rd(pipe_reg(PIPEFRAMECOUNT_A, chosen));
    g_report.frame_count = frame_pre;

    /* Capture the BIOS plane state before we change anything. */
    uint32_t cntr_reg   = pipe_reg(DSPCNTR_A, chosen);
    uint32_t linoff_reg = pipe_reg(DSPLINOFF_A, chosen);
    uint32_t stride_reg = pipe_reg(DSPSTRIDE_A, chosen);
    uint32_t surf_reg   = pipe_reg(DSPSURF_A, chosen);
    uint32_t tileoff_reg= pipe_reg(DSPTILEOFF_A, chosen);

    uint32_t old_cntr   = mmio_rd(cntr_reg);
    g_report.old_dspcntr  = old_cntr;
    g_report.old_dspsurf  = mmio_rd(surf_reg);
    g_report.old_dspstride= mmio_rd(stride_reg);

    /* 4. Build the new plane control: preserve pipe-select + gamma, force
     *    linear (untiled), set our pixel format, enable the plane. */
    uint32_t new_cntr = old_cntr;
    new_cntr &= ~DISPPLANE_PIXFORMAT_MASK;
    new_cntr |= pixfmt;
    new_cntr &= ~DISPPLANE_TILED;     /* GRUB's VBE LFB is linear */
    new_cntr |= DISPPLANE_ENABLE;
    g_report.new_dspcntr = new_cntr;

    /* Program the plane. Per the PRM, write the surface base (DSPSURF) LAST:
     * that write arms an atomic flip to the new configuration on the next
     * vblank. Offsets are zeroed because we draw to the top-left of the
     * buffer with the full pitch as stride. */
    mmio_wr(stride_reg, fb_pitch);
    mmio_wr(linoff_reg, 0);
    mmio_wr(tileoff_reg, 0);
    mmio_wr(cntr_reg, new_cntr);
    mmio_wr(surf_reg, gfx_offset);   /* arms the flip */
    short_settle();                  /* let the register writes post */

    /*
     * Verify the pipe is genuinely scanning out: sample the frame counter again
     * after a vblank-spanning settle and require it to advance. A dead pipe
     * (counter frozen) means re-pointing the plane accomplished nothing visible,
     * so we restore the BIOS plane state and decline -- the framework then falls
     * back to the plain inherited LFB / VGA text instead of pretending success.
     *
     * NOTE: this only proves a pipe is ACTIVE, not that the eDP panel is lit.
     * On the target Lenovo the pipe can scan out while the panel stays dark
     * (panel-power/backlight not re-run for the new mode); that case is the
     * job of display_confirm_visible()'s panel-power check and, ultimately, the
     * on-panel confirm-or-revert gate -- this path is a narrow plane fix only.
     */
    settle_for_frames();
    uint32_t frame_post = mmio_rd(pipe_reg(PIPEFRAMECOUNT_A, chosen));
    g_report.frame_count2 = frame_post;
    g_report.frame_advanced = (frame_post != frame_pre) ? 1 : 0;
    if (!g_report.frame_advanced) {
        /* Restore exactly what the BIOS had so we leave no half-applied state. */
        mmio_wr(stride_reg, g_report.old_dspstride);
        mmio_wr(cntr_reg, old_cntr);
        mmio_wr(surf_reg, g_report.old_dspsurf);
        short_settle();
        return fail("intel: pipe frame counter did not advance after flip; "
                    "pipe is not scanning out -- declining (restored BIOS plane).\n");
    }

    g_report.succeeded = 1;
    g_report.reason = NULL;

    /* Report the same buffer/geometry we are drawing into; the CPU still
     * writes pixels through the inherited LFB physical address. */
    out->framebuffer_addr = (uintptr_t)fb_addr;
    out->width  = fb_w;
    out->height = fb_h;
    out->pitch  = fb_pitch;
    out->bpp    = fb_bpp;
    switch (fb_bpp) {
        case 32: out->format = DISPLAY_FORMAT_XRGB8888; break;
        case 16:
        case 15: out->format = DISPLAY_FORMAT_RGB565; break;
        default: out->format = DISPLAY_FORMAT_UNKNOWN; break;
    }
    return 1;
}

static const display_driver_ops_t intel_ops = {
    "intel",
    DISPLAY_DRIVER_NATIVE_INTEL,
    intel_probe,
    intel_init
};

void intel_gfx_register_driver(void) {
    display_register_driver(&intel_ops);
}
