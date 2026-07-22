#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

#define ACPI_EMMC_MAX_MMIO 4

typedef struct {
    int acpi_found;
    int elan0601_found;
    int pnp0c50_found;
    int baytrail_i2c_found;
    int baytrail_emmc_acpi;
    uint8_t touchpad_i2c_addr;
    uint16_t hid_desc_reg;
    /* Memory32Fixed bases found near HID 80860F14 in DSDT/SSDT. */
    int emmc_mmio_count;
    uint32_t emmc_mmio[ACPI_EMMC_MAX_MMIO];
} acpi_touchpad_info_t;

void acpi_init(void);
const acpi_touchpad_info_t* acpi_get_touchpad_info(void);

#endif
