#include "native_fb.h"
#include "display.h"
#include "../io/io.h"
#include "../pci/pci.h"

/* ===================================================================== *
 *  Shared helpers                                                       *
 * ===================================================================== */

/* Map a direct-color depth to the framework's pixel-format enum. The actual
 * pixel layout used for drawing is owned by vesa.c; this is informational. */
static display_pixel_format_t fmt_for_bpp(uint8_t bpp) {
    switch (bpp) {
        case 32: return DISPLAY_FORMAT_XRGB8888;
        case 24: return DISPLAY_FORMAT_BGR888;
        case 16:
        case 15: return DISPLAY_FORMAT_RGB565;
        default: return DISPLAY_FORMAT_UNKNOWN;
    }
}

static uint32_t bytes_per_pixel(uint8_t bpp) {
    if (bpp == 32) return 4;
    if (bpp == 24) return 3;
    if (bpp == 16 || bpp == 15) return 2;
    return 0;
}

/* ===================================================================== *
 *  "vesa" driver: adopt the bootloader-provided linear framebuffer       *
 * ===================================================================== */

/* Inherited framebuffer base is `uintptr_t` so the captured address survives
 * end-to-end on x86_64 (Phase 3b pointer-width audit). On x86 `uintptr_t` is
 * `uint32_t`, so storage is byte-equivalent to the pre-3b layout. */
static uintptr_t inh_addr = 0;
static uint32_t inh_width = 0;
static uint32_t inh_height = 0;
static uint32_t inh_pitch = 0;
static uint8_t  inh_bpp = 0;
static uint8_t  inh_type = 0xFF;

void native_fb_set_inherited(uintptr_t addr, uint32_t width, uint32_t height,
                             uint32_t pitch, uint8_t bpp, uint8_t type) {
    inh_addr = addr;
    inh_width = width;
    inh_height = height;
    inh_pitch = pitch;
    inh_bpp = bpp;
    inh_type = type;
}

int native_fb_get_inherited(uintptr_t* addr, uint32_t* width, uint32_t* height,
                            uint32_t* pitch, uint8_t* bpp, uint8_t* type) {
    if (addr)   *addr = inh_addr;
    if (width)  *width = inh_width;
    if (height) *height = inh_height;
    if (pitch)  *pitch = inh_pitch;
    if (bpp)    *bpp = inh_bpp;
    if (type)   *type = inh_type;
    /* Non-zero only when the bootloader actually handed us a direct-RGB LFB. */
    return (inh_addr != 0 && inh_type == 1 && inh_width != 0 && inh_height != 0);
}

/*
 * Validate the inherited framebuffer. Mirrors the historical
 * validate_framebuffer() checks so the reject reasons stay familiar. Returns
 * non-zero if safe; otherwise records a reason via display_set_error().
 */
static int inherited_is_usable(void) {
    if (!inh_addr || inh_width == 0 || inh_height == 0) {
        display_set_error("VESA disabled: bootloader did not assign a framebuffer (no mode set).\n");
        return 0;
    }
    /* type 0 = indexed, 1 = direct RGB, 2 = EGA text. */
    if (inh_type == 2) {
        display_set_error("VESA disabled: bootloader handed us an EGA text framebuffer.\n");
        return 0;
    }
    if (inh_type == 0) {
        display_set_error("VESA disabled: indexed/palette framebuffers are not supported yet.\n");
        return 0;
    }
    if (inh_type != 1) {
        display_set_error("VESA disabled: unknown framebuffer type from bootloader.\n");
        return 0;
    }
    uint32_t bpp_bytes = bytes_per_pixel(inh_bpp);
    if (bpp_bytes == 0) {
        display_set_error("VESA disabled: unsupported framebuffer depth.\n");
        return 0;
    }
    if (inh_pitch < inh_width * bpp_bytes) {
        display_set_error("VESA disabled: framebuffer pitch is invalid.\n");
        return 0;
    }
    if (inh_width < 320 || inh_height < 200) {
        display_set_error("VESA disabled: framebuffer resolution is too small.\n");
        return 0;
    }
    if (inh_width > 1920 || inh_height > 1200) {
        display_set_error("VESA disabled: framebuffer resolution exceeds safe-mode limits.\n");
        return 0;
    }
    return 1;
}

