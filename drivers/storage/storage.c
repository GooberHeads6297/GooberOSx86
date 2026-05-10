#include "storage.h"
#include "sdhci.h"
#include "../io/io.h"
#include "../pci/pci.h"
#include "../usb/storage/msc.h"
#include "../../lib/string.h"

#define ATA_REG_DATA       0x00
#define ATA_REG_SECCOUNT0  0x02
#define ATA_REG_LBA0       0x03
#define ATA_REG_LBA1       0x04
#define ATA_REG_LBA2       0x05
#define ATA_REG_HDDEVSEL   0x06
#define ATA_REG_COMMAND    0x07
#define ATA_REG_STATUS     0x07

#define ATA_CMD_IDENTIFY         0xEC
#define ATA_CMD_IDENTIFY_PACKET  0xA1
#define ATA_CMD_READ_PIO         0x20
#define ATA_CMD_WRITE_PIO        0x30
#define ATA_CMD_CACHE_FLUSH      0xE7

#define ATA_SR_ERR   0x01
#define ATA_SR_DRQ   0x08
#define ATA_SR_DF    0x20
#define ATA_SR_DRDY  0x40
#define ATA_SR_BSY   0x80

typedef struct {
    uint16_t io_base;
    uint16_t ctrl_base;
    const char* name;
} ata_channel_t;

static const ata_channel_t ata_channels[2] = {
    { 0x1F0, 0x3F6, "primary" },
    { 0x170, 0x376, "secondary" }
};

static storage_device_info_t storage_devices[STORAGE_MAX_DEVICES];
static int storage_device_count = 0;
static int storage_initialized = 0;

extern void print(const char* str);

static void append_uint(char* dest, int value) {
    char buf[16];
    itoa(value, buf, 10);
    strcat(dest, buf);
}

static void trim_ascii(char* text) {
    size_t start = 0;
    size_t len = strlen(text);
    while (start < len && text[start] == ' ') start++;
    while (len > start && text[len - 1] == ' ') len--;

    size_t out = 0;
    for (size_t i = start; i < len; i++) text[out++] = text[i];
    text[out] = '\0';
}

static void set_ata_location(storage_device_info_t* dev, uint8_t channel, uint8_t drive) {
    dev->location[0] = '\0';
    strcat(dev->location, ata_channels[channel].name);
    strcat(dev->location, " ");
    strcat(dev->location, drive == 0 ? "master" : "slave");
}

static void set_pci_location(storage_device_info_t* dev, uint8_t bus, uint8_t slot, uint8_t func) {
    dev->location[0] = '\0';
    strcat(dev->location, "pci ");
    append_uint(dev->location, bus);
    strcat(dev->location, ":");
    append_uint(dev->location, slot);
    strcat(dev->location, ":");
    append_uint(dev->location, func);
}

static storage_device_info_t* add_storage_device(void) {
    if (storage_device_count >= STORAGE_MAX_DEVICES) return 0;
    memset(&storage_devices[storage_device_count], 0, sizeof(storage_devices[storage_device_count]));
    storage_devices[storage_device_count].present = 1;
    storage_devices[storage_device_count].sector_size = 512;
    return &storage_devices[storage_device_count++];
}

static void ata_delay(uint16_t ctrl_base) {
    for (int i = 0; i < 4; i++) inb(ctrl_base);
}

static int ata_wait_not_busy(uint16_t io_base) {
    int timeout = 100000;
    while (timeout-- > 0) {
        uint8_t status = inb(io_base + ATA_REG_STATUS);
        if (status == 0) return 0;
        if ((status & ATA_SR_BSY) == 0) {
            if (status & (ATA_SR_ERR | ATA_SR_DF)) return 0;
            return 1;
        }
    }
    return 0;
}

static int ata_wait_drq(uint16_t io_base) {
    int timeout = 100000;
    while (timeout-- > 0) {
        uint8_t status = inb(io_base + ATA_REG_STATUS);
        if (status == 0) return 0;
        if (status & (ATA_SR_ERR | ATA_SR_DF)) return 0;
        if ((status & ATA_SR_BSY) == 0 && (status & ATA_SR_DRQ)) return 1;
    }
    return 0;
}

