#ifndef GRUB_BIOS_EMBED_H
#define GRUB_BIOS_EMBED_H

#include <stddef.h>
#include <stdint.h>

#define GRUB_PC_KERNEL_SECTOR_OFF 0x5CU
#define GRUB_PC_BOOT_DRIVE_OFF    0x64U
/* GRUB_BOOT_I386_PC_KERNEL_SEG (0x800) + sector_size/16 (32) */
#define GRUB_PC_DISKBOOT_SEGMENT  0x0820U

/* Patch boot.img fields and write an MBR with a FAT/ESP partition entry.
 * part_type: 0x0C = FAT32 LBA, 0xEF = EFI system partition (eMMC/UEFI). */
int grub_bios_write_mbr(uint8_t* mbr_out,
                        const uint8_t* boot_img, size_t boot_img_size,
                        uint32_t core_first_lba,
                        uint32_t part_start_lba,
                        uint32_t part_sectors,
                        uint8_t part_type);

/* Patch sector 0 of core.img with the BIOS diskboot blocklist (in-place). */
void grub_bios_patch_core_sector0(uint8_t sector0[512],
                                  uint32_t core_first_lba,
                                  size_t core_total_sectors);

#endif
