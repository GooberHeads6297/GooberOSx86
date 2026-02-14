#ifndef USB_HOST_H
#define USB_HOST_H

#include <stdint.h>
#include "../../pci/pci.h"

typedef struct {
    usb_pci_controller_t controller;
    int active;
} usb_host_state_t;

void usb_host_init(void);
void usb_host_poll(void);
int usb_host_ready(void);
uint8_t usb_host_controller_type(void);

#endif
