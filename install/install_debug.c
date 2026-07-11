#include "install_debug.h"
#include "install.h"
#include "../lib/string.h"

extern void print(const char*);

/* #region agent log */
static void dbg_emit(const char* hyp, const char* msg,
                     uint32_t a, uint32_t b, uint32_t c) {
    char buf[16];
    print("[DBG:");
    print(hyp);
    print("] ");
    print(msg);
    print(" a=");
    itoa((int)a, buf, 10);
    print(buf);
    print(" b=");
    itoa((int)b, buf, 10);
    print(buf);
    print(" c=");
    itoa((int)c, buf, 10);
    print(buf);
    print("\n");
}
/* #endregion */

static int dbg_read_abs(const storage_device_info_t* dev, uint32_t lba, void* sector) {
    if (!dev) return -1;
    return storage_read_sector(dev, lba, sector);
}

static uint32_t dbg_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t dbg_cluster_lba(uint32_t part_start, const uint8_t* bpb, uint32_t cluster) {
    uint32_t reserved = (uint32_t)bpb[0x0E] | ((uint32_t)bpb[0x0F] << 8);
    uint32_t fats = (uint32_t)bpb[0x10];
    uint32_t fat_sz = dbg_le32(bpb + 0x24);
    uint32_t spc = (uint32_t)bpb[0x0D];
    uint32_t data_rel = reserved + fats * fat_sz;
    return part_start + data_rel + (cluster - 2U) * spc;
}

static uint32_t dbg_dent_start_cluster(const uint8_t* e) {
    return ((uint32_t)(e[21] << 8 | e[20]) << 16) |
           (uint32_t)(e[27] << 8 | e[26]);
}

static int dbg_find_dir_entry(const storage_device_info_t* dev, uint32_t part_start,
                              const uint8_t* bpb, uint32_t dir_cluster,
                              const char* name83, uint32_t* out_cluster, uint32_t* out_size) {
    uint8_t sector[512];
    uint32_t spc = (uint32_t)bpb[0x0D];
    uint32_t c;
    int i;

    for (c = 0; c < spc; c++) {
        if (dbg_read_abs(dev, dbg_cluster_lba(part_start, bpb, dir_cluster) + c, sector) != 0)
            return -1;
        for (i = 0; i < 512; i += 32) {
            const uint8_t* e = sector + i;
            if (e[0] == 0x00) return -1;
            if (e[0] == 0xE5 || e[11] == 0x0F) continue;
            if (!strncmp((const char*)e, name83, 11)) {
                if (out_cluster) *out_cluster = dbg_dent_start_cluster(e);
                if (out_size) *out_size = dbg_le32(e + 28);
                return 0;
            }
        }
    }
    return -1;
}

