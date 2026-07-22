#include "i2c_hid.h"
#include "../i2c/i2c.h"
#include "../timer/timer.h"
#include "../diagnostics/driver_log.h"
#include "../../lib/string.h"

extern void print(const char* str);

static i2c_hid_device_t g_dev;
static uint8_t g_report_desc[I2C_HID_MAX_REPORT_DESC];

static uint16_t le16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void print_u16_hex(uint16_t v) {
    const char* h = "0123456789ABCDEF";
    char b[7];
    b[0] = '0'; b[1] = 'x';
    b[2] = h[(v >> 12) & 0xF];
    b[3] = h[(v >> 8) & 0xF];
    b[4] = h[(v >> 4) & 0xF];
    b[5] = h[v & 0xF];
    b[6] = '\0';
    print(b);
}

static int i2c_hid_command(uint16_t command) {
    uint8_t cmd[2];
    cmd[0] = 0x00;
    cmd[1] = (uint8_t)command;
    return i2c_write_cmd(g_dev.addr, g_dev.command_reg, cmd, sizeof(cmd));
}

static int read_hid_descriptor(uint8_t addr) {
    uint8_t d[30];
    if (i2c_read_reg16(addr, 0x0001, d, sizeof(d)) != 0) {
        print("[i2c-hid] HID descriptor read failed.\n");
        return -1;
    }

    memset(&g_dev, 0, sizeof(g_dev));
    g_dev.addr = addr;
    g_dev.hid_desc_len = le16(d + 0);
    g_dev.bcd_version = le16(d + 2);
    g_dev.report_desc_len = le16(d + 4);
    g_dev.report_desc_reg = le16(d + 6);
    g_dev.input_reg = le16(d + 8);
    g_dev.max_input_len = le16(d + 10);
    g_dev.output_reg = le16(d + 12);
    g_dev.max_output_len = le16(d + 14);
    g_dev.command_reg = le16(d + 16);
    g_dev.data_reg = le16(d + 18);
    g_dev.vendor_id = le16(d + 20);
    g_dev.product_id = le16(d + 22);
    g_dev.version_id = le16(d + 24);

    /* I2C-HID 1.0 descriptors are typically wHIDDescLength == 30 and
     * bcdVersion 0x0100. Reject obviously blank/garbage responses. */
    if (g_dev.hid_desc_len < 24 || g_dev.hid_desc_len > 30 ||
        g_dev.report_desc_len == 0 ||
        g_dev.report_desc_len > I2C_HID_MAX_REPORT_DESC ||
        g_dev.max_input_len == 0 || g_dev.max_input_len > I2C_HID_MAX_INPUT ||
        g_dev.input_reg == 0 || g_dev.command_reg == 0 ||
        g_dev.vendor_id == 0 || g_dev.vendor_id == 0xFFFF ||
        g_dev.product_id == 0xFFFF) {
        print("[i2c-hid] descriptor values out of supported range.\n");
        driver_log_line("[i2c-hid] descriptor values out of supported range.");
        return -1;
    }

    print("[i2c-hid] descriptor ok, vendor=");
    print_u16_hex(g_dev.vendor_id);
    print(" product=");
    print_u16_hex(g_dev.product_id);
    print(" bcd=");
    print_u16_hex(g_dev.bcd_version);
    print("\n");
    driver_log("[i2c-hid] vendor=");
    driver_log_hex32(g_dev.vendor_id);
    driver_log(" product=");
    driver_log_hex32(g_dev.product_id);
    driver_log("\n");
    return 0;
}

int i2c_hid_init(uint8_t addr) {
    uint8_t drain[I2C_HID_MAX_INPUT];
    uint16_t drain_len = 0;
    int i;

    memset(&g_dev, 0, sizeof(g_dev));
    memset(g_report_desc, 0, sizeof(g_report_desc));

    if (addr == 0) {
        print("[i2c-hid] no I2C address provided.\n");
        return -1;
    }
    if (read_hid_descriptor(addr) != 0) return -1;

    (void)i2c_hid_command(0x08); /* power on */
    timer_busy_wait_ms(10);
    (void)i2c_hid_command(0x01); /* reset */
    /* Spec: host must wait for the reset completion report / settle. */
    timer_busy_wait_ms(60);

    /* Drain any reset-complete / stale input so the first pad poll is clean. */
    for (i = 0; i < 4; i++) {
        if (i2c_read_reg16(addr, g_dev.input_reg, drain,
                           g_dev.max_input_len < sizeof(drain)
                               ? g_dev.max_input_len : (uint16_t)sizeof(drain)) != 0)
            break;
        drain_len = le16(drain);
        if (drain_len < 2) break;
        timer_busy_wait_ms(5);
    }

    if (i2c_read_reg16(addr, g_dev.report_desc_reg,
                       g_report_desc, g_dev.report_desc_len) != 0) {
        print("[i2c-hid] report descriptor read failed.\n");
        driver_log_line("[i2c-hid] report descriptor read failed.");
        return -1;
    }

    g_dev.ready = 1;
    print("[i2c-hid] report descriptor loaded (");
    {
        char buf[8];
        /* tiny length print */
        uint16_t n = g_dev.report_desc_len;
        int p = 0;
        if (n == 0) { buf[p++] = '0'; }
        else {
            char tmp[8]; int t = 0;
            while (n && t < 7) { tmp[t++] = (char)('0' + (n % 10)); n /= 10; }
            while (t > 0) buf[p++] = tmp[--t];
        }
        buf[p] = '\0';
        print(buf);
    }
    print(" bytes).\n");
    driver_log("[i2c-hid] report descriptor loaded, bytes=");
    driver_log_u32(g_dev.report_desc_len);
    driver_log("\n");
    return 0;
}

const i2c_hid_device_t* i2c_hid_get_device(void) {
    return &g_dev;
}

const uint8_t* i2c_hid_get_report_descriptor(uint16_t* len) {
    if (len) *len = g_dev.ready ? g_dev.report_desc_len : 0;
    return g_dev.ready ? g_report_desc : NULL;
}

int i2c_hid_poll_report(uint8_t* report, uint16_t max_len, uint16_t* out_len) {
    uint8_t buf[I2C_HID_MAX_INPUT];
    uint16_t want;
    uint16_t actual;

    if (out_len) *out_len = 0;
    if (!g_dev.ready || !report || max_len == 0) return -1;

    want = g_dev.max_input_len;
    if (want > sizeof(buf)) want = sizeof(buf);

    if (i2c_read_reg16(g_dev.addr, g_dev.input_reg, buf, want) != 0) {
        return -1;
    }

    actual = le16(buf);
    if (actual < 2 || actual > want) {
        return 0;
    }
    actual = (uint16_t)(actual - 2);
    if (actual > max_len) actual = max_len;
    memcpy(report, buf + 2, actual);
    if (out_len) *out_len = actual;
    return actual > 0 ? 1 : 0;
}
