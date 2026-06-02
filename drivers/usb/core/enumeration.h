#ifndef USB_ENUMERATION_H
#define USB_ENUMERATION_H

#include "../usb.h"

void usb_enumerate_devices(void);
int  usb_get_device_count(void);
const usb_device_t* usb_get_device(int index);
void usb_process_hid_reports(void);

/*
 * Hot-plug single-port entry. Resets the active interrupt endpoint, then
 * runs the same reset+address+descriptor+config chain on the given port
 * that the boot-time scan uses. Return value matches usb.h's
 * USB_HID_PROTOCOL_* constants (MOUSE / KEYBOARD / 0 / -1).
 */
int  usb_enumerate_port_hotplug(int port, uint8_t* out_addr);

/* Tear down active interrupt endpoint + clear the address pool on disconnect. */
void usb_enumeration_release_active(void);

#endif