void install_debug_verify_disk(const storage_device_info_t* dev,
                               uint32_t part_start_lba,
                               uint32_t part_sectors,
                               uint32_t template_sectors) {
    uint8_t mbr[512];
    uint8_t core0[512];
    uint8_t bpb[512];
    uint32_t hidden;
    uint32_t sig;
    uint16_t bl_len;
    uint16_t bl_seg;
    uint32_t bl_start_lo;
    int label_ok = 1;
    int i;

    (void)part_sectors;

    if (!dev) return;

    dbg_emit("H8", "template_sectors", template_sectors, INSTALL_FAT_TEMPLATE_SECTORS, 0);

    if (dbg_read_abs(dev, 0, mbr) == 0) {
        sig = (uint32_t)mbr[510] | ((uint32_t)mbr[511] << 8);
        dbg_emit("H4", "mbr_sig_and_kern_lba",
                 sig,
                 (uint32_t)mbr[446],
                 (uint32_t)mbr[0x5C] | ((uint32_t)mbr[0x5D] << 8) |
                     ((uint32_t)mbr[0x5E] << 16) | ((uint32_t)mbr[0x5F] << 24));
        if (mbr[450] == 0xEE) {
            uint8_t gpt[512];
            uint64_t disk_sectors = dev->sectors;
            dbg_emit("H4", "protective_mbr_gpt", 0xEE, (uint32_t)mbr[446], 1);
            /* Primary GPT header at LBA 1. */
            if (dbg_read_abs(dev, 1, gpt) == 0) {
                int ok = (gpt[0] == 'E' && gpt[1] == 'F' && gpt[2] == 'I' &&
                          gpt[3] == ' ' && gpt[4] == 'P' && gpt[5] == 'A' &&
                          gpt[6] == 'R' && gpt[7] == 'T');
                dbg_emit("H4", "gpt_primary_header", ok ? 1U : 0U,
                         dbg_le32(gpt + 72), dbg_le32(gpt + 40));
            }
            /* Backup GPT header at last LBA. */
            if (disk_sectors > 34) {
                uint32_t alt = (uint32_t)(disk_sectors - 1U);
                if (dbg_read_abs(dev, alt, gpt) == 0) {
                    int ok = (gpt[0] == 'E' && gpt[1] == 'F' && gpt[2] == 'I' &&
                              gpt[3] == ' ' && gpt[4] == 'P' && gpt[5] == 'A' &&
                              gpt[6] == 'R' && gpt[7] == 'T');
                    dbg_emit("H4", "gpt_backup_header", ok ? 1U : 0U, alt, 0);
                } else {
                    dbg_emit("H4", "gpt_backup_read_fail", alt, 0, 0);
                }
            }
        }

        /* MBR 0x0C: BIOS core at LBA 1. GPT 0xEE: UEFI-only, no BIOS core. */
        if (mbr[450] == 0x0C) {
            if (dbg_read_abs(dev, INSTALL_MBR_CORE_FIRST_LBA, core0) == 0) {
                bl_start_lo = (uint32_t)core0[0x1F4] | ((uint32_t)core0[0x1F5] << 8) |
                              ((uint32_t)core0[0x1F6] << 16) |
                              ((uint32_t)core0[0x1F7] << 24);
                bl_len = (uint16_t)core0[0x1FC] | ((uint16_t)core0[0x1FD] << 8);
                bl_seg = (uint16_t)core0[0x1FE] | ((uint16_t)core0[0x1FF] << 8);
                dbg_emit("H4", "core_blocklist", bl_start_lo, (uint32_t)bl_len,
                         (uint32_t)bl_seg);
                dbg_emit("H4", "core_lba", INSTALL_MBR_CORE_FIRST_LBA,
                         (uint32_t)mbr[450], 0);
            } else {
                dbg_emit("H4", "core_read_fail", INSTALL_MBR_CORE_FIRST_LBA, 0, 0);
            }
        } else if (mbr[450] == 0xEE) {
            dbg_emit("H4", "gpt_uefi_only_no_bios_core", 0, 0, 0);
        }
    } else {
        dbg_emit("H4", "mbr_read_fail", 0, 0, 0);
    }

    if (dbg_read_abs(dev, part_start_lba, bpb) == 0) {
        uint8_t bpb_copy[512];
        uint32_t root_cluster;
        uint32_t boot_cluster = 0;
        uint32_t grub_cluster = 0;
        uint32_t kernel_size = 0;
        uint32_t cfg_size = 0;
        int found_cfg = 0;
        int found_kernel = 0;

        memcpy(bpb_copy, bpb, sizeof(bpb_copy));
        hidden = dbg_le32(bpb_copy + 0x1C);
        dbg_emit("H2", "bpb_hidden_sectors", hidden, (uint32_t)bpb_copy[0x42], (uint32_t)bpb_copy[0x0D]);
        {
            uint16_t vbr_sig = (uint16_t)bpb_copy[510] | ((uint16_t)bpb_copy[511] << 8);
            dbg_emit("H7", "vbr_boot_signature", (uint32_t)vbr_sig, (uint32_t)bpb_copy[0], (uint32_t)bpb_copy[2]);
        }
        for (i = 0; i < 11; i++) {
            char c = (char)bpb_copy[0x47 + i];
            if (c == ' ') break;
            if (i >= 8) break;
            if (c != "GOOBEROS"[i] && c != "gooberos"[i]) label_ok = 0;
        }
        dbg_emit("H5", "volume_label_ok", label_ok ? 1U : 0U, (uint32_t)bpb_copy[0x47], (uint32_t)bpb_copy[0x48]);

        root_cluster = dbg_le32(bpb_copy + 0x2C);
        {
            uint32_t reserved = (uint32_t)bpb_copy[0x0E] | ((uint32_t)bpb_copy[0x0F] << 8);
            uint32_t fats = (uint32_t)bpb_copy[0x10];
            uint32_t fat_sz = dbg_le32(bpb_copy + 0x24);
            uint32_t data_rel = reserved + fats * fat_sz;
            uint32_t root_lba = part_start_lba + data_rel;
            uint8_t root[512];
            int found_boot = 0;

            if (dbg_read_abs(dev, root_lba, root) == 0) {
                for (i = 0; i < 512; i += 32) {
                    if (root[i] == 0x00) break;
                    if (root[i] == 0xE5) continue;
                    if (!strncmp((const char*)(root + i), "BOOT        ", 11)) found_boot = 1;
                }
                dbg_emit("H1", "root_dir_boot", (uint32_t)found_boot, root_cluster, 0);
            } else {
                dbg_emit("H1", "root_dir_read_fail", root_lba, 0, 0);
            }
        }

        if (dbg_find_dir_entry(dev, part_start_lba, bpb_copy, root_cluster,
                               "BOOT        ", &boot_cluster, NULL) == 0 &&
            dbg_find_dir_entry(dev, part_start_lba, bpb_copy, boot_cluster,
                               "GRUB        ", &grub_cluster, NULL) == 0) {
            if (dbg_find_dir_entry(dev, part_start_lba, bpb_copy, grub_cluster,
                                   "GRUB    CFG ", NULL, &cfg_size) == 0)
                found_cfg = 1;
            if (dbg_find_dir_entry(dev, part_start_lba, bpb_copy, boot_cluster,
                                   "KERNEL  BIN ", NULL, &kernel_size) == 0)
                found_kernel = 1;
        }
        dbg_emit("H8", "boot_files", (uint32_t)found_kernel, (uint32_t)found_cfg, kernel_size);
        if (found_cfg) dbg_emit("H8", "grub_cfg_size", cfg_size, grub_cluster, boot_cluster);
    } else {
        dbg_emit("H2", "bpb_read_fail", part_start_lba, 0, 0);
    }

    if (dbg_read_abs(dev, 0, mbr) == 0) {
        uint32_t part_lba = (uint32_t)mbr[454] | ((uint32_t)mbr[455] << 8) |
                            ((uint32_t)mbr[456] << 16) | ((uint32_t)mbr[457] << 24);
        dbg_emit("H3", "mbr_part_entry", (uint32_t)mbr[450], part_lba, part_start_lba);
    }
}
