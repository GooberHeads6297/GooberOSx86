#include "connector.h"
#include "intel_gfx.h"
#include "../diagnostics/driver_log.h"
#include <stddef.h>

static display_connector_t g_connectors[DISPLAY_MAX_CONNECTORS];
static int g_connector_count = 0;

const char* display_connector_type_name(display_connector_type_t type) {
    switch (type) {
        case DISPLAY_CONN_EDP:      return "eDP";
        case DISPLAY_CONN_LVDS:     return "LVDS";
        case DISPLAY_CONN_HDMI:     return "HDMI-A";
        case DISPLAY_CONN_DP:       return "DP";
        case DISPLAY_CONN_VGA:      return "VGA";
        case DISPLAY_CONN_SIMPLEFB: return "SimpleFB";
        default:                    return "Unknown";
    }
}

const char* display_connector_status_name(display_connector_status_t status) {
    switch (status) {
        case DISPLAY_CONN_STATUS_CONNECTED:    return "connected";
        case DISPLAY_CONN_STATUS_DISCONNECTED: return "disconnected";
        default:                               return "unknown";
    }
}

void display_connectors_reset(void) {
    for (int i = 0; i < DISPLAY_MAX_CONNECTORS; i++) {
        g_connectors[i].present = 0;
        g_connectors[i].name[0] = '\0';
        g_connectors[i].type = DISPLAY_CONN_UNKNOWN;
        g_connectors[i].status = DISPLAY_CONN_STATUS_UNKNOWN;
        g_connectors[i].gmbus_pin = 0;
        g_connectors[i].port_index = 0;
        g_connectors[i].preferred_width = 0;
        g_connectors[i].preferred_height = 0;
        g_connectors[i].monitor_name[0] = '\0';
        g_connectors[i].edid_valid = 0;
        g_connectors[i].hpd_known = 0;
        g_connectors[i].hpd_live = 0;
    }
    g_connector_count = 0;
}

