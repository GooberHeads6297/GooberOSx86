/*
 * HID class driver (boot mouse / boot keyboard / generic pointer).
 *
 * Binding happens after configuration via the class registry. Transport uses
 * the owning HCD's interrupt pipe; reports feed the existing hid report
 * parser / input queue.
 */

#include "hid_boot.h"
#include "hid.h"
#include "../usb.h"
#include "../core/usb_hcd.h"
#include "../../io/io.h"
#include "../../diagnostics/driver_log.h"

extern void print(const char* str);

typedef struct {
    usb_dev_t* dev;
    usb_interface_t* iface;
    uint8_t ep_addr;
    uint16_t mps;
    uint8_t interval;
    int protocol; /* USB_HID_PROTOCOL_* */
    int scheduled;
} hid_iface_priv_t;

static hid_iface_priv_t g_priv_pool[8];
static int g_priv_used[8];

static void hidc_log(const char* s) {
    driver_log(s);
    print(s);
    while (*s) outb(0xE9, *s++);
}

static hid_iface_priv_t* priv_alloc(void) {
    for (int i = 0; i < 8; i++) {
        if (!g_priv_used[i]) {
            g_priv_used[i] = 1;
            g_priv_pool[i].scheduled = 0;
            return &g_priv_pool[i];
        }
    }
    return 0;
}

static void priv_free(hid_iface_priv_t* p) {
    if (!p) return;
    for (int i = 0; i < 8; i++) {
        if (&g_priv_pool[i] == p) {
            g_priv_used[i] = 0;
            return;
        }
    }
}

static usb_endpoint_desc_t* find_int_in(usb_interface_t* iface) {
    for (int i = 0; i < (int)iface->num_endpoints; i++) {
        usb_endpoint_desc_t* e = &iface->ep[i];
        if (!e->present) continue;
        if ((e->address & 0x80) && (e->attributes & 0x03) == 3)
            return e;
    }
    return 0;
}

static int hid_match(usb_dev_t* dev, usb_interface_t* iface) {
    (void)dev;
    if (!iface) return 0;
    if (iface->class_code != USB_CLASS_HID) return 0;
    /* Boot mouse/keyboard, or generic HID (protocol 0) with an interrupt IN. */
    if (iface->protocol == USB_HID_PROTOCOL_MOUSE ||
        iface->protocol == USB_HID_PROTOCOL_KEYBOARD)
        return 1;
    if (iface->subclass == 0 && iface->protocol == 0 && find_int_in(iface))
        return 1;
    return 0;
}

static int hid_set_protocol_boot(usb_dev_t* dev, uint8_t ifnum) {
    uint8_t setup[8];
    if (!dev || !dev->hcd || !dev->hcd->ops || !dev->hcd->ops->control)
        return -1;
    setup[0] = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    setup[1] = USB_HID_REQ_SET_PROTOCOL;
    setup[2] = 0; /* boot */
    setup[3] = 0;
    setup[4] = ifnum;
    setup[5] = 0;
    setup[6] = 0;
    setup[7] = 0;
    return dev->hcd->ops->control(dev->hcd, dev, setup, 0, 0, 0);
}

static int hid_set_idle(usb_dev_t* dev, uint8_t ifnum) {
    uint8_t setup[8];
    if (!dev || !dev->hcd || !dev->hcd->ops || !dev->hcd->ops->control)
        return -1;
    setup[0] = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    setup[1] = USB_HID_REQ_SET_IDLE;
    setup[2] = 0;
    setup[3] = 0;
    setup[4] = ifnum;
    setup[5] = 0;
    setup[6] = 0;
    setup[7] = 0;
    return dev->hcd->ops->control(dev->hcd, dev, setup, 0, 0, 0);
}

static int hid_probe(usb_dev_t* dev, usb_interface_t* iface) {
    usb_endpoint_desc_t* ep;
    hid_iface_priv_t* priv;
    int protocol;

    if (!dev || !iface) return -1;
    ep = find_int_in(iface);
    if (!ep) return -1;

    protocol = iface->protocol;
    if (protocol == 0) protocol = USB_HID_PROTOCOL_MOUSE;

    (void)hid_set_idle(dev, iface->number);
    (void)hid_set_protocol_boot(dev, iface->number);

    if (!dev->hcd->ops->configure_interrupt ||
        dev->hcd->ops->configure_interrupt(dev->hcd, dev, ep->address,
                                           ep->max_packet, ep->interval) != 0) {
        hidc_log("USB-HID: interrupt schedule failed\n");
        return -1;
    }

    priv = priv_alloc();
    if (!priv) return -1;
    priv->dev = dev;
    priv->iface = iface;
    priv->ep_addr = ep->address;
    priv->mps = ep->max_packet;
    priv->interval = ep->interval;
    priv->protocol = protocol;
    priv->scheduled = 1;
    iface->class_priv = priv;

    if (protocol == USB_HID_PROTOCOL_MOUSE) {
        usb_hid_register_boot_pointer_detail(1, 0, dev->port, dev->address,
                                             ep->address, ep->max_packet,
                                             ep->interval);
        usb_hid_attach((int)dev->port, dev->address, USB_HID_PROTOCOL_MOUSE);
        hidc_log("USB-HID: boot pointer bound\n");
    } else if (protocol == USB_HID_PROTOCOL_KEYBOARD) {
        usb_hid_register_boot_keyboard_detail(1, dev->port, dev->address,
                                              ep->address, ep->max_packet,
                                              ep->interval);
        usb_hid_attach((int)dev->port, dev->address, USB_HID_PROTOCOL_KEYBOARD);
        hidc_log("USB-HID: boot keyboard bound\n");
    }
    return 0;
}

static void hid_disconnect(usb_dev_t* dev, usb_interface_t* iface) {
    hid_iface_priv_t* priv;
    if (!iface) return;
    priv = (hid_iface_priv_t*)iface->class_priv;
    if (priv && priv->scheduled && dev && dev->hcd &&
        dev->hcd->ops && dev->hcd->ops->interrupt_stop)
        dev->hcd->ops->interrupt_stop(dev->hcd, dev);
    if (dev) usb_hid_detach((int)dev->port);
    priv_free(priv);
    iface->class_priv = 0;
}

static void hid_poll(usb_dev_t* dev, usb_interface_t* iface) {
    hid_iface_priv_t* priv;
    uint8_t buf[16];
    uint16_t got = 0;

    if (!dev || !iface || !dev->hcd || !dev->hcd->ops) return;
    priv = (hid_iface_priv_t*)iface->class_priv;
    if (!priv || !priv->scheduled) return;
    if (!dev->hcd->ops->interrupt_poll) return;

    if (dev->hcd->ops->interrupt_poll(dev->hcd, dev, buf, sizeof(buf), &got) != 0)
        return;
    if (got == 0) return;

    usb_hid_handle_boot_report_ex(priv->protocol, buf,
                                  (uint8_t)(got > 255 ? 255 : got));
}

static const usb_class_driver_t hid_driver = {
    .name = "hid-boot",
    .match = hid_match,
    .probe = hid_probe,
    .disconnect = hid_disconnect,
    .poll = hid_poll,
};

void usb_class_hid_register(void) {
    for (int i = 0; i < 8; i++) g_priv_used[i] = 0;
    usb_class_register(&hid_driver);
}
