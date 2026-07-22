#include "install_payload.h"
#include "install.h"
#include "iso9660.h"
#include "../kernel.h"
#include "../lib/memory.h"
#include "../lib/string.h"

#ifndef EMBED_INSTALL_PAYLOAD
#define EMBED_INSTALL_PAYLOAD 0
#endif

#if EMBED_INSTALL_PAYLOAD
#ifdef __x86_64__
extern unsigned char _binary_build64_install_kernel_payload_bin_start[];
extern unsigned char _binary_build64_install_kernel_payload_bin_end[];
extern unsigned char _binary_build64_install_grub_cfg_start[];
extern unsigned char _binary_build64_install_grub_cfg_end[];
extern unsigned char _binary_build64_install_boot_img_start[];
extern unsigned char _binary_build64_install_boot_img_end[];
extern unsigned char _binary_build64_install_core_img_start[];
extern unsigned char _binary_build64_install_core_img_end[];
#else
extern unsigned char _binary_build_install_kernel_payload_bin_start[];
extern unsigned char _binary_build_install_kernel_payload_bin_end[];
extern unsigned char _binary_build_install_grub_cfg_start[];
extern unsigned char _binary_build_install_grub_cfg_end[];
extern unsigned char _binary_build_install_boot_img_start[];
extern unsigned char _binary_build_install_boot_img_end[];
extern unsigned char _binary_build_install_core_img_start[];
extern unsigned char _binary_build_install_core_img_end[];
#endif
#endif

typedef enum {
    TPL_NONE = 0,
    TPL_ISO,
    TPL_RAM
} fat_tpl_src_t;

typedef struct {
    int valid;
    const storage_device_info_t* dev;
    uint32_t extent_lba;
    uint32_t byte_size;
} iso_fat_template_t;

static fat_tpl_src_t g_tpl_src;
static iso_fat_template_t g_iso_fat_template;
static const uint8_t* g_ram_tpl;
static size_t g_ram_tpl_size;

static int fat_template_ensure(void) {
    size_t mod_sz = 0;
    const uint8_t* mod;

    if (g_tpl_src != TPL_NONE) return 0;

    /* USB live (Lenovo): GRUB module2 — no ATAPI optical on most laptops. */
    mod = boot_fat_template_module(&mod_sz);
    if (mod && mod_sz >= 512U) {
        g_ram_tpl = mod;
        g_ram_tpl_size = mod_sz;
        g_tpl_src = TPL_RAM;
        return 0;
    }

    /* VBox/QEMU: IDE/ATAPI optical with the hybrid ISO attached. */
    if (iso9660_find_fat_template(&g_iso_fat_template.dev,
                                  &g_iso_fat_template.extent_lba,
                                  &g_iso_fat_template.byte_size) == 0 &&
        g_iso_fat_template.byte_size >= 512U) {
        g_iso_fat_template.valid = 1;
        g_tpl_src = TPL_ISO;
        return 0;
    }

    return -1;
}

static uint32_t fat_template_sector_count(void) {
    size_t bytes = 0;
    if (g_tpl_src == TPL_ISO)
        bytes = g_iso_fat_template.byte_size;
    else if (g_tpl_src == TPL_RAM)
        bytes = g_ram_tpl_size;
    if (bytes < 512U) return INSTALL_FAT_TEMPLATE_SECTORS;
    return (uint32_t)((bytes + 511U) / 512U);
}

int install_payload_available(void) {
    /* GPT ESP install needs the FAT template (module2 or optical ISO).
     * BIOS boot.img/core.img are optional extras embedded for MBR targets. */
    return fat_template_ensure() == 0 ? 1 : 0;
}

int install_payload_kernel(const uint8_t** data, size_t* size) {
#if EMBED_INSTALL_PAYLOAD
    if (!data || !size) return -1;
#ifdef __x86_64__
    *data = _binary_build64_install_kernel_payload_bin_start;
    *size = (size_t)(_binary_build64_install_kernel_payload_bin_end -
                     _binary_build64_install_kernel_payload_bin_start);
#else
    *data = _binary_build_install_kernel_payload_bin_start;
    *size = (size_t)(_binary_build_install_kernel_payload_bin_end -
                     _binary_build_install_kernel_payload_bin_start);
#endif
    return (*size > 0) ? 0 : -1;
#else
    (void)data;
    (void)size;
    return -1;
#endif
}

int install_payload_grub_cfg(const uint8_t** data, size_t* size) {
#if EMBED_INSTALL_PAYLOAD
    if (!data || !size) return -1;
#ifdef __x86_64__
    *data = _binary_build64_install_grub_cfg_start;
    *size = (size_t)(_binary_build64_install_grub_cfg_end -
                     _binary_build64_install_grub_cfg_start);
#else
    *data = _binary_build_install_grub_cfg_start;
    *size = (size_t)(_binary_build_install_grub_cfg_end -
                     _binary_build_install_grub_cfg_start);
#endif
    return (*size > 0) ? 0 : -1;
#else
    (void)data;
    (void)size;
    return -1;
#endif
}

int install_payload_boot_img(const uint8_t** data, size_t* size) {
#if EMBED_INSTALL_PAYLOAD
    if (!data || !size) return -1;
#ifdef __x86_64__
    *data = _binary_build64_install_boot_img_start;
    *size = (size_t)(_binary_build64_install_boot_img_end -
                     _binary_build64_install_boot_img_start);
#else
    *data = _binary_build_install_boot_img_start;
    *size = (size_t)(_binary_build_install_boot_img_end -
                     _binary_build_install_boot_img_start);
#endif
    return (*size > 0) ? 0 : -1;
#else
    (void)data;
    (void)size;
    return -1;
#endif
}

int install_payload_core_img(const uint8_t** data, size_t* size) {
#if EMBED_INSTALL_PAYLOAD
    if (!data || !size) return -1;
#ifdef __x86_64__
    *data = _binary_build64_install_core_img_start;
    *size = (size_t)(_binary_build64_install_core_img_end -
                     _binary_build64_install_core_img_start);
#else
    *data = _binary_build_install_core_img_start;
    *size = (size_t)(_binary_build_install_core_img_end -
                     _binary_build_install_core_img_start);
#endif
    return (*size > 0) ? 0 : -1;
#else
    (void)data;
    (void)size;
    return -1;
#endif
}

int install_payload_fat_template(const uint8_t** data, size_t* size, uint32_t* sectors_out) {
    (void)data;
    (void)size;
    (void)sectors_out;
    return -1;
}

int install_payload_fat_template_sectors(uint32_t* sectors_out) {
    if (!sectors_out) return -1;
    if (fat_template_ensure() != 0) return -1;
    *sectors_out = fat_template_sector_count();
    return 0;
}

int install_payload_read_fat_template_sector(uint32_t sector_index, void* sector_out) {
    uint32_t byte_pos;

    if (!sector_out) return -1;
    if (fat_template_ensure() != 0) return -1;

    if (g_tpl_src == TPL_ISO) {
        return iso9660_read_file_sector(g_iso_fat_template.dev,
                                        g_iso_fat_template.extent_lba,
                                        g_iso_fat_template.byte_size,
                                        sector_index,
                                        sector_out);
    }

    if (g_tpl_src == TPL_RAM) {
        byte_pos = sector_index * 512U;
        memset(sector_out, 0, 512);
        if (byte_pos >= g_ram_tpl_size) return 0;
        {
            size_t chunk = g_ram_tpl_size - byte_pos;
            if (chunk > 512U) chunk = 512U;
            memcpy(sector_out, g_ram_tpl + byte_pos, chunk);
        }
        return 0;
    }

    return -1;
}