static void copy_name(char* dst, size_t dst_sz, const char* src) {
    size_t i = 0;
    if (!dst || dst_sz == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    while (src[i] && i + 1 < dst_sz) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void make_conn_name(char* out, size_t out_sz,
                           display_connector_type_t type, uint32_t index) {
    const char* prefix = display_connector_type_name(type);
    char num[4];
    int n = 0;
    uint32_t v = index ? index : 1U;
    size_t i = 0, j;

    if (!out || out_sz == 0) return;
    while (prefix[i] && i + 1 < out_sz) {
        out[i] = prefix[i];
        i++;
    }
    if (i + 1 < out_sz) out[i++] = '-';
    if (v == 0) num[n++] = '0';
    else {
        char tmp[4];
        int q = 0;
        while (v && q < 3) { tmp[q++] = (char)('0' + (v % 10)); v /= 10; }
        while (q) num[n++] = tmp[--q];
    }
    for (j = 0; j < (size_t)n && i + 1 < out_sz; j++)
        out[i++] = num[j];
    out[i] = '\0';
}

static int connector_append(const display_connector_t* src) {
    display_connector_t* dst;
    if (!src || g_connector_count >= DISPLAY_MAX_CONNECTORS) return 0;
    dst = &g_connectors[g_connector_count];
    *dst = *src;
    dst->present = 1;
    if (dst->name[0] == '\0')
        make_conn_name(dst->name, sizeof(dst->name), dst->type, dst->port_index);
    g_connector_count++;
    return 1;
}

void display_connector_add_simplefb(uint32_t width, uint32_t height,
                                    const char* monitor_name) {
    display_connector_t c;
    int i;

    for (i = 0; i < g_connector_count; i++) {
        if (g_connectors[i].type == DISPLAY_CONN_SIMPLEFB)
            return;
    }

    for (i = 0; i < (int)sizeof(c); i++) ((uint8_t*)&c)[i] = 0;
    c.type = DISPLAY_CONN_SIMPLEFB;
    c.status = DISPLAY_CONN_STATUS_CONNECTED;
    c.port_index = 1;
    c.preferred_width = (uint16_t)width;
    c.preferred_height = (uint16_t)height;
    c.edid_valid = (width >= 320 && height >= 200) ? 1 : 0;
    copy_name(c.monitor_name, sizeof(c.monitor_name),
              monitor_name ? monitor_name : "Firmware");
    make_conn_name(c.name, sizeof(c.name), c.type, c.port_index);
    connector_append(&c);
}

void display_connectors_stub_firmware_panel(uint32_t width, uint32_t height) {
    display_connector_t c;

    display_connectors_reset();

    if (width >= 320 && height >= 200) {
        for (uint32_t i = 0; i < sizeof(c); i++) ((uint8_t*)&c)[i] = 0;
        c.type = DISPLAY_CONN_EDP;
        c.status = DISPLAY_CONN_STATUS_CONNECTED;
        c.port_index = 1;
        c.preferred_width = (uint16_t)width;
        c.preferred_height = (uint16_t)height;
        copy_name(c.monitor_name, sizeof(c.monitor_name), "Internal");
        make_conn_name(c.name, sizeof(c.name), c.type, c.port_index);
        connector_append(&c);
    }

    for (uint32_t i = 0; i < sizeof(c); i++) ((uint8_t*)&c)[i] = 0;
    c.type = DISPLAY_CONN_HDMI;
    c.status = DISPLAY_CONN_STATUS_UNKNOWN;
    c.port_index = 1;
    copy_name(c.monitor_name, sizeof(c.monitor_name), "HDMI");
    make_conn_name(c.name, sizeof(c.name), c.type, c.port_index);
    connector_append(&c);

    for (uint32_t i = 0; i < sizeof(c); i++) ((uint8_t*)&c)[i] = 0;
    c.type = DISPLAY_CONN_DP;
    c.status = DISPLAY_CONN_STATUS_UNKNOWN;
    c.port_index = 1;
    copy_name(c.monitor_name, sizeof(c.monitor_name), "DP");
    make_conn_name(c.name, sizeof(c.name), c.type, c.port_index);
    connector_append(&c);

    display_connector_add_simplefb(width, height, "Firmware-LFB");
    display_connectors_log();
}

int display_connectors_scan(void) {
    return display_connectors_scan_ex(1);
}

int display_connectors_scan_ex(int allow_gmbus) {
    int n;

    display_connectors_reset();
    n = intel_gfx_scan_connectors_ex(g_connectors, DISPLAY_MAX_CONNECTORS,
                                     allow_gmbus ? 1 : 0);
    if (n > 0)
        g_connector_count = n;

    /* Caller may add SimpleFB via display_connector_add_simplefb(). */
    if (g_connector_count == 0) {
        display_connector_add_simplefb(0, 0, "None");
        if (g_connector_count > 0)
            g_connectors[0].status = DISPLAY_CONN_STATUS_UNKNOWN;
    }

    display_connectors_log();
    return g_connector_count;
}

int display_connectors_count(void) {
    return g_connector_count;
}

const display_connector_t* display_connector_get(int index) {
    if (index < 0 || index >= g_connector_count) return NULL;
    return &g_connectors[index];
}

const display_connector_t* display_connector_preferred(void) {
    int i;
    const display_connector_t* fallback = NULL;
    for (i = 0; i < g_connector_count; i++) {
        const display_connector_t* c = &g_connectors[i];
        if (c->status != DISPLAY_CONN_STATUS_CONNECTED) continue;
        if (c->preferred_width >= 320 && c->preferred_height >= 200)
            return c;
        if (!fallback) fallback = c;
    }
    return fallback;
}

void display_connectors_log(void) {
    int i;
    driver_log("[display] connectors: ");
    driver_log_u32((uint32_t)g_connector_count);
    driver_log("\n");

    for (i = 0; i < g_connector_count; i++) {
        const display_connector_t* c = &g_connectors[i];
        driver_log("[display] ");
        driver_log(c->name);
        driver_log(" ");
        driver_log(display_connector_status_name(c->status));
        if (c->status == DISPLAY_CONN_STATUS_CONNECTED &&
            c->preferred_width && c->preferred_height) {
            driver_log(" ");
            driver_log_u32(c->preferred_width);
            driver_log("x");
            driver_log_u32(c->preferred_height);
        }
        if (c->monitor_name[0]) {
            driver_log(" \"");
            driver_log(c->monitor_name);
            driver_log("\"");
        }
        if (c->hpd_known) {
            driver_log(c->hpd_live ? " HPD=1" : " HPD=0");
        }
        driver_log("\n");
    }
}
