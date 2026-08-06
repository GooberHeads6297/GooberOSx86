#include "touchpad.h"
#include "input.h"
#include "../acpi/acpi.h"
#include "../hid/i2c_hid.h"
#include "../i2c/i2c.h"
#include "../timer/timer.h"
#include "../diagnostics/driver_log.h"
#include "../../lib/string.h"

extern void print(const char* str);

/* HID short-item helpers (HID 1.11 §6.2.2). */
#define HID_ITEM_SIZE(tag)  (((tag) & 0x03) == 3 ? 4 : ((tag) & 0x03))
#define HID_ITEM_TYPE(tag)  (((tag) >> 2) & 0x03)
#define HID_ITEM_TAG(tag)   (((tag) >> 4) & 0x0F)

#define HID_TYPE_MAIN   0
#define HID_TYPE_GLOBAL 1
#define HID_TYPE_LOCAL  2

#define HID_MAIN_INPUT  8
#define HID_GLOBAL_USAGE_PAGE 0x0
#define HID_GLOBAL_LOGICAL_MIN 0x1
#define HID_GLOBAL_LOGICAL_MAX 0x2
#define HID_GLOBAL_REPORT_SIZE 0x7
#define HID_GLOBAL_REPORT_ID 0x8
#define HID_GLOBAL_REPORT_COUNT 0x9

#define HID_LOCAL_USAGE 0x0
#define HID_LOCAL_USAGE_MIN 0x1
#define HID_LOCAL_USAGE_MAX 0x2

#define HID_PAGE_GENERIC  0x01
#define HID_PAGE_BUTTON   0x09
#define HID_PAGE_DIGITIZER 0x0D
#define HID_PAGE_CONSUMER 0x0C

#define HID_USAGE_X       0x30
#define HID_USAGE_Y       0x31
#define HID_USAGE_WHEEL   0x38
#define HID_USAGE_TIP     0x42
#define HID_USAGE_CONTACT_COUNT 0x54
#define HID_USAGE_BUTTON1 0x01
#define HID_USAGE_BUTTON2 0x02

typedef struct {
    int valid;
    int bit_offset;
    int bit_size;
    int logical_max;
} hid_field_t;

typedef struct {
    int ready;
    int used_descriptor_map;
    uint8_t report_id;
    int has_digitizer;
    int max_x;
    int max_y;
    hid_field_t x;
    hid_field_t y;
    hid_field_t tip;
    hid_field_t btn_left;
    hid_field_t btn_right;
    hid_field_t contact_count;
    hid_field_t wheel;
    int last_raw_x;
    int last_raw_y;
    int tracking;
    int last_contacts;
    int scroll_accum;
    /* Double-tap → right-click: first tap recorded on finger-up. */
    int tap_armed;
    int tap_x;
    int tap_y;
    uint32_t tap_tick;
    int contact_start_x;
    int contact_start_y;
    int contact_moved;
    int synth_right_down;
    uint32_t report_count;
    uint32_t move_events;
    uint32_t button_events;
    uint32_t scroll_events;
    uint8_t i2c_addr;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t ctrl_bus;
    uint8_t ctrl_slot;
    uint8_t ctrl_func;
    uint16_t ctrl_device_id;
} touchpad_state_t;

#define TP_DOUBLETAP_TICKS 40  /* ~400 ms at 100 Hz */
#define TP_DOUBLETAP_SLACK 40  /* raw pad units */

static touchpad_state_t g_tp;
/* Why touchpad is not ready (for devices / diagnostics). */
static const char* g_tp_skip_reason = "not probed";

static void print_dec(int n) {
    char buf[16];
    itoa(n, buf, 10);
    print(buf);
}

static void write_dec(void (*write)(const char*), int n) {
    char buf[16];
    itoa(n, buf, 10);
    write(buf);
}

static int addr_already_tried(const uint8_t* addrs, int count, uint8_t addr) {
    for (int i = 0; i < count; i++) {
        if (addrs[i] == addr) return 1;
    }
    return 0;
}

