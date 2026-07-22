#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include <stdint.h>

/* Multiboot magic numbers */
#define MULTIBOOT_MAGIC        0x2BADB002
#define MULTIBOOT2_MAGIC       0x36D76289

/* Multiboot info flags */
#define MULTIBOOT_INFO_CMDLINE      (1 << 2)
#define MULTIBOOT_INFO_MEM_MAP      (1 << 6)
#define MULTIBOOT_INFO_FRAMEBUFFER  (1 << 12)

/* Multiboot 1 info structure */
typedef struct {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint32_t vbe_mode;
    uint32_t vbe_interface_seg;
    uint32_t vbe_interface_off;
    uint32_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint8_t  reserved;
} __attribute__((packed)) multiboot_info_t;

/* Multiboot 2 tag types */
#define MULTIBOOT2_TAG_END             0
#define MULTIBOOT2_TAG_CMDLINE         1
#define MULTIBOOT2_TAG_FRAMEBUFFER     8

/* Multiboot 2 command line tag */
typedef struct {
    uint32_t type;
    uint32_t size;
    char     string[1];
} __attribute__((packed)) multiboot2_tag_cmdline_t;

/* Multiboot 2 framebuffer tag */
typedef struct {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint16_t reserved;
} __attribute__((packed)) multiboot2_tag_framebuffer_t;

/* Multiboot 2 generic tag header */
typedef struct {
    uint32_t type;
    uint32_t size;
} __attribute__((packed)) multiboot2_tag_t;

#endif
