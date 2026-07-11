#include "fs_backend.h"
#include "filesystem.h"
#include "../drivers/storage/partition.h"
#include "../drivers/storage/storage.h"
#include "../lib/memory.h"
#include "../lib/string.h"
#include "../kernel.h"

extern void print(const char*);

#define FAT32_MAX_OPEN 8
#define FAT32_CLUSTER_EOF 0x0FFFFFF8U
#define FAT_IO_BUF_SIZE   4096U
#define FAT32_ROOT_FILE_CAP  32
#define FAT32_ROOT_CHILD_CAP 16
#define FAT32_DIR_FILE_CAP   24
#define FAT32_DIR_CHILD_CAP  12
#define FAT32_DIR_POOL_SLOTS 8

/* Reused cluster I/O buffer (x86 bump heap cannot recycle kmalloc). */
static uint8_t g_fat_io_buf[FAT_IO_BUF_SIZE];
static FileEntry g_root_files[FAT32_ROOT_FILE_CAP];
static Directory g_root_children[FAT32_ROOT_CHILD_CAP];

/* Static pools for Desktop and other non-root directories (no kmalloc). */
typedef struct {
    uint32_t fat_cluster; /* 0 = free */
    FileEntry files[FAT32_DIR_FILE_CAP];
    Directory children[FAT32_DIR_CHILD_CAP];
} fat32_dir_pool_t;

static fat32_dir_pool_t g_dir_pools[FAT32_DIR_POOL_SLOTS];
static int g_dir_pool_clock;

typedef struct {
    const storage_device_info_t* dev;
    int device_index;
    int part_index;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t num_fats;
    uint32_t fat_size_sectors;
    uint32_t data_start_lba;
    uint32_t root_cluster;
    uint32_t total_clusters;
    char volume_label[12];
    int mounted;
    uint32_t fat_cache_lba;
    uint8_t fat_cache[512];
    int fat_cache_valid;
    int fat_dirty;
} fat32_vol_t;

static fat32_vol_t g_vol;

static uint8_t* fat_io_buf(void) {
    uint32_t need = g_vol.sectors_per_cluster * g_vol.bytes_per_sector;
    if (need == 0 || need > FAT_IO_BUF_SIZE) return NULL;
    return g_fat_io_buf;
}

static Directory g_root_dir;
static Directory* current_dir = NULL;
static Directory* g_desktop_dir = NULL;
static FileHandle g_handles[FAT32_MAX_OPEN];
static char g_mount_desc[64];

static uint16_t fat_read_le16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t fat_read_le32(const uint8_t* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void fat_write_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static int fat_name_equal(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb) return 0;
    }
    return *a == *b;
}

static uint32_t fat_dent_start_cluster(const uint8_t* e) {
    return ((uint32_t)(e[21] << 8 | e[20]) << 16) |
           (uint32_t)(e[27] << 8 | e[26]);
}

static void fat_dent_set_start_cluster(uint8_t* e, uint32_t cluster) {
    e[20] = (uint8_t)((cluster >> 16) & 0xFF);
    e[21] = (uint8_t)((cluster >> 24) & 0xFF);
    e[26] = (uint8_t)(cluster & 0xFF);
    e[27] = (uint8_t)((cluster >> 8) & 0xFF);
}

static void fat_build_short_name_from_dent(const uint8_t* e, char* out, size_t out_len) {
    int si = 0;
    for (int i = 0; i < 8 && si < (int)out_len - 1; i++) {
        if (e[i] != ' ') out[si++] = (char)e[i];
    }
    if (e[8] != ' ') {
        out[si++] = '.';
        for (int i = 8; i < 11 && si < (int)out_len - 1; i++) {
            if (e[i] != ' ') out[si++] = (char)e[i];
        }
    }
    out[si] = '\0';
}

static int fat_label_matches(const char* label) {
    const char* expect = "GOOBEROS";
    int ei = 0;
    for (int i = 0; i < 11; i++) {
        char c = label[i];
        if (c == ' ' || c == '\0') break;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (expect[ei] == '\0') return 1;
        if (c != expect[ei]) return 0;
        ei++;
    }
    return expect[ei] == '\0';
}

static int fat_part_read(uint32_t rel_lba, void* sector) {
    return partition_read_sector(g_vol.dev, g_vol.part_index, rel_lba, sector);
}

static int fat_part_write(uint32_t rel_lba, const void* sector) {
    return partition_write_sector(g_vol.dev, g_vol.part_index, rel_lba, sector);
}

static uint32_t fat_cluster_to_lba(uint32_t cluster) {
    return g_vol.data_start_lba +
           (cluster - 2U) * g_vol.sectors_per_cluster;
}

static int fat_read_cluster(uint32_t cluster, uint8_t* out) {
    uint32_t lba = fat_cluster_to_lba(cluster);
    uint32_t spc = g_vol.sectors_per_cluster;
    for (uint32_t i = 0; i < spc; i++) {
        if (fat_part_read(lba + i, out + (i * g_vol.bytes_per_sector)) != 0)
            return -1;
    }
    return 0;
}

static int fat_write_cluster(uint32_t cluster, const uint8_t* in) {
    uint32_t lba = fat_cluster_to_lba(cluster);
    uint32_t spc = g_vol.sectors_per_cluster;
    for (uint32_t i = 0; i < spc; i++) {
        if (fat_part_write(lba + i, in + (i * g_vol.bytes_per_sector)) != 0)
            return -1;
    }
    return 0;
}

static int fat_flush_fat_cache(void) {
    if (!g_vol.mounted || !g_vol.fat_cache_valid) return 0;
    if (g_vol.fat_dirty) {
        if (fat_part_write(g_vol.fat_cache_lba, g_vol.fat_cache) != 0) return -1;
        if (g_vol.num_fats > 1) {
            if (fat_part_write(g_vol.fat_cache_lba + g_vol.fat_size_sectors,
                               g_vol.fat_cache) != 0)
                return -1;
        }
        g_vol.fat_dirty = 0;
    }
    return 0;
}

