#ifndef INTEL_GFX_H
#define INTEL_GFX_H

#include <stdint.h>
#include "display.h"

/*
 * Intel integrated graphics support (Gen7 / Ivy Bridge, HD Graphics 4000).
 *
 * SCOPE / HONESTY NOTE
 * --------------------
 * This module deliberately does NOT implement a from-scratch native modeset.
 * A full Intel modeset (programming the DPLL/PLL, transcoder + pipe timing,
 * the panel-fitter, FDI/DDI link training and the LVDS/eDP power-sequencing
 * state machines with carefully timed delays) is enormous and, when wrong,
 * typically leaves the panel permanently dark with no recovery. We keep all of
 * that out of scope on purpose.
 *
 * What this module DOES (the "inherit timings, repoint plane" path)
 * -----------------------------------------------------------------
 * Symptom on the target 2013 Lenovo (Ivy Bridge): VGA text and the GRUB menu
 * are visible, but our linear-framebuffer desktop is a black panel. The BIOS
 * has already lit a pipe/PLL/transcoder for the panel, so the hard part (link
 * up, panel powered, timings + PLL locked) is already done. The likely reason
 * we see nothing is that the primary display PLANE is either disabled, in a
 * tiled pixel format, or scanning out a different surface than the linear
 * framebuffer we draw into.
 *
 * So, WITHOUT touching the PLL / transcoder / pipe timings the BIOS set, we:
 *   1. detect the Intel display controller over PCI (vendor 0x8086, class 0x03)
 *      and read its GTTMMADR MMIO window (BAR0) + GMADR aperture (BAR2),
 *   2. read the already-programmed pipe registers (PIPECONF / PIPESRC) to find
 *      the live pipe whose source size matches the framebuffer we inherited,
 *   3. re-point that pipe's primary display plane at our framebuffer: program
 *      DSPSTRIDE (pitch), DSPLINOFF/DSPTILEOFF = 0, DSPCNTR (linear, untiled,
 *      correct pixel format, plane enabled, preserving pipe-select/gamma) and
 *      finally DSPSURF (the graphics-aperture offset of our buffer, which arms
 *      the surface flip on the next vblank).
 *
 * Every MMIO access is bounds-checked, every register value is sanity-checked,
 * and if anything looks wrong we decline so the framework falls back cleanly to
 * the inherited VESA LFB, then VGA text. We never spin unbounded and we never
 * reprogram timing/PLL state, so the worst case is "no change" (still the same
 * black panel the inherited path would have produced), never a hang or a dead
 * console.
 *
 * NOT RELEVANT: the Intel "media-driver" repository (intel-media-driver) is the
 * VA-API media decode/encode userspace driver. It has nothing to do with KMS /
 * display modeset and is not a reference for any of this; do not chase it.
 */

typedef struct {
    int present;
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint16_t vendor_id;
    uint16_t device_id;
    /* BAR0 = GTTMMADR (MMIO registers + GTT). Low bits masked off; note the
     * window can be 64-bit on this generation, in which case BAR1 holds the
     * high dword (always 0 in our 32-bit physical space). */
    uint32_t mmio_base;
    /* BAR2 = GMADR (linear-framebuffer aperture). Low bits masked off. */
    uint32_t aperture_base;
} intel_gfx_info_t;

/*
 * Diagnostic record describing the last plane-repoint attempt. Populated by the
 * "intel" driver's init() (success or failure) so kernel.c can dump the
 * BIOS-programmed register values we read, what we changed, and the outcome to
 * both the VGA console and the serial log.
 */
