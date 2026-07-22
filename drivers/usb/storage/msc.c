#include "msc.h"
#include "../host/host.h"
#include "../usb.h"
#include "../../timer/timer.h"
#include "../../diagnostics/driver_log.h"
#include "../../../fs/fs_backend.h"
#include "../../../kernel.h"

extern void print(const char* str);

#define CBW_SIGNATURE 0x43425355U
#define CSW_SIGNATURE 0x53425355U
#define CBW_FLAG_IN   0x80
#define SCSI_INQUIRY  0x12
#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_READ_CAPACITY10 0x25
#define SCSI_READ10   0x28
#define SCSI_WRITE10  0x2A

typedef struct __attribute__((packed)) {
    uint32_t dCBWSignature;
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t  bmCBWFlags;
    uint8_t  bCBWLUN;
    uint8_t  bCBWCBLength;
    uint8_t  CBWCB[16];
} usb_msc_cbw_t;

typedef struct __attribute__((packed)) {
    uint32_t dCSWSignature;
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t  bCSWStatus;
} usb_msc_csw_t;

typedef struct {
    int active;
    int port;
    int storage_index;
    uint8_t addr;
    uint8_t ep_out;
    uint8_t ep_in;
    uint16_t mps_out;
    uint16_t mps_in;
    uint32_t tag;
    uint64_t sectors;
    uint32_t sector_size;
} usb_msc_dev_t;

static usb_msc_dev_t g_msc;
static uint8_t g_sector_buf[512];

static void msc_log(const char* s) {
    driver_log(s);
    print(s);
}

static void msc_delay_ms(uint32_t ms) {
    timer_busy_wait_ms(ms);
}

int usb_msc_probe_pci_controller(uint8_t prog_if, uint32_t bar0, usb_msc_probe_result_t* out) {
    if (!out) return 0;

    out->controller_present = 0;
    out->controller_supported = 0;
    out->transport_scaffold_ready = 0;
    out->bulk_only_pending = 0;
    out->host_kind = USB_MSC_HOST_NONE;

    if (bar0 == 0 || bar0 == 0xFFFFFFFFU) return 0;

    out->controller_present = 1;
    out->bulk_only_pending = 0;

    if (prog_if == 0x00) {
        out->host_kind = USB_MSC_HOST_UHCI;
        out->controller_supported = 1;
        out->transport_scaffold_ready = 1;
        return 1;
    }
    if (prog_if == 0x10) {
        out->host_kind = USB_MSC_HOST_OHCI;
        out->controller_supported = 1;
        out->transport_scaffold_ready = 1;
        return 1;
    }
    if (prog_if == 0x20) {
        out->host_kind = USB_MSC_HOST_EHCI;
        out->controller_supported = 1;
        out->transport_scaffold_ready = 1;
        return 1;
    }
    if (prog_if == 0x30) {
        out->host_kind = USB_MSC_HOST_XHCI;
        out->controller_supported = 1;
        out->transport_scaffold_ready = 1;
        return 1;
    }
    return 1;
}

const char* usb_msc_host_name(uint8_t host_kind) {
    if (host_kind == USB_MSC_HOST_UHCI) return "UHCI";
    if (host_kind == USB_MSC_HOST_OHCI) return "OHCI";
    if (host_kind == USB_MSC_HOST_EHCI) return "EHCI";
    if (host_kind == USB_MSC_HOST_XHCI) return "XHCI";
    return "Unknown";
}

