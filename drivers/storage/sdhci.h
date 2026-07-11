#ifndef SDHCI_H
#define SDHCI_H

#include <stdint.h>

typedef struct {
    uint8_t mmio_accessible;
    uint8_t controller_ready;
    uint8_t card_present;
    uint8_t write_protected;
    uint8_t voltage_18;
    uint8_t voltage_30;
    uint8_t voltage_33;
    uint8_t supports_8bit;
    uint8_t initialized;
    uint8_t high_capacity;
    uint16_t rca;
    uint16_t host_version;
    uint32_t caps0;
    uint32_t caps1;
    uint32_t mmio_base;
    uint32_t sector_count;
    uint32_t last_status;
    uint8_t init_step;
} sdhci_probe_result_t;

int sdhci_probe_pci_controller(uint8_t bus,
                               uint8_t slot,
                               uint8_t func,
                               uint32_t bar0,
                               sdhci_probe_result_t* out);
/* Probe SDHCI at a fixed MMIO base (ACPI 80860F14 path). baytrail_quirks=1. */
int sdhci_probe_mmio(uint32_t mmio_base,
                     int baytrail_quirks,
                     uint8_t bus,
                     uint8_t slot,
                     uint8_t func,
                     sdhci_probe_result_t* out);
int sdhci_read_sector(uint8_t bus,
                      uint8_t slot,
                      uint8_t func,
                      uint32_t lba,
                      void* out_sector);
int sdhci_write_sector(uint8_t bus,
                       uint8_t slot,
                       uint8_t func,
                       uint32_t lba,
                       const void* in_sector);
int sdhci_flush(uint8_t bus, uint8_t slot, uint8_t func);

#endif
