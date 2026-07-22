#ifndef CONFIG_BOOT_H
#define CONFIG_BOOT_H

/* Parse Config/boot.cfg on installed FAT and resync Config/grub-boot.cfg. */
void config_boot_apply(void);

#endif