static int bot_command(uint8_t lun, uint8_t flags, uint32_t data_len,
                       const uint8_t* cdb, uint8_t cdb_len,
                       uint8_t* data) {
    usb_msc_cbw_t cbw;
    usb_msc_csw_t csw;
    uint8_t i;

    if (!g_msc.active) return -1;
    if (cdb_len > 16) cdb_len = 16;

    cbw.dCBWSignature = CBW_SIGNATURE;
    cbw.dCBWTag = ++g_msc.tag;
    cbw.dCBWDataTransferLength = data_len;
    cbw.bmCBWFlags = flags;
    cbw.bCBWLUN = lun;
    cbw.bCBWCBLength = cdb_len;
    for (i = 0; i < 16; i++) cbw.CBWCB[i] = 0;
    for (i = 0; i < cdb_len; i++) cbw.CBWCB[i] = cdb[i];

    if (host_bulk_transfer(g_msc.ep_out, (uint8_t*)&cbw, 31, 0) != 0)
        return -1;

    if (data_len > 0 && data) {
        if (flags & CBW_FLAG_IN) {
            if (host_bulk_transfer(g_msc.ep_in, data, (uint16_t)data_len, 1) != 0)
                return -1;
        } else {
            if (host_bulk_transfer(g_msc.ep_out, data, (uint16_t)data_len, 0) != 0)
                return -1;
        }
    }

    if (host_bulk_transfer(g_msc.ep_in, (uint8_t*)&csw, 13, 1) != 0)
        return -1;
    if (csw.dCSWSignature != CSW_SIGNATURE) return -1;
    if (csw.dCSWTag != cbw.dCBWTag) return -1;
    if (csw.bCSWStatus != 0) return -1;
    return 0;
}

