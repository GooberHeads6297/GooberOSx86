#ifndef USB_MSC_H
#define USB_MSC_H

#include <stdint.h>
#include "../../storage/storage.h"

typedef enum {
    USB_MSC_HOST_NONE = 0,
    USB_MSC_HOST_UHCI,
    USB_MSC_HOST_OHCI,
    USB_MSC_HOST_EHCI,
    USB_MSC_HOST_XHCI
} usb_msc_host_kind_t;

typedef struct {
    uint8_t controller_present;
    uint8_t controller_supported;
    uint8_t transport_scaffold_ready;
    uint8_t bulk_only_pending;
    uint8_t host_kind;
} usb_msc_probe_result_t;

int usb_msc_probe_pci_controller(uint8_t prog_if, uint32_t bar0, usb_msc_probe_result_t* out);
const char* usb_msc_host_name(uint8_t host_kind);

/*
 * Attach a BOT/SCSI LUN after SET_CONFIGURATION + bulk EP configure.
 * Registers a READY storage_device_info_t on success.
 */
int usb_msc_attach(int port, uint8_t addr,
                   uint8_t ep_out, uint8_t ep_in,
                   uint16_t mps_out, uint16_t mps_in);
void usb_msc_detach(int port);
void usb_msc_detach_all(void);
int usb_msc_is_attached(void);
int usb_msc_attached_port(void);
int usb_msc_storage_index(void);

int usb_msc_read_sector(const storage_device_info_t* device, uint32_t lba, void* out_sector);
int usb_msc_write_sector(const storage_device_info_t* device, uint32_t lba, const void* in_sector);

#endif
