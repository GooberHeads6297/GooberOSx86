#include "ahci.h"
#include "../pci/pci.h"
#include "../../lib/string.h"

extern void print(const char*);

#define PCI_COMMAND_OFFSET 0x04
#define PCI_COMMAND_MEMORY 0x0002
#define PCI_COMMAND_BUSMASTER 0x0004

#define AHCI_MAX_HBAS 2
#define AHCI_MAX_PORTS 32

#define AHCI_GHC_AE   (1U << 31)
#define AHCI_GHC_IE   (1U << 1)
#define AHCI_GHC_HR   (1U << 0)

#define AHCI_CAP_S64A (1U << 31)
#define AHCI_CAP_NCS(c) ((((c) >> 8) & 0x1FU) + 1U)

#define AHCI_PORT_CMD_ST  (1U << 0)
#define AHCI_PORT_CMD_FRE (1U << 4)
#define AHCI_PORT_CMD_FR  (1U << 14)
#define AHCI_PORT_CMD_CR  (1U << 15)
#define AHCI_PORT_CMD_CLO (1U << 3)

#define AHCI_PORT_TFD_ERR (1U << 0)
#define AHCI_PORT_TFD_DRQ (1U << 3)
#define AHCI_PORT_TFD_BSY (1U << 7)

#define AHCI_SSTS_DET_MASK 0x0FU
#define AHCI_SSTS_DET_PRESENT 0x03U

#define ATA_CMD_IDENTIFY    0xEC
#define ATA_CMD_READ_DMA    0xC8
#define ATA_CMD_WRITE_DMA   0xCA
#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_CACHE_FLUSH 0xE7

#define ATA_DEV_BUSY 0x80
#define ATA_DEV_DRQ  0x08

typedef struct {
    uint8_t active;
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint8_t port;
    uint8_t use_dma;
    uint32_t abar;
    uint32_t sector_count;
} ahci_disk_t;

static ahci_disk_t g_disks[AHCI_MAX_HBAS * 4];
static int g_disk_count;

/* Identity-mapped DMA scratch (must stay below 4 GiB physically). */
static uint8_t g_clb[1024] __attribute__((aligned(1024)));
static uint8_t g_fb[256] __attribute__((aligned(256)));
static uint8_t g_ctba[256] __attribute__((aligned(128)));
static uint8_t g_prdt_buf[512] __attribute__((aligned(4)));

static uint32_t mmio_r32(uint32_t base, uint32_t off) {
    return *(volatile uint32_t*)(uintptr_t)(base + off);
}

static void mmio_w32(uint32_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t*)(uintptr_t)(base + off) = val;
}

static uint32_t port_base(uint32_t abar, uint8_t port) {
    return abar + 0x100U + (uint32_t)port * 0x80U;
}

static void ahci_spin(uint32_t n) {
    for (uint32_t i = 0; i < n; i++) { }
}

static int ahci_wait_clear(uint32_t pb, uint32_t reg, uint32_t mask) {
    for (uint32_t i = 0; i < 50000U; i++) {
        if ((mmio_r32(pb, reg) & mask) == 0) return 1;
    }
    return 0;
}

static int ahci_stop_port(uint32_t pb) {
    uint32_t cmd = mmio_r32(pb, 0x18);
    cmd &= ~(AHCI_PORT_CMD_ST | AHCI_PORT_CMD_FRE);
    mmio_w32(pb, 0x18, cmd);
    if (!ahci_wait_clear(pb, 0x18, AHCI_PORT_CMD_FR)) return 0;
    if (!ahci_wait_clear(pb, 0x18, AHCI_PORT_CMD_CR)) return 0;
    return 1;
}

static int ahci_start_port(uint32_t pb) {
    uint32_t cmd;
    if (!ahci_wait_clear(pb, 0x18, AHCI_PORT_CMD_CR)) return 0;
    cmd = mmio_r32(pb, 0x18);
    cmd |= AHCI_PORT_CMD_FRE;
    mmio_w32(pb, 0x18, cmd);
    cmd |= AHCI_PORT_CMD_ST;
    mmio_w32(pb, 0x18, cmd);
    return 1;
}

static void trim_model(char* text) {
    size_t start = 0;
    size_t len = strlen(text);
    size_t out = 0;
    while (start < len && text[start] == ' ') start++;
    while (len > start && text[len - 1] == ' ') len--;
    for (size_t i = start; i < len; i++) text[out++] = text[i];
    text[out] = '\0';
    if (text[0] == '\0') strcpy(text, "AHCI disk");
}

static ahci_disk_t* ahci_find(uint8_t bus, uint8_t slot, uint8_t func, uint8_t port) {
    for (int i = 0; i < g_disk_count; i++) {
        if (g_disks[i].active &&
            g_disks[i].bus == bus &&
            g_disks[i].slot == slot &&
            g_disks[i].func == func &&
            g_disks[i].port == port)
            return &g_disks[i];
    }
    return 0;
}

