#ifndef USB_CORE_H
#define USB_CORE_H

#include "usb_hcd.h"
#include "usb_device.h"
#include "usb_class.h"

/*
 * New USB stack entry points. Selected with gooberos.usb.stack=new.
 * Controllers stay active independent of whether a pointer was found.
 */
void usb_core_init(void);
void usb_core_poll(void);
int  usb_core_has_pointer(void);
int  usb_core_has_keyboard(void);

/* Enumerate every online HCD once (boot path). */
void usb_core_enumerate_all(void);

/* Hotplug helpers. */
void usb_core_hotplug_poll(void);

#endif