static int fat_load_fat_sector(uint32_t fat_lba) {
    if (g_vol.fat_cache_valid && g_vol.fat_cache_lba == fat_lba) return 0;
    if (fat_flush_fat_cache() != 0) return -1;
    if (fat_part_read(fat_lba, g_vol.fat_cache) != 0) return -1;
    g_vol.fat_cache_lba = fat_lba;
    g_vol.fat_cache_valid = 1;
    return 0;
}

static uint32_t fat_get_entry(uint32_t cluster) {
    uint32_t fat_offset = cluster * 4U;
    uint32_t fat_lba = g_vol.reserved_sectors + (fat_offset / g_vol.bytes_per_sector);
    uint32_t off = fat_offset % g_vol.bytes_per_sector;
    uint32_t entry;
    if (fat_load_fat_sector(fat_lba) != 0) return 0xFFFFFFFFU;
    entry = fat_read_le32(g_vol.fat_cache + off) & 0x0FFFFFFFU;
    return entry;
}

static int fat_set_entry(uint32_t cluster, uint32_t value) {
    uint32_t fat_offset = cluster * 4U;
    uint32_t fat_lba = g_vol.reserved_sectors + (fat_offset / g_vol.bytes_per_sector);
    uint32_t off = fat_offset % g_vol.bytes_per_sector;
    uint32_t cur;
    if (fat_load_fat_sector(fat_lba) != 0) return -1;
    cur = fat_read_le32(g_vol.fat_cache + off) & 0xF0000000U;
    fat_write_le32(g_vol.fat_cache + off, cur | (value & 0x0FFFFFFFU));
    g_vol.fat_dirty = 1;
    return 0;
}

static uint32_t g_next_free_hint = 2;

static uint32_t fat_alloc_cluster(void) {
    uint32_t end = g_vol.total_clusters + 2;
    uint32_t start = g_next_free_hint;
    uint32_t c;

    if (start < 2 || start >= end) start = 2;

    for (c = start; c < end; c++) {
        if (fat_get_entry(c) == 0) {
            if (fat_set_entry(c, FAT32_CLUSTER_EOF) != 0) return 0;
            g_next_free_hint = c + 1;
            return c;
        }
    }
    for (c = 2; c < start; c++) {
        if (fat_get_entry(c) == 0) {
            if (fat_set_entry(c, FAT32_CLUSTER_EOF) != 0) return 0;
            g_next_free_hint = c + 1;
            return c;
        }
    }
    return 0;
}

static uint32_t fat_next_cluster(uint32_t cluster) {
    uint32_t next = fat_get_entry(cluster);
    if (next < 2 || next >= g_vol.total_clusters + 2) return FAT32_CLUSTER_EOF;
    return next;
}

static void fat_free_chain(uint32_t cluster) {
    while (cluster >= 2 && cluster < g_vol.total_clusters + 2) {
        uint32_t next = fat_get_entry(cluster);
        fat_set_entry(cluster, 0);
        if (next >= FAT32_CLUSTER_EOF) break;
        cluster = next;
    }
}

static int fat_parse_bpb(const uint8_t* bpb) {
    uint32_t total_sectors16 = fat_read_le16(bpb + 0x13);
    uint32_t total_sectors32 = fat_read_le32(bpb + 0x20);
    uint32_t total_sectors = total_sectors16 ? total_sectors16 : total_sectors32;
    uint32_t root_cluster;
    uint32_t data_sectors;
    uint32_t root_dir_sectors = 0;

    g_vol.bytes_per_sector = fat_read_le16(bpb + 0x0B);
    g_vol.sectors_per_cluster = bpb[0x0D];
    g_vol.reserved_sectors = fat_read_le16(bpb + 0x0E);
    g_vol.num_fats = bpb[0x10];
    g_vol.fat_size_sectors = fat_read_le32(bpb + 0x24);
    root_cluster = fat_read_le32(bpb + 0x2C);

    if (g_vol.bytes_per_sector != 512) return -1;
    if (g_vol.sectors_per_cluster == 0 || g_vol.num_fats == 0) return -1;
    if (g_vol.fat_size_sectors == 0) return -1;
    if (root_cluster < 2) return -1;

    g_vol.root_cluster = root_cluster;
    g_vol.data_start_lba = g_vol.reserved_sectors + g_vol.num_fats * g_vol.fat_size_sectors +
                           root_dir_sectors;
    data_sectors = total_sectors - g_vol.data_start_lba;
    g_vol.total_clusters = data_sectors / g_vol.sectors_per_cluster;

    for (int i = 0; i < 11; i++) g_vol.volume_label[i] = bpb[0x47 + i];
    g_vol.volume_label[11] = '\0';

    return 0;
}

static int fat_has_goober_signature(void) {
    return fat_label_matches(g_vol.volume_label);
}

