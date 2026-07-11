#ifndef PARTITION_H
#define PARTITION_H

#include "storage.h"
#include <stdint.h>

#define PARTITION_MAX_ENTRIES 64

typedef struct {
    uint8_t  present;
    uint8_t  gpt;
    uint8_t  bootable;
    uint8_t  mbr_type;
    uint64_t start_lba;
    uint64_t sector_count;
    char     name[37];
} partition_info_t;

int partition_count(const storage_device_info_t* dev);
int partition_get_info(const storage_device_info_t* dev, int index,
                       partition_info_t* out);
int partition_read_sector(const storage_device_info_t* dev, int part_index,
                          uint32_t rel_lba, void* out_sector);
int partition_write_sector(const storage_device_info_t* dev, int part_index,
                           uint32_t rel_lba, const void* in_sector);
const char* partition_type_name(uint8_t mbr_type);
void partition_print_table(const storage_device_info_t* dev,
                           void (*emit)(const char*));

#endif
