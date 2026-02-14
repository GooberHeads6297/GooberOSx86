#ifndef USB_UHCI_H
#define USB_UHCI_H

#include "../../pci/pci.h"

int uhci_init(const usb_pci_controller_t* controller);
void uhci_poll(void);
int uhci_controller_healthy(void);
int uhci_pointer_enumeration_allowed(void);

#endif
