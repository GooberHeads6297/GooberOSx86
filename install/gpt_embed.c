#include "gpt_embed.h"
#include "../lib/string.h"

extern void print(const char*);

/* EFI System Partition type GUID (mixed-endian GPT encoding). */
static const uint8_t k_esp_type_guid[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

/* Stable unique partition GUID for GooberOS ESP. */
static const uint8_t k_esp_unique_guid[16] = {
    0x47, 0x4F, 0x4F, 0x42, 0x45, 0x52, 0x4F, 0x53, /* "GOOBEROS" */
    0x45, 0x53, 0x50, 0x30, 0x30, 0x30, 0x31, 0x00  /* "ESP0001" */
};

static const uint8_t k_disk_guid[16] = {
    0x47, 0x4F, 0x4F, 0x42, 0x45, 0x52, 0x44, 0x49, /* "GOOBERDI" */
    0x53, 0x4B, 0x30, 0x30, 0x30, 0x30, 0x31, 0x00  /* "SK00001" */
};

static void gpt_wle16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void gpt_wle32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void gpt_wle64(uint8_t* p, uint64_t v) {
    gpt_wle32(p, (uint32_t)(v & 0xFFFFFFFFU));
    gpt_wle32(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFU));
}

static void gpt_print_u32(uint32_t v) {
    char tmp[12];
    int n = 0;
    int i;
    if (v == 0) {
        print("0");
        return;
    }
    while (v > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    for (i = n - 1; i >= 0; i--) {
        char c[2];
        c[0] = tmp[i];
        c[1] = '\0';
        print(c);
    }
}

/* IEEE CRC32 (Ethernet / GPT). */
static uint32_t gpt_crc32(const uint8_t* data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFU;
    uint32_t i, b;
    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (b = 0; b < 8; b++) {
            if (crc & 1U)
                crc = (crc >> 1) ^ 0xEDB88320U;
            else
                crc >>= 1;
        }
    }
    return ~crc;
}

static int gpt_write_sector(const storage_device_info_t* dev, uint64_t disk_sectors,
                            uint64_t lba, const uint8_t* sector) {
    if (lba > 0xFFFFFFFFULL) {
        print("install: GPT write failed at LBA (over 32-bit)\n");
        return -1;
    }
    if (disk_sectors == 0 || lba >= disk_sectors) {
        print("install: GPT write failed at LBA ");
        gpt_print_u32((uint32_t)lba);
        print(" (past end of disk, sectors=");
        gpt_print_u32((uint32_t)disk_sectors);
        print(")\n");
        return -1;
    }
    if (storage_write_sector(dev, (uint32_t)lba, sector) != 0) {
        print("install: GPT write failed at LBA ");
        gpt_print_u32((uint32_t)lba);
        print("\n");
        return -1;
    }
    return 0;
}

static void gpt_fill_protective_mbr(uint8_t mbr[512], uint64_t disk_sectors) {
    uint32_t size_lba;
    memset(mbr, 0, 512);
    mbr[446] = 0x00; /* non-bootable */
    mbr[447] = 0x00;
    mbr[448] = 0x02;
    mbr[449] = 0x00;
    mbr[450] = 0xEE; /* GPT protective */
    mbr[451] = 0xFF;
    mbr[452] = 0xFF;
    mbr[453] = 0xFF;
    gpt_wle32(mbr + 454, 1U); /* start LBA */
    if (disk_sectors > 0x100000000ULL)
        size_lba = 0xFFFFFFFFU;
    else if (disk_sectors > 1)
        size_lba = (uint32_t)(disk_sectors - 1U);
    else
        size_lba = 0;
    gpt_wle32(mbr + 458, size_lba);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
}

static void gpt_fill_entry(uint8_t entry[128], uint64_t first_lba, uint64_t last_lba) {
    int i;
    memset(entry, 0, 128);
    for (i = 0; i < 16; i++) entry[i] = k_esp_type_guid[i];
    for (i = 0; i < 16; i++) entry[16 + i] = k_esp_unique_guid[i];
    gpt_wle64(entry + 32, first_lba);
    gpt_wle64(entry + 40, last_lba);
    /* Attribute bit 0 = Required Partition (helps some UEFI boot managers). */
    gpt_wle64(entry + 48, 1ULL);
    /* UTF-16LE name "GooberOS" */
    {
        static const char name[] = "GooberOS";
        for (i = 0; name[i] && i < 35; i++) {
            entry[56 + i * 2] = (uint8_t)name[i];
            entry[57 + i * 2] = 0;
        }
    }
}

