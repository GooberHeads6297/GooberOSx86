#ifndef USB_HID_H
#define USB_HID_H

#include <stdint.h>

void usb_hid_init(void);
void usb_hid_register_boot_pointer(uint8_t present, uint8_t is_touchpad);
void usb_hid_register_boot_pointer_detail(uint8_t present, uint8_t is_touchpad,
                                          uint8_t port, uint8_t address,
                                          uint8_t endpoint, uint16_t max_packet,
                                          uint8_t interval);
void usb_hid_register_boot_keyboard(uint8_t present);
void usb_hid_register_boot_keyboard_detail(uint8_t present, uint8_t port,
                                           uint8_t address, uint8_t endpoint,
                                           uint16_t max_packet, uint8_t interval);
void usb_hid_handle_boot_report(const uint8_t* report, uint8_t length);
int usb_hid_has_pointer_device(void);
int usb_hid_has_touchpad_device(void);
int usb_hid_has_keyboard_device(void);

/*
 * Canonical hot-plug attach/detach hooks.
 *
 * usb_hid_attach() takes a freshly enumerated boot HID device and registers
 * it with the input subsystem. It logs a single "[usb] hotplug: ..." line
 * tagged with the originating port + USB address so the operator can
 * correlate the event with QEMU monitor commands or a real-hardware USB
 * trace. The same sink is used for both boot-time enumeration and the
 * hot-plug poll path so the format is consistent.
 *
 *   protocol == USB_HID_PROTOCOL_MOUSE     -> registers boot pointer
 *   protocol == USB_HID_PROTOCOL_KEYBOARD  -> registers boot keyboard
 *
 * usb_hid_detach() unregisters whatever the active boot HID device was
 * (mouse or keyboard) and emits the disconnect log line. The input
 * subsystem's per-device queue drain is the caller's responsibility.
 */
void usb_hid_attach(int port, uint8_t address, int protocol);
void usb_hid_detach(int port);

#endif