static int ata_poll_ready(uint16_t io_base, int* is_packet_device) {
    int timeout = 100000;
    uint8_t status;

    while (timeout-- > 0) {
        status = inb(io_base + ATA_REG_STATUS);
        if (status == 0) return 0;
        if ((status & ATA_SR_BSY) == 0) break;
    }

    if (timeout <= 0) return 0;

    if (status & ATA_SR_ERR) {
        uint8_t lba1 = inb(io_base + ATA_REG_LBA1);
        uint8_t lba2 = inb(io_base + ATA_REG_LBA2);
        if ((lba1 == 0x14 && lba2 == 0xEB) || (lba1 == 0x69 && lba2 == 0x96)) {
            *is_packet_device = 1;
            return 1;
        }
        return 0;
    }

    timeout = 100000;
    while (timeout-- > 0) {
        status = inb(io_base + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) return 0;
        if (status & ATA_SR_DF) return 0;
        if (status & ATA_SR_DRQ) return 1;
        if ((status & ATA_SR_DRDY) && (status & ATA_SR_DRQ) == 0) continue;
    }

    return 0;
}

static int ata_identify_words(uint16_t io_base, uint16_t ctrl_base, uint8_t drive, uint8_t command, uint16_t* identify, int* packet_device) {
    *packet_device = 0;

    outb(ctrl_base, 0x02);
    outb(io_base + ATA_REG_HDDEVSEL, (uint8_t)(0xA0 | (drive << 4)));
    ata_delay(ctrl_base);

    outb(io_base + ATA_REG_SECCOUNT0, 0);
    outb(io_base + ATA_REG_LBA0, 0);
    outb(io_base + ATA_REG_LBA1, 0);
    outb(io_base + ATA_REG_LBA2, 0);
    outb(io_base + ATA_REG_COMMAND, command);

    if (!ata_poll_ready(io_base, packet_device)) return 0;
    if (*packet_device && command != ATA_CMD_IDENTIFY_PACKET) return 0;

    for (int i = 0; i < 256; i++) identify[i] = inw(io_base + ATA_REG_DATA);
    return 1;
}

static void ata_copy_model(char* out, const uint16_t* identify) {
    int pos = 0;
    for (int i = 27; i <= 46; i++) {
        out[pos++] = (char)((identify[i] >> 8) & 0xFF);
        out[pos++] = (char)(identify[i] & 0xFF);
    }
    out[pos] = '\0';
    trim_ascii(out);
    if (out[0] == '\0') strcpy(out, "Unnamed ATA device");
}

