#include "bios_disk.h"

static bios_drive_info_t drives[BIOS_MAX_DRIVES];
static int drive_count = 0;

void bios_disk_scan(void) {
    drive_count = 0;
    for (int i = 0; i < BIOS_MAX_DRIVES; i++) {
        drives[i].drive = 0;
        drives[i].sectors = 0;
        drives[i].sector_size = 512;
        drives[i].present = 0;
    }

    // Placeholder: assume primary BIOS drive exists at 0x80.
    // Real INT 13h enumeration requires a real-mode thunk.
    drives[0].drive = 0x80;
    drives[0].sectors = 0;
    drives[0].sector_size = 512;
    drives[0].present = 1;
    drive_count = 1;
}

int bios_disk_count(void) {
    return drive_count;
}

const bios_drive_info_t* bios_disk_get(int index) {
    if (index < 0 || index >= drive_count) return 0;
    return &drives[index];
}

int bios_read_lba(uint8_t drive, uint64_t lba, uint16_t count, void* out) {
    (void)drive; (void)lba; (void)count; (void)out;
    return -1;
}

int bios_write_lba(uint8_t drive, uint64_t lba, uint16_t count, const void* in) {
    (void)drive; (void)lba; (void)count; (void)in;
    return -1;
}