static uint32_t hid_udata(const uint8_t* p, int size) {
    uint32_t v = 0;
    for (int i = 0; i < size; i++)
        v |= ((uint32_t)p[i]) << (8 * i);
    return v;
}

static int hid_sdata(const uint8_t* p, int size) {
    uint32_t u = hid_udata(p, size);
    if (size == 1) return (int)(int8_t)u;
    if (size == 2) return (int)(int16_t)u;
    return (int)u;
}

static uint32_t extract_bits(const uint8_t* data, uint16_t len, int bit_off, int bit_size) {
    uint32_t v = 0;
    int i;
    if (!data || bit_size <= 0 || bit_size > 32 || bit_off < 0) return 0;
    for (i = 0; i < bit_size; i++) {
        int bit = bit_off + i;
        int byte = bit / 8;
        if (byte < 0 || (uint16_t)byte >= len) break;
        if (data[byte] & (1U << (bit % 8)))
            v |= (1U << i);
    }
    return v;
}

static void set_field(hid_field_t* f, int bit_off, int bit_size, int logical_max) {
    if (!f || f->valid) return;
    f->valid = 1;
    f->bit_offset = bit_off;
    f->bit_size = bit_size;
    f->logical_max = logical_max > 0 ? logical_max : ((1 << bit_size) - 1);
}

/*
 * Compact HID report-descriptor walker. Captures the first X/Y/buttons/
 * tip/contact-count/wheel fields we will use for Lenovo ELAN-class pads.
 */
