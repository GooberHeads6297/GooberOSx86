#ifndef XHCI_HCD_H
#define XHCI_HCD_H

#include "../core/usb_hcd.h"

/* usb_hcd_ops for the modular (new-stack) xHCI driver. */
extern const usb_hcd_ops_t usb_xhci_hcd_ops;

#endif
