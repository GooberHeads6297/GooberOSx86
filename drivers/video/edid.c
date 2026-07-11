#include "edid.h"
#include "../diagnostics/driver_log.h"

static uint16_t edid_detailed_hactive(const uint8_t* dtd) {
    return (uint16_t)dtd[2] | (uint16_t)((dtd[4] & 0xF0U) << 4);
}

static uint16_t edid_detailed_vactive(const uint8_t* dtd) {
    return (uint16_t)dtd[5] | (uint16_t)((dtd[7] & 0xF0U) << 4);
}

static void copy_monitor_name(const uint8_t* desc, char* out) {
    int pos = 0;
    for (int i = 5; i < 18 && pos < 13; i++) {
        char c = (char)desc[i];
        if (c == '\n' || c == '\r') break;
        if (c < 32 || c > 126) c = '?';
        out[pos++] = c;
    }
    out[pos] = '\0';
}

int edid_parse_block(const uint8_t* edid, edid_info_t* out) {
    static const uint8_t header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    uint8_t sum = 0;

    if (!edid || !out) return 0;
    out->valid = 0;
    out->preferred_width = 0;
    out->preferred_height = 0;
    out->monitor_name[0] = '\0';

    for (int i = 0; i < 8; i++) {
        if (edid[i] != header[i]) return 0;
    }
    for (int i = 0; i < 128; i++)
        sum = (uint8_t)(sum + edid[i]);
    if (sum != 0) return 0;

    for (int block = 0; block < 4; block++) {
        const uint8_t* d = edid + 54 + block * 18;
        uint16_t pixel_clock = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
        if (pixel_clock != 0 && out->preferred_width == 0) {
            out->preferred_width = edid_detailed_hactive(d);
            out->preferred_height = edid_detailed_vactive(d);
        } else if (pixel_clock == 0 && d[3] == 0xFC) {
            copy_monitor_name(d, out->monitor_name);
        }
    }

    out->valid = 1;
    return 1;
}

void edid_log_info(const edid_info_t* info) {
    if (!info || !info->valid) {
        driver_log_line("[display] EDID: no valid EDID block available.");
        return;
    }
    driver_log("[display] EDID: valid");
    if (info->monitor_name[0]) {
        driver_log(" monitor=");
        driver_log(info->monitor_name);
    }
    if (info->preferred_width && info->preferred_height) {
        driver_log(" preferred=");
        driver_log_u32(info->preferred_width);
        driver_log("x");
        driver_log_u32(info->preferred_height);
    }
    driver_log("\n");
}
