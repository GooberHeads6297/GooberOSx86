#ifndef INSTALL_H
#define INSTALL_H

#include "../drivers/storage/storage.h"

#define INSTALL_PARTITION_START_LBA   2048U
/* MBR style: GRUB core.img immediately after the MBR. */
#define INSTALL_MBR_CORE_FIRST_LBA    1U
/* Must stay in sync with scripts/prepare-fat-template.sh (valid FAT32 ESP). */
#define INSTALL_FAT_TEMPLATE_MIB      36U
#define INSTALL_FAT_TEMPLATE_SECTORS  (INSTALL_FAT_TEMPLATE_MIB * 1024U * 1024U / 512U)
#define INSTALL_MIN_DISK_SECTORS      (INSTALL_PARTITION_START_LBA + INSTALL_FAT_TEMPLATE_SECTORS + 34U)

typedef enum {
    INSTALL_STYLE_MBR = 0, /* BIOS/legacy: bootable MBR + FAT32 (type 0x0C) */
    INSTALL_STYLE_GPT      /* UEFI: protective MBR + GPT ESP */
} install_partition_style_t;

const char* install_partition_style_name(install_partition_style_t style);

int install_payload_available(void);
int install_fat32_to_device(const storage_device_info_t* dev,
                            install_partition_style_t style);

/*
 * Memory-only "install": stamp a marker into live memfs so the session is
 * treated as an installed RAM root (no block device). Persistence still
 * requires a READY FAT32 target via install fat32.
 */
int install_memory_only(void);

#endif
