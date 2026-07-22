#ifndef HID_BOOT_H
#define HID_BOOT_H

#include "../core/usb_class.h"

/* Register boot-protocol (and generic pointer/keyboard) HID class driver. */
void usb_class_hid_register(void);

#endif
