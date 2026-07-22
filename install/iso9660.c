#include "iso9660.h"
#include "../lib/memory.h"
#include "../lib/string.h"

#define ISO_SECTOR_SIZE 2048U

enum {
    ISO_ERR_NONE = 0,
    ISO_ERR_NO_OPTICAL = 1,
    ISO_ERR_READ_PVD = 2,
    ISO_ERR_BAD_PVD = 3,
    ISO_ERR_NO_TEMPLATE = 4
};

static int g_iso_last_err = ISO_ERR_NONE;

const char* iso9660_last_error(void) {
    switch (g_iso_last_err) {
    case ISO_ERR_NO_OPTICAL:
        return "no ATAPI optical and no GRUB module2 FAT_PART.IMG (USB live needs module2)";
    case ISO_ERR_READ_PVD:
        return "optical read failed at ISO sector 16 (volume descriptor)";
    case ISO_ERR_BAD_PVD:
        return "sector 16 is not a valid ISO9660 primary volume descriptor";
    case ISO_ERR_NO_TEMPLATE:
        return "BOOT/INSTALL/FAT_PAR0.IMG not found on the live ISO";
    default:
        return "unknown ISO9660 error";
    }
}

static uint32_t iso_le32(const uint8_t* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int iso_name_equal(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb) return 0;
    }
    return *a == *b;
}

static int iso_read_sector(const storage_device_info_t* dev, uint32_t lba, void* out) {
    return storage_read_optical_sector(dev, lba, out);
}

static int iso_match_dirent_name(const uint8_t* rec, const char* want) {
    uint8_t len = rec[0];
    uint8_t name_len;
    const char* name;
    char scratch[64];
    int i;
    int si = 0;

    if (len < 33) return 0;
    name_len = rec[32];
    if (name_len == 0) return 0;
    name = (const char*)(rec + 33);
    for (i = 0; i < name_len && si < (int)sizeof(scratch) - 1; i++) {
        if (name[i] == ';') break;
        scratch[si++] = name[i];
    }
    scratch[si] = '\0';
    return iso_name_equal(scratch, want);
}

static int iso_find_in_dir(const storage_device_info_t* dev,
                           uint32_t dir_lba,
                           uint32_t dir_size,
                           const char* name,
                           uint32_t* out_lba,
                           uint32_t* out_size,
                           int want_dir) {
    uint8_t sector[ISO_SECTOR_SIZE];
    uint32_t bytes_seen = 0;
    uint32_t cur_lba = dir_lba;

    while (bytes_seen < dir_size) {
        uint32_t off;
        if (iso_read_sector(dev, cur_lba, sector) != 0) return -1;
        for (off = 0; off < ISO_SECTOR_SIZE && bytes_seen < dir_size; ) {
            const uint8_t* rec = sector + off;
            uint8_t rec_len = rec[0];
            if (rec_len == 0) {
                off = ISO_SECTOR_SIZE;
                break;
            }
            if (iso_match_dirent_name(rec, name)) {
                uint8_t flags = rec[25];
                if (want_dir && !(flags & 0x02)) return -1;
                if (!want_dir && (flags & 0x02)) return -1;
                if (out_lba) *out_lba = iso_le32(rec + 2);
                if (out_size) *out_size = iso_le32(rec + 10);
                return 0;
            }
            off += rec_len;
            bytes_seen += rec_len;
        }
        cur_lba++;
    }
    return -1;
}

static int iso_walk_path(const storage_device_info_t* dev,
                         uint32_t root_lba,
                         uint32_t root_size,
                         const char* path,
                         uint32_t* out_lba,
                         uint32_t* out_size) {
    char segment[64];
    uint32_t dir_lba = root_lba;
    uint32_t dir_size = root_size;
    const char* p = path;

    while (*p == '/') p++;
    while (*p) {
        int si = 0;
        int is_last = 0;
        while (*p && *p != '/') {
            if (si >= (int)sizeof(segment) - 1) return -1;
            segment[si++] = *p++;
        }
        segment[si] = '\0';
        if (*p == '/') {
            p++;
            is_last = 0;
        } else {
            is_last = 1;
        }
        if (segment[0] == '\0') return -1;
        if (is_last) {
            return iso_find_in_dir(dev, dir_lba, dir_size, segment,
                                 out_lba, out_size, 0);
        }
        {
            uint32_t next_lba = 0;
            uint32_t next_size = 0;
            if (iso_find_in_dir(dev, dir_lba, dir_size, segment,
                                &next_lba, &next_size, 1) != 0)
                return -1;
            dir_lba = next_lba;
            dir_size = next_size;
        }
    }
    return -1;
}