static void parse_report_descriptor(void) {
    uint16_t len = 0;
    const uint8_t* d = i2c_hid_get_report_descriptor(&len);
    uint16_t i = 0;
    uint32_t usage_page = 0;
    uint32_t usage = 0;
    uint32_t usages[16];
    int usage_count = 0;
    uint32_t usage_min = 0;
    uint32_t usage_max = 0;
    int has_usage_range = 0;
    int report_size = 0;
    int report_count = 0;
    int logical_max = 0;
    int report_id = 0;
    int bit_pos = 0;
    int current_report_id = -1;

    g_tp.report_id = 0;
    g_tp.has_digitizer = 0;
    g_tp.max_x = 2047;
    g_tp.max_y = 2047;
    g_tp.used_descriptor_map = 0;
    memset(&g_tp.x, 0, sizeof(g_tp.x));
    memset(&g_tp.y, 0, sizeof(g_tp.y));
    memset(&g_tp.tip, 0, sizeof(g_tp.tip));
    memset(&g_tp.btn_left, 0, sizeof(g_tp.btn_left));
    memset(&g_tp.btn_right, 0, sizeof(g_tp.btn_right));
    memset(&g_tp.contact_count, 0, sizeof(g_tp.contact_count));
    memset(&g_tp.wheel, 0, sizeof(g_tp.wheel));

    if (!d || len == 0) return;

    while (i < len) {
        uint8_t prefix = d[i++];
        int size;
        int type;
        int tag;
        const uint8_t* payload;

        if (prefix == 0xFE) { /* long item: unsupported, abort walker safely */
            if (i + 1 >= len) break;
            size = d[i];
            i = (uint16_t)(i + 2 + size);
            continue;
        }

        size = HID_ITEM_SIZE(prefix);
        type = HID_ITEM_TYPE(prefix);
        tag = HID_ITEM_TAG(prefix);
        if ((uint16_t)(i + size) > len) break;
        payload = d + i;
        i = (uint16_t)(i + size);

        if (type == HID_TYPE_GLOBAL) {
            switch (tag) {
                case HID_GLOBAL_USAGE_PAGE:
                    usage_page = hid_udata(payload, size);
                    if (usage_page == HID_PAGE_DIGITIZER) g_tp.has_digitizer = 1;
                    break;
                case HID_GLOBAL_LOGICAL_MAX:
                    logical_max = hid_sdata(payload, size);
                    break;
                case HID_GLOBAL_REPORT_SIZE:
                    report_size = (int)hid_udata(payload, size);
                    break;
                case HID_GLOBAL_REPORT_ID:
                    report_id = (int)hid_udata(payload, size);
                    if (report_id != current_report_id) {
                        current_report_id = report_id;
                        bit_pos = 0;
                        if (g_tp.report_id == 0 && report_id != 0)
                            g_tp.report_id = (uint8_t)report_id;
                    }
                    break;
                case HID_GLOBAL_REPORT_COUNT:
                    report_count = (int)hid_udata(payload, size);
                    break;
                default:
                    break;
            }
        } else if (type == HID_TYPE_LOCAL) {
            if (tag == HID_LOCAL_USAGE) {
                usage = hid_udata(payload, size);
                if (usage_count < (int)(sizeof(usages) / sizeof(usages[0])))
                    usages[usage_count++] = usage;
            } else if (tag == HID_LOCAL_USAGE_MIN) {
                usage_min = hid_udata(payload, size);
                has_usage_range = 1;
            } else if (tag == HID_LOCAL_USAGE_MAX) {
                usage_max = hid_udata(payload, size);
                has_usage_range = 1;
            }
        } else if (type == HID_TYPE_MAIN && tag == HID_MAIN_INPUT) {
            int c;
            for (c = 0; c < report_count; c++) {
                uint32_t u = 0;
                if (c < usage_count) {
                    u = usages[c];
                } else if (has_usage_range) {
                    u = usage_min + (uint32_t)c;
                    if (usage_max >= usage_min && u > usage_max)
                        u = usage_max;
                } else if (usage_count == 1) {
                    u = usages[0];
                } else if (usage_count > 1 && c >= usage_count) {
                    u = usages[usage_count - 1] + (uint32_t)(c - (usage_count - 1));
                }

                if (usage_page == HID_PAGE_GENERIC || usage_page == HID_PAGE_DIGITIZER) {
                    if (u == HID_USAGE_X)
                        set_field(&g_tp.x, bit_pos, report_size, logical_max);
                    else if (u == HID_USAGE_Y)
                        set_field(&g_tp.y, bit_pos, report_size, logical_max);
                    else if (u == HID_USAGE_WHEEL)
                        set_field(&g_tp.wheel, bit_pos, report_size, logical_max);
                }
                if (usage_page == HID_PAGE_DIGITIZER) {
                    if (u == HID_USAGE_TIP)
                        set_field(&g_tp.tip, bit_pos, report_size, 1);
                    else if (u == HID_USAGE_CONTACT_COUNT)
                        set_field(&g_tp.contact_count, bit_pos, report_size, logical_max);
                }
                if (usage_page == HID_PAGE_BUTTON) {
                    if (u == HID_USAGE_BUTTON1)
                        set_field(&g_tp.btn_left, bit_pos, report_size, 1);
                    else if (u == HID_USAGE_BUTTON2)
                        set_field(&g_tp.btn_right, bit_pos, report_size, 1);
                }
                if (usage_page == HID_PAGE_CONSUMER && u == HID_USAGE_WHEEL)
                    set_field(&g_tp.wheel, bit_pos, report_size, logical_max);

                bit_pos += report_size;
            }
            usage_count = 0;
            usage = 0;
            usage_min = usage_max = 0;
            has_usage_range = 0;
        } else if (type == HID_TYPE_MAIN) {
            /* Collection / End Collection / Output / Feature: reset locals. */
            usage_count = 0;
            usage = 0;
            usage_min = usage_max = 0;
            has_usage_range = 0;
        }
    }

    if (g_tp.x.valid && g_tp.x.logical_max > 0) g_tp.max_x = g_tp.x.logical_max;
    if (g_tp.y.valid && g_tp.y.logical_max > 0) g_tp.max_y = g_tp.y.logical_max;

    if (g_tp.x.valid && g_tp.y.valid) {
        g_tp.used_descriptor_map = 1;
    }

    print("[touchpad] report map: id=");
    print_dec((int)g_tp.report_id);
    print(" x@");
    print_dec(g_tp.x.bit_offset);
    print("/");
    print_dec(g_tp.x.bit_size);
    print(" y@");
    print_dec(g_tp.y.bit_offset);
    print("/");
    print_dec(g_tp.y.bit_size);
    print(" decoder=");
    print(g_tp.used_descriptor_map ? "descriptor" : "elan-fallback");
    print("\n");
    driver_log("[touchpad] map id=");
    driver_log_u32(g_tp.report_id);
    driver_log(" max=");
    driver_log_u32((uint32_t)g_tp.max_x);
    driver_log("x");
    driver_log_u32((uint32_t)g_tp.max_y);
    driver_log(g_tp.used_descriptor_map ? " decoder=descriptor\n" : " decoder=elan-fallback\n");
}

