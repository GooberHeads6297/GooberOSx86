#ifndef USB_HCD_H
#define USB_HCD_H

#include "usb_types.h"
#include "usb_device.h"
#include "../../pci/pci.h"

#define USB_HCD_MAX 8

struct usb_hcd;

typedef struct usb_hcd_ops {
    const char* name;
    uint8_t prog_if;   /* PCI prog_if: 0x00 UHCI, 0x10 OHCI, 0x20 EHCI, 0x30 xHCI */

    int  (*probe)(struct usb_hcd* hcd, const usb_pci_controller_t* pci);
    void (*shutdown)(struct usb_hcd* hcd);
    void (*poll)(struct usb_hcd* hcd);
    int  (*healthy)(struct usb_hcd* hcd);

    int  (*port_count)(struct usb_hcd* hcd);
    int  (*port_connected)(struct usb_hcd* hcd, int port);
    int  (*port_speed)(struct usb_hcd* hcd, int port); /* usb_speed_t */
    int  (*port_reset)(struct usb_hcd* hcd, int port);
    int  (*port_change_pending)(struct usb_hcd* hcd, int port);
    void (*port_change_ack)(struct usb_hcd* hcd, int port);

    /* Allocate HC slot/resources for a freshly reset root port. */
    int  (*device_enable)(struct usb_hcd* hcd, usb_dev_t* dev);
    void (*device_disable)(struct usb_hcd* hcd, usb_dev_t* dev);

    /* Address Device (xHCI) / SET_ADDRESS (UHCI/OHCI/EHCI). */
    int  (*device_address)(struct usb_hcd* hcd, usb_dev_t* dev, uint8_t addr);

    int  (*ep0_mps)(struct usb_hcd* hcd, usb_dev_t* dev, uint16_t mps);

    int  (*control)(struct usb_hcd* hcd, usb_dev_t* dev,
                    uint8_t* setup8, uint8_t* data, uint16_t len, int dir_in);
    int  (*configure_interrupt)(struct usb_hcd* hcd, usb_dev_t* dev,
                                uint8_t ep_addr, uint16_t mps, uint8_t interval);
    int  (*interrupt_poll)(struct usb_hcd* hcd, usb_dev_t* dev,
                           uint8_t* out, uint16_t max_len, uint16_t* got);
    void (*interrupt_stop)(struct usb_hcd* hcd, usb_dev_t* dev);
} usb_hcd_ops_t;

typedef struct usb_hcd {
    const usb_hcd_ops_t* ops;
    usb_pci_controller_t pci;
    void* priv;
    int online;
    int healthy;
    uint8_t id;
} usb_hcd_t;

void usb_hcd_registry_reset(void);
int  usb_hcd_register_driver(const usb_hcd_ops_t* ops);
int  usb_hcd_probe_all(void);
int  usb_hcd_count(void);
usb_hcd_t* usb_hcd_get(int index);
void usb_hcd_poll_all(void);

/* Register built-in HCDs (xHCI new + legacy adapters). */
void usb_hcd_register_builtins(void);

#endif
