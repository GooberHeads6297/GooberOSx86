#ifndef USB_UHCI_H
#define USB_UHCI_H

#include "../../pci/pci.h"

int uhci_init(const usb_pci_controller_t* controller);
void uhci_poll(void);

#endif