static int fat32_mount_device_internal(int device_index, int part_index, int require_sig) {
    uint8_t bpb[512];
    const storage_device_info_t* dev;

    storage_scan();
    dev = storage_get(device_index);
    if (!dev || !dev->present) return -1;
    if (partition_read_sector(dev, part_index, 0, bpb) != 0) return -1;

    memset(&g_vol, 0, sizeof(g_vol));
    g_vol.dev = dev;
    g_vol.device_index = device_index;
    g_vol.part_index = part_index;
    if (fat_parse_bpb(bpb) != 0) return -1;
    if (require_sig && !fat_has_goober_signature()) return -1;

    g_vol.mounted = 1;
    g_vol.fat_cache_valid = 0;
    g_vol.fat_dirty = 0;
    g_desktop_dir = NULL;
    g_next_free_hint = 2;
    memset(g_dir_pools, 0, sizeof(g_dir_pools));
    g_dir_pool_clock = 0;

    for (int i = 0; i < MAX_NAME_LEN; i++) g_root_dir.name[i] = 0;
    g_root_dir.name[0] = '/';
    g_root_dir.name[1] = '\0';
    g_root_dir.files = g_root_files;
    g_root_dir.file_count = 0;
    g_root_dir.files_cap = FAT32_ROOT_FILE_CAP;
    g_root_dir.parent = NULL;
    g_root_dir.children = g_root_children;
    g_root_dir.child_count = 0;
    g_root_dir.children_cap = FAT32_ROOT_CHILD_CAP;
    g_root_dir.fat32 = 1;
    g_root_dir.fat_cluster = g_vol.root_cluster;

    current_dir = &g_root_dir;

    g_mount_desc[0] = '\0';
    {
        char buf[16];
        strcat(g_mount_desc, "fat32:dev");
        itoa(device_index, buf, 10);
        strcat(g_mount_desc, buf);
        strcat(g_mount_desc, ":part");
        itoa(part_index, buf, 10);
        strcat(g_mount_desc, buf);
        if (g_vol.volume_label[0] != ' ' && g_vol.volume_label[0] != '\0') {
            int li;
            strcat(g_mount_desc, " (");
            li = (int)strlen(g_mount_desc);
            for (int vi = 0; vi < 11 && g_vol.volume_label[vi] != ' ' &&
                            g_vol.volume_label[vi] != '\0' &&
                            li < (int)sizeof(g_mount_desc) - 2;
                 vi++, li++) {
                g_mount_desc[li] = g_vol.volume_label[vi];
            }
            g_mount_desc[li] = '\0';
            strcat(g_mount_desc, ")");
        }
    }
    return 0;
}

int fat32_mount_device(int device_index, int part_index) {
    return fat32_mount_device_internal(device_index, part_index, 1);
}

static void fat_clear_dir_cache(Directory* dir) {
    if (!dir) return;
    dir->file_count = 0;
    dir->child_count = 0;
}

static void fat_decode_lfn(const uint8_t* entries, int count, char* out, size_t out_len) {
    int pos = 0;
    for (int seq = count; seq >= 1; seq--) {
        const uint8_t* e = entries + (seq - 1) * 32;
        for (int i = 0; i < 5 && pos < (int)out_len - 1; i++) {
            uint16_t ch = fat_read_le16(e + 1 + i * 2);
            if (ch == 0 || ch == 0xFFFF) continue;
            out[pos++] = (ch < 128) ? (char)ch : '?';
        }
        for (int i = 0; i < 6 && pos < (int)out_len - 1; i++) {
            uint16_t ch = fat_read_le16(e + 14 + i * 2);
            if (ch == 0 || ch == 0xFFFF) continue;
            out[pos++] = (ch < 128) ? (char)ch : '?';
        }
        for (int i = 0; i < 2 && pos < (int)out_len - 1; i++) {
            uint16_t ch = fat_read_le16(e + 28 + i * 2);
            if (ch == 0 || ch == 0xFFFF) continue;
            out[pos++] = (ch < 128) ? (char)ch : '?';
        }
    }
    out[pos] = '\0';
}

static int fat_dir_entry_exists(Directory* dir, const char* name, int want_dir) {
    uint8_t* cluster_buf;
    uint32_t cluster;
    char pending_lfn[MAX_NAME_LEN];
    int pending_lfn_valid = 0;

    if (!dir || !name || !dir->fat32) return 0;
    cluster_buf = fat_io_buf();
    if (!cluster_buf) return 0;

    cluster = dir->fat_cluster;
    while (cluster >= 2 && cluster < g_vol.total_clusters + 2) {
        if (fat_read_cluster(cluster, cluster_buf) != 0) return 0;
        pending_lfn_valid = 0;
        for (uint32_t off = 0; off < g_vol.sectors_per_cluster * 512; off += 32) {
            const uint8_t* e = cluster_buf + off;
            char entry_name[MAX_NAME_LEN];

            if (e[0] == 0x00) return 0;
            if (e[0] == 0xE5) {
                pending_lfn_valid = 0;
                continue;
            }
            if (e[11] == 0x0F) {
                int seq = e[0] & 0x3F;
                if (e[0] & 0x40) {
                    fat_decode_lfn(cluster_buf + off - (uint32_t)(seq - 1) * 32U,
                                   seq, pending_lfn, sizeof(pending_lfn));
                    pending_lfn_valid = 1;
                }
                continue;
            }
            if (e[11] & 0x08) {
                pending_lfn_valid = 0;
                continue;
            }

            if (pending_lfn_valid) {
                strncpy(entry_name, pending_lfn, MAX_NAME_LEN - 1);
                entry_name[MAX_NAME_LEN - 1] = '\0';
                pending_lfn_valid = 0;
            } else {
                fat_build_short_name_from_dent(e, entry_name, sizeof(entry_name));
            }

            if (entry_name[0] == '.' &&
                (entry_name[1] == '\0' ||
                 (entry_name[1] == '.' && entry_name[2] == '\0')))
                continue;

            if (want_dir && !(e[11] & 0x10)) continue;
            if (!want_dir && (e[11] & 0x10)) continue;

            if (fat_name_equal(entry_name, name)) return 1;
        }
        {
            uint32_t next = fat_get_entry(cluster);
            if (next >= FAT32_CLUSTER_EOF) break;
            cluster = next;
        }
    }
    return 0;
}

