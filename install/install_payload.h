#ifndef INSTALL_PAYLOAD_H
#define INSTALL_PAYLOAD_H

#include <stddef.h>
#include <stdint.h>

int install_payload_available(void);
int install_payload_kernel(const uint8_t** data, size_t* size);
int install_payload_grub_cfg(const uint8_t** data, size_t* size);
int install_payload_boot_img(const uint8_t** data, size_t* size);
int install_payload_core_img(const uint8_t** data, size_t* size);
int install_payload_fat_template(const uint8_t** data, size_t* size,
                                 uint32_t* sectors_out);
int install_payload_fat_template_sectors(uint32_t* sectors_out);
int install_payload_read_fat_template_sector(uint32_t sector_index, void* sector_out);

#endif