static int field_value(const hid_field_t* f, const uint8_t* r, uint16_t len, int* out) {
    uint32_t raw;
    if (!f || !f->valid || !out) return 0;
    raw = extract_bits(r, len, f->bit_offset, f->bit_size);
    *out = (int)raw;
    return 1;
}

static int field_value_signed(const hid_field_t* f, const uint8_t* r, uint16_t len, int* out) {
    uint32_t raw;
    if (!field_value(f, r, len, out)) return 0;
    raw = (uint32_t)*out;
    if (f->bit_size > 0 && f->bit_size < 32 && (raw & (1U << (f->bit_size - 1)))) {
        raw |= (~0U) << f->bit_size;
        *out = (int)raw;
    }
    return 1;
}

static int clamp_coord(int v, int maxv) {
    if (v < 0) return 0;
    if (maxv > 0 && v > maxv) return maxv;
    return v;
}

/*
 * Conservative ELAN0601-style layout used when the descriptor walker cannot
 * locate X/Y fields. Validated against max range before emitting motion.
 */
static int decode_elan_fallback(const uint8_t* r, uint16_t len,
                                int* x, int* y, uint8_t* buttons, int* contacts) {
    uint16_t off = 0;
    int rx, ry;
    uint8_t b;

    if (!r || len < 5 || !x || !y || !buttons || !contacts) return 0;

    if (g_tp.report_id != 0) {
        if (r[0] != g_tp.report_id) return 0;
        off = 1;
    }
    if (len < off + 5) return 0;

    b = (uint8_t)(r[off] & 0x03);
    rx = (int)r[off + 1] | (((int)r[off + 2] & 0x0F) << 8);
    ry = (int)r[off + 3] | (((int)r[off + 4] & 0x0F) << 8);
    rx = clamp_coord(rx, g_tp.max_x);
    ry = clamp_coord(ry, g_tp.max_y);

    /* Reject obvious garbage outside the configured range envelope. */
    if (rx > g_tp.max_x || ry > g_tp.max_y) return 0;
    if (rx == 0 && ry == 0 && b == 0) return 0;

    *x = rx;
    *y = ry;
    *buttons = b;
    *contacts = (rx || ry) ? 1 : 0;
    if (len >= off + 6) {
        /* Some ELAN reports stash a contact count near the tip. */
        int c = r[off] & 0x07;
        if (c >= 1 && c <= 5) *contacts = c;
    }
    return 1;
}

