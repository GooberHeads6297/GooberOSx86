#include "grub_bios_embed.h"
#include "../lib/string.h"
#define GRUB_DISK_SECTOR_SIZE 512U
#define GRUB_LIST_ENTRY_SIZE  12U

static void grub_wle16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void grub_wle32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void grub_wle64(uint8_t* p, uint64_t v) {
    grub_wle32(p, (uint32_t)(v & 0xFFFFFFFFU));
    grub_wle32(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFU));
}

static void grub_encode_chs(uint32_t lba, uint8_t out[3]) {
    uint32_t cylinder, head, sector;

    if (lba >= 0xFBFC00U) {
        out[0] = 0xFF;
        out[1] = 0xFF;
        out[2] = 0xFF;
        return;
    }

    cylinder = lba / (255U * 63U);
    head = (lba / 63U) % 255U;
    sector = (lba % 63U) + 1U;

    if (cylinder >= 1024U) {
        out[0] = 0xFF;
        out[1] = 0xFF;
        out[2] = 0xFF;
        return;
    }

    out[0] = (uint8_t)head;
    out[1] = (uint8_t)(((cylinder >> 2) & 0xC0U) | (sector & 0x3FU));
    out[2] = (uint8_t)(cylinder & 0xFFU);
}

int grub_bios_write_mbr(uint8_t* mbr_out,
                        const uint8_t* boot_img, size_t boot_img_size,
                        uint32_t core_first_lba,
                        uint32_t part_start_lba,
                        uint32_t part_sectors,
                        uint8_t part_type) {
    uint32_t part_end_lba;
    uint8_t* pe;

    if (!mbr_out || !boot_img || boot_img_size < 440) return -1;
    if (part_type == 0) part_type = 0x0C;

    memset(mbr_out, 0, GRUB_DISK_SECTOR_SIZE);
    memcpy(mbr_out, boot_img, 440);

    grub_wle32(mbr_out + GRUB_PC_KERNEL_SECTOR_OFF, core_first_lba);
    grub_wle32(mbr_out + GRUB_PC_KERNEL_SECTOR_OFF + 4, 0);
    mbr_out[GRUB_PC_BOOT_DRIVE_OFF] = 0xFF;

    pe = mbr_out + 446;
    pe[0] = 0x80;
    pe[4] = part_type;
    grub_encode_chs(part_start_lba, pe + 1);
    part_end_lba = part_start_lba + part_sectors - 1U;
    grub_encode_chs(part_end_lba, pe + 5);
    grub_wle32(pe + 8, part_start_lba);
    grub_wle32(pe + 12, part_sectors);

    mbr_out[510] = 0x55;
    mbr_out[511] = 0xAA;
    return 0;
}

static uint16_t grub_rle16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

void grub_bios_patch_core_sector0(uint8_t sector0[512],
                                  uint32_t core_first_lba,
                                  size_t core_total_sectors) {
    uint32_t list_off;
    uint32_t term_off;
    int guard;

    if (!sector0 || core_total_sectors <= 1U) return;

    list_off = GRUB_DISK_SECTOR_SIZE - GRUB_LIST_ENTRY_SIZE;
    term_off = list_off - GRUB_LIST_ENTRY_SIZE;

    /* Clear any placeholder blocklist chain left by grub-mkimage. */
    for (guard = 0; guard < 16; guard++) {
        uint32_t off = list_off - (uint32_t)guard * GRUB_LIST_ENTRY_SIZE;
        if (off < 0x180U) break;
        if (grub_rle16(sector0 + off + 8) == 0) break;
        memset(sector0 + off, 0, GRUB_LIST_ENTRY_SIZE);
    }

    memset(sector0 + term_off, 0, GRUB_LIST_ENTRY_SIZE);
    grub_wle64(sector0 + list_off, (uint64_t)core_first_lba + 1U);
    grub_wle16(sector0 + list_off + 8, (uint16_t)(core_total_sectors - 1U));
    grub_wle16(sector0 + list_off + 10, GRUB_PC_DISKBOOT_SEGMENT);
}
