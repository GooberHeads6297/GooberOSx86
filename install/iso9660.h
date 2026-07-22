#ifndef ISO9660_H
#define ISO9660_H

#include "../drivers/storage/storage.h"

/* Locate boot/install/FAT_PART.IMG on the first optical drive. */
int iso9660_find_fat_template(const storage_device_info_t** dev_out,
                              uint32_t* extent_lba_out,
                              uint32_t* byte_size_out);

/* Read one 512-byte sector from an open ISO file extent. */
int iso9660_read_file_sector(const storage_device_info_t* dev,
                             uint32_t extent_lba,
                             uint32_t file_size,
                             uint32_t sector_index,
                             void* sector_out);

/* Human-readable detail after iso9660_find_fat_template() fails. */
const char* iso9660_last_error(void);

#endif