typedef struct {
    int attempted;         /* init() ran the repoint path at all */
    int succeeded;         /* plane was repointed */
    const char* reason;    /* human-readable failure/skip reason (NULL on ok) */

    uint32_t mmio_base;
    uint32_t aperture_base;

    int pipe;              /* selected pipe index (0=A,1=B,2=C), -1 if none */
    uint32_t pipeconf;     /* PIPECONF of the selected pipe */
    uint32_t pipe_w;       /* pipe source width  (from PIPESRC) */
    uint32_t pipe_h;       /* pipe source height (from PIPESRC) */
    uint32_t frame_count;  /* PIPE frame counter sampled BEFORE the flip */
    uint32_t frame_count2; /* PIPE frame counter sampled AFTER the flip+settle */
    int      frame_advanced; /* counter advanced across the flip (pipe alive) */

    uint32_t fb_addr;      /* framebuffer physical address we targeted */
    uint32_t gfx_offset;   /* fb_addr - aperture_base (the DSPSURF value) */
    uint32_t fb_w;
    uint32_t fb_h;
    uint32_t fb_pitch;
    uint8_t  fb_bpp;

    uint32_t old_dspcntr;
    uint32_t new_dspcntr;
    uint32_t old_dspsurf;
    uint32_t old_dspstride;
} intel_plane_report_t;

/*
 * Read-only scanout-health probe used by the display visibility gate
 * (display_confirm_visible() in kernel.c). Given the resolution of a candidate
 * framebuffer, this finds the BIOS-programmed pipe whose source size matches,
 * reports whether that pipe is enabled, samples the pipe frame counter twice
 * across a bounded settle to see if the pipe is actively scanning out, and
 * reads the (read-only) panel-power and backlight registers so the caller can
 * tell whether the eDP panel is actually being lit.
 *
 * This touches NO write registers and NEVER spins unbounded. It exists so the
 * framework can reject a framebuffer that scans out to a dark/dead pipe before
 * committing to it. It does not (and must not) attempt any modeset.
 */
typedef struct {
    int present;            /* an Intel display controller was found + mapped */

    int pipe;               /* pipe whose PIPESRC matches (w,h); -1 if none */
    uint32_t pipeconf;      /* PIPECONF of the matched pipe (0 if none) */
    int pipe_enabled;       /* PIPECONF_ENABLE set on the matched pipe */

    uint32_t frame_before;  /* PIPEFRAMECOUNT before the settle */
    uint32_t frame_after;   /* PIPEFRAMECOUNT after the settle */
    int frame_advanced;     /* frame_after != frame_before (pipe scanning out) */

    /* Panel-power sequencer (PCH PP_STATUS / PP_CONTROL). *_known is 0 when the
     * register read back as an implausible all-0/all-1 value (i.e. we could not
     * trust it), in which case the caller must not treat it as evidence. */
    uint32_t pp_status;
    uint32_t pp_control;
    int panel_power_known;
    int panel_power_on;     /* PP_STATUS panel-on status bit */

    /* Backlight PWM enable (CPU-side and PCH-side OR'd). */
    uint32_t blc_cpu;
    uint32_t blc_pch;
    int backlight_known;
    int backlight_on;
} intel_scanout_probe_t;

/* Run the read-only scanout-health probe described above. Returns non-zero if
 * an Intel controller was present and probed (out->present); zero (with out
 * zeroed) if no Intel GPU is present so the caller can skip the Intel-specific
 * heuristics. out must not be NULL. */
int intel_gfx_probe_scanout(uint32_t fb_w, uint32_t fb_h, intel_scanout_probe_t* out);

/* Detect an Intel display controller. Returns non-zero and fills *out if one
 * is present. out may be NULL. */
int intel_gfx_detect(intel_gfx_info_t* out);

/* Register the Intel display driver with the framework. Registered at LOW
 * priority (after the inherited "vesa" LFB and "bochs" dispi drivers) so the
 * known-good inherited framebuffer always wins in "auto" mode; the plane-
 * repoint path is normally reached only when the user forces it with
 * gooberos.display=intel. */
void intel_gfx_register_driver(void);

/* Return the diagnostic record from the most recent init() attempt (it always
 * exists; .attempted is 0 if init() was never run). Never NULL. */
const intel_plane_report_t* intel_gfx_get_plane_report(void);

#endif
