#include "partition.h"
#include "../../lib/string.h"

static uint16_t part_read_le16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t part_read_le32(const uint8_t* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int part_guid_equal_basic_data(const uint8_t* g) {
    static const uint8_t basic_data[16] = {
        0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
        0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
    };
    for (int i = 0; i < 16; i++) {
        if (g[i] != basic_data[i]) return 0;
    }
    return 1;
}

static int device_has_gpt(const storage_device_info_t* dev) {
    uint8_t sector0[512];
    int i;

    if (!dev || storage_read_sector(dev, 0, sector0) != 0) return 0;
    if (sector0[510] != 0x55 || sector0[511] != 0xAA) return 0;
    for (i = 0; i < 4; i++) {
        const uint8_t* entry = sector0 + 446 + (i * 16);
        if (entry[4] == 0xEE) return 1;
    }
    return 0;
}

static int load_gpt_entries(const storage_device_info_t* dev,
                            partition_info_t* out, int max_out, int* out_count) {
    uint8_t sector[512];
    uint8_t entry_sector[512];
    uint32_t entry_lba;
    uint32_t entry_count;
    uint32_t entry_size;
    int count = 0;
    int i;

    if (storage_read_sector(dev, 1, sector) != 0) return -1;
    if (strncmp((const char*)sector, "EFI PART", 8) != 0) return -1;

    entry_lba = part_read_le32(sector + 72);
    entry_count = part_read_le32(sector + 80);
    entry_size = part_read_le32(sector + 84);
    if (entry_size < 128) return -1;

    for (i = 0; i < (int)entry_count && count < max_out; i++) {
        uint32_t byte_offset = (uint32_t)i * entry_size;
        uint32_t sector_lba = entry_lba + (byte_offset / 512U);
        uint32_t sector_offset = byte_offset % 512U;
        const uint8_t* entry;
        uint64_t first_lba;
        uint64_t last_lba;
        int name_pos = 0;

        if (storage_read_sector(dev, sector_lba, entry_sector) != 0) return -1;
        entry = entry_sector + sector_offset;
        if (entry[0] == 0 && entry[1] == 0 && entry[2] == 0 && entry[3] == 0 &&
            entry[4] == 0 && entry[5] == 0 && entry[6] == 0 && entry[7] == 0) {
            continue;
        }

        first_lba = (uint64_t)part_read_le32(entry + 32) |
                      ((uint64_t)part_read_le32(entry + 36) << 32);
        last_lba = (uint64_t)part_read_le32(entry + 40) |
                     ((uint64_t)part_read_le32(entry + 44) << 32);
        if (last_lba < first_lba) continue;

        out[count].present = 1;
        out[count].gpt = 1;
        out[count].bootable = 0;
        out[count].mbr_type = part_guid_equal_basic_data(entry) ? 0x0C : 0;
        out[count].start_lba = first_lba;
        out[count].sector_count = last_lba - first_lba + 1;
        out[count].name[0] = '\0';
        for (int n = 0; n < 36; n++) {
            uint16_t ch = part_read_le16(entry + 56 + (n * 2));
            if (ch == 0) break;
            if (name_pos < (int)sizeof(out[count].name) - 1) {
                out[count].name[name_pos++] =
                    (ch >= 32 && ch < 127) ? (char)ch : '?';
            }
        }
        out[count].name[name_pos] = '\0';
        count++;
    }

    *out_count = count;
    return 0;
}

static int load_mbr_entries(const storage_device_info_t* dev,
                            partition_info_t* out, int max_out, int* out_count) {
    uint8_t sector0[512];
    int count = 0;

    if (storage_read_sector(dev, 0, sector0) != 0) return -1;
    if (sector0[510] != 0x55 || sector0[511] != 0xAA) return -1;

    for (int i = 0; i < 4 && count < max_out; i++) {
        const uint8_t* entry = sector0 + 446 + (i * 16);
        uint8_t type = entry[4];
        uint32_t start = part_read_le32(entry + 8);
        uint32_t sectors = part_read_le32(entry + 12);
        if (type == 0 || sectors == 0) continue;

        out[count].present = 1;
        out[count].gpt = 0;
        out[count].bootable = (entry[0] == 0x80) ? 1 : 0;
        out[count].mbr_type = type;
        out[count].start_lba = start;
        out[count].sector_count = sectors;
        out[count].name[0] = '\0';
        count++;
    }

    *out_count = count;
    return 0;
}

static int partition_load_table(const storage_device_info_t* dev,
                                partition_info_t* table, int* out_count) {
    if (!dev || !table || !out_count) return -1;
    *out_count = 0;

    if (device_has_gpt(dev)) {
        return load_gpt_entries(dev, table, PARTITION_MAX_ENTRIES, out_count);
    }
    return load_mbr_entries(dev, table, PARTITION_MAX_ENTRIES, out_count);
}

int partition_count(const storage_device_info_t* dev) {
    partition_info_t table[PARTITION_MAX_ENTRIES];
    int count = 0;
    if (partition_load_table(dev, table, &count) != 0) return 0;
    return count;
}

int partition_get_info(const storage_device_info_t* dev, int index,
                       partition_info_t* out) {
    partition_info_t table[PARTITION_MAX_ENTRIES];
    int count = 0;

    if (!out || index < 0) return -1;
    if (partition_load_table(dev, table, &count) != 0) return -1;
    if (index >= count) return -1;
    *out = table[index];
    return 0;
}

int partition_read_sector(const storage_device_info_t* dev, int part_index,
                          uint32_t rel_lba, void* out_sector) {
    partition_info_t info;

    if (!dev || !out_sector) return -1;
    if (partition_get_info(dev, part_index, &info) != 0) return -1;
    if (info.start_lba + (uint64_t)rel_lba > 0xFFFFFFFFU) return -1;
    return storage_read_sector(dev, (uint32_t)(info.start_lba + rel_lba), out_sector);
}

int partition_write_sector(const storage_device_info_t* dev, int part_index,
                           uint32_t rel_lba, const void* in_sector) {
    partition_info_t info;

    if (!dev || !in_sector) return -1;
    if (partition_get_info(dev, part_index, &info) != 0) return -1;
    if (info.start_lba + (uint64_t)rel_lba > 0xFFFFFFFFU) return -1;
    return storage_write_sector(dev, (uint32_t)(info.start_lba + rel_lba), in_sector);
}

const char* partition_type_name(uint8_t mbr_type) {
    switch (mbr_type) {
        case 0x00: return "Unused";
        case 0x07: return "NTFS/exFAT/HPFS";
        case 0x0B:
        case 0x0C: return "FAT32";
        case 0x82: return "Linux swap";
        case 0x83: return "Linux";
        case 0xEE: return "GPT protective";
        case 0xEF: return "EFI system";
        case 0xAF: return "Apple HFS+";
        default:   return "Unknown";
    }
}

void partition_print_table(const storage_device_info_t* dev,
                           void (*emit)(const char*)) {
    partition_info_t table[PARTITION_MAX_ENTRIES];
    int count = 0;
    char buf[16];

    if (!dev || !emit) return;
    if (partition_load_table(dev, table, &count) != 0) {
        emit("partition: failed to read partition table\n");
        return;
    }

    if (device_has_gpt(dev)) {
        emit("GPT partitions:\n");
    } else {
        emit("MBR partitions:\n");
    }

    if (count == 0) {
        emit("  No populated entries.\n");
        return;
    }

    for (int i = 0; i < count; i++) {
        emit("  [");
        itoa(i, buf, 10);
        emit(buf);
        emit("] ");
        if (table[i].gpt) {
            emit("GPT");
            if (table[i].name[0]) {
                emit(" name ");
                emit(table[i].name);
            }
        } else {
            emit("type 0x");
            itoa((int)table[i].mbr_type, buf, 16);
            emit(buf);
            emit(" ");
            emit(partition_type_name(table[i].mbr_type));
            if (table[i].bootable) emit(", bootable");
        }
        emit(", start ");
        itoa((int)table[i].start_lba, buf, 10);
        emit(buf);
        emit(", sectors ");
        itoa((int)table[i].sector_count, buf, 10);
        emit(buf);
        emit("\n");
    }
}
