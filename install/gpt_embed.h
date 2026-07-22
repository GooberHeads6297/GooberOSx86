#ifndef GPT_EMBED_H
#define GPT_EMBED_H

#include <stdint.h>
#include <stddef.h>
#include "../drivers/storage/storage.h"

/*
 * Write protective MBR + primary/backup GPT with one EFI System Partition
 * starting at part_start_lba. Gap LBA 34..(part_start-1) is zeroed.
 * UEFI-only (non-bootable protective MBR).
 */
int gpt_write_esp_layout(const storage_device_info_t* dev,
                         uint64_t disk_sectors,
                         uint32_t part_start_lba,
                         uint32_t* out_part_sectors);

#endif
