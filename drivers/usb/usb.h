#ifndef USB_H
#define USB_H

#include <stdint.h>

void usb_init(void);
void usb_poll(void);
int usb_has_pointer_device(void);
int usb_has_touchpad_device(void);

#endif
