#include "intel_gfx.h"
#include "display.h"
#include "connector.h"
#include "native_fb.h"
#include "vesa.h"
#include "../diagnostics/driver_log.h"
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

/* Intel GMBUS/DDC block. We use it only for bounded EDID reads; no mode or
 * power sequencing registers are touched. */
#define GMBUS0                 0x5100U
#define GMBUS1                 0x5104U
#define GMBUS2                 0x5108U
#define GMBUS3                 0x510CU
#define GMBUS4                 0x5110U
#define GMBUS5                 0x5120U

#define GMBUS_PIN_VGADDC       2U
#define GMBUS_PIN_PANEL        3U
#define GMBUS_PIN_DPC          4U
#define GMBUS_PIN_DPB          5U
#define GMBUS_PIN_DPD          6U

#define GMBUS1_SW_CLR_INT      (1U << 31)
#define GMBUS1_SW_RDY          (1U << 30)
#define GMBUS1_CYCLE_WAIT      (0U << 25)
#define GMBUS1_CYCLE_INDEX     (1U << 25)
#define GMBUS1_CYCLE_STOP      (4U << 25)
#define GMBUS1_BYTE_COUNT(n)   ((uint32_t)(n) << 16)
#define GMBUS1_SLAVE_ADDR(a)   ((uint32_t)(a) << 1)
#define GMBUS1_SLAVE_READ      (1U << 0)

#define GMBUS2_NAK             (1U << 10)
#define GMBUS2_HW_RDY          (1U << 11)
#define GMBUS2_ACTIVE          (1U << 9)

#define EDID_I2C_ADDR          0x50U

/*
 * PCH hotplug detect status (Ivy Bridge / similar). Read-only; used only to
 * report connected vs disconnected for Ubuntu-style connector lines.
 * Bit layout (Gen7 PCH): PORT B/C/D live bits in SDEISR-style HPD status.
 */
#define PCH_PORT_HOTPLUG       0xC4030U
#define PCH_HPD_PORTB_LIVE     (1U << 0)
#define PCH_HPD_PORTC_LIVE     (1U << 8)
#define PCH_HPD_PORTD_LIVE     (1U << 16)

/* Valleyview/Bay Trail: display block regs include hotplug at 0x61114. */
#define VLV_PORT_HOTPLUG_STAT  0x61114U
#define VLV_HPD_PORTB_LIVE     (1U << 29)
#define VLV_HPD_PORTC_LIVE     (1U << 28)
#define VLV_HPD_PORTD_LIVE     (1U << 27)
#define VLV_PIPEA_PP_STATUS    0x61200U
#define VLV_PIPEB_PP_STATUS    0x61300U

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
#define INTEL_MMIO_REG_LIMIT_VLV 0x200000U

/* Valleyview/Bay Trail: display block is offset into GTTMMADR. */
#define VLV_DISPLAY_MMIO_OFFSET 0x180000U

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
static uint32_t g_display_mmio_offset = 0;
static uint32_t g_mmio_write_limit = INTEL_MMIO_REG_LIMIT;
static int g_logged_present_gate = 0;
static intel_plane_report_t g_report;
static int g_present_cache_state = 0; /* 0 unknown, 1 usable, -1 disabled */
static uint32_t g_present_cache_w = 0;
static uint32_t g_present_cache_h = 0;
static uint32_t g_present_cache_mmio = 0;
static int g_present_cache_pipe = -1;
static uint32_t g_present_timeout_streak = 0;

int intel_gfx_uses_ivb_display_regs(uint16_t device_id) {
    switch (device_id) {
        case 0x0152: case 0x0156: case 0x015A:
        case 0x0162: case 0x0166: case 0x016A:
            return 1;
        default:
            return 0;
    }
}

static int intel_is_braswell(uint16_t device_id) {
    switch (device_id) {
        case 0x22B0:
        case 0x22B1: /* Acer R3-131T-C1YF / Braswell Intel HD Graphics */
        case 0x22B2:
        case 0x22B3:
            return 1;
        default:
            return 0;
    }
}

