#ifndef USB_CLASS_H
#define USB_CLASS_H

#include "usb_device.h"

typedef struct usb_class_driver {
    const char* name;
    /* Return 1 if this driver claims the interface. */
    int  (*match)(usb_dev_t* dev, usb_interface_t* iface);
    int  (*probe)(usb_dev_t* dev, usb_interface_t* iface);
    void (*disconnect)(usb_dev_t* dev, usb_interface_t* iface);
    void (*poll)(usb_dev_t* dev, usb_interface_t* iface);
} usb_class_driver_t;

void usb_class_registry_init(void);
int  usb_class_register(const usb_class_driver_t* drv);
int  usb_class_bind_device(usb_dev_t* dev);
void usb_class_unbind_device(usb_dev_t* dev);
void usb_class_poll_all(void);

/* Built-in class drivers. */
void usb_class_hid_register(void);

#endif
