#ifndef INSTALL_DEBUG_H
#define INSTALL_DEBUG_H

#include <stddef.h>
#include "../drivers/storage/storage.h"

void install_debug_verify_disk(const storage_device_info_t* dev,
                               uint32_t part_start_lba,
                               uint32_t part_sectors,
                               uint32_t template_sectors);

#endif
