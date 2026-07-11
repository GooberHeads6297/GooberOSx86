#include "install.h"
#include "install_payload.h"
#include "iso9660.h"
#include "../fs/filesystem.h"
#include "../lib/string.h"

extern void print(const char*);
extern char* itoa(int value, char* str, int base);

const char* install_partition_style_name(install_partition_style_t style) {
    switch (style) {
    case INSTALL_STYLE_MBR: return "MBR";
    case INSTALL_STYLE_GPT: return "GPT";
    default:                return "?";
    }
}

int install_memory_only(void) {
    static const char marker[] =
        "GooberOS memory-only install\n"
        "Root filesystem: memfs (RAM)\n"
        "This is not persistent across reboot.\n"
        "Use `install fat32 <id> YES MBR|GPT` for a durable FAT32 system.\n";

    print("install: memory-only install (memfs marker)\n");
    if (fs_write("INSTALLED.MEM", (const uint8_t*)marker, sizeof(marker) - 1) != 0) {
        print("install: failed to write INSTALLED.MEM to memfs\n");
        return -1;
    }
    if (fs_write("etc/install-mode.txt",
                 (const uint8_t*)"memory\n", 7) != 0) {
        /* etc/ may already exist from memfs seed; best-effort. */
        print("install: warning: could not write etc/install-mode.txt\n");
    }
    print("install: memory-only root ready (live memfs).\n");
    print("  Files: INSTALLED.MEM\n");
    print("  Note: reboot loses this unless you also run install fat32.\n");
    return 0;
}

int install_fat32_format_and_write(const storage_device_info_t* dev,
                                   const uint8_t* boot_img, size_t boot_img_size,
                                   const uint8_t* core_img, size_t core_img_size,
                                   uint32_t fat_template_sectors,
                                   install_partition_style_t style);

int install_fat32_to_device(const storage_device_info_t* dev,
                            install_partition_style_t style) {
    const uint8_t* boot_img = NULL;
    const uint8_t* core_img = NULL;
    size_t boot_img_size = 0;
    size_t core_img_size = 0;
    uint32_t fat_template_sectors = 0;

    if (!dev) {
        print("install: no device selected\n");
        return -1;
    }
    if (!install_payload_available()) {
        print("install: payload not available.\n");
        print("  FAT template missing. On USB live (Lenovo), reboot and pick an\n");
        print("  entry that loads module2 /boot/install/FAT_PART.IMG\n");
        print("  (Normal boot, or eMMC install live). VBox can use IDE optical.\n");
        print("  Detail: ");
        print(iso9660_last_error());
        print("\n");
        return -1;
    }
    install_payload_boot_img(&boot_img, &boot_img_size);
    install_payload_core_img(&core_img, &core_img_size);
    if (install_payload_fat_template_sectors(&fat_template_sectors) != 0 ||
        fat_template_sectors == 0) {
        print("install: FAT partition template missing\n");
        print("  ");
        print(iso9660_last_error());
        print("\n");
        print("  Use Basic display / Normal boot, or IDE optical.\n");
        return -1;
    }

    if (dev->install_state != STORAGE_INSTALL_STATE_READY) {
        print("install: target is not ready for writes\n");
        print("  Device: ");
        print(dev->model[0] ? dev->model : "storage device");
        print(" (");
        print(dev->location);
        print(")\n");
        print("  Reason: ");
        print(storage_install_state_reason(dev));
        print("\n");
        print("  Hint: use `install list` for valid target IDs (not `devices`).\n");
        return -1;
    }
    if (dev->sectors == 0) {
        print("install: device sector count unknown; refusing install\n");
        print("install: (need IDENTIFY/geometry so GPT backup LBA is in range)\n");
        return -1;
    }
    if (dev->sectors < INSTALL_MIN_DISK_SECTORS) {
        char buf[16];
        print("install: device too small (need at least ~");
        itoa((int)(INSTALL_MIN_DISK_SECTORS / 2048U), buf, 10);
        print(buf);
        print(" MiB total)\n");
        return -1;
    }

    print("install: FAT32 deploy (");
    print(install_partition_style_name(style));
    print(") to ");
    print(dev->model[0] ? dev->model : "storage device");
    print("\n");

    if (style != INSTALL_STYLE_GPT &&
        (boot_img_size == 0 || core_img_size == 0)) {
        print("install: warning: GRUB boot/core images missing; BIOS boot may fail\n");
    }

    if (install_fat32_format_and_write(dev, boot_img, boot_img_size,
                                       core_img, core_img_size,
                                       fat_template_sectors, style) != 0) {
        print("install: FAT32 deploy failed\n");
        return -1;
    }

    print("install: FAT32 deploy complete\n");
    print("install: Reboot from the target disk.\n");
    return 0;
}
