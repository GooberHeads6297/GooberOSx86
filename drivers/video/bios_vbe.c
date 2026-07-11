#include "bios_vbe.h"
#include "drivers/bios/rm_thunk.h"
#include "drivers/diagnostics/driver_log.h"
#include "drivers/io/io.h"
#include "lib/string.h"

#ifdef __i386__

typedef struct __attribute__((packed)) {
    uint16_t pref_w;
    uint16_t pref_h;
    uint8_t  ok;
    uint8_t  pad;
    uint16_t mode;
    uint32_t lfb_phys;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t  bpp;
    uint8_t  pad2[3];
} bios_vbe_params_t;

static void vbe_serial(const char* s) {
    while (*s) outb(0xE9, (uint8_t)*s++);
}

static void vbe_serial_hex32(uint32_t v) {
    const char* hex = "0123456789ABCDEF";
    char buf[11];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 8; i++)
        buf[2 + i] = hex[(v >> ((7 - i) * 4)) & 0xF];
    buf[10] = '\0';
    vbe_serial(buf);
}

void bios_enable_a20(void) {
    /* Fast A20 gate (port 0x92, bit 1). */
    uint8_t v = inb(0x92);
    if (!(v & 0x02)) {
        outb(0x92, (uint8_t)(v | 0x02));
        vbe_serial("[vbe] A20 enabled via port 0x92\n");
    }
}

void bios_vbe_log_result(const bios_vbe_result_t* res, int rm_status) {
    if (rm_status != 0) {
        driver_log("[vbe] BIOS trampoline gateway failed, status=");
        driver_log_u32((uint32_t)rm_status);
        driver_log_line("");
        vbe_serial("[vbe] BIOS trampoline gateway failed\n");
        return;
    }
    if (!res || !res->ok) {
        driver_log_line("[vbe] BIOS modeset failed (no suitable linear mode).");
        vbe_serial("[vbe] BIOS modeset failed\n");
        return;
    }
    driver_log("[vbe] BIOS modeset OK mode=0x");
    driver_log_hex32((uint32_t)res->mode);
    driver_log(" ");
    driver_log_u32(res->width);
    driver_log("x");
    driver_log_u32(res->height);
    driver_log("x");
    driver_log_u32((uint32_t)res->bpp);
    driver_log(" pitch=");
    driver_log_u32(res->pitch);
    driver_log(" LFB=");
    driver_log_hex32(res->lfb_phys);
    driver_log_line("");
    vbe_serial("[vbe] BIOS modeset OK LFB=");
    vbe_serial_hex32(res->lfb_phys);
    vbe_serial(" pitch=");
    vbe_serial_hex32(res->pitch);
    vbe_serial("\n");
}

int bios_vbe_pitch_stride_ok(uint32_t lfb_phys, uint32_t width, uint32_t height,
                             uint32_t pitch, uint8_t bpp) {
    volatile uint8_t* base;
    uint32_t bpx;

    (void)width;
    (void)height;
    if (!lfb_phys || !pitch) return 0;
    if (bpp == 32) bpx = 4;
    else if (bpp == 24) bpx = 3;
    else return 0;
    if (pitch < width * bpx) return 0;

    base = (volatile uint8_t*)(uintptr_t)lfb_phys;
    base[0] = 0xAA;
    base[1] = 0xBB;
    base[pitch]     = 0x55;
    base[pitch + 1] = 0x66;

    if (base[0] != 0xAA || base[1] != 0xBB) return 0;
    if (base[pitch] != 0x55 || base[pitch + 1] != 0x66) return 0;
    if (pitch >= 2 && base[2] == 0x55) return 0;
    return 1;
}