static void gpt_fill_header(uint8_t hdr[512],
                            uint64_t current_lba,
                            uint64_t alternate_lba,
                            uint64_t first_usable,
                            uint64_t last_usable,
                            uint64_t entries_lba,
                            uint32_t entries_crc) {
    memset(hdr, 0, 512);
    memcpy(hdr, "EFI PART", 8);
    gpt_wle32(hdr + 8, 0x00010000U); /* revision 1.0 */
    gpt_wle32(hdr + 12, 92U);        /* header size */
    gpt_wle32(hdr + 16, 0);          /* crc32 placeholder */
    gpt_wle64(hdr + 24, current_lba);
    gpt_wle64(hdr + 32, alternate_lba);
    gpt_wle64(hdr + 40, first_usable);
    gpt_wle64(hdr + 48, last_usable);
    for (int i = 0; i < 16; i++) hdr[56 + i] = k_disk_guid[i];
    gpt_wle64(hdr + 72, entries_lba);
    gpt_wle32(hdr + 80, 128U);       /* number of entries */
    gpt_wle32(hdr + 84, 128U);       /* size of entry */
    gpt_wle32(hdr + 88, entries_crc);
    {
        uint32_t crc = gpt_crc32(hdr, 92);
        gpt_wle32(hdr + 16, crc);
    }
}

/* Protective MBR + primary/backup GPT; clear LBA 34..(part_start-1). */
static int gpt_write_tables(const storage_device_info_t* dev,
                            uint64_t disk_sectors,
                            uint32_t part_start_lba,
                            uint32_t* out_part_sectors) {
    uint8_t mbr[512];
    uint8_t hdr[512];
    uint8_t entries[512 * 32];
    uint8_t zero[512];
    uint64_t alternate_lba;
    uint64_t first_usable = 34;
    uint64_t last_usable;
    uint64_t part_first;
    uint64_t part_last;
    uint64_t backup_entries_lba;
    uint32_t entries_crc;
    uint32_t i;

    if (!dev) return -1;
    if (disk_sectors == 0) {
        print("install: GPT refused: disk sector count is 0\n");
        return -1;
    }
    if (disk_sectors < 2048U + 34U + 34U) {
        print("install: GPT refused: disk too small (sectors=");
        gpt_print_u32((uint32_t)disk_sectors);
        print(")\n");
        return -1;
    }
    if (part_start_lba < 34U) return -1;

    alternate_lba = disk_sectors - 1U;
    backup_entries_lba = alternate_lba - 32U;
    last_usable = backup_entries_lba - 1U;
    if (last_usable < part_start_lba) {
        print("install: GPT refused: no room for ESP (last_usable < part_start)\n");
        return -1;
    }

    part_first = part_start_lba;
    part_last = last_usable;
    if (out_part_sectors)
        *out_part_sectors = (uint32_t)(part_last - part_first + 1U);

    memset(entries, 0, sizeof(entries));
    gpt_fill_entry(entries, part_first, part_last);
    entries_crc = gpt_crc32(entries, 128U * 128U);

    print("install: writing GPT...\n");

    gpt_fill_protective_mbr(mbr, disk_sectors);
    if (gpt_write_sector(dev, disk_sectors, 0, mbr) != 0) return -1;

    /* Primary partition entries at LBA 2..33 */
    for (i = 0; i < 32U; i++) {
        if (gpt_write_sector(dev, disk_sectors, 2U + i, entries + (i * 512U)) != 0)
            return -1;
    }

    gpt_fill_header(hdr, 1, alternate_lba, first_usable, last_usable, 2, entries_crc);
    if (gpt_write_sector(dev, disk_sectors, 1, hdr) != 0) return -1;

    /* Backup entries then backup header */
    for (i = 0; i < 32U; i++) {
        if (gpt_write_sector(dev, disk_sectors, backup_entries_lba + i,
                             entries + (i * 512U)) != 0)
            return -1;
    }
    gpt_fill_header(hdr, alternate_lba, 1, first_usable, last_usable,
                    backup_entries_lba, entries_crc);
    if (gpt_write_sector(dev, disk_sectors, alternate_lba, hdr) != 0) return -1;

    memset(zero, 0, sizeof(zero));
    for (i = 34U; i < part_start_lba && i < 2048U; i++) {
        if (gpt_write_sector(dev, disk_sectors, i, zero) != 0) return -1;
    }

    return 0;
}

int gpt_write_esp_layout(const storage_device_info_t* dev,
                         uint64_t disk_sectors,
                         uint32_t part_start_lba,
                         uint32_t* out_part_sectors) {
    return gpt_write_tables(dev, disk_sectors, part_start_lba, out_part_sectors);
}
