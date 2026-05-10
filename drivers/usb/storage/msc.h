#ifndef USB_MSC_H
#define USB_MSC_H

#include <stdint.h>

typedef enum {
    USB_MSC_HOST_NONE = 0,
    USB_MSC_HOST_UHCI,
    USB_MSC_HOST_OHCI,
    USB_MSC_HOST_EHCI,
    USB_MSC_HOST_XHCI
} usb_msc_host_kind_t;

typedef struct {
    uint8_t controller_present;
    uint8_t controller_supported;
    uint8_t transport_scaffold_ready;
    uint8_t bulk_only_pending;
    uint8_t host_kind;
} usb_msc_probe_result_t;

int usb_msc_probe_pci_controller(uint8_t prog_if, uint32_t bar0, usb_msc_probe_result_t* out);
const char* usb_msc_host_name(uint8_t host_kind);

#endif
