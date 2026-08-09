#include "install.h"
#include "install_debug.h"
#include "install_payload.h"
#include "grub_bios_embed.h"
#include "gpt_embed.h"
#include "../lib/string.h"

extern void print(const char*);

/* VESA desktop provides a strong definition; other builds leave this NULL. */
void install_ui_yield(void) __attribute__((weak));

static install_progress_fn g_install_progress_cb;

void install_set_progress_callback(install_progress_fn fn) {
    g_install_progress_cb = fn;
}

static void install_notify_progress(uint32_t pct, const char* msg) {
    if (pct > 100U) pct = 100U;
    if (g_install_progress_cb)
        g_install_progress_cb(pct, msg);
    if (install_ui_yield)
        install_ui_yield();
}

static int inst_write_core_embed(const storage_device_info_t* dev,
                                 const uint8_t* core, size_t core_size,
                                 uint32_t core_first_lba) {
    uint8_t sector0[512];
    uint8_t sector[512];
    size_t nsec;
    size_t offset;
    uint32_t lba;
    size_t i;

    if (!core || core_size == 0) return 0;
    if (core_first_lba == 0 || core_first_lba >= INSTALL_PARTITION_START_LBA) return -1;

    nsec = (core_size + 511U) / 512U;
    if (core_first_lba + nsec > INSTALL_PARTITION_START_LBA) return -1;

    memcpy(sector0, core, 512);
    if (core_size < 512) memset(sector0 + core_size, 0, 512 - core_size);
    grub_bios_patch_core_sector0(sector0, core_first_lba, nsec);
    if (storage_write_sector(dev, core_first_lba, sector0) != 0) return -1;

    offset = 512;
    lba = core_first_lba + 1U;
    for (i = 1; i < nsec; i++) {
        size_t chunk = core_size - offset;
        if (chunk > 512) chunk = 512;
        memset(sector, 0, sizeof(sector));
        if (chunk > 0) memcpy(sector, core + offset, chunk);
        if (storage_write_sector(dev, lba, sector) != 0) return -1;
        offset += chunk;
        lba++;
    }
    return 0;
}

