#ifndef I2C_HID_H
#define I2C_HID_H

#include <stdint.h>

#define I2C_HID_MAX_REPORT_DESC 512
#define I2C_HID_MAX_INPUT       128

typedef struct {
    uint16_t hid_desc_len;
    uint16_t bcd_version;
    uint16_t report_desc_len;
    uint16_t report_desc_reg;
    uint16_t input_reg;
    uint16_t max_input_len;
    uint16_t output_reg;
    uint16_t max_output_len;
    uint16_t command_reg;
    uint16_t data_reg;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t version_id;
    uint8_t addr;
    int ready;
} i2c_hid_device_t;

int i2c_hid_init(uint8_t addr);
const i2c_hid_device_t* i2c_hid_get_device(void);
const uint8_t* i2c_hid_get_report_descriptor(uint16_t* len);
int i2c_hid_poll_report(uint8_t* report, uint16_t max_len, uint16_t* out_len);

#endif