static int intel_is_valleyview(uint16_t device_id) {
    switch (device_id) {
        case 0x0F30: case 0x0F31: case 0x0F32: case 0x0F33:
        case 0x0F34: case 0x0F35: case 0x0F36: case 0x0F37:
        case 0x0155: case 0x0157:
            return 1;
        default:
            return 0;
    }
}

int intel_gfx_is_valleyview(uint16_t device_id) {
    return intel_is_valleyview(device_id);
}

int intel_gfx_is_braswell(uint16_t device_id) {
    return intel_is_braswell(device_id);
}

int intel_gfx_is_bay_trail_class(void) {
    intel_gfx_info_t gpu;
    if (!intel_gfx_detect(&gpu) || !gpu.present) return 0;
    return intel_is_valleyview(gpu.device_id) || intel_is_braswell(gpu.device_id);
}

static void intel_set_register_layout(uint16_t device_id) {
    if (intel_is_valleyview(device_id) || intel_is_braswell(device_id)) {
        g_display_mmio_offset = VLV_DISPLAY_MMIO_OFFSET;
        g_mmio_write_limit = INTEL_MMIO_REG_LIMIT_VLV;
    } else {
        g_display_mmio_offset = 0;
        g_mmio_write_limit = INTEL_MMIO_REG_LIMIT;
    }
}

const char* intel_gfx_generation_name(uint16_t device_id) {
    if (intel_gfx_uses_ivb_display_regs(device_id)) return "Ivy Bridge/Gen7";
    if (intel_is_valleyview(device_id)) return "Bay Trail/Valleyview";
    if (intel_is_braswell(device_id)) return "Braswell/Gen8";
    return "unsupported Intel generation";
}

int intel_gfx_supports_plane_repoint(uint16_t device_id) {
    return intel_gfx_uses_ivb_display_regs(device_id) ||
           intel_is_valleyview(device_id) ||
           intel_is_braswell(device_id);
}

int intel_gfx_has_gmbus_ddc(uint16_t device_id) {
    return intel_gfx_uses_ivb_display_regs(device_id) ||
           intel_is_braswell(device_id) ||
           intel_is_valleyview(device_id);
}

static int intel_device_supported(uint16_t device_id) {
    return intel_gfx_supports_plane_repoint(device_id);
}

