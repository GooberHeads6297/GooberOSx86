#include "storage.h"
#include "sdhci.h"
#include "ahci.h"
#include "../io/io.h"
#include "../pci/pci.h"
#include "../pci/iosf_mbi.h"
#include "../usb/storage/msc.h"
#include "../acpi/acpi.h"
#include "../video/intel_gfx.h"
#include "../../lib/string.h"
#include "../../kernel.h"

#define ATA_REG_DATA       0x00
#define ATA_REG_ERROR      0x01
#define ATA_REG_FEATURES   0x01
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
#define ATA_CMD_PACKET           0xA0

#define OPTICAL_BLOCK_SIZE       2048U

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

/*
 * Wait until BSY clears. Stale ERR/DF from a *previous* command must NOT
 * fail this wait -- they clear when a new command is written (OSDev ATA PIO).
 */
static int ata_wait_bsy_clear(uint16_t io_base) {
    int timeout = 100000;
    while (timeout-- > 0) {
        uint8_t status = inb(io_base + ATA_REG_STATUS);
        if (status == 0xFF) return 0; /* floating bus */
        if ((status & ATA_SR_BSY) == 0) return 1;
    }
    return 0;
}

/* After a command: fail on ERR/DF once BSY is clear (post-command result). */
static int ata_wait_not_busy(uint16_t io_base) {
    int timeout = 100000;
    while (timeout-- > 0) {
        uint8_t status = inb(io_base + ATA_REG_STATUS);
        if (status == 0xFF) return 0;
        if ((status & ATA_SR_BSY) == 0) {
            if (status & (ATA_SR_ERR | ATA_SR_DF)) return 0;
            return 1;
        }
    }
    return 0;
}

/*
 * Poll for DRQ after issuing a command. The first few status reads may still
 * show stale ERR from the previous command -- ignore those briefly.
 */