static const storage_device_info_t* iso_first_optical(void) {
    int count = storage_count();
    int i;
    for (i = 0; i < count; i++) {
        const storage_device_info_t* dev = storage_get(i);
        if (dev && dev->present && dev->type == STORAGE_TYPE_OPTICAL)
            return dev;
    }
    return NULL;
}

int iso9660_find_fat_template(const storage_device_info_t** dev_out,
                              uint32_t* extent_lba_out,
                              uint32_t* byte_size_out) {
    const storage_device_info_t* dev;
    uint8_t pvd[ISO_SECTOR_SIZE];
    uint32_t root_lba;
    uint32_t root_size;
    uint32_t file_lba = 0;
    uint32_t file_size = 0;
    static const char* paths[] = {
        "boot/INSTALL/FAT_PAR0.IMG",
        "BOOT/INSTALL/FAT_PAR0.IMG",
        "boot/INSTALL/FAT_PART.IMG",
        "BOOT/INSTALL/FAT_PART.IMG",
        "boot/install/FAT_PAR0.IMG",
        "boot/install/FAT_PART.IMG",
        "boot/install/fat-partition.img"
    };
    int pi;

    if (!dev_out || !extent_lba_out || !byte_size_out) return -1;

    g_iso_last_err = ISO_ERR_NONE;
    storage_scan();

    dev = iso_first_optical();
    if (!dev) {
        g_iso_last_err = ISO_ERR_NO_OPTICAL;
        return -1;
    }
    if (iso_read_sector(dev, 16, pvd) != 0) {
        g_iso_last_err = ISO_ERR_READ_PVD;
        return -1;
    }
    if (pvd[0] != 1 || pvd[1] != 'C' || pvd[2] != 'D' || pvd[3] != '0' ||
        pvd[4] != '0' || pvd[5] != '1') {
        g_iso_last_err = ISO_ERR_BAD_PVD;
        return -1;
    }
    root_lba = iso_le32(pvd + 156 + 2);
    root_size = iso_le32(pvd + 156 + 10);
    for (pi = 0; pi < 7; pi++) {
        if (iso_walk_path(dev, root_lba, root_size, paths[pi],
                          &file_lba, &file_size) == 0 &&
            file_size > 0) {
            *dev_out = dev;
            *extent_lba_out = file_lba;
            *byte_size_out = file_size;
            return 0;
        }
    }
    g_iso_last_err = ISO_ERR_NO_TEMPLATE;
    return -1;
}

int iso9660_read_file_sector(const storage_device_info_t* dev,
                             uint32_t extent_lba,
                             uint32_t file_size,
                             uint32_t sector_index,
                             void* sector_out) {
    uint8_t iso_buf[ISO_SECTOR_SIZE];
    uint32_t byte_pos = sector_index * 512U;
    uint32_t iso_lba;
    uint32_t offset;

    if (!dev || !sector_out) return -1;
    if (byte_pos >= file_size) {
        memset(sector_out, 0, 512);
        return 0;
    }

    iso_lba = extent_lba + (byte_pos / ISO_SECTOR_SIZE);
    offset = byte_pos % ISO_SECTOR_SIZE;

    if (iso_read_sector(dev, iso_lba, iso_buf) != 0) return -1;

    if (offset + 512U <= ISO_SECTOR_SIZE) {
        memcpy(sector_out, iso_buf + offset, 512);
        return 0;
    }

    {
        uint8_t iso_buf2[ISO_SECTOR_SIZE];
        size_t first = ISO_SECTOR_SIZE - offset;
        size_t second = 512U - first;
        memcpy(sector_out, iso_buf + offset, first);
        if (iso_read_sector(dev, iso_lba + 1, iso_buf2) != 0) return -1;
        memcpy((uint8_t*)sector_out + first, iso_buf2, second);
    }
    return 0;
}