void bios_vbe_clear_framebuffer(const bios_vbe_result_t* fb) {
    volatile uint8_t* base;
    uint32_t y;

    if (!fb || !fb->ok || !fb->lfb_phys || !fb->pitch || !fb->height) return;
    base = (volatile uint8_t*)(uintptr_t)fb->lfb_phys;

    for (y = 0; y < fb->height; y++) {
        volatile uint8_t* row = base + y * fb->pitch;
        uint32_t x;
        if (fb->bpp == 32) {
            for (x = 0; x < fb->width; x++)
                ((volatile uint32_t*)row)[x] = 0x0005080CU;
        } else if (fb->bpp == 24) {
            for (x = 0; x < fb->width; x++) {
                row[x * 3]     = 0x0C;
                row[x * 3 + 1] = 0x08;
                row[x * 3 + 2] = 0x05;
            }
        }
    }
}

int bios_vbe_pitch_geometry_ok(uint32_t width, uint32_t height,
                               uint32_t pitch, uint8_t bpp) {
    uint32_t min_pitch;
    uint32_t max_pitch;

    if (width < 320 || height < 200 || !pitch) return 0;
    if (bpp != 24 && bpp != 32) return 0;
    min_pitch = width * ((uint32_t)bpp / 8U);
    max_pitch = min_pitch + (min_pitch / 2U) + 64U;
    return pitch >= min_pitch && pitch <= max_pitch;
}

static int vbe_params_accept(bios_vbe_params_t* params, bios_vbe_result_t* out,
                             const char* fail_tag, int stride_probe) {
    if (!params->ok || params->lfb_phys == 0 ||
        params->width < 320 || params->height < 200 ||
        (params->bpp != 24 && params->bpp != 32)) {
        if (fail_tag) {
            vbe_serial(fail_tag);
            vbe_serial("\n");
        }
        return 0;
    }

    if (!bios_vbe_pitch_geometry_ok(params->width, params->height,
                                    params->pitch, params->bpp)) {
        driver_log("[vbe] BIOS pitch ");
        driver_log_u32(params->pitch);
        driver_log(" outside geometry bounds for ");
        driver_log_u32(params->width);
        driver_log("x");
        driver_log_u32(params->height);
        driver_log_line("");
        return 0;
    }

    if (stride_probe &&
        !bios_vbe_pitch_stride_ok(params->lfb_phys, params->width, params->height,
                                  params->pitch, params->bpp)) {
        driver_log_line("[vbe] BIOS LFB pitch stride test failed.");
        vbe_serial("[vbe] pitch stride test failed\n");
        return 0;
    }

    if (out) {
        out->ok = 1;
        out->lfb_phys = params->lfb_phys;
        out->width = params->width;
        out->height = params->height;
        out->pitch = params->pitch;
        out->bpp = params->bpp;
        out->mode = params->mode;
    }
    return 1;
}

int bios_vbe_query_current_mode_ex(bios_vbe_result_t* out, int stride_probe) {
    bios_vbe_params_t* params = (bios_vbe_params_t*)(uintptr_t)BIOS_VBE_PARAMS_PHYS;
    int rm_status;

    if (out) {
        out->ok = 0;
        out->lfb_phys = 0;
        out->width = out->height = out->pitch = 0;
        out->bpp = 0;
        out->mode = 0;
    }

    if (!bios_rm_init()) {
        driver_log_line("[vbe] rm thunk init failed (blob too large).");
        return 0;
    }

    memset(params, 0, sizeof(*params));
    bios_enable_a20();

    driver_log_line("[vbe] querying BIOS current VBE mode (4F03/4F01, no modeset)...");
    vbe_serial("[vbe] querying current VBE mode\n");

    rm_status = bios_rm_call(BIOS_RM_FN_VBE_QUERY_CURRENT, params);
    if (rm_status != 0) {
        bios_vbe_log_result(out, rm_status);
        return 0;
    }

    if (!vbe_params_accept(params, out, "[vbe] current-mode query rejected\n",
                           stride_probe)) {
        bios_vbe_log_result(out, 0);
        return 0;
    }

    bios_vbe_log_result(out, 0);
    return 1;
}

int bios_vbe_query_current_mode(bios_vbe_result_t* out) {
    return bios_vbe_query_current_mode_ex(out, 1);
}

