#ifndef BIOS_DISK_H
#define BIOS_DISK_H

#include <stddef.h>
#include <stdint.h>

#define BIOS_MAX_DRIVES 16

typedef struct {
    uint8_t drive;
    uint64_t sectors;
    uint32_t sector_size;
    int present;
} bios_drive_info_t;

void bios_disk_scan(void);
int bios_disk_count(void);
const bios_drive_info_t* bios_disk_get(int index);
int bios_read_lba(uint8_t drive, uint64_t lba, uint16_t count, void* out);
int bios_write_lba(uint8_t drive, uint64_t lba, uint16_t count, const void* in);

#endif