static int decode_descriptor_report(const uint8_t* r, uint16_t len,
                                    int* x, int* y, uint8_t* buttons,
                                    int* contacts, int8_t* wheel) {
    int rx = 0, ry = 0, tip = 0, ccount = 0, w = 0;
    int bl = 0, br = 0;
    const uint8_t* body = r;
    uint16_t body_len = len;

    if (!r || !x || !y || !buttons || !contacts || !wheel) return 0;
    if (g_tp.report_id != 0) {
        if (len < 1 || r[0] != g_tp.report_id) return 0;
        /* Walker bit_pos is relative to payload after the report-ID byte. */
        body = r + 1;
        body_len = (uint16_t)(len - 1);
    }

    if (!field_value(&g_tp.x, body, body_len, &rx)) return 0;
    if (!field_value(&g_tp.y, body, body_len, &ry)) return 0;
    (void)field_value(&g_tp.tip, body, body_len, &tip);
    (void)field_value(&g_tp.btn_left, body, body_len, &bl);
    (void)field_value(&g_tp.btn_right, body, body_len, &br);
    (void)field_value(&g_tp.contact_count, body, body_len, &ccount);
    (void)field_value_signed(&g_tp.wheel, body, body_len, &w);

    rx = clamp_coord(rx, g_tp.max_x);
    ry = clamp_coord(ry, g_tp.max_y);
    if (rx == 0 && ry == 0 && !bl && !br && !tip) return 0;

    *x = rx;
    *y = ry;
    *buttons = 0;
    if (bl) *buttons |= (1U << INPUT_BUTTON_LEFT);
    if (br) *buttons |= (1U << INPUT_BUTTON_RIGHT);
    if (ccount > 0) *contacts = ccount;
    else *contacts = tip ? 1 : ((rx || ry) ? 1 : 0);
    *wheel = (int8_t)w;
    return 1;
}

static void emit_pointer(int x, int y, uint8_t buttons, int contacts, int8_t wheel) {
    int8_t scroll = wheel;
    int dx = 0;
    int dy = 0;
    int phys_right = (buttons & (1U << INPUT_BUTTON_RIGHT)) != 0;

    /* Explicit wheel field wins; else two-finger vertical motion scrolls. */
    if (scroll == 0 && contacts >= 2 && g_tp.tracking) {
        int raw_dy = y - g_tp.last_raw_y;
        g_tp.scroll_accum += raw_dy;
        /* ~40 units of pad travel ≈ one notch; invert so up-swipe scrolls up. */
        while (g_tp.scroll_accum <= -40) {
            scroll--;
            g_tp.scroll_accum += 40;
        }
        while (g_tp.scroll_accum >= 40) {
            scroll++;
            g_tp.scroll_accum -= 40;
        }
        g_tp.last_raw_x = x;
        g_tp.last_raw_y = y;
        g_tp.tap_armed = 0;
        g_tp.synth_right_down = 0;
        if (scroll != 0) {
            g_tp.scroll_events++;
            if (g_tp.scroll_events == 1) {
                print("[touchpad] first scroll event wheel=");
                print_dec((int)scroll);
                print("\n");
                driver_log_line("[touchpad] first scroll event.");
            }
        }
        /* Keep buttons + scroll; do not warp cursor while two-finger scrolling. */
        input_report_pointer_delta(INPUT_DEVICE_I2C_TOUCHPAD, 0, 0, buttons, scroll);
        return;
    }

    if (contacts >= 1) {
        if (!g_tp.tracking) {
            int dx_tap, dy_tap;
            g_tp.tracking = 1;
            g_tp.last_raw_x = x;
            g_tp.last_raw_y = y;
            g_tp.contact_start_x = x;
            g_tp.contact_start_y = y;
            g_tp.contact_moved = 0;
            g_tp.scroll_accum = 0;

            /* Second tap of a double-tap → synthesize right-click. */
            dx_tap = x - g_tp.tap_x;
            dy_tap = y - g_tp.tap_y;
            if (!phys_right && g_tp.tap_armed &&
                (int32_t)(timer_ticks() - g_tp.tap_tick) <= TP_DOUBLETAP_TICKS &&
                dx_tap > -TP_DOUBLETAP_SLACK && dx_tap < TP_DOUBLETAP_SLACK &&
                dy_tap > -TP_DOUBLETAP_SLACK && dy_tap < TP_DOUBLETAP_SLACK) {
                g_tp.synth_right_down = 1;
                g_tp.tap_armed = 0;
                buttons |= (1U << INPUT_BUTTON_RIGHT);
                if (g_tp.button_events == 0)
                    driver_log_line("[touchpad] double-tap right-click.");
            } else {
                g_tp.synth_right_down = 0;
            }

            /* First contact: apply buttons only (no jump). */
            input_report_pointer_delta(INPUT_DEVICE_I2C_TOUCHPAD, 0, 0, buttons, 0);
            return;
        }
        dx = x - g_tp.last_raw_x;
        dy = y - g_tp.last_raw_y;
        g_tp.last_raw_x = x;
        g_tp.last_raw_y = y;
        {
            int adx = x - g_tp.contact_start_x;
            int ady = y - g_tp.contact_start_y;
            if (adx < 0) adx = -adx;
            if (ady < 0) ady = -ady;
            if (adx + ady > TP_DOUBLETAP_SLACK) g_tp.contact_moved = 1;
        }

        /* Scale pad deltas toward a comfortable desktop speed. */
        if (g_tp.max_x > 0) dx = (dx * 80) / (g_tp.max_x > 80 ? g_tp.max_x / 16 : 16);
        if (g_tp.max_y > 0) dy = (dy * 80) / (g_tp.max_y > 80 ? g_tp.max_y / 16 : 16);
        /* Screen Y grows downward; HID digitizer Y usually grows downward already. */
        if (g_tp.synth_right_down && !phys_right)
            buttons |= (1U << INPUT_BUTTON_RIGHT);
    } else {
        /* Finger up: arm first tap if it was a short stationary contact. */
        if (g_tp.tracking && !g_tp.contact_moved && !phys_right &&
            !g_tp.synth_right_down) {
            g_tp.tap_armed = 1;
            g_tp.tap_x = g_tp.last_raw_x;
            g_tp.tap_y = g_tp.last_raw_y;
            g_tp.tap_tick = timer_ticks();
        } else if (g_tp.synth_right_down) {
            /* Complete synthetic right click release. */
            buttons &= (uint8_t)~(1U << INPUT_BUTTON_RIGHT);
        }
        g_tp.tracking = 0;
        g_tp.scroll_accum = 0;
        g_tp.synth_right_down = 0;
        g_tp.contact_moved = 0;
        dx = 0;
        dy = 0;
    }

    if (g_tp.report_count == 0) {
        print("[touchpad] first decoded report: x=");
        print_dec(x);
        print(" y=");
        print_dec(y);
        print(" buttons=");
        print_dec((int)buttons);
        print(" contacts=");
        print_dec(contacts);
        print("\n");
        driver_log("[touchpad] first decoded report x=");
        driver_log_u32((uint32_t)x);
        driver_log(" y=");
        driver_log_u32((uint32_t)y);
        driver_log("\n");
    }
    g_tp.report_count++;
    if (dx || dy) g_tp.move_events++;
    if (buttons) g_tp.button_events++;
    if (scroll) g_tp.scroll_events++;

    input_report_pointer_delta(INPUT_DEVICE_I2C_TOUCHPAD, dx, dy, buttons, scroll);
}

