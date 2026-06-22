#include "i2c_hid.h"
#include "../i2c/i2c.h"
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

    if (g_dev.hid_desc_len < 24 || g_dev.report_desc_len == 0 ||
        g_dev.report_desc_len > I2C_HID_MAX_REPORT_DESC ||
        g_dev.max_input_len == 0 || g_dev.max_input_len > I2C_HID_MAX_INPUT) {
        print("[i2c-hid] descriptor values out of supported range.\n");
        return -1;
    }

    print("[i2c-hid] descriptor ok, vendor=");
    print_u16_hex(g_dev.vendor_id);
    print(" product=");
    print_u16_hex(g_dev.product_id);
    print("\n");
    return 0;
}

int i2c_hid_init(uint8_t addr) {
    memset(&g_dev, 0, sizeof(g_dev));
    memset(g_report_desc, 0, sizeof(g_report_desc));

    if (addr == 0) {
        print("[i2c-hid] no I2C address provided.\n");
        return -1;
    }
    if (read_hid_descriptor(addr) != 0) return -1;

    (void)i2c_hid_command(0x08); /* power on */
    (void)i2c_hid_command(0x01); /* reset */

    if (i2c_read_reg16(addr, g_dev.report_desc_reg,
                       g_report_desc, g_dev.report_desc_len) != 0) {
        print("[i2c-hid] report descriptor read failed.\n");
        return -1;
    }

    g_dev.ready = 1;
    print("[i2c-hid] report descriptor loaded.\n");
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