static int ata_rw_sector(uint8_t channel, uint8_t drive, uint32_t lba, void* buffer, int write) {
    uint16_t io_base;
    uint16_t ctrl_base;

    if (channel >= 2 || drive >= 2 || lba > 0x0FFFFFFF) return -1;

    io_base = ata_channels[channel].io_base;
    ctrl_base = ata_channels[channel].ctrl_base;

    if (!ata_wait_not_busy(io_base)) return -1;

    outb(ctrl_base, 0x02);
    outb(io_base + ATA_REG_HDDEVSEL, (uint8_t)(0xE0 | (drive << 4) | ((lba >> 24) & 0x0F)));
    ata_delay(ctrl_base);

    outb(io_base + ATA_REG_SECCOUNT0, 1);
    outb(io_base + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(io_base + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(io_base + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    outb(io_base + ATA_REG_COMMAND, write ? ATA_CMD_WRITE_PIO : ATA_CMD_READ_PIO);

    if (!ata_wait_drq(io_base)) return -1;

    if (write) {
        const uint16_t* words = (const uint16_t*)buffer;
        for (int i = 0; i < 256; i++) outw(io_base + ATA_REG_DATA, words[i]);
        ata_delay(ctrl_base);
        return 0;
    }

    {
        uint16_t* words = (uint16_t*)buffer;
        for (int i = 0; i < 256; i++) words[i] = inw(io_base + ATA_REG_DATA);
    }

    ata_delay(ctrl_base);
    return 0;
}

static void scan_ata_devices(void) {
    for (uint8_t channel = 0; channel < 2; channel++) {
        for (uint8_t drive = 0; drive < 2; drive++) {
            uint16_t identify[256];
            int packet_device = 0;
            storage_device_info_t* dev;

            if (!ata_identify_words(ata_channels[channel].io_base,
                                    ata_channels[channel].ctrl_base,
                                    drive,
                                    ATA_CMD_IDENTIFY,
                                    identify,
                                    &packet_device)) {
                if (!packet_device) continue;
                if (!ata_identify_words(ata_channels[channel].io_base,
                                        ata_channels[channel].ctrl_base,
                                        drive,
                                        ATA_CMD_IDENTIFY_PACKET,
                                        identify,
                                        &packet_device)) {
                    continue;
                }
            }

            dev = add_storage_device();
            if (!dev) return;

            dev->bus = STORAGE_BUS_ATA;
            dev->ata_channel = channel;
            dev->ata_drive = drive;
            dev->sector_size = 512;
            dev->backend = STORAGE_BACKEND_ATA_PIO;
            dev->install_state = STORAGE_INSTALL_STATE_READY;
            ata_copy_model(dev->model, identify);
            set_ata_location(dev, channel, drive);

            if (packet_device || (identify[0] & 0x8000U)) {
                dev->type = STORAGE_TYPE_OPTICAL;
                dev->selectable = 0;
                dev->direct_install_supported = 0;
                dev->backend = STORAGE_BACKEND_NONE;
                dev->install_state = STORAGE_INSTALL_STATE_UNAVAILABLE;
                continue;
            }

            dev->type = (identify[217] == 1) ? STORAGE_TYPE_SSD : STORAGE_TYPE_HDD;
            dev->selectable = 1;
            dev->direct_install_supported = 1;
            dev->sectors = ((uint64_t)identify[61] << 16) | identify[60];
            if (identify[83] & (1U << 10)) {
                uint64_t sectors48 =
                    (uint64_t)identify[100] |
                    ((uint64_t)identify[101] << 16) |
                    ((uint64_t)identify[102] << 32) |
                    ((uint64_t)identify[103] << 48);
                if (sectors48 != 0) dev->sectors = sectors48;
            }
        }
    }
}

static void add_pci_storage_device(uint8_t bus, uint8_t slot, uint8_t func, uint8_t type, uint8_t backend, uint8_t install_state, uint8_t class_code, uint8_t sub_class, uint8_t prog_if) {
    storage_device_info_t* dev = add_storage_device();
    uint32_t id;

    if (!dev) return;

    id = pci_read_config_dword(bus, slot, func, 0x00);

    dev->bus = STORAGE_BUS_PCI;
    dev->type = type;
    dev->pci_bus = bus;
    dev->pci_slot = slot;
    dev->pci_func = func;
    dev->vendor_id = (uint16_t)(id & 0xFFFF);
    dev->device_id = (uint16_t)((id >> 16) & 0xFFFF);
    dev->bar0 = pci_read_config_dword(bus, slot, func, 0x10);
    dev->backend = backend;
    dev->install_state = install_state;
    dev->class_code = class_code;
    dev->sub_class = sub_class;
    dev->prog_if = prog_if;
    dev->selectable = 0;
    dev->direct_install_supported = 0;
    strcpy(dev->model, storage_type_name(type));
    set_pci_location(dev, bus, slot, func);
}

static void probe_pci_storage_backend(storage_device_info_t* dev) {
    if (!dev || dev->bus != STORAGE_BUS_PCI) return;

    if (dev->backend == STORAGE_BACKEND_SDHCI) {
        sdhci_probe_result_t probe;
        if (sdhci_probe_pci_controller(dev->pci_bus, dev->pci_slot, dev->pci_func, dev->bar0, &probe)) {
            dev->write_protected = probe.write_protected;
            dev->init_step = probe.init_step;
            dev->last_status = probe.last_status;
            dev->sector_size = 512;
            dev->sectors = probe.sector_count;
            if (probe.initialized && probe.sector_count != 0) {
                dev->selectable = probe.write_protected ? 0 : 1;
                dev->direct_install_supported = probe.write_protected ? 0 : 1;
                dev->install_state = probe.write_protected ? STORAGE_INSTALL_STATE_CONTROLLER_ONLY : STORAGE_INSTALL_STATE_READY;
                strcpy(dev->model, "eMMC device");
            } else if (probe.card_present) {
                dev->install_state = STORAGE_INSTALL_STATE_CONTROLLER_ONLY;
                strcpy(dev->model, "SDHCI/eMMC slot");
            } else {
                dev->install_state = STORAGE_INSTALL_STATE_DRIVER_MISSING;
                strcpy(dev->model, "SDHCI controller");
            }
        }
        return;
    }

    if (dev->backend == STORAGE_BACKEND_USB_MASS_STORAGE) {
        usb_msc_probe_result_t probe;
        if (usb_msc_probe_pci_controller(dev->prog_if, dev->bar0, &probe)) {
            if (probe.transport_scaffold_ready) dev->install_state = STORAGE_INSTALL_STATE_CONTROLLER_ONLY;
            strcpy(dev->model, usb_msc_host_name(probe.host_kind));
            strcat(dev->model, " USB controller");
        }
        return;
    }
}

static void scan_pci_storage_controllers(void) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint16_t vendor_id = pci_read_config_word((uint8_t)bus, slot, func, 0x00);
                uint8_t base_class;
                uint8_t sub_class;
                uint8_t prog_if;
                if (vendor_id == 0xFFFF) continue;

                base_class = pci_read_config_byte((uint8_t)bus, slot, func, 0x0B);
                sub_class = pci_read_config_byte((uint8_t)bus, slot, func, 0x0A);
                prog_if = pci_read_config_byte((uint8_t)bus, slot, func, 0x09);

                if (base_class == 0x01) {
                    if (sub_class == 0x01) add_pci_storage_device((uint8_t)bus, slot, func, STORAGE_TYPE_IDE_CONTROLLER, STORAGE_BACKEND_IDE, STORAGE_INSTALL_STATE_CONTROLLER_ONLY, base_class, sub_class, prog_if);
                    else if (sub_class == 0x06) add_pci_storage_device((uint8_t)bus, slot, func, STORAGE_TYPE_AHCI_CONTROLLER, STORAGE_BACKEND_AHCI, STORAGE_INSTALL_STATE_DRIVER_MISSING, base_class, sub_class, prog_if);
                    else if (sub_class == 0x08) add_pci_storage_device((uint8_t)bus, slot, func, STORAGE_TYPE_NVME_CONTROLLER, STORAGE_BACKEND_NVME, STORAGE_INSTALL_STATE_DRIVER_MISSING, base_class, sub_class, prog_if);
                } else if (base_class == 0x08 && sub_class == 0x05) {
                    add_pci_storage_device((uint8_t)bus, slot, func, STORAGE_TYPE_EMMC_CONTROLLER, STORAGE_BACKEND_SDHCI, STORAGE_INSTALL_STATE_DRIVER_MISSING, base_class, sub_class, prog_if);
                } else if (base_class == 0x0C && sub_class == 0x03) {
                    add_pci_storage_device((uint8_t)bus, slot, func, STORAGE_TYPE_USB_CONTROLLER, STORAGE_BACKEND_USB_MASS_STORAGE, STORAGE_INSTALL_STATE_DRIVER_MISSING, base_class, sub_class, prog_if);
                }
            }
        }
    }
}