static int ata_wait_drq(uint16_t io_base) {
    int timeout = 100000;
    int polls = 0;
    while (timeout-- > 0) {
        uint8_t status = inb(io_base + ATA_REG_STATUS);
        polls++;
        if (status == 0xFF) return 0;
        if (polls > 4 && (status & (ATA_SR_ERR | ATA_SR_DF))) return 0;
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
    uint16_t aligned[256];
    uint8_t status;
    int i;

    if (channel >= 2 || drive >= 2 || lba > 0x0FFFFFFF) return -1;
    if (!buffer) return -1;

    io_base = ata_channels[channel].io_base;
    ctrl_base = ata_channels[channel].ctrl_base;

    /* Pre-command: only wait for BSY clear (ignore stale ERR). */
    if (!ata_wait_bsy_clear(io_base)) return -1;

    outb(ctrl_base, 0x02); /* nIEN: polling, no IRQs */
    outb(io_base + ATA_REG_HDDEVSEL,
         (uint8_t)(0xE0 | (drive << 4) | ((lba >> 24) & 0x0F)));
    ata_delay(ctrl_base);
    if (!ata_wait_bsy_clear(io_base)) return -1;

    outb(io_base + ATA_REG_SECCOUNT0, 1);
    outb(io_base + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(io_base + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(io_base + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    outb(io_base + ATA_REG_COMMAND, write ? ATA_CMD_WRITE_PIO : ATA_CMD_READ_PIO);

    if (!ata_wait_drq(io_base)) return -1;

    if (write) {
        memcpy(aligned, buffer, 512);
        for (i = 0; i < 256; i++) {
            outw(io_base + ATA_REG_DATA, aligned[i]);
            /* Tiny delay between words (OSDev: avoid REP OUTSW). */
            (void)inb(ctrl_base);
        }
        ata_delay(ctrl_base);
        if (!ata_wait_not_busy(io_base)) return -1;
        status = inb(io_base + ATA_REG_STATUS);
        if (status & (ATA_SR_ERR | ATA_SR_DF)) return -1;
        /* Caller should storage_flush() after bulk writes (install does). */
        return 0;
    }

    for (i = 0; i < 256; i++) aligned[i] = inw(io_base + ATA_REG_DATA);
    memcpy(buffer, aligned, 512);
    ata_delay(ctrl_base);
    return 0;
}

static int ata_packet_send_cdb(uint16_t io_base, uint16_t ctrl_base,
                               uint8_t drive, const uint8_t* cdb) {
    int i;

    if (!ata_wait_not_busy(io_base)) return -1;

    outb(ctrl_base, 0x02);
    outb(io_base + ATA_REG_HDDEVSEL, (uint8_t)(0xA0 | (drive << 4)));
    ata_delay(ctrl_base);
    outb(io_base + ATA_REG_FEATURES, 0);
    outb(io_base + ATA_REG_SECCOUNT0, 0);
    outb(io_base + ATA_REG_LBA0, 0);
    outb(io_base + ATA_REG_LBA1, 0);
    outb(io_base + ATA_REG_LBA2, 0);
    outb(io_base + ATA_REG_COMMAND, ATA_CMD_PACKET);
    if (!ata_wait_drq(io_base)) return -1;

    for (i = 0; i < 6; i++) {
        uint16_t word = (uint16_t)cdb[i * 2] | ((uint16_t)cdb[i * 2 + 1] << 8);
        outw(io_base + ATA_REG_DATA, word);
    }
    return 0;
}

static int ata_packet_read_data(uint16_t io_base, uint16_t* out, size_t words_total) {
    size_t words_read = 0;

    while (words_read < words_total) {
        int timeout = 100000;
        uint8_t status;

        while (timeout-- > 0) {
            status = inb(io_base + ATA_REG_STATUS);
            if (status & (ATA_SR_ERR | ATA_SR_DF)) return -1;
            if ((status & ATA_SR_BSY) == 0) break;
        }
        if (timeout <= 0) return -1;

        status = inb(io_base + ATA_REG_STATUS);
        if (status & (ATA_SR_ERR | ATA_SR_DF)) return -1;
        if (status & ATA_SR_BSY) continue;

        if (status & ATA_SR_DRQ) {
            uint16_t byte_count =
                (uint16_t)((inb(io_base + ATA_REG_LBA2) << 8) |
                           inb(io_base + ATA_REG_LBA1));
            uint16_t words;
            int i;

            if (byte_count == 0) byte_count = OPTICAL_BLOCK_SIZE;
            words = byte_count / 2;
            for (i = 0; i < (int)words && words_read < words_total; i++)
                out[words_read++] = inw(io_base + ATA_REG_DATA);
        } else {
            return (words_read >= words_total) ? 0 : -1;
        }
    }

    ata_wait_not_busy(io_base);
    return 0;
}

static void ata_packet_fill_read_cdb(uint8_t* cdb, uint8_t opcode,
                                     uint32_t lba, uint16_t block_count) {
    memset(cdb, 0, 12);
    cdb[0] = opcode;
    cdb[2] = (uint8_t)((lba >> 24) & 0xFF);
    cdb[3] = (uint8_t)((lba >> 16) & 0xFF);
    cdb[4] = (uint8_t)((lba >> 8) & 0xFF);
    cdb[5] = (uint8_t)(lba & 0xFF);
    if (opcode == 0xA8) {
        cdb[6] = (uint8_t)((block_count >> 24) & 0xFF);
        cdb[7] = (uint8_t)((block_count >> 16) & 0xFF);
        cdb[8] = (uint8_t)((block_count >> 8) & 0xFF);
        cdb[9] = (uint8_t)(block_count & 0xFF);
    } else {
        cdb[8] = (uint8_t)((block_count >> 8) & 0xFF);
        cdb[9] = (uint8_t)(block_count & 0xFF);
    }
}

static int ata_packet_read_cd_blocks(uint8_t channel, uint8_t drive,
                                     uint32_t lba, uint16_t block_count,
                                     void* buffer) {
    uint16_t io_base;
    uint16_t ctrl_base;
    uint8_t cdb[12];
    uint16_t* out;
    size_t words_total;
    static const uint8_t read_ops[] = { 0x28, 0xA8 };
    int opi;

    if (channel >= 2 || drive >= 2 || !buffer || block_count == 0) return -1;

    io_base = ata_channels[channel].io_base;
    ctrl_base = ata_channels[channel].ctrl_base;
    out = (uint16_t*)buffer;
    words_total = (size_t)block_count * (OPTICAL_BLOCK_SIZE / 2);

    for (opi = 0; opi < 2; opi++) {
        ata_packet_fill_read_cdb(cdb, read_ops[opi], lba, block_count);
        if (ata_packet_send_cdb(io_base, ctrl_base, drive, cdb) != 0)
            continue;
        if (ata_packet_read_data(io_base, out, words_total) == 0) {
            ata_delay(ctrl_base);
            return 0;
        }
        ata_wait_not_busy(io_base);
    }

    return -1;
}

static int ata_packet_read_sectors(uint8_t channel, uint8_t drive,
                                   uint32_t lba, uint16_t count, void* buffer) {
    uint16_t io_base;
    uint16_t ctrl_base;
    uint8_t cdb[12];
    uint16_t* out;

    if (channel >= 2 || drive >= 2 || !buffer || count == 0) return -1;

    io_base = ata_channels[channel].io_base;
    ctrl_base = ata_channels[channel].ctrl_base;

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x28;
    cdb[2] = (uint8_t)((lba >> 24) & 0xFF);
    cdb[3] = (uint8_t)((lba >> 16) & 0xFF);
    cdb[4] = (uint8_t)((lba >> 8) & 0xFF);
    cdb[5] = (uint8_t)(lba & 0xFF);
    cdb[8] = (uint8_t)((count >> 8) & 0xFF);
    cdb[9] = (uint8_t)(count & 0xFF);

    if (ata_packet_send_cdb(io_base, ctrl_base, drive, cdb) != 0) return -1;

    out = (uint16_t*)buffer;
    if (ata_packet_read_data(io_base, out, (size_t)count * 256U) != 0) return -1;

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
                dev->sector_size = OPTICAL_BLOCK_SIZE;
                dev->backend = STORAGE_BACKEND_ATA_PIO;
                dev->selectable = 0;
                dev->direct_install_supported = 0;
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

static int storage_pci_slot_used(uint8_t bus, uint8_t slot, uint8_t func) {
    int i;
    for (i = 0; i < storage_device_count; i++) {
        if (!storage_devices[i].present || storage_devices[i].bus != STORAGE_BUS_PCI)
            continue;
        if (storage_devices[i].pci_bus == bus &&
            storage_devices[i].pci_slot == slot &&
            storage_devices[i].pci_func == func)
            return 1;
    }
    return 0;
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
    /* Reference GooberOS: raw BAR0 from config space (BIOS-assigned).
     * AHCI uses BAR5 (ABAR); probe_ahci_device re-reads 0x24. */
    if (backend == STORAGE_BACKEND_AHCI)
        dev->bar0 = pci_read_config_dword(bus, slot, func, 0x24);
    else
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

static int storage_is_intel_sdhci_id(uint16_t vendor_id, uint16_t device_id) {
    if (vendor_id != 0x8086U) return 0;
    return device_id == 0x0F14U || device_id == 0x0F15U || device_id == 0x0F16U ||
           device_id == 0x119A || device_id == 0x119B;
}

static const char* storage_intel_sdhci_label(uint16_t device_id) {
    switch (device_id) {
        case 0x0F14: return "Intel Bay Trail eMMC";
        case 0x0F15: return "Intel Bay Trail SDIO";
        case 0x0F16: return "Intel Bay Trail SD";
        case 0x0F50: return "Intel Bay Trail eMMC (PCI)";
        case 0x0F51: return "Intel Bay Trail SDIO (PCI)";
        case 0x0F52: return "Intel Bay Trail SD (PCI)";
        case 0x119A: return "Intel Braswell eMMC";
        case 0x119B: return "Intel Braswell SD";
        default: return "Intel SDHCI";
    }
}

static int storage_platform_expects_emmc(void) {
    const acpi_touchpad_info_t* acpi;
    if (intel_gfx_is_bay_trail_class()) return 1;
    acpi = acpi_get_touchpad_info();
    if (acpi && (acpi->baytrail_i2c_found || acpi->baytrail_emmc_acpi)) return 1;
    return 0;
}

static int storage_pci_read_id(uint8_t bus, uint8_t slot, uint8_t func,
                               uint16_t* vendor_out, uint16_t* device_out) {
    uint32_t id = pci_read_config_dword(bus, slot, func, 0x00);
    uint16_t ven = (uint16_t)(id & 0xFFFFU);
    uint16_t dev = (uint16_t)((id >> 16) & 0xFFFFU);
    if (vendor_out) *vendor_out = ven;
    if (device_out) *device_out = dev;
    return ven != 0xFFFFU && ven != 0x0000U;
}

/* Enable + re-read only for known Bay Trail LPSS BDFs (never on empty slots). */
static int storage_pci_wake_bdf(uint8_t bus, uint8_t slot, uint8_t func,
                                uint16_t* vendor_out, uint16_t* device_out) {
    uint16_t ven;
    uint16_t dev;

    if (storage_pci_read_id(bus, slot, func, &ven, &dev)) {
        if (vendor_out) *vendor_out = ven;
        if (device_out) *device_out = dev;
        return 1;
    }

    pci_enable_device(bus, slot, func);
    if (storage_pci_read_id(bus, slot, func, &ven, &dev)) {
        if (vendor_out) *vendor_out = ven;
        if (device_out) *device_out = dev;
        return 1;
    }
    return 0;
}

static void storage_add_intel_sdhci(uint8_t bus, uint8_t slot, uint8_t func,
                                    uint16_t vendor_id, uint16_t device_id) {
    uint8_t base_class;
    uint8_t sub_class;
    uint8_t prog_if;

    if (storage_pci_slot_used(bus, slot, func)) return;

    base_class = pci_read_config_byte(bus, slot, func, 0x0B);
    sub_class = pci_read_config_byte(bus, slot, func, 0x0A);
    prog_if = pci_read_config_byte(bus, slot, func, 0x09);

    add_pci_storage_device(bus, slot, func,
                           STORAGE_TYPE_EMMC_CONTROLLER,
                           STORAGE_BACKEND_SDHCI,
                           STORAGE_INSTALL_STATE_DRIVER_MISSING,
                           base_class, sub_class, prog_if);
    {
        storage_device_info_t* d = &storage_devices[storage_device_count - 1];
        if (d->present) {
            strcpy(d->model, storage_intel_sdhci_label(device_id));
            d->vendor_id = vendor_id;
            d->device_id = device_id;
        }
    }
}

/*
 * Bay Trail eMMC is usually 8086:0F14. Do NOT poke LPC/PMCSR on empty slots —
 * that regressed detection on 80M4. Only read config; match by ID or class.
 */
static void scan_intel_sdhci_by_id(void) {
    static const uint16_t ids[] = {
        0x0F14, 0x0F15, 0x0F16, 0x119A, 0x119B,
        /* Broader Valleyview / Cherry Trail SDHCI-ish IDs seen in the wild */
        0x0F50, 0x0F51, 0x0F52,
    };
    uint16_t bus;
    uint8_t slot, func;
    size_t k;

    for (bus = 0; bus < 256; bus++) {
        for (slot = 0; slot < 32; slot++) {
            for (func = 0; func < 8; func++) {
                uint32_t id = pci_read_config_dword((uint8_t)bus, slot, func, 0x00);
                uint16_t ven = (uint16_t)(id & 0xFFFFU);
                uint16_t dev = (uint16_t)((id >> 16) & 0xFFFFU);
                if (ven != 0x8086U) continue;
                if (storage_pci_slot_used((uint8_t)bus, slot, func)) continue;
                for (k = 0; k < sizeof(ids) / sizeof(ids[0]); k++) {
                    if (dev != ids[k]) continue;
                    storage_add_intel_sdhci((uint8_t)bus, slot, func, ven, dev);
                    break;
                }
            }
        }
    }
}

static void probe_ahci_device(storage_device_info_t* dev) {
    ahci_probe_result_t probe;
    uint32_t abar;

    if (!dev || dev->backend != STORAGE_BACKEND_AHCI) return;
    if (dev->install_state == STORAGE_INSTALL_STATE_READY && dev->sectors != 0)
        return;

    pci_enable_device(dev->pci_bus, dev->pci_slot, dev->pci_func);
    /* AHCI ABAR is BAR5 (offset 0x24). */
    abar = pci_read_config_dword(dev->pci_bus, dev->pci_slot, dev->pci_func, 0x24);
    if (abar == 0 || abar == 0xFFFFFFFFU)
        abar = dev->bar0;

    if (ahci_probe_pci_controller(dev->pci_bus, dev->pci_slot, dev->pci_func,
                                  abar, &probe) &&
        probe.initialized && probe.sector_count != 0) {
        dev->bar0 = probe.abar;
        dev->ata_channel = probe.port; /* reuse field as AHCI port index */
        dev->sectors = probe.sector_count;
        dev->sector_size = probe.sector_size ? probe.sector_size : 512;
        dev->selectable = 1;
        dev->direct_install_supported = 1;
        dev->install_state = STORAGE_INSTALL_STATE_READY;
        dev->type = STORAGE_TYPE_SSD;
        strcpy(dev->model, probe.model[0] ? probe.model : "AHCI disk");
        print("Storage: AHCI ready ");
        print(dev->location);
        print(" port=");
        {
            char b[8];
            itoa((int)probe.port, b, 10);
            print(b);
        }
        print("\n");
    } else {
        dev->install_state = STORAGE_INSTALL_STATE_CONTROLLER_ONLY;
        strcpy(dev->model, "AHCI controller");
    }
}

static void probe_sdhci_device(storage_device_info_t* dev) {
    sdhci_probe_result_t probe;
    uint32_t bar0;
    int ok;

    if (!dev || dev->backend != STORAGE_BACKEND_SDHCI) return;
    if (dev->install_state == STORAGE_INSTALL_STATE_READY && dev->sectors != 0)
        return;

    bar0 = dev->bar0;
    if (dev->pci_bus == 0xFFU) {
        /* ACPI / fixed-MMIO path — no PCI config. */
        ok = sdhci_probe_mmio(bar0, 1, 0, 0x1E, 0, &probe);
        if (ok) {
            dev->pci_bus = 0;
            dev->pci_slot = 0x1E;
            dev->pci_func = 0;
        }
    } else {
        pci_enable_device(dev->pci_bus, dev->pci_slot, dev->pci_func);
        if (bar0 == 0 || bar0 == 0xFFFFFFFFU)
            bar0 = pci_read_config_dword(dev->pci_bus, dev->pci_slot, dev->pci_func, 0x10);
        dev->bar0 = bar0;
        ok = sdhci_probe_pci_controller(dev->pci_bus, dev->pci_slot, dev->pci_func,
                                        bar0, &probe);
    }

    if (ok) {
        dev->write_protected = probe.write_protected;
        dev->init_step = probe.init_step;
        dev->last_status = probe.last_status;
        dev->sector_size = 512;
        dev->sectors = probe.sector_count;
        if (probe.initialized && probe.sector_count != 0) {
            dev->selectable = probe.write_protected ? 0 : 1;
            dev->direct_install_supported = probe.write_protected ? 0 : 1;
            dev->install_state = probe.write_protected
                ? STORAGE_INSTALL_STATE_CONTROLLER_ONLY
                : STORAGE_INSTALL_STATE_READY;
            strcpy(dev->model, "eMMC device");
            print("Storage: eMMC ready ");
            print(dev->location);
            print(" sectors=");
            { char b[16]; itoa((int)probe.sector_count, b, 10); print(b); }
            print("\n");
        } else if (probe.card_present) {
            dev->install_state = STORAGE_INSTALL_STATE_CONTROLLER_ONLY;
            strcpy(dev->model, "SDHCI/eMMC slot");
            print("Storage: eMMC probe incomplete ");
            print(dev->location);
            print(" step=");
            { char b[16]; itoa((int)probe.init_step, b, 10); print(b); }
            print(" status=");
            { char b[16]; itoa((int)probe.last_status, b, 16); print(b); }
            print("\n");
        } else {
            dev->install_state = STORAGE_INSTALL_STATE_DRIVER_MISSING;
            strcpy(dev->model, "SDHCI controller");
        }
    } else {
        dev->init_step = probe.init_step;
        dev->last_status = probe.last_status;
        dev->install_state = STORAGE_INSTALL_STATE_DRIVER_MISSING;
        strcpy(dev->model, "SDHCI probe failed");
    }
}

static void probe_pci_storage_backend(storage_device_info_t* dev) {
    if (!dev || dev->bus != STORAGE_BUS_PCI) return;

    if (dev->backend == STORAGE_BACKEND_SDHCI) {
        probe_sdhci_device(dev);
        return;
    }

    if (dev->backend == STORAGE_BACKEND_AHCI) {
        probe_ahci_device(dev);
        return;
    }

    if (dev->backend == STORAGE_BACKEND_USB_MASS_STORAGE) {
        usb_msc_probe_result_t probe;
        if (usb_msc_probe_pci_controller(dev->prog_if, dev->bar0, &probe)) {
            if (probe.transport_scaffold_ready)
                dev->install_state = STORAGE_INSTALL_STATE_CONTROLLER_ONLY;
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
    int i;
    int sdhci_seen = 0;
    int emmc_ready = 0;
    int ahci_ready = 0;

    for (i = 0; i < storage_device_count; i++) {
        if (storage_devices[i].backend == STORAGE_BACKEND_SDHCI) {
            sdhci_seen = 1;
            if (storage_devices[i].install_state == STORAGE_INSTALL_STATE_READY &&
                storage_devices[i].selectable)
                emmc_ready = 1;
        }
        if (storage_devices[i].backend == STORAGE_BACKEND_AHCI &&
            storage_devices[i].install_state == STORAGE_INSTALL_STATE_READY)
            ahci_ready = 1;
    }

    print("Storage scan: ");
    itoa(storage_device_count, buf, 10);
    print(buf);
    print(" path(s), ");
    itoa(storage_target_count(), buf, 10);
    print(buf);
    print(" install target(s).\n");
#ifdef __x86_64__
    if (emmc_ready)
        print("Storage: eMMC ready (`install list`).\n");
    else if (sdhci_seen)
        print("Storage: SDHCI found; eMMC probe incomplete (`install list`).\n");
    if (ahci_ready)
        print("Storage: AHCI/SATA ready (`install list`).\n");
#endif
}

static int storage_has_sdhci(void) {
    int i;
    for (i = 0; i < storage_device_count; i++) {
        if (storage_devices[i].present &&
            storage_devices[i].backend == STORAGE_BACKEND_SDHCI)
            return 1;
    }
    return 0;
}

/*
 * Lenovo 80M4 / Bay Trail: firmware hides 8086:0F14 from PCI (ACPI mode).
 * 1) Clear SCC_MMC_CTL PCI_CFG_DIS via IOSF so the device reappears.
 * 2) If still missing, probe ACPI Memory32Fixed bases for HID 80860F14.
 */
static void storage_baytrail_emmc_bringup(void) {
    uint32_t before = 0;
    uint32_t after = 0;
    const acpi_touchpad_info_t* acpi;
    char buf[16];
    int i;

    if (!iosf_mbi_available() && !storage_platform_expects_emmc())
        return;

    if (iosf_mbi_available()) {
        if (iosf_baytrail_scc_enable_pci_emmc(&before, &after)) {
            print("storage: IOSF SCC_MMC_CTL ");
            itoa((int)before, buf, 16); print(buf);
            print(" -> ");
            itoa((int)after, buf, 16); print(buf);
            print("\n");
            if ((before & SCC_CTL_PCI_CFG_DIS) != 0) {
                print("storage: restored Bay Trail eMMC PCI config\n");
                /* Only re-scan for newly visible SDHCI IDs — avoid duplicating USB. */
                scan_intel_sdhci_by_id();
                /* Also pick up class 08/05 if firmware exposes it that way. */
                {
                    uint16_t bus;
                    uint8_t slot, func;
                    for (bus = 0; bus < 256; bus++) {
                        for (slot = 0; slot < 32; slot++) {
                            for (func = 0; func < 8; func++) {
                                uint16_t vendor_id;
                                uint8_t base_class, sub_class, prog_if;
                                if (storage_pci_slot_used((uint8_t)bus, slot, func)) continue;
                                vendor_id = pci_read_config_word((uint8_t)bus, slot, func, 0x00);
                                if (vendor_id == 0xFFFF) continue;
                                base_class = pci_read_config_byte((uint8_t)bus, slot, func, 0x0B);
                                sub_class = pci_read_config_byte((uint8_t)bus, slot, func, 0x0A);
                                prog_if = pci_read_config_byte((uint8_t)bus, slot, func, 0x09);
                                if (base_class == 0x08 && sub_class == 0x05) {
                                    add_pci_storage_device((uint8_t)bus, slot, func,
                                        STORAGE_TYPE_EMMC_CONTROLLER, STORAGE_BACKEND_SDHCI,
                                        STORAGE_INSTALL_STATE_DRIVER_MISSING,
                                        base_class, sub_class, prog_if);
                                }
                            }
                        }
                    }
                }
            }
        } else {
            print("storage: IOSF SCC_MMC_CTL read/write failed\n");
        }
    }

    if (storage_has_sdhci()) return;

    acpi = acpi_get_touchpad_info();
    if (!acpi) return;

    if (acpi->baytrail_emmc_acpi)
        print("storage: ACPI HID 80860F14 present; trying MMIO candidates\n");

    for (i = 0; i < acpi->emmc_mmio_count; i++) {
        storage_device_info_t* dev;
        if (storage_device_count >= STORAGE_MAX_DEVICES) break;
        if (acpi->emmc_mmio[i] == 0) continue;

        print("storage: ACPI eMMC MMIO ");
        itoa((int)acpi->emmc_mmio[i], buf, 16);
        print(buf);
        print("\n");

        dev = add_storage_device();
        if (!dev) break;
        dev->bus = STORAGE_BUS_PCI;
        dev->type = STORAGE_TYPE_EMMC_CONTROLLER;
        dev->backend = STORAGE_BACKEND_SDHCI;
        dev->install_state = STORAGE_INSTALL_STATE_DRIVER_MISSING;
        dev->pci_bus = 0xFF; /* MMIO-only sentinel */
        dev->pci_slot = 0x1E;
        dev->pci_func = 0;
        dev->vendor_id = 0x8086;
        dev->device_id = 0x0F14;
        dev->bar0 = acpi->emmc_mmio[i];
        dev->class_code = 0x08;
        dev->sub_class = 0x05;
        strcpy(dev->model, "Intel Bay Trail eMMC (ACPI)");
        strcpy(dev->location, "acpi 80860F14");
    }
}

void storage_scan(void) {
    const boot_config_t* cfg = boot_get_config();
    int storage_off = cfg && cfg->storage[0] && strcmp(cfg->storage, "off") == 0;

    memset(storage_devices, 0, sizeof(storage_devices));
    storage_device_count = 0;

    scan_ata_devices();
    if (!storage_off) {
        /* Reference GooberOS order: class scan, then Intel ID match, then probe. */
        scan_pci_storage_controllers();
        scan_intel_sdhci_by_id();
        if (!storage_has_sdhci())
            storage_baytrail_emmc_bringup();
        for (int i = 0; i < storage_device_count; i++) {
            probe_pci_storage_backend(&storage_devices[i]);
        }
    }
}

/*
 * Print every PCI function that looks like storage/USB or Intel 0Fxx so we can
 * see where Bay Trail eMMC actually lives when `devices` only shows USB.
 */
void storage_print_pci_inventory(void) {
    int n = 0;
    print("PCI inventory (storage/USB/Intel 0Fxx):\n");
    print("  build=2026-07-10-emmc-uefi4\n");
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t id = pci_read_config_dword((uint8_t)bus, slot, func, 0x00);
                uint16_t ven = (uint16_t)(id & 0xFFFFU);
                uint16_t dev = (uint16_t)((id >> 16) & 0xFFFFU);
                uint8_t base_class, sub_class, prog_if;
                uint32_t bar0;
                char buf[16];
                int interesting;

                if (ven == 0xFFFFU || ven == 0) continue;
                base_class = pci_read_config_byte((uint8_t)bus, slot, func, 0x0B);
                sub_class = pci_read_config_byte((uint8_t)bus, slot, func, 0x0A);
                prog_if = pci_read_config_byte((uint8_t)bus, slot, func, 0x09);
                interesting =
                    (base_class == 0x01) ||
                    (base_class == 0x08) ||
                    (base_class == 0x0C && sub_class == 0x03) ||
                    (ven == 0x8086U && (dev & 0xFF00U) == 0x0F00U);
                if (!interesting) continue;

                bar0 = pci_read_config_dword((uint8_t)bus, slot, func, 0x10);
                print("  ");
                itoa((int)bus, buf, 10); print(buf); print(":");
                itoa((int)slot, buf, 10); print(buf); print(":");
                itoa((int)func, buf, 10); print(buf);
                print("  ");
                itoa((int)ven, buf, 16); print(buf); print(":");
                itoa((int)dev, buf, 16); print(buf);
                print("  class ");
                itoa((int)base_class, buf, 16); print(buf); print("/");
                itoa((int)sub_class, buf, 16); print(buf); print(".");
                itoa((int)prog_if, buf, 16); print(buf);
                print("  bar0=");
                itoa((int)bar0, buf, 16); print(buf);
                if (ven == 0x8086U && (dev == 0x0F14U || dev == 0x0F15U || dev == 0x0F16U))
                    print("  <-- eMMC/SD candidate");
                if (base_class == 0x08 && sub_class == 0x05)
                    print("  <-- SDHCI class");
                print("\n");
                n++;
            }
        }
    }
    if (n == 0) print("  (none)\n");
}

void storage_probe_sdhci(void) {
    int i;
    for (i = 0; i < storage_device_count; i++) {
        if (storage_devices[i].present &&
            storage_devices[i].backend == STORAGE_BACKEND_SDHCI)
            probe_sdhci_device(&storage_devices[i]);
    }
}

void storage_print_hw_summary(void) {
    int i;
    int n = 0;

    print("SDHCI/eMMC controllers:\n");
    for (i = 0; i < storage_device_count; i++) {
        const storage_device_info_t* d = &storage_devices[i];
        char buf[16];
        if (!d->present || d->backend != STORAGE_BACKEND_SDHCI) continue;
        n++;
        print("  ");
        print(d->location);
        print(" ");
        itoa((int)d->vendor_id, buf, 16); print(buf); print(":");
        itoa((int)d->device_id, buf, 16); print(buf);
        print(" ");
        print(storage_install_state_name(d->install_state));
        if (d->install_state == STORAGE_INSTALL_STATE_READY) {
            print(" sectors=");
            itoa((int)d->sectors, buf, 10);
            print(buf);
        } else if (d->init_step) {
            print(" step=");
            itoa((int)d->init_step, buf, 10);
            print(buf);
        }
        print("\n");
    }
    if (n == 0) {
        print("  (none -- expected 8086:0F14 on Bay Trail eMMC laptops)\n");
        if (storage_platform_expects_emmc()) {
            uint16_t ven = 0;
            uint16_t dev = 0;
            print("  Bay Trail eMMC probe at pci 0:30:0 raw id=");
            if (storage_pci_wake_bdf(0, 30, 0, &ven, &dev)) {
                char buf[16];
                itoa((int)ven, buf, 16); print(buf); print(":");
                itoa((int)dev, buf, 16); print(buf);
            } else {
                print("unreadable");
            }
            print("\n");
        }
    }
}

void storage_init(void) {
    print("storage: build=2026-07-10-emmc-uefi4\n");
    storage_scan();
    if (!storage_initialized) {
        print_scan_summary();
        storage_print_hw_summary();
        storage_print_pci_inventory();
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
    if (!device->present) return -1;
    if (device->type == STORAGE_TYPE_OPTICAL) return -1;
    if (device->backend == STORAGE_BACKEND_ATA_PIO) {
        return ata_rw_sector(device->ata_channel, device->ata_drive, lba, out_sector, 0);
    }
    if (device->backend == STORAGE_BACKEND_SDHCI) {
        return sdhci_read_sector(device->pci_bus, device->pci_slot, device->pci_func, lba, out_sector);
    }
    if (device->backend == STORAGE_BACKEND_AHCI) {
        return ahci_read_sector(device->pci_bus, device->pci_slot, device->pci_func,
                                device->ata_channel, lba, out_sector);
    }
    return -1;
}

int storage_read_optical_sector(const storage_device_info_t* device, uint32_t lba,
                                void* out_sector) {
    if (!device || !out_sector) return -1;
    if (!device->present || device->type != STORAGE_TYPE_OPTICAL) return -1;
    if (device->backend != STORAGE_BACKEND_ATA_PIO) return -1;
    return ata_packet_read_cd_blocks(device->ata_channel, device->ata_drive,
                                     lba, 1, out_sector);
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
    if (device->backend == STORAGE_BACKEND_AHCI) {
        return ahci_write_sector(device->pci_bus, device->pci_slot, device->pci_func,
                                 device->ata_channel, lba, in_sector);
    }
    return -1;
}

int storage_flush(const storage_device_info_t* device) {
    uint16_t io_base;
    uint16_t ctrl_base;

    if (!device) return -1;
    if (!device->present) return -1;

    if (device->backend == STORAGE_BACKEND_SDHCI) {
        return sdhci_flush(device->pci_bus, device->pci_slot, device->pci_func);
    }
    if (device->backend == STORAGE_BACKEND_AHCI) {
        return ahci_flush(device->pci_bus, device->pci_slot, device->pci_func,
                          device->ata_channel);
    }
    if (device->backend != STORAGE_BACKEND_ATA_PIO) return -1;

    io_base = ata_channels[device->ata_channel].io_base;
    ctrl_base = ata_channels[device->ata_channel].ctrl_base;
    if (!ata_wait_bsy_clear(io_base)) return -1;
    outb(ctrl_base, 0x02);
    outb(io_base + ATA_REG_HDDEVSEL,
         (uint8_t)(0xE0 | (device->ata_drive << 4)));
    ata_delay(ctrl_base);
    if (!ata_wait_bsy_clear(io_base)) return -1;
    outb(io_base + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    if (!ata_wait_bsy_clear(io_base)) return -1;
    {
        uint8_t status = inb(io_base + ATA_REG_STATUS);
        if (status & (ATA_SR_ERR | ATA_SR_DF)) return -1;
    }
    return 0;
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
    if (device->backend == STORAGE_BACKEND_AHCI) {
        return "AHCI controller detected; no SATA disk came up on a port yet.";
    }
    if (device->backend == STORAGE_BACKEND_NVME) {
        return "NVMe path: controller inventoried; admin/I/O queue scaffold pending "
               "(FAT32 install will work once namespace block I/O is implemented).";
    }
    if (device->backend == STORAGE_BACKEND_USB_MASS_STORAGE) {
        if (device->prog_if == 0x30) return "XHCI + USB MSC: host present; BOT/SCSI block I/O pending for FAT32 install.";
        if (device->prog_if == 0x20) return "EHCI + USB MSC: host present; BOT/SCSI block I/O pending for FAT32 install.";
        if (device->prog_if == 0x10) return "OHCI + USB MSC: host present; BOT/SCSI block I/O pending for FAT32 install.";
        if (device->install_state == STORAGE_INSTALL_STATE_CONTROLLER_ONLY) {
            return "USB host scaffold recognized; Bulk-Only Transport + SCSI pending for FAT32 install.";
        }
        return "USB MSC: controller detected; mass-storage block transport pending for FAT32 install.";
    }
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
