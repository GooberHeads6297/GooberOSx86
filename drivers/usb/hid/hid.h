#ifndef USB_HID_H
#define USB_HID_H

#include <stdint.h>

void usb_hid_init(void);
void usb_hid_register_boot_pointer(uint8_t present, uint8_t is_touchpad);
void usb_hid_handle_boot_report(const uint8_t* report, uint8_t length);
int usb_hid_has_pointer_device(void);
int usb_hid_has_touchpad_device(void);

#endif