static void install_u32_to_dec(uint32_t v, char* out, size_t out_len) {
    char tmp[12];
    int n = 0;
    size_t i;
    if (!out || out_len == 0) return;
    if (v == 0) {
        out[0] = '0';
        if (out_len > 1) out[1] = '\0';
        return;
    }
    while (v > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    i = 0;
    while (n > 0 && i + 1 < out_len)
        out[i++] = tmp[--n];
    out[i] = '\0';
}

static void install_report_pct(const char* phase, uint32_t done, uint32_t total) {
    char line[96];
    char done_s[12];
    char total_s[12];
    char pct_s[12];
    uint32_t pct;
    size_t i = 0;
    size_t j;

    if (total == 0) total = 1;
    pct = (done * 100U) / total;
    if (done >= total) pct = 100U;
    install_u32_to_dec(done, done_s, sizeof(done_s));
    install_u32_to_dec(total, total_s, sizeof(total_s));
    install_u32_to_dec(pct, pct_s, sizeof(pct_s));

    /* "install: <phase> N% (done/total)" */
    const char* prefix = "install: ";
    for (j = 0; prefix[j] && i + 1 < sizeof(line); j++) line[i++] = prefix[j];
    if (phase) {
        for (j = 0; phase[j] && i + 1 < sizeof(line); j++) line[i++] = phase[j];
    }
    if (i + 1 < sizeof(line)) line[i++] = ' ';
    for (j = 0; pct_s[j] && i + 1 < sizeof(line); j++) line[i++] = pct_s[j];
    if (i + 1 < sizeof(line)) line[i++] = '%';
    if (i + 1 < sizeof(line)) line[i++] = ' ';
    if (i + 1 < sizeof(line)) line[i++] = '(';
    for (j = 0; done_s[j] && i + 1 < sizeof(line); j++) line[i++] = done_s[j];
    if (i + 1 < sizeof(line)) line[i++] = '/';
    for (j = 0; total_s[j] && i + 1 < sizeof(line); j++) line[i++] = total_s[j];
    if (i + 1 < sizeof(line)) line[i++] = ')';
    if (i + 1 < sizeof(line)) line[i++] = '\n';
    line[i] = '\0';
    print(line);
    /* Progress bar uses install_notify_progress from the blast loop. */
}

static int inst_blast_partition_template(const storage_device_info_t* dev,
                                         uint32_t template_sectors,
                                         uint32_t part_sectors) {
    enum { BLAST_CHUNK = 32 };
    uint8_t chunk[512 * BLAST_CHUNK];
    uint32_t to_write = template_sectors;
    uint32_t lba;
    uint32_t last_pct = 0xFFFFFFFFu;

    if (template_sectors == 0) return -1;
    if (to_write > part_sectors) to_write = part_sectors;

    for (lba = 0; lba < to_write; ) {
        uint32_t n = to_write - lba;
        uint32_t i;

        if (n > BLAST_CHUNK) n = BLAST_CHUNK;
        for (i = 0; i < n; i++) {
            if (install_payload_read_fat_template_sector(lba + i,
                                                         chunk + (i * 512U)) != 0)
                return -1;
        }
        if (storage_write_sectors(dev, INSTALL_PARTITION_START_LBA + lba,
                                  chunk, n) != 0)
            return -1;
        lba += n;

        /* Yield every chunk; print about every 5% of the copy. */
        {
            uint32_t copy_pct = (lba * 100U) / to_write;
            uint32_t overall = 10U + (lba * 80U) / to_write;
            if (overall > 90U) overall = 90U;
            if (g_install_progress_cb)
                g_install_progress_cb(overall, "copying FAT32 template");
            if (install_ui_yield)
                install_ui_yield();
            if (copy_pct != last_pct && (copy_pct % 5U) == 0U) {
                last_pct = copy_pct;
                install_report_pct("copying FAT32 template", lba, to_write);
            }
        }
    }
    return 0;
}

static int inst_write_mbr_layout(const storage_device_info_t* dev,
                                 uint64_t disk_sectors,
                                 uint32_t* out_part_sectors,
                                 const uint8_t* boot_img, size_t boot_img_size,
                                 const uint8_t* core_img, size_t core_img_size) {
    uint8_t mbr[512];
    uint32_t part_sectors;
    size_t core_nsec;

    if (!boot_img || boot_img_size < 440 || !core_img || core_img_size == 0) {
        print("install: MBR style needs boot.img and core.img\n");
        return -1;
    }

    part_sectors = (uint32_t)(disk_sectors - INSTALL_PARTITION_START_LBA);
    core_nsec = (core_img_size + 511U) / 512U;
    if (INSTALL_MBR_CORE_FIRST_LBA + core_nsec > INSTALL_PARTITION_START_LBA) {
        print("install: core.img too large for MBR gap before partition\n");
        return -1;
    }

    print("install: MBR layout...\n");
    install_notify_progress(5U, "MBR layout");
    if (grub_bios_write_mbr(mbr, boot_img, boot_img_size,
                            INSTALL_MBR_CORE_FIRST_LBA,
                            INSTALL_PARTITION_START_LBA, part_sectors,
                            0x0C) != 0) {
        print("install: MBR build failed\n");
        return -1;
    }
    if (storage_write_sector(dev, 0, mbr) != 0) {
        print("install: MBR write failed at LBA 0\n");
        return -1;
    }

    print("install: embedding GRUB core.img...\n");
    install_notify_progress(8U, "embedding GRUB core.img");
    if (inst_write_core_embed(dev, core_img, core_img_size,
                              INSTALL_MBR_CORE_FIRST_LBA) != 0) {
        print("install: core.img embed failed\n");
        return -1;
    }

    if (out_part_sectors) *out_part_sectors = part_sectors;
    return 0;
}

static int inst_write_gpt_layout(const storage_device_info_t* dev,
                                 uint64_t disk_sectors,
                                 uint32_t* out_part_sectors) {
    print("install: GPT layout...\n");
    install_notify_progress(5U, "GPT layout");
    if (gpt_write_esp_layout(dev, disk_sectors, INSTALL_PARTITION_START_LBA,
                             out_part_sectors) != 0) {
        print("install: GPT ESP layout failed\n");
        return -1;
    }
    return 0;
}

int install_fat32_format_and_write(const storage_device_info_t* dev,
                                   const uint8_t* boot_img, size_t boot_img_size,
                                   const uint8_t* core_img, size_t core_img_size,
                                   uint32_t fat_template_sectors,
                                   install_partition_style_t style) {
    uint32_t part_sectors;
    uint64_t disk_sectors;

    if (!dev) return -1;
    if (dev->install_state != STORAGE_INSTALL_STATE_READY) return -1;
    if (fat_template_sectors == 0) return -1;

    if (dev->sectors == 0) {
        print("install: device sector count unknown; refusing install\n");
        return -1;
    }
    if (dev->sectors < INSTALL_MIN_DISK_SECTORS) {
        print("install: device too small for FAT32 install layout\n");
        return -1;
    }

    disk_sectors = (uint64_t)dev->sectors;
    part_sectors = (uint32_t)(disk_sectors - INSTALL_PARTITION_START_LBA);

    if (style == INSTALL_STYLE_MBR) {
        if (inst_write_mbr_layout(dev, disk_sectors, &part_sectors,
                                  boot_img, boot_img_size,
                                  core_img, core_img_size) != 0)
            return -1;
    } else {
        if (inst_write_gpt_layout(dev, disk_sectors, &part_sectors) != 0)
            return -1;
    }

    print("install: writing FAT32 template...\n");
    install_notify_progress(10U, "writing FAT32 template");
    if (inst_blast_partition_template(dev, fat_template_sectors, part_sectors) != 0)
        return -1;

    print("install: patching volume boot record...\n");
    install_notify_progress(92U, "patching volume boot record");
    /* Patch BPB HiddenSectors (and FAT32 backup VBR) to partition LBA 2048. */
    {
        uint8_t bpb[512];
        uint32_t hidden = INSTALL_PARTITION_START_LBA;
        uint16_t backup_lba;
        if (storage_read_sector(dev, INSTALL_PARTITION_START_LBA, bpb) == 0) {
            bpb[28] = (uint8_t)(hidden & 0xFF);
            bpb[29] = (uint8_t)((hidden >> 8) & 0xFF);
            bpb[30] = (uint8_t)((hidden >> 16) & 0xFF);
            bpb[31] = (uint8_t)((hidden >> 24) & 0xFF);
            (void)storage_write_sector(dev, INSTALL_PARTITION_START_LBA, bpb);
            backup_lba = (uint16_t)bpb[50] | ((uint16_t)bpb[51] << 8);
            if (backup_lba != 0 && backup_lba < 32) {
                (void)storage_write_sector(
                    dev, INSTALL_PARTITION_START_LBA + backup_lba, bpb);
            }
        }
    }

    print("install: flushing...\n");
    install_notify_progress(96U, "flushing");
    if (storage_flush(dev) != 0) return -1;

    install_debug_verify_disk(dev, INSTALL_PARTITION_START_LBA, part_sectors,
                              fat_template_sectors);
    install_notify_progress(100U, "complete");
    return 0;
}