static void print_scan_summary(void) {
    char buf[16];

    print("Storage scan: ");
    itoa(storage_device_count, buf, 10);
    print(buf);
    print(" path(s), ");
    itoa(storage_target_count(), buf, 10);
    print(buf);
    print(" install target(s).\n");
}

void storage_scan(void) {
    memset(storage_devices, 0, sizeof(storage_devices));
    storage_device_count = 0;

    scan_ata_devices();
    scan_pci_storage_controllers();
    for (int i = 0; i < storage_device_count; i++) {
        probe_pci_storage_backend(&storage_devices[i]);
    }
}

void storage_init(void) {
    storage_scan();
    if (!storage_initialized) {
        print_scan_summary();
        storage_initialized = 1;
    }
}

int storage_count(void) {
    return storage_device_count;
}

const storage_device_info_t* storage_get(int index) {
    if (index < 0 || index >= storage_device_count) return 0;
    return &storage_devices[index];
}

int storage_target_count(void) {
    int count = 0;
    for (int i = 0; i < storage_device_count; i++) {
        if (storage_devices[i].present && storage_devices[i].selectable) count++;
    }
    return count;
}

const storage_device_info_t* storage_get_target(int index) {
    int current = 0;
    for (int i = 0; i < storage_device_count; i++) {
        if (!storage_devices[i].present || !storage_devices[i].selectable) continue;
        if (current == index) return &storage_devices[i];
        current++;
    }
    return 0;
}

int storage_read_sector(const storage_device_info_t* device, uint32_t lba, void* out_sector) {
    if (!device || !out_sector) return -1;
    if (!device->present || device->type == STORAGE_TYPE_OPTICAL) return -1;
    if (device->backend == STORAGE_BACKEND_ATA_PIO) {
        return ata_rw_sector(device->ata_channel, device->ata_drive, lba, out_sector, 0);
    }
    if (device->backend == STORAGE_BACKEND_SDHCI) {
        return sdhci_read_sector(device->pci_bus, device->pci_slot, device->pci_func, lba, out_sector);
    }
    return -1;
}

int storage_write_sector(const storage_device_info_t* device, uint32_t lba, const void* in_sector) {
    if (!device || !in_sector) return -1;
    if (!device->present) return -1;
    if (device->backend == STORAGE_BACKEND_ATA_PIO) {
        return ata_rw_sector(device->ata_channel, device->ata_drive, lba, (void*)in_sector, 1);
    }
    if (device->backend == STORAGE_BACKEND_SDHCI) {
        return sdhci_write_sector(device->pci_bus, device->pci_slot, device->pci_func, lba, in_sector);
    }
    return -1;
}

