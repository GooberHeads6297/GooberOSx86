#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

typedef struct {
    int acpi_found;
    int elan0601_found;
    int pnp0c50_found;
    int baytrail_i2c_found;
    uint8_t touchpad_i2c_addr;
    uint16_t hid_desc_reg;
} acpi_touchpad_info_t;

void acpi_init(void);
const acpi_touchpad_info_t* acpi_get_touchpad_info(void);

#endif
