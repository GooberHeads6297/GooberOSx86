#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>

#define STORAGE_MAX_DEVICES 16

typedef enum {
    STORAGE_BUS_UNKNOWN = 0,
    STORAGE_BUS_ATA,
    STORAGE_BUS_PCI
} storage_bus_t;

typedef enum {
    STORAGE_TYPE_UNKNOWN = 0,
    STORAGE_TYPE_HDD,
    STORAGE_TYPE_SSD,
    STORAGE_TYPE_OPTICAL,
    STORAGE_TYPE_IDE_CONTROLLER,
    STORAGE_TYPE_AHCI_CONTROLLER,
    STORAGE_TYPE_NVME_CONTROLLER,
    STORAGE_TYPE_EMMC_CONTROLLER,
    STORAGE_TYPE_USB_CONTROLLER
} storage_type_t;

typedef enum {
    STORAGE_BACKEND_NONE = 0,
    STORAGE_BACKEND_ATA_PIO,
    STORAGE_BACKEND_IDE,
    STORAGE_BACKEND_AHCI,
    STORAGE_BACKEND_NVME,
    STORAGE_BACKEND_SDHCI,
    STORAGE_BACKEND_USB_MASS_STORAGE
} storage_backend_t;

typedef enum {
    STORAGE_INSTALL_STATE_UNAVAILABLE = 0,
    STORAGE_INSTALL_STATE_READY,
    STORAGE_INSTALL_STATE_CONTROLLER_ONLY,
    STORAGE_INSTALL_STATE_DRIVER_MISSING
} storage_install_state_t;

typedef struct {
    uint8_t present;
    uint8_t selectable;
    uint8_t direct_install_supported;
    uint8_t bus;
    uint8_t type;
    uint8_t backend;
    uint8_t install_state;
    uint8_t write_protected;
    uint8_t init_step;
    uint8_t prog_if;
    uint8_t class_code;
    uint8_t sub_class;
    uint8_t ata_channel;
    uint8_t ata_drive;
    uint8_t pci_bus;
    uint8_t pci_slot;
    uint8_t pci_func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t bar0;
    uint32_t last_status;
    uint64_t sectors;
    uint32_t sector_size;
    char model[41];
    char location[24];
} storage_device_info_t;

void storage_init(void);
void storage_scan(void);
/* Probe deferred SDHCI controllers (eMMC) if not already brought up. */
void storage_probe_sdhci(void);
/* Boot log: list SDHCI/eMMC PCI nodes and probe state. */
void storage_print_hw_summary(void);
/* Dump PCI storage/USB/Intel-0Fxx nodes for hardware bring-up. */
void storage_print_pci_inventory(void);
int storage_count(void);
const storage_device_info_t* storage_get(int index);
int storage_target_count(void);
const storage_device_info_t* storage_get_target(int index);
int storage_read_sector(const storage_device_info_t* device, uint32_t lba, void* out_sector);
int storage_read_optical_sector(const storage_device_info_t* device, uint32_t lba,
                                void* out_sector);
int storage_write_sector(const storage_device_info_t* device, uint32_t lba, const void* in_sector);
int storage_flush(const storage_device_info_t* device);
const char* storage_backend_name(uint8_t backend);
const char* storage_install_state_name(uint8_t state);
const char* storage_install_state_reason(const storage_device_info_t* device);
const char* storage_bus_name(uint8_t bus);
const char* storage_type_name(uint8_t type);

#endif
