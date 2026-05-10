#include "msc.h"

int usb_msc_probe_pci_controller(uint8_t prog_if, uint32_t bar0, usb_msc_probe_result_t* out) {
    if (!out) return 0;

    out->controller_present = 0;
    out->controller_supported = 0;
    out->transport_scaffold_ready = 0;
    out->bulk_only_pending = 0;
    out->host_kind = USB_MSC_HOST_NONE;

    if (bar0 == 0 || bar0 == 0xFFFFFFFFU) return 0;

    out->controller_present = 1;
    out->bulk_only_pending = 1;

    if (prog_if == 0x00) {
        out->host_kind = USB_MSC_HOST_UHCI;
        out->controller_supported = 1;
        out->transport_scaffold_ready = 1;
        return 1;
    }
    if (prog_if == 0x10) {
        out->host_kind = USB_MSC_HOST_OHCI;
        return 1;
    }
    if (prog_if == 0x20) {
        out->host_kind = USB_MSC_HOST_EHCI;
        return 1;
    }
    if (prog_if == 0x30) {
        out->host_kind = USB_MSC_HOST_XHCI;
        return 1;
    }
    return 1;
}

const char* usb_msc_host_name(uint8_t host_kind) {
    if (host_kind == USB_MSC_HOST_UHCI) return "UHCI";
    if (host_kind == USB_MSC_HOST_OHCI) return "OHCI";
    if (host_kind == USB_MSC_HOST_EHCI) return "EHCI";
    if (host_kind == USB_MSC_HOST_XHCI) return "XHCI";
    return "Unknown";
}
