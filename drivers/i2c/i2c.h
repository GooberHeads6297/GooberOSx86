#ifndef I2C_H
#define I2C_H

#include <stdint.h>

typedef struct {
    uintptr_t mmio;
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint16_t vendor_id;
    uint16_t device_id;
    int ready;
} i2c_bus_t;

int i2c_init(void);
int i2c_init_controller(int index);
int i2c_controller_count(void);
uint16_t i2c_controller_device_id(int index);
const i2c_bus_t* i2c_get_bus(void);
int i2c_read_reg16(uint8_t addr, uint16_t reg, uint8_t* data, uint16_t len);
int i2c_write_cmd(uint8_t addr, uint16_t reg, const uint8_t* data, uint16_t len);

#endif
