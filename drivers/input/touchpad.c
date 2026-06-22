#include "touchpad.h"
#include "input.h"
#include "../acpi/acpi.h"
#include "../hid/i2c_hid.h"
#include "../i2c/i2c.h"
#include "../../lib/string.h"

extern void print(const char* str);

typedef struct {
    int ready;
    uint8_t report_id;
    int has_digitizer;
    int max_x;
    int max_y;
    uint32_t report_count;
} touchpad_state_t;

static touchpad_state_t g_tp;

static void print_dec(int n) {
    char buf[16];
    itoa(n, buf, 10);
    print(buf);
}

static int addr_already_tried(const uint8_t* addrs, int count, uint8_t addr) {
    for (int i = 0; i < count; i++) {
        if (addrs[i] == addr) return 1;
    }
    return 0;
}

static void parse_report_descriptor(void) {
    uint16_t len = 0;
    const uint8_t* d = i2c_hid_get_report_descriptor(&len);
    g_tp.report_id = 0;
    g_tp.has_digitizer = 0;
    g_tp.max_x = 2047;
    g_tp.max_y = 2047;
    if (!d || len == 0) return;

    for (uint16_t i = 0; i < len; i++) {
        if (i + 1 < len && d[i] == 0x85 && g_tp.report_id == 0) {
            g_tp.report_id = d[i + 1];
        }
        if (i + 1 < len && d[i] == 0x05 && d[i + 1] == 0x0D) {
            g_tp.has_digitizer = 1;
        }
        /*
         * Cheap first-pass range discovery: when a report descriptor has
         * Usage(X), a 16-bit Logical Maximum often follows nearby.
         */
        if (i + 5 < len && d[i] == 0x09 && d[i + 1] == 0x30 && d[i + 2] == 0x26) {
            g_tp.max_x = (int)d[i + 3] | ((int)d[i + 4] << 8);
        }
        if (i + 5 < len && d[i] == 0x09 && d[i + 1] == 0x31 && d[i + 2] == 0x26) {
            g_tp.max_y = (int)d[i + 3] | ((int)d[i + 4] << 8);
        }
    }
}

void touchpad_init(void) {
    memset(&g_tp, 0, sizeof(g_tp));

    const acpi_touchpad_info_t* info = acpi_get_touchpad_info();
    int acpi_match = info && (info->elan0601_found || info->pnp0c50_found);
    int baytrail_probe = info && info->baytrail_i2c_found;
    if (!acpi_match && !baytrail_probe) {
        print("[touchpad] no ACPI HID-over-I2C touchpad found.\n");
        return;
    }
    if (!acpi_match && baytrail_probe) {
        print("[touchpad] no exact ACPI match; probing Bay Trail I2C for touchpad.\n");
    }

    int controllers = i2c_controller_count();
    if (controllers <= 0) {
        print("[touchpad] no I2C controllers available.\n");
        return;
    }

    uint8_t addrs[5];
    int addr_count = 0;
    if (info->touchpad_i2c_addr && !addr_already_tried(addrs, addr_count, info->touchpad_i2c_addr))
        addrs[addr_count++] = info->touchpad_i2c_addr;
    if (!addr_already_tried(addrs, addr_count, 0x15)) addrs[addr_count++] = 0x15;
    if (!addr_already_tried(addrs, addr_count, 0x2C)) addrs[addr_count++] = 0x2C;
    if (!addr_already_tried(addrs, addr_count, 0x10)) addrs[addr_count++] = 0x10;
    if (!addr_already_tried(addrs, addr_count, 0x20)) addrs[addr_count++] = 0x20;

    for (int i = 0; i < controllers; i++) {
        print("[touchpad] probing I2C controller ");
        print_dec(i);
        print(" for HID device.\n");
        if (i2c_init_controller(i) != 0) {
            continue;
        }
        for (int a = 0; a < addr_count; a++) {
            print("[touchpad] trying HID-I2C addr ");
            print_dec((int)addrs[a]);
            print(".\n");
            if (i2c_hid_init(addrs[a]) == 0) {
                break;
            }
        }
        if (i2c_hid_get_device()->ready) break;
    }

    if (!i2c_hid_get_device()->ready) {
        print("[touchpad] HID-over-I2C init failed on all controllers.\n");
        return;
    }

    parse_report_descriptor();
    input_set_i2c_touchpad_active(1);
    g_tp.ready = 1;
    print("[touchpad] I2C HID touchpad ready, range ");
    print_dec(g_tp.max_x);
    print("x");
    print_dec(g_tp.max_y);
    print("\n");
}

int touchpad_ready(void) {
    return g_tp.ready;
}

static void decode_elan_report(const uint8_t* r, uint16_t len) {
    if (!r || len < 5) return;

    uint16_t off = 0;
    if (g_tp.report_id != 0) {
        if (r[0] != g_tp.report_id) return;
        off = 1;
    }
    if (len < off + 5) return;

    /*
     * ELAN0601 first-pass profile. Many ELAN HID-I2C reports place button /
     * contact state first, then 12-bit little-endian-ish X/Y coordinates.
     * Once real hardware logs give us the exact descriptor/report bytes this
     * can be tightened to the exact report ID and field positions.
     */
    uint8_t buttons = r[off] & 0x03;
    int x = (int)r[off + 1] | (((int)r[off + 2] & 0x0F) << 8);
    int y = (int)r[off + 3] | (((int)r[off + 4] & 0x0F) << 8);

    if (x == 0 && y == 0 && buttons == 0) return;
    if (g_tp.report_count == 0) {
        print("[touchpad] first decoded report: x=");
        print_dec(x);
        print(" y=");
        print_dec(y);
        print(" buttons=");
        print_dec((int)buttons);
        print("\n");
    }
    g_tp.report_count++;
    input_report_pointer_absolute_scaled(INPUT_DEVICE_I2C_TOUCHPAD,
                                         x, y, g_tp.max_x, g_tp.max_y,
                                         buttons, 0);
}

void touchpad_poll(void) {
    uint8_t report[I2C_HID_MAX_INPUT];
    uint16_t len = 0;
    if (!g_tp.ready) return;
    int rc = i2c_hid_poll_report(report, sizeof(report), &len);
    if (rc <= 0 || len == 0) return;
    decode_elan_report(report, len);
}
