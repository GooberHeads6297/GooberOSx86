#ifndef PCI_H
#define PCI_H

#include <stdint.h>

typedef struct {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint8_t prog_if;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t bar0;
} usb_pci_controller_t;

void pci_init(void);
int pci_find_usb_controllers(usb_pci_controller_t* out, int max_out);
uint32_t pci_read_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

#endif
