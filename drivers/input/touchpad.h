#ifndef TOUCHPAD_H
#define TOUCHPAD_H

#include <stdint.h>

void touchpad_init(void);
void touchpad_poll(void);
int touchpad_ready(void);

/* Durable status for `devices` / boot diagnostics. */
void touchpad_print_status(void (*write)(const char*));

uint16_t touchpad_vendor_id(void);
uint16_t touchpad_product_id(void);
uint8_t touchpad_i2c_addr(void);
const char* touchpad_decoder_name(void);

#endif
