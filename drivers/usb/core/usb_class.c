#include "usb_class.h"

#define USB_CLASS_MAX 8

static const usb_class_driver_t* g_classes[USB_CLASS_MAX];
static int g_class_count = 0;

void usb_class_registry_init(void) {
    g_class_count = 0;
}

int usb_class_register(const usb_class_driver_t* drv) {
    if (!drv || g_class_count >= USB_CLASS_MAX) return -1;
    g_classes[g_class_count++] = drv;
    return 0;
}

int usb_class_bind_device(usb_dev_t* dev) {
    int bound = 0;
    if (!dev) return 0;
    for (int i = 0; i < (int)dev->num_interfaces; i++) {
        usb_interface_t* iface = &dev->ifaces[i];
        if (iface->class_bound) continue;
        for (int c = 0; c < g_class_count; c++) {
            const usb_class_driver_t* drv = g_classes[c];
            if (!drv->match || !drv->match(dev, iface)) continue;
            if (drv->probe && drv->probe(dev, iface) == 0) {
                iface->class_bound = 1;
                bound++;
                break;
            }
        }
    }
    return bound;
}

void usb_class_unbind_device(usb_dev_t* dev) {
    if (!dev) return;
    for (int i = 0; i < (int)dev->num_interfaces; i++) {
        usb_interface_t* iface = &dev->ifaces[i];
        if (!iface->class_bound) continue;
        for (int c = 0; c < g_class_count; c++) {
            const usb_class_driver_t* drv = g_classes[c];
            if (drv->match && drv->match(dev, iface) && drv->disconnect)
                drv->disconnect(dev, iface);
        }
        iface->class_bound = 0;
        iface->class_priv = 0;
    }
}

void usb_class_poll_all(void) {
    int n = usb_dev_count();
    for (int di = 0; di < n; di++) {
        usb_dev_t* dev = usb_dev_get(di);
        if (!dev || dev->state != USB_DEV_CONFIGURED) continue;
        for (int i = 0; i < (int)dev->num_interfaces; i++) {
            usb_interface_t* iface = &dev->ifaces[i];
            if (!iface->class_bound) continue;
            for (int c = 0; c < g_class_count; c++) {
                const usb_class_driver_t* drv = g_classes[c];
                if (drv->match && drv->match(dev, iface) && drv->poll)
                    drv->poll(dev, iface);
            }
        }
    }
}