static int scsi_inquiry(void) {
    uint8_t cdb[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    uint8_t inq[36];
    return bot_command(0, CBW_FLAG_IN, 36, cdb, 6, inq);
}

static int scsi_test_unit_ready(void) {
    uint8_t cdb[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    return bot_command(0, 0, 0, cdb, 6, 0);
}

static int scsi_read_capacity(uint64_t* out_sectors, uint32_t* out_size) {
    uint8_t cdb[10] = { SCSI_READ_CAPACITY10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t cap[8];
    uint32_t last_lba;
    uint32_t block_len;

    if (bot_command(0, CBW_FLAG_IN, 8, cdb, 10, cap) != 0) return -1;
    last_lba = ((uint32_t)cap[0] << 24) | ((uint32_t)cap[1] << 16) |
               ((uint32_t)cap[2] << 8) | (uint32_t)cap[3];
    block_len = ((uint32_t)cap[4] << 24) | ((uint32_t)cap[5] << 16) |
                ((uint32_t)cap[6] << 8) | (uint32_t)cap[7];
    if (block_len == 0) block_len = 512;
    if (out_sectors) *out_sectors = (uint64_t)last_lba + 1ULL;
    if (out_size) *out_size = block_len;
    return 0;
}

static int scsi_read10(uint32_t lba, void* out_sector) {
    uint8_t cdb[10];
    cdb[0] = SCSI_READ10;
    cdb[1] = 0;
    cdb[2] = (uint8_t)((lba >> 24) & 0xFF);
    cdb[3] = (uint8_t)((lba >> 16) & 0xFF);
    cdb[4] = (uint8_t)((lba >> 8) & 0xFF);
    cdb[5] = (uint8_t)(lba & 0xFF);
    cdb[6] = 0;
    cdb[7] = 0;
    cdb[8] = 1;
    cdb[9] = 0;
    return bot_command(0, CBW_FLAG_IN, 512, cdb, 10, (uint8_t*)out_sector);
}

static int scsi_write10(uint32_t lba, const void* in_sector) {
    uint8_t cdb[10];
    uint16_t i;
    for (i = 0; i < 512; i++) g_sector_buf[i] = ((const uint8_t*)in_sector)[i];
    cdb[0] = SCSI_WRITE10;
    cdb[1] = 0;
    cdb[2] = (uint8_t)((lba >> 24) & 0xFF);
    cdb[3] = (uint8_t)((lba >> 16) & 0xFF);
    cdb[4] = (uint8_t)((lba >> 8) & 0xFF);
    cdb[5] = (uint8_t)(lba & 0xFF);
    cdb[6] = 0;
    cdb[7] = 0;
    cdb[8] = 1;
    cdb[9] = 0;
    return bot_command(0, 0, 512, cdb, 10, g_sector_buf);
}

static void maybe_auto_mount_live(int storage_index) {
    const boot_config_t* cfg = boot_get_config();
    if (!cfg || !cfg->root[0]) return;
    /* Only auto-mount removable sticks on live boots (do not stomp installed root). */
    if (!(cfg->root[0] == 'l' && cfg->root[1] == 'i' &&
          cfg->root[2] == 'v' && cfg->root[3] == 'e' && cfg->root[4] == '\0'))
        return;
    if (fat32_is_mounted()) return;
    if (fat32_mount_device_loose(storage_index, 0) == 0)
        msc_log("[usb-msc] auto-mounted partition 0 (live).\n");
}

int usb_msc_attach(int port, uint8_t addr,
                   uint8_t ep_out, uint8_t ep_in,
                   uint16_t mps_out, uint16_t mps_in) {
    int tries;
    int idx;
    uint64_t sectors = 0;
    uint32_t ssize = 512;

    if (g_msc.active) usb_msc_detach(g_msc.port);

    g_msc.active = 1;
    g_msc.port = port;
    g_msc.addr = addr;
    g_msc.ep_out = ep_out;
    g_msc.ep_in = ep_in;
    g_msc.mps_out = mps_out ? mps_out : 64;
    g_msc.mps_in = mps_in ? mps_in : 64;
    g_msc.tag = 1;
    g_msc.storage_index = -1;
    g_msc.sectors = 0;
    g_msc.sector_size = 512;

    (void)scsi_inquiry();
    for (tries = 0; tries < 8; tries++) {
        if (scsi_test_unit_ready() == 0) break;
        msc_delay_ms(50);
    }
    if (scsi_read_capacity(&sectors, &ssize) != 0) {
        msc_log("[usb-msc] READ_CAPACITY failed.\n");
        g_msc.active = 0;
        return -1;
    }
    if (ssize != 512) {
        msc_log("[usb-msc] unsupported sector size (need 512).\n");
        g_msc.active = 0;
        return -1;
    }

    g_msc.sectors = sectors;
    g_msc.sector_size = ssize;

    idx = storage_register_usb_msc(port, sectors, "USB MSC");
    if (idx < 0) {
        msc_log("[usb-msc] storage register failed.\n");
        g_msc.active = 0;
        return -1;
    }
    g_msc.storage_index = idx;

    /* Prove the block path with a sector-0 READ before advertising READY. */
    {
        const storage_device_info_t* dev = storage_get(idx);
        if (!dev || usb_msc_read_sector(dev, 0, g_sector_buf) != 0) {
            msc_log("[usb-msc] sector 0 READ failed.\n");
            storage_unregister_usb_msc(idx);
            g_msc.active = 0;
            g_msc.storage_index = -1;
            return -1;
        }
    }

    msc_log("[usb-msc] attached\n");
    maybe_auto_mount_live(idx);
    return 0;
}

void usb_msc_detach(int port) {
    if (!g_msc.active) return;
    if (port >= 0 && g_msc.port != port) return;

    if (fat32_is_mounted() &&
        fat32_mount_device_index() == g_msc.storage_index) {
        fat32_unmount();
        msc_log("[usb-msc] unmounted volume on detach.\n");
    }
    if (g_msc.storage_index >= 0)
        storage_unregister_usb_msc(g_msc.storage_index);
    msc_log("[usb-msc] detached\n");
    g_msc.active = 0;
    g_msc.storage_index = -1;
    g_msc.port = -1;
}

void usb_msc_detach_all(void) {
    if (g_msc.active) usb_msc_detach(g_msc.port);
}

int usb_msc_is_attached(void) { return g_msc.active; }
int usb_msc_attached_port(void) { return g_msc.active ? g_msc.port : -1; }
int usb_msc_storage_index(void) { return g_msc.active ? g_msc.storage_index : -1; }

int usb_msc_read_sector(const storage_device_info_t* device, uint32_t lba, void* out_sector) {
    if (!device || !out_sector || !g_msc.active) return -1;
    if (device->backend != STORAGE_BACKEND_USB_MASS_STORAGE) return -1;
    if ((int)device->ata_channel != g_msc.port) return -1;
    if (lba >= g_msc.sectors) return -1;
    return scsi_read10(lba, out_sector);
}

int usb_msc_write_sector(const storage_device_info_t* device, uint32_t lba, const void* in_sector) {
    if (!device || !in_sector || !g_msc.active) return -1;
    if (device->backend != STORAGE_BACKEND_USB_MASS_STORAGE) return -1;
    if ((int)device->ata_channel != g_msc.port) return -1;
    if (lba >= g_msc.sectors) return -1;
    return scsi_write10(lba, in_sector);
}