int bios_vbe_set_best_mode(uint32_t pref_w, uint32_t pref_h, bios_vbe_result_t* out) {
    bios_vbe_params_t* params = (bios_vbe_params_t*)(uintptr_t)BIOS_VBE_PARAMS_PHYS;
    int rm_status;

    if (out) {
        out->ok = 0;
        out->lfb_phys = 0;
        out->width = out->height = out->pitch = 0;
        out->bpp = 0;
        out->mode = 0;
    }

    if (!bios_rm_init()) {
        driver_log_line("[vbe] rm thunk init failed (blob too large).");
        return 0;
    }

    memset(params, 0, sizeof(*params));
    if (pref_w > 0xFFFFU) pref_w = 0xFFFFU;
    if (pref_h > 0xFFFFU) pref_h = 0xFFFFU;
    params->pref_w = (uint16_t)pref_w;
    params->pref_h = (uint16_t)pref_h;

    bios_enable_a20();

    driver_log_line("[vbe] probing BIOS VBE modes via real-mode trampoline...");
    vbe_serial("[vbe] entering real mode for VBE modeset\n");

    rm_status = bios_rm_call(BIOS_RM_FN_VBE_SET_MODE, params);
    if (rm_status != 0) {
        bios_vbe_log_result(out, rm_status);
        return 0;
    }

    if (!vbe_params_accept(params, out, "[vbe] params rejected after RM return\n",
                           1)) {
        bios_vbe_log_result(out, 0);
        return 0;
    }

    if (out)
        bios_vbe_clear_framebuffer(out);

    bios_vbe_log_result(out, 0);
    return 1;
}

#else /* !__i386__ */

void bios_enable_a20(void) {}
int bios_vbe_pitch_stride_ok(uint32_t lfb_phys, uint32_t width, uint32_t height,
                             uint32_t pitch, uint8_t bpp) {
    volatile uint8_t* base;
    uint32_t bpx;

    (void)height;
    if (!lfb_phys || !pitch) return 0;
    if (bpp == 32) bpx = 4;
    else if (bpp == 24) bpx = 3;
    else if (bpp == 16 || bpp == 15) bpx = 2;
    else return 0;
    if (pitch < width * bpx) return 0;

    /* Same stride probe as the BIOS path — works on UEFI GOP LFBs too. */
    base = (volatile uint8_t*)(uintptr_t)lfb_phys;
    base[0] = 0xAA;
    base[1] = 0xBB;
    base[pitch]     = 0x55;
    base[pitch + 1] = 0x66;

    if (base[0] != 0xAA || base[1] != 0xBB) return 0;
    if (base[pitch] != 0x55 || base[pitch + 1] != 0x66) return 0;
    if (pitch >= 2 && base[2] == 0x55) return 0;
    return 1;
}
void bios_vbe_clear_framebuffer(const bios_vbe_result_t* fb) { (void)fb; }

void bios_vbe_log_result(const bios_vbe_result_t* res, int rm_status) {
    (void)res;
    (void)rm_status;
}

int bios_vbe_set_best_mode(uint32_t pref_w, uint32_t pref_h, bios_vbe_result_t* out) {
    (void)pref_w;
    (void)pref_h;
    if (out) out->ok = 0;
    return 0;
}

int bios_vbe_query_current_mode(bios_vbe_result_t* out) {
    if (out) out->ok = 0;
    return 0;
}

int bios_vbe_query_current_mode_ex(bios_vbe_result_t* out, int stride_probe) {
    (void)stride_probe;
    if (out) out->ok = 0;
    return 0;
}

int bios_vbe_pitch_geometry_ok(uint32_t width, uint32_t height,
                               uint32_t pitch, uint8_t bpp) {
    uint32_t min_pitch;
    uint32_t max_pitch;

    if (width < 320 || height < 200 || !pitch) return 0;
    if (bpp != 24 && bpp != 32) return 0;
    min_pitch = width * ((uint32_t)bpp / 8U);
    max_pitch = min_pitch + (min_pitch / 2U) + 64U;
    return pitch >= min_pitch && pitch <= max_pitch;
}

#endif