static inline int mmio_ok(uint32_t reg) {
    /* Require a plausible high MMIO BAR and an in-range, dword-aligned reg. */
    return (g_mmio_base != 0) && ((g_mmio_base & 0xFFFU) == 0) &&
           (reg < g_mmio_write_limit) && ((reg & 0x3U) == 0);
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

/* Read a register in the display block (VLV/Braswell: offset 0x180000). */
static inline uint32_t intel_display_mmio_rd(uint32_t disp_reg) {
    uint32_t abs;
    if (g_mmio_base == 0 || (g_mmio_base & 0xFFFU) != 0) return 0;
    if ((disp_reg & 0x3U) != 0) return 0;
    abs = g_display_mmio_offset + disp_reg;
    if (abs >= g_mmio_write_limit) return 0;
    return *(volatile uint32_t*)(uintptr_t)(g_mmio_base + abs);
}

static inline int intel_uses_vlv_display_block(void) {
    return g_display_mmio_offset != 0;
}

static inline uint32_t pipe_reg(uint32_t base_a, int pipe) {
    return g_display_mmio_offset + base_a + (uint32_t)pipe * INTEL_PIPE_STRIDE;
}

static int intel_find_matching_pipe(uint32_t fb_w, uint32_t fb_h) {
    for (int p = 0; p < INTEL_NUM_PIPES; p++) {
        uint32_t conf = mmio_rd(pipe_reg(PIPECONF_A, p));
        if (!(conf & PIPECONF_ENABLE)) continue;
        uint32_t src = mmio_rd(pipe_reg(PIPESRC_A, p));
        uint32_t pw = ((src >> 16) & 0xFFFFU) + 1U;
        uint32_t ph = (src & 0xFFFFU) + 1U;
        if (pw == fb_w && ph == fb_h) return p;
    }
    return -1;
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

static int gmbus_wait(uint32_t mask, int want_set, uint32_t spins_max) {
    for (uint32_t i = 0; i < spins_max; i++) {
        uint32_t v = mmio_rd_ro(GMBUS2);
        if (v & GMBUS2_NAK) return 0;
        if (want_set) {
            if ((v & mask) == mask) return 1;
        } else {
            if ((v & mask) == 0) return 1;
        }
        __asm__ volatile("inb $0x80, %%al" ::: "al");
    }
    return 0;
}

static void gmbus_reset(void) {
    if (!mmio_ok(GMBUS1)) return;
    mmio_wr(GMBUS1, GMBUS1_SW_CLR_INT);
    mmio_wr(GMBUS1, 0);
    mmio_wr(GMBUS5, 0);
}

static int gmbus_read_edid_pin(uint32_t pin, uint8_t* block) {
    if (!block || !mmio_ok(GMBUS0) || !mmio_ok(GMBUS1) || !mmio_rd_ok(GMBUS3))
        return 0;

    gmbus_reset();
    mmio_wr(GMBUS0, pin);

    /* Set EDID EEPROM byte offset to 0, then repeated-start read 128 bytes. */
    mmio_wr(GMBUS3, 0);
    mmio_wr(GMBUS1, GMBUS1_SW_RDY | GMBUS1_CYCLE_INDEX |
                    GMBUS1_BYTE_COUNT(1) | GMBUS1_SLAVE_ADDR(EDID_I2C_ADDR));
    if (!gmbus_wait(GMBUS2_HW_RDY, 1, 100000U)) {
        gmbus_reset();
        return 0;
    }

    mmio_wr(GMBUS1, GMBUS1_SW_RDY | GMBUS1_CYCLE_STOP |
                    GMBUS1_BYTE_COUNT(128) |
                    GMBUS1_SLAVE_ADDR(EDID_I2C_ADDR) | GMBUS1_SLAVE_READ);
    for (int i = 0; i < 128; i += 4) {
        if (!gmbus_wait(GMBUS2_HW_RDY, 1, 100000U)) {
            gmbus_reset();
            return 0;
        }
        uint32_t d = mmio_rd_ro(GMBUS3);
        block[i + 0] = (uint8_t)(d & 0xFF);
        block[i + 1] = (uint8_t)((d >> 8) & 0xFF);
        block[i + 2] = (uint8_t)((d >> 16) & 0xFF);
        block[i + 3] = (uint8_t)((d >> 24) & 0xFF);
    }

    if (!gmbus_wait(GMBUS2_ACTIVE, 0, 100000U)) {
        gmbus_reset();
        return 0;
    }
    gmbus_reset();
    return 1;
}

int intel_gfx_read_edid(edid_info_t* out) {
    static const uint32_t pins[] = {
        GMBUS_PIN_PANEL, GMBUS_PIN_VGADDC, GMBUS_PIN_DPB, GMBUS_PIN_DPC, GMBUS_PIN_DPD
    };
    uint8_t block[128];
    intel_gfx_info_t gpu;

    if (!out) return 0;
    out->valid = 0;
    out->preferred_width = 0;
    out->preferred_height = 0;
    out->monitor_name[0] = '\0';

    if (!intel_gfx_detect(&gpu) || !gpu.present) {
        driver_log_line("[display] EDID/DDC: no Intel display controller.");
        return 0;
    }
    /*
     * Bay Trail / Braswell (Acer R3-131T, Lenovo 80M4): any display-block or
     * GMBUS MMIO read while the power well is down can stall the CPU bus so
     * hard the boot watchdog's timer IRQ never fires. This mirrors the guard
     * in the connector scan — never touch Intel DDC on these SoCs.
     */
    if (intel_gfx_is_bay_trail_class()) {
        driver_log_line("[display] Bay Trail/Braswell: GMBUS EDID probe skipped "
                        "(MMIO hang risk).");
        return 0;
    }
    if (!intel_gfx_has_gmbus_ddc(gpu.device_id)) {
        driver_log("[display] EDID/DDC: Intel device ");
        driver_log_hex32(gpu.device_id);
        driver_log_line(" outside guarded GMBUS/DDC allowlist.");
        return 0;
    }
    if (gpu.mmio_base == 0 || (gpu.mmio_base & 0xFFFU) != 0) {
        driver_log_line("[display] EDID/DDC: invalid Intel MMIO base.");
        return 0;
    }

    driver_log("[display] EDID/DDC: Intel ");
    driver_log(intel_gfx_generation_name(gpu.device_id));
    driver_log(" device=");
    driver_log_hex32(gpu.device_id);
    driver_log_line(" using bounded GMBUS probe.");

    g_mmio_base = gpu.mmio_base;
    intel_set_register_layout(gpu.device_id);
    for (uint32_t p = 0; p < sizeof(pins) / sizeof(pins[0]); p++) {
        for (int i = 0; i < 128; i++) block[i] = 0;
        driver_log("[display] EDID/DDC: trying Intel GMBUS pin ");
        driver_log_u32(pins[p]);
        driver_log(".\n");
        if (gmbus_read_edid_pin(pins[p], block) && edid_parse_block(block, out)) {
            driver_log("[display] EDID/DDC: valid EDID on GMBUS pin ");
            driver_log_u32(pins[p]);
            driver_log(".\n");
            edid_log_info(out);
            return 1;
        }
    }

    driver_log_line("[display] EDID/DDC: no valid EDID block from guarded Intel GMBUS pins.");
    return 0;
}

static void intel_conn_name(char* out, size_t out_sz,
                            display_connector_type_t type, uint32_t index) {
    const char* prefix = display_connector_type_name(type);
    size_t i = 0;
    uint32_t v = index ? index : 1U;
    char num[4];
    int n = 0, q;
    char tmp[4];

    if (!out || out_sz == 0) return;
    while (prefix[i] && i + 1 < out_sz) {
        out[i] = prefix[i];
        i++;
    }
    if (i + 1 < out_sz) out[i++] = '-';
    if (v == 0) num[n++] = '0';
    else {
        q = 0;
        while (v && q < 3) { tmp[q++] = (char)('0' + (v % 10)); v /= 10; }
        while (q) num[n++] = tmp[--q];
    }
    for (q = 0; q < n && i + 1 < out_sz; q++)
        out[i++] = num[q];
    out[i] = '\0';
}

static int intel_hpd_for_pin(uint32_t pin, int* known, int* live) {
    uint32_t hpd;
    uint32_t pp;
    if (!known || !live) return 0;
    *known = 0;
    *live = 0;

    if (intel_uses_vlv_display_block()) {
        hpd = intel_display_mmio_rd(VLV_PORT_HOTPLUG_STAT);
        if (hpd == 0 || hpd == 0xFFFFFFFFU) return 0;
        switch (pin) {
            case GMBUS_PIN_DPB:
                *known = 1;
                *live = (hpd & VLV_HPD_PORTB_LIVE) ? 1 : 0;
                return 1;
            case GMBUS_PIN_DPC:
                *known = 1;
                *live = (hpd & VLV_HPD_PORTC_LIVE) ? 1 : 0;
                return 1;
            case GMBUS_PIN_DPD:
                *known = 1;
                *live = (hpd & VLV_HPD_PORTD_LIVE) ? 1 : 0;
                return 1;
            case GMBUS_PIN_PANEL:
                pp = intel_display_mmio_rd(VLV_PIPEA_PP_STATUS);
                if (pp == 0 || pp == 0xFFFFFFFFU) {
                    pp = intel_display_mmio_rd(VLV_PIPEB_PP_STATUS);
                }
                if (pp != 0 && pp != 0xFFFFFFFFU) {
                    *known = 1;
                    *live = (pp & PP_STATUS_ON) ? 1 : 0;
                    return 1;
                }
                return 0;
            case GMBUS_PIN_VGADDC:
                *known = 0;
                return 0;
            default:
                *known = 0;
                return 0;
        }
    }

    if (!mmio_rd_ok(PCH_PORT_HOTPLUG)) return 0;
    hpd = mmio_rd_ro(PCH_PORT_HOTPLUG);
    /* All-0 / all-1 are untrustworthy on unmapped PCH windows. */
    if (hpd == 0 || hpd == 0xFFFFFFFFU) return 0;
    *known = 1;
    switch (pin) {
        case GMBUS_PIN_DPB: *live = (hpd & PCH_HPD_PORTB_LIVE) ? 1 : 0; break;
        case GMBUS_PIN_DPC: *live = (hpd & PCH_HPD_PORTC_LIVE) ? 1 : 0; break;
        case GMBUS_PIN_DPD: *live = (hpd & PCH_HPD_PORTD_LIVE) ? 1 : 0; break;
        case GMBUS_PIN_PANEL:
            /* eDP/LVDS: treat panel-power-on as connected when readable. */
            {
                pp = mmio_rd_ro(PCH_PP_STATUS);
                if (pp != 0 && pp != 0xFFFFFFFFU) {
                    *live = (pp & PP_STATUS_ON) ? 1 : 0;
                } else {
                    *known = 0;
                }
            }
            break;
        case GMBUS_PIN_VGADDC:
            /* VGA has no reliable HPD on many boards; leave unknown. */
            *known = 0;
            break;
        default:
            *known = 0;
            break;
    }
    return *known;
}

int intel_gfx_scan_connectors(display_connector_t* out, int max_out) {
    return intel_gfx_scan_connectors_ex(out, max_out, 1);
}

int intel_gfx_scan_connectors_ex(display_connector_t* out, int max_out,
                                 int allow_gmbus) {
    typedef struct {
        uint32_t pin;
        display_connector_type_t type;
        uint32_t index;
    } port_map_t;

    static const port_map_t ports[] = {
        { GMBUS_PIN_PANEL, DISPLAY_CONN_EDP,  1 },
        { GMBUS_PIN_DPB,   DISPLAY_CONN_DP,   1 },
        { GMBUS_PIN_DPC,   DISPLAY_CONN_HDMI, 1 },
        { GMBUS_PIN_DPD,   DISPLAY_CONN_DP,   2 },
        { GMBUS_PIN_VGADDC, DISPLAY_CONN_VGA, 1 },
    };

    intel_gfx_info_t gpu;
    uint8_t block[128];
    int count = 0;
    uint32_t i;

    if (!out || max_out <= 0) return 0;

    if (!intel_gfx_detect(&gpu) || !gpu.present) return 0;
    if (!intel_gfx_has_gmbus_ddc(gpu.device_id)) return 0;
    if (gpu.mmio_base == 0 || (gpu.mmio_base & 0xFFFU) != 0) return 0;

    /*
     * Bay Trail/Braswell: any display-block MMIO (even HPD reads) while the
     * firmware LFB is live can hang the panel on GRUB blue (Lenovo 80M4).
     * Use display_connectors_stub_firmware_panel() during boot instead.
     */
    if (intel_gfx_is_bay_trail_class()) {
        driver_log_line("[display] Bay Trail: Intel connector MMIO scan skipped.");
        return 0;
    }

    g_mmio_base = gpu.mmio_base;
    intel_set_register_layout(gpu.device_id);

    driver_log("[display] connector scan: Intel ");
    driver_log(intel_gfx_generation_name(gpu.device_id));
    driver_log(" device=");
    driver_log_hex32(gpu.device_id);
    if (!allow_gmbus)
        driver_log(" (GMBUS/DDC deferred — firmware LFB live)");
    driver_log("\n");

    for (i = 0; i < sizeof(ports) / sizeof(ports[0]) && count < max_out; i++) {
        display_connector_t* c = &out[count];
        edid_info_t edid;
        int hpd_known = 0, hpd_live = 0;
        int have_edid = 0;
        uint32_t j;

        for (j = 0; j < sizeof(*c); j++) ((uint8_t*)c)[j] = 0;
        for (j = 0; j < sizeof(edid); j++) ((uint8_t*)&edid)[j] = 0;
        for (j = 0; j < 128; j++) block[j] = 0;

        c->present = 1;
        c->type = ports[i].type;
        c->port_index = ports[i].index;
        c->gmbus_pin = ports[i].pin;
        intel_conn_name(c->name, sizeof(c->name), c->type, c->port_index);

        /* HPD / panel-power are read-only; safe even when DDC is deferred. */
        intel_hpd_for_pin(ports[i].pin, &hpd_known, &hpd_live);
        c->hpd_known = hpd_known;
        c->hpd_live = hpd_live;

        if (allow_gmbus) {
            have_edid = gmbus_read_edid_pin(ports[i].pin, block) &&
                        edid_parse_block(block, &edid) && edid.valid;
        }

        if (have_edid) {
            c->edid_valid = 1;
            c->preferred_width = edid.preferred_width;
            c->preferred_height = edid.preferred_height;
            for (j = 0; j < sizeof(c->monitor_name) - 1 && edid.monitor_name[j]; j++)
                c->monitor_name[j] = edid.monitor_name[j];
            c->monitor_name[j] = '\0';
            c->status = DISPLAY_CONN_STATUS_CONNECTED;
        } else if (hpd_known && hpd_live) {
            c->status = DISPLAY_CONN_STATUS_CONNECTED;
        } else if (hpd_known && !hpd_live) {
            c->status = DISPLAY_CONN_STATUS_DISCONNECTED;
        } else {
            c->status = DISPLAY_CONN_STATUS_UNKNOWN;
        }

        if (ports[i].pin == GMBUS_PIN_PANEL &&
            c->status == DISPLAY_CONN_STATUS_UNKNOWN &&
            hpd_known && hpd_live) {
            c->status = DISPLAY_CONN_STATUS_CONNECTED;
        }

        count++;
    }

    return count;
}

int intel_gfx_wait_vblank(uint32_t timeout_ticks) {
    intel_gfx_info_t gpu;
    const display_mode_info_t* mode = display_get_mode();
    if (!mode || !mode->available || mode->width == 0 || mode->height == 0)
        return 0;
    if (g_present_cache_state < 0 &&
        g_present_cache_w == mode->width && g_present_cache_h == mode->height)
        return 0;

    if (g_present_cache_state <= 0 ||
        g_present_cache_w != mode->width || g_present_cache_h != mode->height) {
        g_present_cache_state = 0;
        g_present_cache_w = mode->width;
        g_present_cache_h = mode->height;
        g_present_cache_pipe = -1;
        g_present_cache_mmio = 0;
        g_present_timeout_streak = 0;

        if (!intel_gfx_detect(&gpu) || !gpu.present) {
            g_present_cache_state = -1;
            return 0;
        }
        if (!intel_device_supported(gpu.device_id)) {
            if (!g_logged_present_gate) {
                driver_log("[intel] present: device ");
                driver_log_hex32(gpu.device_id);
                driver_log_line(" not in guarded Ivy Bridge allowlist; vblank disabled.");
                g_logged_present_gate = 1;
            }
            g_present_cache_state = -1;
            return 0;
        }
        if (gpu.mmio_base == 0 || (gpu.mmio_base & 0xFFFU) != 0) {
            g_present_cache_state = -1;
            return 0;
        }

        g_mmio_base = gpu.mmio_base;
        int pipe = intel_find_matching_pipe(mode->width, mode->height);
        if (pipe < 0) {
            if (!g_logged_present_gate) {
                driver_log_line("[intel] present: no enabled pipe matches active framebuffer geometry.");
                g_logged_present_gate = 1;
            }
            g_present_cache_state = -1;
            return 0;
        }

        g_present_cache_mmio = gpu.mmio_base;
        g_present_cache_pipe = pipe;
        g_present_cache_state = 1;
    }

    g_mmio_base = g_present_cache_mmio;
    int pipe = g_present_cache_pipe;
    uint32_t before = mmio_rd(pipe_reg(PIPEFRAMECOUNT_A, pipe));
    uint32_t start = timer_ticks();
    uint32_t spins = 0;
    uint32_t budget = timeout_ticks ? timeout_ticks : 2U;
    while ((timer_ticks() - start) <= budget) {
        uint32_t now = mmio_rd(pipe_reg(PIPEFRAMECOUNT_A, pipe));
        if (now != before) {
            if (!g_logged_present_gate) {
                driver_log("[intel] present: vblank wait active on pipe ");
                driver_log_u32((uint32_t)pipe);
                driver_log(".\n");
                g_logged_present_gate = 1;
            }
            g_present_timeout_streak = 0;
            return 1;
        }
        __asm__ volatile("inb $0x80, %%al" ::: "al");
        if (++spins >= 1000000U) break;
    }
    if (++g_present_timeout_streak >= 4U) {
        driver_log_line("[intel] present: vblank wait repeatedly timed out; disabling vblank for this mode.");
        g_present_cache_state = -1;
    }
    return 0;
}

static int intel_present_frame(void) {
    vesa_present();
    if (g_report.succeeded && g_report.pipe >= 0) {
        uint32_t surf_reg = pipe_reg(DSPSURF_A, g_report.pipe);
        if (mmio_ok(surf_reg)) {
            mmio_wr(surf_reg, g_report.gfx_offset);
            return 1;
        }
    }
    return 1;
}

/* ===================================================================== *
 *  Read-only scanout-health probe (visibility gate helper)             *
 * ===================================================================== */

/* Treat an all-zero or all-ones read as "register not trustworthy" (the BAR
 * is unmapped at that offset, the block is power-gated, or we mis-decoded). */
static int reg_trustworthy(uint32_t v) {
    return (v != 0u && v != 0xFFFFFFFFu);
}

/* DSPSURF address field: bits 31:12 are the graphics-aperture page base. */
#define DSPSURF_ADDR_MASK      0xFFFFF000U

int intel_gfx_read_firmware_scanout(uint32_t expect_w, uint32_t expect_h,
                                    intel_firmware_scanout_t* out) {
    intel_gfx_info_t gpu;
    int best_pipe = -1;
    uint32_t best_stride = 0;
    uint32_t best_surf = 0;
    uint32_t best_cntr = 0;

    if (!out) return 0;
    for (uint32_t i = 0; i < sizeof(*out); i++) ((uint8_t*)out)[i] = 0;
    out->pipe = -1;

    if (!intel_gfx_detect(&gpu) || !gpu.present)
        return 0;
    /* Bay Trail / Braswell: display MMIO hangs when the power well is down. */
    if (intel_is_valleyview(gpu.device_id) || intel_is_braswell(gpu.device_id))
        return 0;
    intel_set_register_layout(gpu.device_id);
    if (!intel_gfx_supports_plane_repoint(gpu.device_id))
        return 0;
    if (gpu.mmio_base == 0 || (gpu.mmio_base & 0xFFFU) != 0)
        return 0;
    if (gpu.aperture_base == 0)
        return 0;

    g_mmio_base = gpu.mmio_base;

    for (int p = 0; p < INTEL_NUM_PIPES; p++) {
        uint32_t conf = mmio_rd(pipe_reg(PIPECONF_A, p));
        uint32_t src  = mmio_rd(pipe_reg(PIPESRC_A, p));
        uint32_t pw = ((src >> 16) & 0xFFFFU) + 1U;
        uint32_t ph = (src & 0xFFFFU) + 1U;
        uint32_t cntr = mmio_rd(pipe_reg(DSPCNTR_A, p));
        uint32_t stride = mmio_rd(pipe_reg(DSPSTRIDE_A, p));
        uint32_t surf = mmio_rd(pipe_reg(DSPSURF_A, p));

        if (!(conf & PIPECONF_ENABLE)) continue;
        if (!(cntr & DISPPLANE_ENABLE)) continue;
        if (surf == 0 || surf == 0xFFFFFFFFU) continue;
        if (stride == 0 || stride == 0xFFFFFFFFU) continue;
        if (pw < 320 || ph < 200) continue;

        if (pw == expect_w && ph == expect_h) {
            best_pipe = p;
            best_stride = stride;
            best_surf = surf;
            best_cntr = cntr;
            break;
        }
        if (best_pipe < 0) {
            best_pipe = p;
            best_stride = stride;
            best_surf = surf;
            best_cntr = cntr;
        }
    }

    if (best_pipe < 0)
        return 0;

    {
        uint32_t gfx_off = best_surf & DSPSURF_ADDR_MASK;
        uint32_t fb_phys = gpu.aperture_base + gfx_off;
        if (gfx_off > (512U * 1024U * 1024U))
            return 0;
        out->ok = 1;
        out->pipe = best_pipe;
        out->fb_phys = fb_phys;
        out->stride = best_stride;
        out->gfx_offset = gfx_off;
        out->dspcntr = best_cntr;
        return 1;
    }
}

int intel_gfx_probe_scanout(uint32_t fb_w, uint32_t fb_h, intel_scanout_probe_t* out) {
    if (!out) return 0;
    for (uint32_t i = 0; i < sizeof(*out); i++) ((uint8_t*)out)[i] = 0;
    out->pipe = -1;

    intel_gfx_info_t gpu;
    if (!intel_gfx_detect(&gpu) || !gpu.present)
        return 0;                         /* no Intel GPU -> caller skips us */
    /* Bay Trail / Braswell: never touch display MMIO for scanout probes. */
    if (intel_is_valleyview(gpu.device_id) || intel_is_braswell(gpu.device_id))
        return 0;
    intel_set_register_layout(gpu.device_id);
    if (!intel_gfx_supports_plane_repoint(gpu.device_id)) {
        driver_log("[intel] scanout: ");
        driver_log(intel_gfx_generation_name(gpu.device_id));
        driver_log(" device ");
        driver_log_hex32(gpu.device_id);
        driver_log_line(" detected; plane-repoint not implemented for this generation.");
        return 0;
    }
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
    intel_set_register_layout(gpu.device_id);
    if (!intel_device_supported(gpu.device_id))
        return fail("intel: display controller is outside guarded plane-repoint allowlist.\n");

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

    uint32_t cntr_reg   = pipe_reg(DSPCNTR_A, chosen);
    uint32_t linoff_reg = pipe_reg(DSPLINOFF_A, chosen);
    uint32_t stride_reg = pipe_reg(DSPSTRIDE_A, chosen);
    uint32_t surf_reg   = pipe_reg(DSPSURF_A, chosen);
    uint32_t tileoff_reg= pipe_reg(DSPTILEOFF_A, chosen);

    uint32_t old_cntr   = mmio_rd(cntr_reg);
    uint32_t old_stride = mmio_rd(stride_reg);
    uint32_t old_surf   = mmio_rd(surf_reg);
    g_report.old_dspcntr  = old_cntr;
    g_report.old_dspsurf  = old_surf;
    g_report.old_dspstride= old_stride;

    /*
     * MS Basic Display: if firmware already scans the inherited buffer with
     * matching stride/format, do not rewrite plane registers (avoid breaking
     * a working handoff on Bay Trail / Braswell panels).
     */
    {
        int fmt_ok2 = 0;
        uint32_t cur_pixfmt = plane_pixfmt_for_bpp(fb_bpp, &fmt_ok2);
        uint32_t cur_off = old_surf & DSPSURF_ADDR_MASK;
        if (fmt_ok2 &&
            (old_cntr & DISPPLANE_ENABLE) &&
            cur_off == gfx_offset &&
            old_stride == fb_pitch &&
            (old_cntr & DISPPLANE_PIXFORMAT_MASK) == cur_pixfmt &&
            !(old_cntr & DISPPLANE_TILED)) {
            g_report.succeeded = 1;
            g_report.reason = NULL;
            g_report.new_dspcntr = old_cntr;
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
    }

    /* Sample the frame counter BEFORE the flip; we compare against a post-flip
     * sample to prove the pipe is actually scanning out (see below). */
    uint32_t frame_pre = mmio_rd(pipe_reg(PIPEFRAMECOUNT_A, chosen));
    g_report.frame_count = frame_pre;

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
    intel_init,
    intel_gfx_wait_vblank,
    NULL,
    intel_present_frame,
    NULL
};

void intel_gfx_register_driver(void) {
    display_register_driver(&intel_ops);
}
