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
    uint32_t bar1;
} usb_pci_controller_t;

typedef struct {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t bar0;
    uint32_t bar1;
} i2c_pci_controller_t;

/*
 * A PCI display controller (base class 0x03). Reports all six raw BARs so a
 * display driver can pick out the linear-framebuffer aperture and the MMIO
 * register window itself (e.g. Bochs/QEMU stdvga LFB in BAR0, Intel GMADR
 * aperture in BAR2 and GTTMMADR registers in BAR0).
 */
typedef struct {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint8_t sub_class;
    uint8_t prog_if;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t bar[6];
} pci_display_device_t;

void pci_init(void);
int pci_find_usb_controllers(usb_pci_controller_t* out, int max_out);
int pci_find_i2c_controllers(i2c_pci_controller_t* out, int max_out);

/*
 * Enumerate PCI display controllers (base class 0x03). Returns the number
 * found. Up to max_out entries are written to out (out may be NULL to just
 * count). Passive read-only scan; never sizes or writes BARs.
 */
int pci_find_display_controllers(pci_display_device_t* out, int max_out);
uint8_t pci_read_config_byte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_read_config_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint32_t pci_read_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write_config_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);
void pci_write_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

#endif