static void decode_report(const uint8_t* r, uint16_t len) {
    int x = 0, y = 0, contacts = 0;
    uint8_t buttons = 0;
    int8_t wheel = 0;
    int ok = 0;

    if (g_tp.used_descriptor_map)
        ok = decode_descriptor_report(r, len, &x, &y, &buttons, &contacts, &wheel);
    if (!ok)
        ok = decode_elan_fallback(r, len, &x, &y, &buttons, &contacts);
    if (!ok) return;

    g_tp.last_contacts = contacts;
    emit_pointer(x, y, buttons, contacts, wheel);
}

static int prefer_controller_index(int pass) {
    /*
     * Acer R3-131T ELAN0501 sits on 8086:22C1 (Linux: 808622C1:05). Probe
     * that device ID first, then remaining LPSS I2C controllers.
     */
    int n = i2c_controller_count();
    int i;
    int seen_preferred = 0;
    int seen_other = 0;
    if (pass < 0 || n <= 0) return pass;

    for (i = 0; i < n; i++) {
        if (i2c_controller_device_id(i) == 0x22C1U) {
            if (seen_preferred == pass) return i;
            seen_preferred++;
        }
    }
    for (i = 0; i < n; i++) {
        if (i2c_controller_device_id(i) == 0x22C1U) continue;
        if (seen_preferred + seen_other == pass) return i;
        seen_other++;
    }
    return pass < n ? pass : 0;
}

