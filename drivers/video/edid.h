#ifndef EDID_H
#define EDID_H

#include <stdint.h>

typedef struct {
    int valid;
    uint16_t preferred_width;
    uint16_t preferred_height;
    char monitor_name[14];
} edid_info_t;

int edid_parse_block(const uint8_t* edid, edid_info_t* out);
void edid_log_info(const edid_info_t* info);

#endif