int storage_flush(const storage_device_info_t* device) {
    uint16_t io_base;

    if (!device) return -1;
    if (!device->present) return -1;

    if (device->backend == STORAGE_BACKEND_SDHCI) {
        return sdhci_flush(device->pci_bus, device->pci_slot, device->pci_func);
    }
    if (device->backend != STORAGE_BACKEND_ATA_PIO) return -1;

    io_base = ata_channels[device->ata_channel].io_base;
    if (!ata_wait_not_busy(io_base)) return -1;
    outb(io_base + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    return ata_wait_not_busy(io_base) ? 0 : -1;
}

const char* storage_backend_name(uint8_t backend) {
    if (backend == STORAGE_BACKEND_ATA_PIO) return "ATA PIO";
    if (backend == STORAGE_BACKEND_IDE) return "IDE";
    if (backend == STORAGE_BACKEND_AHCI) return "AHCI";
    if (backend == STORAGE_BACKEND_NVME) return "NVMe";
    if (backend == STORAGE_BACKEND_SDHCI) return "SDHCI/eMMC";
    if (backend == STORAGE_BACKEND_USB_MASS_STORAGE) return "USB mass storage";
    return "None";
}

const char* storage_install_state_name(uint8_t state) {
    if (state == STORAGE_INSTALL_STATE_READY) return "ready";
    if (state == STORAGE_INSTALL_STATE_CONTROLLER_ONLY) return "controller only";
    if (state == STORAGE_INSTALL_STATE_DRIVER_MISSING) return "driver missing";
    return "unavailable";
}

const char* storage_install_state_reason(const storage_device_info_t* device) {
    if (!device) return "No device";
    if (device->install_state == STORAGE_INSTALL_STATE_READY) return "Block path is ready for direct installs.";
    if (device->backend == STORAGE_BACKEND_SDHCI) {
        if (device->install_state == STORAGE_INSTALL_STATE_CONTROLLER_ONLY) {
            if (device->write_protected) {
                return "eMMC path is readable, but the device reports write protection so installs are blocked.";
            }
            return "SDHCI/eMMC controller is responding, but card initialization or writable media setup is still incomplete.";
        }
        return "SDHCI/eMMC controller detected, but no live eMMC media path came up yet.";
    }
    if (device->backend == STORAGE_BACKEND_USB_MASS_STORAGE) {
        if (device->prog_if == 0x30) return "XHCI controller detected, but xHCI plus USB mass-storage transport are not implemented yet.";
        if (device->prog_if == 0x20) return "EHCI controller detected, but EHCI plus USB mass-storage transport are not implemented yet.";
        if (device->prog_if == 0x10) return "OHCI controller detected, but OHCI plus USB mass-storage transport are not implemented yet.";
        if (device->install_state == STORAGE_INSTALL_STATE_CONTROLLER_ONLY) {
            return "USB host scaffold is recognized, but Bulk-Only Transport and SCSI block commands are still pending.";
        }
        return "USB controller detected, but USB mass-storage and SCSI block transport are not implemented yet.";
    }
    if (device->backend == STORAGE_BACKEND_AHCI) return "AHCI controller detected, but SATA/AHCI block I/O is not implemented yet.";
    if (device->backend == STORAGE_BACKEND_NVME) return "NVMe controller detected, but NVMe queue and namespace support are not implemented yet.";
    if (device->backend == STORAGE_BACKEND_IDE) return "IDE controller detected without a directly enumerated ATA disk path.";
    if (device->type == STORAGE_TYPE_OPTICAL) return "Optical/ATAPI media is not a direct install target.";
    return "No direct install backend is available for this path.";
}

const char* storage_bus_name(uint8_t bus) {
    if (bus == STORAGE_BUS_ATA) return "ATA";
    if (bus == STORAGE_BUS_PCI) return "PCI";
    return "Unknown";
}

const char* storage_type_name(uint8_t type) {
    if (type == STORAGE_TYPE_HDD) return "HDD";
    if (type == STORAGE_TYPE_SSD) return "SSD";
    if (type == STORAGE_TYPE_OPTICAL) return "Optical";
    if (type == STORAGE_TYPE_IDE_CONTROLLER) return "IDE controller";
    if (type == STORAGE_TYPE_AHCI_CONTROLLER) return "AHCI controller";
    if (type == STORAGE_TYPE_NVME_CONTROLLER) return "NVMe controller";
    if (type == STORAGE_TYPE_EMMC_CONTROLLER) return "eMMC/SD controller";
    if (type == STORAGE_TYPE_USB_CONTROLLER) return "USB controller";
    return "Unknown";
}