void touchpad_init(void) {
    const i2c_hid_device_t* hid;
    const i2c_bus_t* bus;
    memset(&g_tp, 0, sizeof(g_tp));
    g_tp_skip_reason = "probing";

    const acpi_touchpad_info_t* info = acpi_get_touchpad_info();
    int acpi_match = info && (info->elan0601_found || info->pnp0c50_found);
    int baytrail_probe = info && (info->baytrail_i2c_found ||
                                  info->braswell_i2c_found);
    int controllers = i2c_controller_count();
    /* Narrow: prefer 22C1 then a few more LPSS I2C instances. */
    int max_controllers = 4;

    if (!acpi_match && !baytrail_probe && controllers > 0) {
#ifdef __x86_64__
        /*
         * Acer R3-131T / Braswell UEFI: poking ungated LPSS I2C MMIO without
         * an ACPI HID node can bus-stall. Skip the blind multi-controller
         * probe; touchpad stays off rather than hanging the boot.
         */
        print("[touchpad] no ACPI HID match; skipping PCI I2C MMIO probe (x64 UEFI).\n");
        driver_log_line("[touchpad] skipping PCI I2C probe without ACPI HID on x64.");
        g_tp_skip_reason = "no ACPI HID match (x64)";
        return;
#else
        print("[touchpad] no ACPI HID match; probing PCI I2C controllers anyway.\n");
        driver_log_line("[touchpad] probing PCI I2C without ACPI HID match.");
        baytrail_probe = 1;
#endif
    }

    if (!acpi_match && !baytrail_probe) {
        print("[touchpad] no ACPI HID-over-I2C touchpad found.\n");
        driver_log_line("[touchpad] no ACPI HID-over-I2C touchpad found.");
        g_tp_skip_reason = "no ACPI HID-over-I2C";
        return;
    }
    if (!acpi_match && baytrail_probe) {
        print("[touchpad] no exact ACPI match; probing Bay Trail/Braswell I2C.\n");
        driver_log_line("[touchpad] no exact ACPI match; probing Bay Trail/Braswell I2C.");
    }

    if (controllers <= 0) {
        print("[touchpad] no I2C controllers available.\n");
        driver_log_line("[touchpad] no I2C controllers available.");
        g_tp_skip_reason = "no I2C controllers";
        return;
    }

    uint8_t addrs[5];
    int addr_count = 0;
    /* Prefer ACPI-provided address when known. */
    if (info && info->touchpad_i2c_addr &&
        !addr_already_tried(addrs, addr_count, info->touchpad_i2c_addr))
        addrs[addr_count++] = info->touchpad_i2c_addr;
    /* Lenovo 80M4 ELAN0601 defaults to 0x15. */
    if (!addr_already_tried(addrs, addr_count, 0x15)) addrs[addr_count++] = 0x15;
    if (!addr_already_tried(addrs, addr_count, 0x2C)) addrs[addr_count++] = 0x2C;
    if (!addr_already_tried(addrs, addr_count, 0x10)) addrs[addr_count++] = 0x10;
    if (!addr_already_tried(addrs, addr_count, 0x20)) addrs[addr_count++] = 0x20;

    if (controllers < max_controllers) max_controllers = controllers;

    for (int i = 0; i < max_controllers; i++) {
        int idx = prefer_controller_index(i);
        print("[touchpad] probing I2C controller ");
        print_dec(idx);
        print(" for HID device.\n");
        if (i2c_init_controller(idx) != 0) {
            continue;
        }
        for (int a = 0; a < addr_count; a++) {
            print("[touchpad] trying HID-I2C addr 0x");
            {
                char hx[3];
                const char* h = "0123456789ABCDEF";
                hx[0] = h[(addrs[a] >> 4) & 0xF];
                hx[1] = h[addrs[a] & 0xF];
                hx[2] = '\0';
                print(hx);
            }
            print(".\n");
            if (i2c_hid_init(addrs[a]) == 0) {
                g_tp.i2c_addr = addrs[a];
                break;
            }
        }
        if (i2c_hid_get_device()->ready) break;
    }

    hid = i2c_hid_get_device();
    if (!hid->ready) {
        print("[touchpad] HID-over-I2C init failed on all controllers.\n");
        driver_log_line("[touchpad] HID-over-I2C init failed on all controllers.");
        g_tp_skip_reason = "HID-over-I2C init failed";
        return;
    }

    bus = i2c_get_bus();
    if (bus && bus->ready) {
        g_tp.ctrl_bus = bus->bus;
        g_tp.ctrl_slot = bus->slot;
        g_tp.ctrl_func = bus->func;
        g_tp.ctrl_device_id = bus->device_id;
    }
    g_tp.vendor_id = hid->vendor_id;
    g_tp.product_id = hid->product_id;

    parse_report_descriptor();
    input_set_i2c_touchpad_active(1);
    g_tp.ready = 1;
    g_tp_skip_reason = "ready";

    print("[touchpad] I2C HID touchpad ready, range ");
    print_dec(g_tp.max_x);
    print("x");
    print_dec(g_tp.max_y);
    print(" vid=");
    {
        char buf[8];
        const char* h = "0123456789ABCDEF";
        buf[0] = '0'; buf[1] = 'x';
        buf[2] = h[(g_tp.vendor_id >> 12) & 0xF];
        buf[3] = h[(g_tp.vendor_id >> 8) & 0xF];
        buf[4] = h[(g_tp.vendor_id >> 4) & 0xF];
        buf[5] = h[g_tp.vendor_id & 0xF];
        buf[6] = '\0';
        print(buf);
    }
    print("\n");
    driver_log("[touchpad] I2C HID touchpad ready, range ");
    driver_log_u32((uint32_t)g_tp.max_x);
    driver_log("x");
    driver_log_u32((uint32_t)g_tp.max_y);
    driver_log("\n");
}

