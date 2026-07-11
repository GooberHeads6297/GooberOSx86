#ifndef BIOS_VBE_H
#define BIOS_VBE_H

#include <stdint.h>

typedef struct {
    uint32_t lfb_phys;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t  bpp;
    uint16_t mode;
    int      ok;
} bios_vbe_result_t;

int bios_vbe_set_best_mode(uint32_t pref_w, uint32_t pref_h, bios_vbe_result_t* out);
int bios_vbe_query_current_mode(bios_vbe_result_t* out);
/* stride_probe=0: geometry-only accept (no LFB writes; Bay Trail inherit path). */
int bios_vbe_query_current_mode_ex(bios_vbe_result_t* out, int stride_probe);
int bios_vbe_pitch_geometry_ok(uint32_t width, uint32_t height,
                               uint32_t pitch, uint8_t bpp);
void bios_vbe_log_result(const bios_vbe_result_t* res, int rm_status);

/* Enable the A20 line so CPU accesses above 1 MiB reach the VBE LFB. */
void bios_enable_a20(void);

/*
 * Validate that row N+1 is exactly pitch bytes after row N in the LFB.
 * Catches the rainbow-tear symptom caused by a bogus bytes_per_scanline.
 */
int bios_vbe_pitch_stride_ok(uint32_t lfb_phys, uint32_t width, uint32_t height,
                             uint32_t pitch, uint8_t bpp);

/* Clear the full framebuffer using the BIOS-reported pitch (not width*bpp). */
void bios_vbe_clear_framebuffer(const bios_vbe_result_t* fb);

#endif