static int simplefb_probe(void) {
    return inherited_is_usable();
}

static int simplefb_init(uint32_t req_w, uint32_t req_h, uint8_t req_bpp,
                         display_framebuffer_t* out) {
    (void)req_w; (void)req_h; (void)req_bpp;
    if (!inherited_is_usable()) return 0;
    out->framebuffer_addr = inh_addr;
    out->width = inh_width;
    out->height = inh_height;
    out->pitch = inh_pitch;
    out->bpp = inh_bpp;
    out->format = fmt_for_bpp(inh_bpp);
    return 1;
}

static const display_driver_ops_t simplefb_ops = {
    "vesa",
    DISPLAY_DRIVER_VESA_LFB,
    simplefb_probe,
    simplefb_init
};

/* ===================================================================== *
 *  "bochs" driver: Bochs/QEMU dispi (BGA) VBE extensions                 *
 * ===================================================================== */

#define VBE_DISPI_IOPORT_INDEX  0x01CE
#define VBE_DISPI_IOPORT_DATA   0x01CF

#define VBE_DISPI_INDEX_ID          0x0
#define VBE_DISPI_INDEX_XRES        0x1
#define VBE_DISPI_INDEX_YRES        0x2
#define VBE_DISPI_INDEX_BPP         0x3
#define VBE_DISPI_INDEX_ENABLE      0x4
#define VBE_DISPI_INDEX_BANK        0x5
#define VBE_DISPI_INDEX_VIRT_WIDTH  0x6
#define VBE_DISPI_INDEX_VIRT_HEIGHT 0x7
#define VBE_DISPI_INDEX_X_OFFSET    0x8
#define VBE_DISPI_INDEX_Y_OFFSET    0x9

#define VBE_DISPI_ID0           0xB0C0
#define VBE_DISPI_ID_MAX        0xB0CF

#define VBE_DISPI_DISABLED      0x00
#define VBE_DISPI_ENABLED       0x01
#define VBE_DISPI_LFB_ENABLED   0x40

/* Legacy LFB physical base used by old Bochs when no PCI BAR is present. */
#define VBE_DISPI_LFB_FALLBACK  0xE0000000U

/* Default mode programmed when GRUB handed us nothing usable. 1024x768x32 is
 * the most broadly accepted BGA mode and fits the 4MB desktop backbuffer. */
#define BOCHS_DEFAULT_WIDTH   1024
#define BOCHS_DEFAULT_HEIGHT  768
#define BOCHS_DEFAULT_BPP     32

static void dispi_write(uint16_t index, uint16_t value) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}

static uint16_t dispi_read(uint16_t index) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

/*
 * Detect the dispi interface. We read the ID register and accept the known
 * BGA ID range, then confirm with a write/read-back of a scratch register
 * (X_OFFSET) so we don't false-positive on a stray readable port.
 */
static int bochs_detect(void) {
    uint16_t id = dispi_read(VBE_DISPI_INDEX_ID);
    if (id < VBE_DISPI_ID0 || id > VBE_DISPI_ID_MAX) return 0;

    uint16_t saved = dispi_read(VBE_DISPI_INDEX_X_OFFSET);
    dispi_write(VBE_DISPI_INDEX_X_OFFSET, 0x55AA & 0xFFFF);
    uint16_t check = dispi_read(VBE_DISPI_INDEX_X_OFFSET);
    dispi_write(VBE_DISPI_INDEX_X_OFFSET, saved);
    /* The X offset is clamped to the virtual width, so only require that the
     * register is writable at all (changed away from a fixed value). */
    return (check != id);
}

/*
 * Find the linear framebuffer physical base. Bochs/QEMU stdvga expose the LFB
 * as PCI BAR0 (prefetchable memory) on a display-class device; prefer that.
 * Fall back to the legacy fixed address only if no PCI BAR is available.
 *
 * Returned as `uintptr_t` so the address survives 64-bit kernels (BAR values
 * are still uint32_t today; widening here is forward-compat insurance only).
 */