int touchpad_ready(void) {
    return g_tp.ready;
}

uint16_t touchpad_vendor_id(void) { return g_tp.vendor_id; }
uint16_t touchpad_product_id(void) { return g_tp.product_id; }
uint8_t touchpad_i2c_addr(void) { return g_tp.i2c_addr; }

const char* touchpad_decoder_name(void) {
    if (!g_tp.ready) return "none";
    return g_tp.used_descriptor_map ? "descriptor" : "elan-fallback";
}

void touchpad_print_status(void (*write)(const char*)) {
    if (!write) return;
    write("I2C touchpad: ");
    if (!g_tp.ready) {
        write("no (");
        write(g_tp_skip_reason ? g_tp_skip_reason : "unknown");
        write(")\n");
        return;
    }
    write("yes addr=0x");
    {
        char hx[3];
        const char* h = "0123456789ABCDEF";
        hx[0] = h[(g_tp.i2c_addr >> 4) & 0xF];
        hx[1] = h[g_tp.i2c_addr & 0xF];
        hx[2] = '\0';
        write(hx);
    }
    write(" vid=");
    write_dec(write, (int)g_tp.vendor_id);
    write(" pid=");
    write_dec(write, (int)g_tp.product_id);
    write(" decoder=");
    write(touchpad_decoder_name());
    write(" moves=");
    write_dec(write, (int)g_tp.move_events);
    write(" clicks=");
    write_dec(write, (int)g_tp.button_events);
    write(" scrolls=");
    write_dec(write, (int)g_tp.scroll_events);
    write("\n");
}

void touchpad_poll(void) {
    uint8_t report[I2C_HID_MAX_INPUT];
    uint16_t len = 0;
    if (!g_tp.ready) return;
    /* USB and I2C pointers coexist: both may emit events. */
    int rc = i2c_hid_poll_report(report, sizeof(report), &len);
    if (rc <= 0 || len == 0) return;
    decode_report(report, len);
}
