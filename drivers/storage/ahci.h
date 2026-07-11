#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>

typedef struct {
    uint8_t initialized;
    uint8_t port;
    uint8_t is_atapi;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t abar;
    uint32_t sector_count;
    uint32_t sector_size;
    char model[41];
} ahci_probe_result_t;

/*
 * Probe one AHCI HBA (PCI class 01:06). On success, out describes the first
 * usable SATA disk port (not ATAPI). Returns 1 if a disk was found.
 */
int ahci_probe_pci_controller(uint8_t bus,
                              uint8_t slot,
                              uint8_t func,
                              uint32_t abar_bar5,
                              ahci_probe_result_t* out);

int ahci_read_sector(uint8_t bus, uint8_t slot, uint8_t func,
                     uint8_t port, uint32_t lba, void* out_sector);
int ahci_write_sector(uint8_t bus, uint8_t slot, uint8_t func,
                      uint8_t port, uint32_t lba, const void* in_sector);
int ahci_flush(uint8_t bus, uint8_t slot, uint8_t func, uint8_t port);

#endif