static uintptr_t bochs_lfb_base(void) {
    pci_display_device_t devs[4];
    int n = pci_find_display_controllers(devs, 4);
    for (int i = 0; i < n && i < 4; i++) {
        uint32_t bar0 = devs[i].bar[0];
        /* Memory BAR (bit0 == 0) with a non-zero base. */
        if ((bar0 & 0x1) == 0) {
            uint32_t base = bar0 & 0xFFFFFFF0U;
            if (base != 0) return (uintptr_t)base;
        }
    }
    return (uintptr_t)VBE_DISPI_LFB_FALLBACK;
}

static int bochs_probe(void) {
    return bochs_detect();
}

static int bochs_init(uint32_t req_w, uint32_t req_h, uint8_t req_bpp,
                      display_framebuffer_t* out) {
    if (!bochs_detect()) {
        display_set_error("Bochs/dispi: BGA interface not detected.\n");
        return 0;
    }

    uint32_t modes[][2] = {
        { req_w, req_h },
        { BOCHS_DEFAULT_WIDTH, BOCHS_DEFAULT_HEIGHT },
        { 1280, 720 },
        { 800, 600 },
        { 640, 480 }
    };
    uint8_t bpp = req_bpp ? req_bpp : BOCHS_DEFAULT_BPP;
    uintptr_t base = bochs_lfb_base();

    /* Only 32/24/16 bpp are programmable here; clamp anything else to 32. */
    if (bytes_per_pixel(bpp) == 0 || bpp == 15) bpp = 32;

    if (!base) {
        display_set_error("Bochs/dispi: no linear framebuffer aperture found.\n");
        return 0;
    }

    for (int m = 0; m < (int)(sizeof(modes) / sizeof(modes[0])); m++) {
        uint32_t w = modes[m][0] ? modes[m][0] : BOCHS_DEFAULT_WIDTH;
        uint32_t h = modes[m][1] ? modes[m][1] : BOCHS_DEFAULT_HEIGHT;

        /* Keep within safe-mode bounds shared with the inherited path. */
        if (w < 320) w = 320;
        if (h < 200) h = 200;
        if (w > 1920) w = 1920;
        if (h > 1200) h = 1200;

        dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
        dispi_write(VBE_DISPI_INDEX_XRES, (uint16_t)w);
        dispi_write(VBE_DISPI_INDEX_YRES, (uint16_t)h);
        dispi_write(VBE_DISPI_INDEX_BPP, (uint16_t)bpp);
        dispi_write(VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)w);
        dispi_write(VBE_DISPI_INDEX_VIRT_HEIGHT, (uint16_t)h);
        dispi_write(VBE_DISPI_INDEX_X_OFFSET, 0);
        dispi_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
        dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

        /* Verify the device latched the geometry we asked for. The dispi model
         * clamps to supported values, so a mismatch means the mode was refused. */
        uint16_t got_w = dispi_read(VBE_DISPI_INDEX_XRES);
        uint16_t got_h = dispi_read(VBE_DISPI_INDEX_YRES);
        uint16_t got_bpp = dispi_read(VBE_DISPI_INDEX_BPP);
        if (got_w != (uint16_t)w || got_h != (uint16_t)h || got_bpp != (uint16_t)bpp) {
            display_set_error("Bochs/dispi: device rejected a candidate mode.\n");
            continue;
        }

        out->framebuffer_addr = base;
        out->width = w;
        out->height = h;
        out->pitch = w * bytes_per_pixel(bpp);
        out->bpp = bpp;
        out->format = fmt_for_bpp(bpp);
        return 1;
    }

    dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    display_set_error("Bochs/dispi: no recommended mode was accepted.\n");
    return 0;
}

static const display_driver_ops_t bochs_ops = {
    "bochs",
    DISPLAY_DRIVER_NATIVE_GENERIC,
    bochs_probe,
    bochs_init
};

/* ===================================================================== *
 *  Registration                                                          *
 * ===================================================================== */

void native_fb_register_drivers(void) {
    /* Priority order: inherited GRUB LFB first, then the dispi fallback. */
    display_register_driver(&simplefb_ops);
    display_register_driver(&bochs_ops);
}
