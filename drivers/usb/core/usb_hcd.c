#include "usb_hcd.h"
#include "../host/xhci_hcd.h"
#include "../../pci/pci.h"
#include "../../io/io.h"
#include "../../diagnostics/driver_log.h"

extern void print(const char* str);

#define USB_HCD_DRIVER_MAX 4

static const usb_hcd_ops_t* g_drivers[USB_HCD_DRIVER_MAX];
static int g_driver_count = 0;
static usb_hcd_t g_hcds[USB_HCD_MAX];
static int g_hcd_count = 0;

static void hcd_log(const char* s) {
    driver_log(s);
    print(s);
    while (*s) outb(0xE9, *s++);
}

void usb_hcd_registry_reset(void) {
    g_driver_count = 0;
    g_hcd_count = 0;
    for (int i = 0; i < USB_HCD_MAX; i++) {
        g_hcds[i].ops = 0;
        g_hcds[i].priv = 0;
        g_hcds[i].online = 0;
        g_hcds[i].healthy = 0;
    }
}

int usb_hcd_register_driver(const usb_hcd_ops_t* ops) {
    if (!ops || g_driver_count >= USB_HCD_DRIVER_MAX) return -1;
    g_drivers[g_driver_count++] = ops;
    return 0;
}

void usb_hcd_register_builtins(void) {
    usb_hcd_register_driver(&usb_xhci_hcd_ops);
}

int usb_hcd_probe_all(void) {
    usb_pci_controller_t list[USB_HCD_MAX];
    int n = pci_find_usb_controllers(list, USB_HCD_MAX);
    int brought = 0;

    hcd_log("USB-HCD: probing controllers (new stack)\n");
    for (int i = 0; i < n && g_hcd_count < USB_HCD_MAX; i++) {
        const usb_hcd_ops_t* match = 0;
        for (int d = 0; d < g_driver_count; d++) {
            if (g_drivers[d]->prog_if == list[i].prog_if) {
                match = g_drivers[d];
                break;
            }
        }
        if (!match) continue;

        usb_hcd_t* hcd = &g_hcds[g_hcd_count];
        hcd->ops = match;
        hcd->pci = list[i];
        hcd->priv = 0;
        hcd->online = 0;
        hcd->healthy = 0;
        hcd->id = (uint8_t)g_hcd_count;

        hcd_log("USB-HCD: candidate ");
        hcd_log(match->name);
        hcd_log("\n");

        if (match->probe && match->probe(hcd, &list[i]) == 0) {
            hcd->online = 1;
            hcd->healthy = match->healthy ? match->healthy(hcd) : 1;
            g_hcd_count++;
            brought++;
            hcd_log("USB-HCD: online ");
            hcd_log(match->name);
            hcd_log("\n");
            /* Keep probing remaining controllers — do not stop for pointer. */
        } else {
            hcd_log("USB-HCD: probe failed ");
            hcd_log(match->name);
            hcd_log("\n");
        }
    }
    return brought;
}

int usb_hcd_count(void) {
    return g_hcd_count;
}

usb_hcd_t* usb_hcd_get(int index) {
    if (index < 0 || index >= g_hcd_count) return 0;
    return &g_hcds[index];
}

void usb_hcd_poll_all(void) {
    for (int i = 0; i < g_hcd_count; i++) {
        usb_hcd_t* h = &g_hcds[i];
        if (!h->online || !h->ops || !h->ops->poll) continue;
        h->ops->poll(h);
        if (h->ops->healthy)
            h->healthy = h->ops->healthy(h);
    }
}