static int fat_attach_dir_pool(Directory* dir) {
    int i;
    int free_slot = -1;
    int slot;

    if (!dir || dir == &g_root_dir) return 0;
    if (dir->files && dir->files_cap > 0 && dir->children && dir->children_cap > 0)
        return 0;

    for (i = 0; i < FAT32_DIR_POOL_SLOTS; i++) {
        if (g_dir_pools[i].fat_cluster == dir->fat_cluster) {
            dir->files = g_dir_pools[i].files;
            dir->files_cap = FAT32_DIR_FILE_CAP;
            dir->children = g_dir_pools[i].children;
            dir->children_cap = FAT32_DIR_CHILD_CAP;
            return 0;
        }
        if (g_dir_pools[i].fat_cluster == 0 && free_slot < 0)
            free_slot = i;
    }

    if (free_slot >= 0) {
        slot = free_slot;
    } else {
        slot = g_dir_pool_clock % FAT32_DIR_POOL_SLOTS;
        g_dir_pool_clock++;
    }

    g_dir_pools[slot].fat_cluster = dir->fat_cluster;
    memset(g_dir_pools[slot].files, 0, sizeof(g_dir_pools[slot].files));
    memset(g_dir_pools[slot].children, 0, sizeof(g_dir_pools[slot].children));
    dir->files = g_dir_pools[slot].files;
    dir->files_cap = FAT32_DIR_FILE_CAP;
    dir->children = g_dir_pools[slot].children;
    dir->children_cap = FAT32_DIR_CHILD_CAP;
    return 0;
}

static int fat_refresh_dir(Directory* dir) {
    uint8_t* cluster_buf;
    uint32_t cluster;
    FileEntry* files;
    Directory* children;
    size_t file_count = 0;
    size_t child_count = 0;
    size_t file_cap;
    size_t child_cap;

    if (!dir || !dir->fat32) return -1;

    fat_attach_dir_pool(dir);
    fat_clear_dir_cache(dir);
    files = dir->files;
    children = dir->children;
    file_cap = dir->files_cap;
    child_cap = dir->children_cap;
    if (!files || !children || file_cap == 0 || child_cap == 0) return -1;

    cluster_buf = fat_io_buf();
    if (!cluster_buf) return -1;

    cluster = dir->fat_cluster;
    while (cluster >= 2 && cluster < g_vol.total_clusters + 2) {
        char pending_lfn[MAX_NAME_LEN];
        int pending_lfn_valid = 0;

        if (fat_read_cluster(cluster, cluster_buf) != 0) break;
        for (uint32_t off = 0; off < g_vol.sectors_per_cluster * 512; off += 32) {
            const uint8_t* e = cluster_buf + off;
            char entry_name[MAX_NAME_LEN];

            if (e[0] == 0x00) goto done_scan;
            if (e[0] == 0xE5) {
                pending_lfn_valid = 0;
                continue;
            }
            if (e[11] == 0x0F) {
                int seq = e[0] & 0x3F;
                if (e[0] & 0x40) {
                    fat_decode_lfn(cluster_buf + off - (uint32_t)(seq - 1) * 32U,
                                   seq, pending_lfn, sizeof(pending_lfn));
                    pending_lfn_valid = 1;
                }
                continue;
            }
            if (e[11] & 0x08) {
                pending_lfn_valid = 0;
                continue;
            }

            if (pending_lfn_valid) {
                strncpy(entry_name, pending_lfn, MAX_NAME_LEN - 1);
                entry_name[MAX_NAME_LEN - 1] = '\0';
                pending_lfn_valid = 0;
            } else {
                fat_build_short_name_from_dent(e, entry_name, sizeof(entry_name));
            }

            if (entry_name[0] == '.' &&
                (entry_name[1] == '\0' ||
                 (entry_name[1] == '.' && entry_name[2] == '\0')))
                continue;

            if (e[11] & 0x10) {
                if (child_count >= child_cap) goto done_scan;
                for (size_t j = 0; j < MAX_NAME_LEN; j++)
                    children[child_count].name[j] = 0;
                strncpy(children[child_count].name, entry_name, MAX_NAME_LEN - 1);
                children[child_count].name[MAX_NAME_LEN - 1] = '\0';
                children[child_count].files = NULL;
                children[child_count].file_count = 0;
                children[child_count].files_cap = 0;
                children[child_count].parent = dir;
                children[child_count].children = NULL;
                children[child_count].child_count = 0;
                children[child_count].children_cap = 0;
                children[child_count].fat32 = 1;
                children[child_count].fat_cluster = fat_dent_start_cluster(e);
                child_count++;
            } else {
                if (file_count >= file_cap) goto done_scan;
                for (size_t j = 0; j < MAX_NAME_LEN; j++)
                    files[file_count].name[j] = 0;
                strncpy(files[file_count].name, entry_name, MAX_NAME_LEN - 1);
                files[file_count].name[MAX_NAME_LEN - 1] = '\0';
                files[file_count].data = NULL;
                files[file_count].size =
                    (size_t)((uint32_t)e[28] | ((uint32_t)e[29] << 8) |
                              ((uint32_t)e[30] << 16) | ((uint32_t)e[31] << 24));
                files[file_count].owned = 0;
                files[file_count].fat32 = 1;
                files[file_count].fat_cluster = fat_dent_start_cluster(e);
                files[file_count].is_dir = 0;
                file_count++;
            }
        }
        {
            uint32_t next = fat_get_entry(cluster);
            if (next >= FAT32_CLUSTER_EOF) break;
            cluster = next;
        }
    }

done_scan:
    dir->files = files;
    dir->file_count = file_count;
    dir->files_cap = file_cap;
    dir->children = children;
    dir->child_count = child_count;
    dir->children_cap = child_cap;
    return 0;
}

