#include "usb_device.h"

static usb_dev_t g_devs[USB_CORE_MAX_DEVICES];

void usb_dev_table_reset(void) {
    for (int i = 0; i < USB_CORE_MAX_DEVICES; i++) {
        g_devs[i].in_use = 0;
        g_devs[i].hcd = 0;
        g_devs[i].hcd_priv = 0;
        g_devs[i].num_interfaces = 0;
        g_devs[i].state = USB_DEV_DETACHED;
    }
}

usb_dev_t* usb_dev_alloc(struct usb_hcd* hcd, int port, usb_speed_t speed) {
    for (int i = 0; i < USB_CORE_MAX_DEVICES; i++) {
        if (g_devs[i].in_use) continue;
        usb_dev_t* d = &g_devs[i];
        d->in_use = 1;
        d->hcd = hcd;
        d->port = (uint8_t)port;
        d->address = 0;
        d->speed = speed;
        d->state = USB_DEV_ATTACHED;
        d->slot_id = 0;
        d->ep0_mps = 8;
        d->id_vendor = 0;
        d->id_product = 0;
        d->config_value = 0;
        d->num_interfaces = 0;
        d->hcd_priv = 0;
        for (int j = 0; j < USB_CORE_MAX_INTERFACES; j++) {
            d->ifaces[j].class_priv = 0;
            d->ifaces[j].class_bound = 0;
            d->ifaces[j].num_endpoints = 0;
        }
        return d;
    }
    return 0;
}

void usb_dev_free(usb_dev_t* dev) {
    if (!dev) return;
    dev->in_use = 0;
    dev->hcd = 0;
    dev->hcd_priv = 0;
    dev->state = USB_DEV_DETACHED;
    for (int j = 0; j < USB_CORE_MAX_INTERFACES; j++) {
        dev->ifaces[j].class_priv = 0;
        dev->ifaces[j].class_bound = 0;
    }
}

usb_dev_t* usb_dev_by_port(struct usb_hcd* hcd, int port) {
    for (int i = 0; i < USB_CORE_MAX_DEVICES; i++) {
        if (!g_devs[i].in_use) continue;
        if (g_devs[i].hcd == hcd && g_devs[i].port == (uint8_t)port)
            return &g_devs[i];
    }
    return 0;
}

int usb_dev_count(void) {
    int n = 0;
    for (int i = 0; i < USB_CORE_MAX_DEVICES; i++)
        if (g_devs[i].in_use) n++;
    return n;
}

usb_dev_t* usb_dev_get(int index) {
    int n = 0;
    for (int i = 0; i < USB_CORE_MAX_DEVICES; i++) {
        if (!g_devs[i].in_use) continue;
        if (n == index) return &g_devs[i];
        n++;
    }
    return 0;
}