static int ahci_issue_command(ahci_disk_t* disk,
                              uint8_t ata_cmd,
                              uint32_t lba,
                              uint16_t count,
                              void* buffer,
                              int write,
                              int is_identify) {
    uint32_t pb;
    uint32_t* cl;
    uint32_t* prdt;
    uint8_t* cfis;
    uint32_t ci;
    uint32_t tfd;
    uint32_t spins;

    if (!disk) return 0;
    pb = port_base(disk->abar, disk->port);

    if (!ahci_stop_port(pb)) return 0;

    memset(g_clb, 0, sizeof(g_clb));
    memset(g_fb, 0, sizeof(g_fb));
    memset(g_ctba, 0, sizeof(g_ctba));
    if (buffer && !is_identify)
        memcpy(g_prdt_buf, buffer, write ? 512 : 0);
    else
        memset(g_prdt_buf, 0, sizeof(g_prdt_buf));

    mmio_w32(pb, 0x00, (uint32_t)(uintptr_t)g_clb);
    mmio_w32(pb, 0x04, 0);
    mmio_w32(pb, 0x08, (uint32_t)(uintptr_t)g_fb);
    mmio_w32(pb, 0x0C, 0);
    mmio_w32(pb, 0x10, 0xFFFFFFFFU); /* clear IS */
    mmio_w32(pb, 0x30, 0xFFFFFFFFU); /* clear SERR */

    cl = (uint32_t*)g_clb;
    prdt = (uint32_t*)(g_ctba + 0x80);
    cfis = (uint8_t*)g_ctba;

    /* Command header slot 0 */
    cl[0] = (uint32_t)(5) | (write ? (1U << 6) : 0) | (1U << 16); /* CFL=5 dwords, W, PRDTL=1 */
    cl[1] = 0;
    cl[2] = (uint32_t)(uintptr_t)g_ctba;
    cl[3] = 0;

    /* PRDT entry 0 */
    prdt[0] = (uint32_t)(uintptr_t)g_prdt_buf;
    prdt[1] = 0;
    prdt[2] = 0;
    prdt[3] = (512U - 1U) | (1U << 31); /* DBC + I */

    memset(cfis, 0, 64);
    cfis[0] = 0x27; /* H2D Register FIS */
    cfis[1] = 1 << 7; /* command */
    cfis[2] = ata_cmd;
    if (!is_identify) {
        cfis[4] = (uint8_t)(lba & 0xFF);
        cfis[5] = (uint8_t)((lba >> 8) & 0xFF);
        cfis[6] = (uint8_t)((lba >> 16) & 0xFF);
        cfis[7] = 0x40; /* LBA mode */
        cfis[8] = (uint8_t)((lba >> 24) & 0x0F);
        cfis[12] = (uint8_t)(count & 0xFF);
        cfis[13] = (uint8_t)((count >> 8) & 0xFF);
    } else {
        cfis[7] = 0xA0;
    }

    if (!ahci_start_port(pb)) return 0;

    /* Wait not busy */
    for (spins = 0; spins < 50000U; spins++) {
        tfd = mmio_r32(pb, 0x20);
        if ((tfd & (AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ)) == 0) break;
    }
    if (spins >= 50000U) return 0;

    mmio_w32(pb, 0x38, 1U); /* CI slot 0 */

    for (spins = 0; spins < 200000U; spins++) {
        ci = mmio_r32(pb, 0x38);
        tfd = mmio_r32(pb, 0x20);
        if ((ci & 1U) == 0) break;
        if (tfd & AHCI_PORT_TFD_ERR) {
            ahci_stop_port(pb);
            return 0;
        }
    }
    if (ci & 1U) {
        ahci_stop_port(pb);
        return 0;
    }

    if (buffer && !write)
        memcpy(buffer, g_prdt_buf, 512);

    ahci_stop_port(pb);
    return 1;
}

static int ahci_identify_port(uint32_t abar, uint8_t port,
                              ahci_probe_result_t* out, ahci_disk_t* disk) {
    uint16_t identify[256];
    uint32_t sectors28;
    uint64_t sectors48;

    disk->abar = abar;
    disk->port = port;
    disk->use_dma = 1;

    if (!ahci_issue_command(disk, ATA_CMD_IDENTIFY, 0, 0, identify, 0, 1)) {
        disk->use_dma = 0;
        if (!ahci_issue_command(disk, ATA_CMD_IDENTIFY, 0, 0, identify, 0, 1))
            return 0;
    }

    if (identify[0] & 0x8000U) return 0; /* ATAPI */

    {
        int pos = 0;
        for (int i = 27; i <= 46; i++) {
            out->model[pos++] = (char)((identify[i] >> 8) & 0xFF);
            out->model[pos++] = (char)(identify[i] & 0xFF);
        }
        out->model[pos] = '\0';
        trim_model(out->model);
    }

    sectors28 = ((uint32_t)identify[61] << 16) | identify[60];
    sectors48 = 0;
    if (identify[83] & (1U << 10)) {
        sectors48 =
            (uint64_t)identify[100] |
            ((uint64_t)identify[101] << 16) |
            ((uint64_t)identify[102] << 32) |
            ((uint64_t)identify[103] << 48);
    }
    out->sector_count = (uint32_t)(sectors48 ? sectors48 : sectors28);
    out->sector_size = 512;
    out->port = port;
    out->is_atapi = 0;
    out->initialized = out->sector_count != 0;
    disk->sector_count = out->sector_count;
    return out->initialized;
}