static void fat_to_short_name(const char* name, char out11[11]) {
    int i;
    for (i = 0; i < 11; i++) out11[i] = ' ';
    const char* dot = name;
    while (*dot && *dot != '.') dot++;
    for (i = 0; i < 8 && name[i] && name[i] != '.'; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out11[i] = c;
    }
    if (*dot == '.') {
        dot++;
        for (i = 0; i < 3 && dot[i]; i++) {
            char c = dot[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            out11[8 + i] = c;
        }
    }
}

static int fat_add_dir_entry(Directory* dir, const char* name, uint8_t attr,
                             uint32_t cluster, uint32_t size) {
    uint8_t* cluster_buf;
    uint32_t dir_cluster = dir->fat_cluster;
    char short_name[11];

    fat_to_short_name(name, short_name);
    cluster_buf = fat_io_buf();
    if (!cluster_buf) return -1;

    while (dir_cluster >= 2) {
        if (fat_read_cluster(dir_cluster, cluster_buf) != 0) {
            return -1;
        }
        for (uint32_t off = 0; off < g_vol.sectors_per_cluster * 512; off += 32) {
            uint8_t* e = cluster_buf + off;
            if (e[0] == 0x00 || e[0] == 0xE5) {
                for (int i = 0; i < 11; i++) e[i] = (uint8_t)short_name[i];
                e[11] = attr;
                fat_dent_set_start_cluster(e, cluster);
                fat_write_le32(e + 28, size);
                e[12] = 0;
                e[13] = 0;
                if (fat_write_cluster(dir_cluster, cluster_buf) != 0) {
                    return -1;
                }
                fat_refresh_dir(dir);
                return 0;
            }
        }
        {
            uint32_t next = fat_get_entry(dir_cluster);
            if (next >= FAT32_CLUSTER_EOF) {
                uint32_t new_c = fat_alloc_cluster();
                if (new_c == 0) return -1;
                if (fat_set_entry(dir_cluster, new_c) != 0) return -1;
                memset(cluster_buf, 0, g_vol.sectors_per_cluster * 512);
                if (fat_write_cluster(new_c, cluster_buf) != 0) return -1;
                dir_cluster = new_c;
                continue;
            }
            dir_cluster = next;
        }
    }

    return -1;
}

static int fat_update_dir_entry_size(Directory* dir, const char* name, uint32_t size) {
    uint32_t cluster = dir->fat_cluster;
    uint8_t* cluster_buf = fat_io_buf();
    char pending_lfn[MAX_NAME_LEN];
    int pending_lfn_valid = 0;

    if (!cluster_buf) return -1;

    while (cluster >= 2) {
        if (fat_read_cluster(cluster, cluster_buf) != 0) return -1;
        pending_lfn_valid = 0;
        for (uint32_t off = 0; off < g_vol.sectors_per_cluster * 512; off += 32) {
            uint8_t* e = cluster_buf + off;
            char entry_name[MAX_NAME_LEN];

            if (e[0] == 0x00) return -1;
            if (e[0] == 0xE5) {
                pending_lfn_valid = 0;
                continue;
            }
            if (e[11] == 0x0F) {
                int seq = e[0] & 0x3F;
                if (e[0] & 0x40) {
                    fat_decode_lfn(cluster_buf + off - (uint32_t)(seq - 1) * 32U,
                                   seq, pending_lfn, sizeof(pending_lfn));
                    pending_lfn_valid = 1;
                }
                continue;
            }
            if (e[11] & 0x08) {
                pending_lfn_valid = 0;
                continue;
            }

            if (pending_lfn_valid) {
                strncpy(entry_name, pending_lfn, MAX_NAME_LEN - 1);
                entry_name[MAX_NAME_LEN - 1] = '\0';
                pending_lfn_valid = 0;
            } else {
                fat_build_short_name_from_dent(e, entry_name, sizeof(entry_name));
            }

            if (fat_name_equal(entry_name, name)) {
                fat_write_le32(e + 28, size);
                return fat_write_cluster(cluster, cluster_buf);
            }
        }
        {
            uint32_t next = fat_get_entry(cluster);
            if (next >= FAT32_CLUSTER_EOF) break;
            cluster = next;
        }
    }
    return -1;
}

static int fat_remove_dir_entry(Directory* dir, const char* name) {
    uint32_t cluster = dir->fat_cluster;
    uint8_t* cluster_buf = fat_io_buf();
    char pending_lfn[MAX_NAME_LEN];
    int pending_lfn_valid = 0;

    if (!cluster_buf) return -1;

    while (cluster >= 2) {
        if (fat_read_cluster(cluster, cluster_buf) != 0) break;
        pending_lfn_valid = 0;
        for (uint32_t off = 0; off < g_vol.sectors_per_cluster * 512; off += 32) {
            uint8_t* e = cluster_buf + off;
            char entry_name[MAX_NAME_LEN];

            if (e[0] == 0x00) return -1;
            if (e[0] == 0xE5) {
                pending_lfn_valid = 0;
                continue;
            }
            if (e[11] == 0x0F) {
                int seq = e[0] & 0x3F;
                if (e[0] & 0x40) {
                    fat_decode_lfn(cluster_buf + off - (uint32_t)(seq - 1) * 32U,
                                   seq, pending_lfn, sizeof(pending_lfn));
                    pending_lfn_valid = 1;
                }
                continue;
            }
            if (e[11] & 0x08) {
                pending_lfn_valid = 0;
                continue;
            }

            if (pending_lfn_valid) {
                strncpy(entry_name, pending_lfn, MAX_NAME_LEN - 1);
                entry_name[MAX_NAME_LEN - 1] = '\0';
                pending_lfn_valid = 0;
            } else {
                fat_build_short_name_from_dent(e, entry_name, sizeof(entry_name));
            }

            if (fat_name_equal(entry_name, name)) {
                e[0] = 0xE5;
                if (fat_write_cluster(cluster, cluster_buf) != 0) {
                    return -1;
                }
                fat_refresh_dir(dir);
                return 0;
            }
        }
        {
            uint32_t next = fat_get_entry(cluster);
            if (next >= FAT32_CLUSTER_EOF) break;
            cluster = next;
        }
    }
    return -1;
}

static int fat_write_file_clusters(uint32_t first_cluster, const uint8_t* data, size_t size) {
    uint8_t* cluster_buf;
    uint32_t cluster = first_cluster;
    size_t written = 0;
    size_t csize = g_vol.sectors_per_cluster * g_vol.bytes_per_sector;

    if (first_cluster == 0) return -1;
    cluster_buf = fat_io_buf();
    if (!cluster_buf) return -1;

    while (written < size || (written == 0 && size == 0)) {
        size_t chunk = size - written;
        if (chunk > csize) chunk = csize;
        memset(cluster_buf, 0, csize);
        if (chunk > 0) memcpy(cluster_buf, data + written, chunk);
        if (fat_write_cluster(cluster, cluster_buf) != 0) {
            return -1;
        }
        written += chunk;
        if (written >= size) break;
        {
            uint32_t next = fat_get_entry(cluster);
            if (next >= FAT32_CLUSTER_EOF) {
                next = fat_alloc_cluster();
                if (next == 0) return -1;
                if (fat_set_entry(cluster, next) != 0) return -1;
                cluster = next;
            } else {
                cluster = next;
            }
        }
    }

    {
        uint32_t next = fat_get_entry(cluster);
        while (next < FAT32_CLUSTER_EOF && next >= 2) {
            uint32_t follow = fat_get_entry(next);
            fat_set_entry(next, 0);
            next = follow;
        }
        fat_set_entry(cluster, FAT32_CLUSTER_EOF);
    }

    return 0;
}

int fat32_try_auto_mount(const char* root_cfg) {
    int dev_count;
    int i;

    if (root_cfg && root_cfg[0] && strcmp(root_cfg, "live") == 0) return -1;

    if (root_cfg && root_cfg[0] && strcmp(root_cfg, "auto") != 0) {
        int dev_idx = -1;
        int part_idx = -1;
        const char* colon = root_cfg;
        while (*colon && *colon != ':') colon++;
        if (*colon == ':') {
            char dev_buf[8];
            size_t n = (size_t)(colon - root_cfg);
            if (n >= sizeof(dev_buf)) return -1;
            memcpy(dev_buf, root_cfg, n);
            dev_buf[n] = '\0';
            dev_idx = atoi(dev_buf);
            part_idx = atoi(colon + 1);
            if (fat32_mount_device_internal(dev_idx, part_idx, 0) == 0) return 0;
        }
        return -1;
    }

    storage_scan();
    dev_count = storage_count();
    for (i = 0; i < dev_count && i < 8; i++) {
        const storage_device_info_t* dev = storage_get(i);
        int pcount;
        int p;
        if (!dev || !dev->present) continue;
        if (dev->install_state != STORAGE_INSTALL_STATE_READY) continue;
        pcount = partition_count(dev);
        if (pcount > 16) pcount = 16;
        for (p = 0; p < pcount; p++) {
            if (fat32_mount_device(i, p) == 0) return 0;
            memset(&g_vol, 0, sizeof(g_vol));
        }
    }
    return -1;
}

int fat32_is_mounted(void) {
    return g_vol.mounted;
}

void fat32_init_cwd(void) {
    if (g_vol.mounted) current_dir = &g_root_dir;
}

const char* fat32_mount_label(void) {
    return g_vol.volume_label;
}

int fat32_mount_device_index(void) {
    return g_vol.device_index;
}

int fat32_mount_part_index(void) {
    return g_vol.part_index;
}

FileHandle* fat32_dir_open(Directory* dir, const char* filename) {
    if (!dir || !filename) return NULL;
    fat_refresh_dir(dir);
    for (size_t i = 0; i < dir->file_count; i++) {
        if (fat_name_equal(filename, dir->files[i].name)) {
            for (int h = 0; h < FAT32_MAX_OPEN; h++) {
                if (!g_handles[h].used) {
                    g_handles[h].used = 1;
                    g_handles[h].entry = &dir->files[i];
                    g_handles[h].offset = 0;
                    g_handles[h].fat_cluster = dir->files[i].fat_cluster;
                    g_handles[h].fat_cluster_base = 0;
                    return &g_handles[h];
                }
            }
            return NULL;
        }
    }
    return NULL;
}

size_t fat32_read(FileHandle* fh, uint8_t* buffer, size_t bytes) {
    size_t csize = g_vol.sectors_per_cluster * g_vol.bytes_per_sector;
    uint8_t* cluster_buf;
    size_t total = 0;

    if (!fh || !fh->entry || !buffer) return 0;
    if (fh->offset >= fh->entry->size) return 0;

    cluster_buf = fat_io_buf();
    if (!cluster_buf) return 0;

    while (total < bytes && fh->offset < fh->entry->size) {
        uint32_t cluster = fh->fat_cluster;
        size_t cluster_off = fh->offset - fh->fat_cluster_base;
        size_t avail_in_cluster = csize - cluster_off;
        size_t remain = fh->entry->size - fh->offset;
        size_t to_read = bytes - total;
        if (to_read > avail_in_cluster) to_read = avail_in_cluster;
        if (to_read > remain) to_read = remain;

        if (fat_read_cluster(cluster, cluster_buf) != 0) break;
        memcpy(buffer + total, cluster_buf + cluster_off, to_read);
        fh->offset += to_read;
        total += to_read;

        if (cluster_off + to_read >= csize) {
            uint32_t next = fat_get_entry(cluster);
            if (next >= FAT32_CLUSTER_EOF) break;
            fh->fat_cluster = next;
            fh->fat_cluster_base += csize;
        }
    }

    return total;
}

void fat32_close(FileHandle* fh) {
    if (!fh) return;
    fh->used = 0;
    fh->entry = NULL;
    fh->offset = 0;
    fh->fat_cluster = 0;
    fh->fat_cluster_base = 0;
}

int fat32_list(void) {
    fat_refresh_dir(current_dir);
    print("Contents of directory: ");
    print(current_dir->name);
    print("\n");
    for (size_t i = 0; i < current_dir->file_count; i++) {
        print(current_dir->files[i].name);
        print("\n");
    }
    for (size_t i = 0; i < current_dir->child_count; i++) {
        print("<DIR> ");
        print(current_dir->children[i].name);
        print("\n");
    }
    if (current_dir->file_count == 0 && current_dir->child_count == 0) {
        print("(empty)\n");
        if (current_dir != &g_root_dir)
            print("hint: try 'cd /' then 'ls' for boot/, EFI/, Desktop/\n");
    }
    return 0;
}

static Directory* fat_find_child(Directory* dir, const char* name) {
    fat_refresh_dir(dir);
    for (size_t i = 0; i < dir->child_count; i++) {
        if (fat_name_equal(name, dir->children[i].name)) return &dir->children[i];
    }
    return NULL;
}

int fat32_change_dir(const char* path) {
    const char* seg;
    const char* next;
    char segment[MAX_NAME_LEN];

    if (!path || path[0] == '\0') return -1;
    if (path[0] == '/' && path[1] == '\0') {
        current_dir = &g_root_dir;
        return 0;
    }

    seg = path;
    if (seg[0] == '/') seg++;

    while (*seg) {
        next = seg;
        while (*next && *next != '/') next++;
        {
            size_t len = (size_t)(next - seg);
            if (len >= MAX_NAME_LEN) return -1;
            memcpy(segment, seg, len);
            segment[len] = '\0';
        }

        if (fat_name_equal(segment, "..")) {
            if (current_dir->parent) current_dir = current_dir->parent;
            else return -1;
        } else {
            Directory* found = fat_find_child(current_dir, segment);
            if (!found) {
                print("cd: Directory not found: ");
                print(segment);
                print("\n");
                return -1;
            }
            current_dir = found;
        }

        seg = (*next == '/') ? next + 1 : next;
    }
    return 0;
}

int fat32_cd_up(void) {
    if (!current_dir || !current_dir->parent) return -1;
    current_dir = current_dir->parent;
    return 0;
}

int fat32_dir_create(Directory* dir, const char* filename) {
    uint32_t cluster;
    if (!dir || !filename) return -1;
    if (fat_dir_entry_exists(dir, filename, 0)) return -1;
    fat_refresh_dir(dir);
    for (size_t i = 0; i < dir->file_count; i++) {
        if (fat_name_equal(filename, dir->files[i].name)) return -1;
    }
    cluster = fat_alloc_cluster();
    if (cluster == 0) return -1;
    {
        uint8_t* z = fat_io_buf();
        if (!z) return -1;
        memset(z, 0, g_vol.sectors_per_cluster * 512);
        if (fat_write_cluster(cluster, z) != 0) return -1;
    }
    if (fat_add_dir_entry(dir, filename, 0x20, cluster, 0) != 0) return -1;
    return fat32_sync();
}

int fat32_create(const char* filename) {
    return fat32_dir_create(current_dir, filename);
}

int fat32_dir_write(Directory* dir, const char* filename,
                    const uint8_t* data, size_t size) {
    FileEntry* target = NULL;
    uint32_t cluster;

    if (!dir || !filename) return -1;
    fat_refresh_dir(dir);
    for (size_t i = 0; i < dir->file_count; i++) {
        if (fat_name_equal(filename, dir->files[i].name)) {
            target = &dir->files[i];
            break;
        }
    }

    if (target) {
        fat_free_chain(target->fat_cluster);
        cluster = fat_alloc_cluster();
        if (cluster == 0) return -1;
        target->fat_cluster = cluster;
    } else {
        cluster = fat_alloc_cluster();
        if (cluster == 0) return -1;
        if (fat_add_dir_entry(dir, filename, 0x20, cluster, (uint32_t)size) != 0)
            return -1;
        fat_refresh_dir(dir);
        for (size_t i = 0; i < dir->file_count; i++) {
            if (fat_name_equal(filename, dir->files[i].name)) {
                target = &dir->files[i];
                break;
            }
        }
    }

    if (fat_write_file_clusters(cluster, data, size) != 0) return -1;
    if (target) {
        if (fat_update_dir_entry_size(dir, filename, (uint32_t)size) != 0) return -1;
        target->size = size;
    }
    return fat32_sync();
}

int fat32_write(const char* filename, const uint8_t* data, size_t size) {
    return fat32_dir_write(current_dir, filename, data, size);
}

int fat32_delete(const char* filename) {
    FileEntry* target = NULL;
    if (!filename) return -1;
    fat_refresh_dir(current_dir);
    for (size_t i = 0; i < current_dir->file_count; i++) {
        if (fat_name_equal(filename, current_dir->files[i].name)) {
            target = &current_dir->files[i];
            break;
        }
    }
    if (!target) return -1;
    fat_free_chain(target->fat_cluster);
    return fat_remove_dir_entry(current_dir, filename);
}

static int fat_write_dot_entries(uint32_t self_cluster, uint32_t parent_cluster) {
    uint8_t* z = fat_io_buf();
    uint8_t* e;
    int i;

    if (!z) return -1;
    memset(z, 0, g_vol.sectors_per_cluster * 512);

    e = z;
    for (i = 0; i < 11; i++) e[i] = ' ';
    e[0] = '.';
    e[11] = 0x10;
    fat_dent_set_start_cluster(e, self_cluster);

    e = z + 32;
    for (i = 0; i < 11; i++) e[i] = ' ';
    e[0] = '.';
    e[1] = '.';
    e[11] = 0x10;
    fat_dent_set_start_cluster(e, parent_cluster);

    return fat_write_cluster(self_cluster, z);
}

int fat32_dir_create_dir(Directory* dir, const char* dirname) {
    uint32_t cluster;
    if (!dir || !dirname) return -1;
    if (fat_dir_entry_exists(dir, dirname, 1)) return -1;
    fat_refresh_dir(dir);
    for (size_t i = 0; i < dir->child_count; i++) {
        if (fat_name_equal(dirname, dir->children[i].name)) return -1;
    }
    cluster = fat_alloc_cluster();
    if (cluster == 0) return -1;
    if (fat_write_dot_entries(cluster, dir->fat_cluster) != 0) return -1;
    if (fat_add_dir_entry(dir, dirname, 0x10, cluster, 0) != 0) return -1;
    return fat32_sync();
}

int fat32_create_dir(const char* dirname) {
    return fat32_dir_create_dir(current_dir, dirname);
}

int fat32_dir_rename(Directory* dir, const char* old_name, const char* new_name) {
    uint8_t* cluster_buf;
    uint32_t cluster;
    char short_name[11];
    char pending_lfn[MAX_NAME_LEN];
    int pending_lfn_valid = 0;

    if (!dir || !old_name || !new_name || !old_name[0] || !new_name[0]) return -1;
    if (fat_name_equal(old_name, new_name)) return 0;
    if (fat_dir_entry_exists(dir, new_name, 0) || fat_dir_entry_exists(dir, new_name, 1))
        return -1;

    fat_to_short_name(new_name, short_name);
    cluster_buf = fat_io_buf();
    if (!cluster_buf) return -1;

    cluster = dir->fat_cluster;
    while (cluster >= 2 && cluster < g_vol.total_clusters + 2) {
        if (fat_read_cluster(cluster, cluster_buf) != 0) return -1;
        pending_lfn_valid = 0;
        for (uint32_t off = 0; off < g_vol.sectors_per_cluster * 512; off += 32) {
            uint8_t* e = cluster_buf + off;
            char entry_name[MAX_NAME_LEN];
            int i;

            if (e[0] == 0x00) return -1;
            if (e[0] == 0xE5) {
                pending_lfn_valid = 0;
                continue;
            }
            if (e[11] == 0x0F) {
                int seq = e[0] & 0x3F;
                if (e[0] & 0x40) {
                    fat_decode_lfn(cluster_buf + off - (uint32_t)(seq - 1) * 32U,
                                   seq, pending_lfn, sizeof(pending_lfn));
                    pending_lfn_valid = 1;
                }
                continue;
            }
            if (e[11] & 0x08) {
                pending_lfn_valid = 0;
                continue;
            }

            if (pending_lfn_valid) {
                strncpy(entry_name, pending_lfn, MAX_NAME_LEN - 1);
                entry_name[MAX_NAME_LEN - 1] = '\0';
                pending_lfn_valid = 0;
            } else {
                fat_build_short_name_from_dent(e, entry_name, sizeof(entry_name));
            }

            if (entry_name[0] == '.' &&
                (entry_name[1] == '\0' ||
                 (entry_name[1] == '.' && entry_name[2] == '\0')))
                continue;

            if (!fat_name_equal(entry_name, old_name)) continue;

            for (i = 0; i < 11; i++) e[i] = (uint8_t)short_name[i];
            if (fat_write_cluster(cluster, cluster_buf) != 0) return -1;
            fat_refresh_dir(dir);
            return fat32_sync();
        }
        {
            uint32_t next = fat_get_entry(cluster);
            if (next >= FAT32_CLUSTER_EOF) break;
            cluster = next;
        }
    }
    return -1;
}

int fat32_rename(const char* old_name, const char* new_name) {
    return fat32_dir_rename(current_dir, old_name, new_name);
}

int fat32_delete_dir(const char* dirname) {
    Directory* child = fat_find_child(current_dir, dirname);
    if (!child) return -1;
    fat_free_chain(child->fat_cluster);
    return fat_remove_dir_entry(current_dir, dirname);
}

const char* fat32_get_cwd(void) {
    static char path[256];
    const char* parts[32];
    int count = 0;
    Directory* it = current_dir;

    for (size_t i = 0; i < sizeof(path); i++) path[i] = 0;
    if (!it || it == &g_root_dir) {
        path[0] = '/';
        path[1] = '\0';
        return path;
    }
    while (it && it != &g_root_dir && count < 32) {
        parts[count++] = it->name;
        it = it->parent;
    }
    path[0] = '/';
    path[1] = '\0';
    for (int i = count - 1; i >= 0; i--) {
        strcat(path, parts[i]);
        if (i > 0) strcat(path, "/");
    }
    return path;
}

const Directory* fat32_get_current_dir(void) {
    return current_dir;
}

Directory* fat32_get_cwd_dir(void) {
    return current_dir;
}

Directory* fat32_dir_find_child(Directory* dir, const char* name) {
    return fat_find_child(dir, name);
}

void fat32_set_current_dir(Directory* dir) {
    if (dir) current_dir = dir;
}

void fat32_dir_refresh(Directory* dir) {
    if (!dir || !dir->fat32 || !g_vol.mounted) return;
    fat_refresh_dir(dir);
}

Directory* fat32_get_desktop_dir(void) {
    Directory* found;
    size_t i;

    if (!g_vol.mounted) return NULL;
    if (g_desktop_dir && g_desktop_dir->fat32)
        return g_desktop_dir;

    /* Resolve once: one root refresh, then find Desktop without a second refresh. */
    fat_refresh_dir(&g_root_dir);
    for (i = 0; i < g_root_dir.child_count; i++) {
        if (fat_name_equal(g_root_dir.children[i].name, "Desktop")) {
            g_desktop_dir = &g_root_dir.children[i];
            return g_desktop_dir;
        }
    }

    if (fat_dir_entry_exists(&g_root_dir, "Desktop", 1)) {
        fat_refresh_dir(&g_root_dir);
        for (i = 0; i < g_root_dir.child_count; i++) {
            if (fat_name_equal(g_root_dir.children[i].name, "Desktop")) {
                g_desktop_dir = &g_root_dir.children[i];
                return g_desktop_dir;
            }
        }
    }

    if (fat32_dir_create_dir(&g_root_dir, "Desktop") != 0) return NULL;
    fat_refresh_dir(&g_root_dir);
    found = NULL;
    for (i = 0; i < g_root_dir.child_count; i++) {
        if (fat_name_equal(g_root_dir.children[i].name, "Desktop")) {
            found = &g_root_dir.children[i];
            break;
        }
    }
    g_desktop_dir = found;
    return g_desktop_dir;
}

int fat32_sync(void) {
    if (!g_vol.mounted) return -1;
    /* Flush dirty FAT/dir sectors only. ATA/AHCI CACHE_FLUSH on every
     * create/write freezes the desktop for seconds (VM "lag" with live input).
     * Explicit `sync` / install paths can still call storage_flush. */
    return fat_flush_fat_cache();
}

const char* fat32_mount_description(void) {
    return g_mount_desc;
}