int ahci_probe_pci_controller(uint8_t bus,
                              uint8_t slot,
                              uint8_t func,
                              uint32_t abar_bar5,
                              ahci_probe_result_t* out) {
    uint16_t cmd;
    uint32_t abar;
    uint32_t cap;
    uint32_t pi;
    uint32_t ghci;
    uint8_t port;

    if (!out) return 0;
    memset(out, 0, sizeof(*out));

    if ((abar_bar5 & 0x1U) != 0) return 0;
    abar = abar_bar5 & ~0xFU;
    if (abar == 0 || abar == 0xFFFFFFF0U) return 0;

    cmd = pci_read_config_word(bus, slot, func, PCI_COMMAND_OFFSET);
    cmd = (uint16_t)(cmd | PCI_COMMAND_MEMORY | PCI_COMMAND_BUSMASTER);
    pci_write_config_word(bus, slot, func, PCI_COMMAND_OFFSET, cmd);

    {
        uint32_t id = pci_read_config_dword(bus, slot, func, 0x00);
        out->vendor_id = (uint16_t)(id & 0xFFFFU);
        out->device_id = (uint16_t)((id >> 16) & 0xFFFFU);
    }
    out->abar = abar;

    /* Enable AHCI mode */
    ghci = mmio_r32(abar, 0x04);
    if ((ghci & AHCI_GHC_AE) == 0) {
        mmio_w32(abar, 0x04, ghci | AHCI_GHC_AE);
        ahci_spin(10000U);
    }

    cap = mmio_r32(abar, 0x00);
    pi = mmio_r32(abar, 0x0C);
    (void)cap;

    for (port = 0; port < AHCI_MAX_PORTS; port++) {
        uint32_t pb;
        uint32_t ssts;
        uint32_t sig;
        ahci_disk_t* disk;

        if ((pi & (1U << port)) == 0) continue;
        pb = port_base(abar, port);
        ssts = mmio_r32(pb, 0x28);
        if ((ssts & AHCI_SSTS_DET_MASK) != AHCI_SSTS_DET_PRESENT) continue;

        sig = mmio_r32(pb, 0x24);
        if (sig == 0xEB140101U) continue; /* ATAPI */

        if (g_disk_count >= (int)(sizeof(g_disks) / sizeof(g_disks[0]))) break;
        disk = &g_disks[g_disk_count];
        memset(disk, 0, sizeof(*disk));
        disk->bus = bus;
        disk->slot = slot;
        disk->func = func;

        if (!ahci_identify_port(abar, port, out, disk)) continue;

        disk->active = 1;
        g_disk_count++;
        print("ahci: disk ready port ");
        {
            char b[8];
            itoa((int)port, b, 10);
            print(b);
        }
        print(" sectors=");
        {
            char b[16];
            itoa((int)out->sector_count, b, 10);
            print(b);
        }
        print("\n");
        return 1;
    }
    return 0;
}

int ahci_read_sector(uint8_t bus, uint8_t slot, uint8_t func,
                     uint8_t port, uint32_t lba, void* out_sector) {
    ahci_disk_t* disk = ahci_find(bus, slot, func, port);
    if (!disk || !out_sector) return -1;
    if (disk->sector_count && lba >= disk->sector_count) return -1;
    if (ahci_issue_command(disk, ATA_CMD_READ_DMA, lba, 1, out_sector, 0, 0))
        return 0;
    if (ahci_issue_command(disk, ATA_CMD_READ_PIO, lba, 1, out_sector, 0, 0))
        return 0;
    return -1;
}

int ahci_write_sector(uint8_t bus, uint8_t slot, uint8_t func,
                      uint8_t port, uint32_t lba, const void* in_sector) {
    ahci_disk_t* disk = ahci_find(bus, slot, func, port);
    if (!disk || !in_sector) return -1;
    if (disk->sector_count && lba >= disk->sector_count) return -1;
    if (ahci_issue_command(disk, ATA_CMD_WRITE_DMA, lba, 1, (void*)in_sector, 1, 0))
        return 0;
    if (ahci_issue_command(disk, ATA_CMD_WRITE_PIO, lba, 1, (void*)in_sector, 1, 0))
        return 0;
    return -1;
}

int ahci_flush(uint8_t bus, uint8_t slot, uint8_t func, uint8_t port) {
    ahci_disk_t* disk = ahci_find(bus, slot, func, port);
    if (!disk) return -1;
    return ahci_issue_command(disk, ATA_CMD_CACHE_FLUSH, 0, 0, 0, 0, 0) ? 0 : -1;
}
