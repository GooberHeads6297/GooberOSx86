#ifndef USB_TYPES_H
#define USB_TYPES_H

#include <stdint.h>

/* Controller / device / pipe ownership types for the redesigned USB stack. */

typedef enum {
    USB_SPEED_UNKNOWN = 0,
    USB_SPEED_LOW     = 1,  /* 1.5 Mbps  — Slot Context speed value */
    USB_SPEED_FULL    = 2,  /* 12 Mbps   — Slot Context speed value */
    USB_SPEED_HIGH    = 3,  /* 480 Mbps */
    USB_SPEED_SUPER   = 4   /* 5 Gbps */
} usb_speed_t;

typedef enum {
    USB_XFER_CONTROL = 0,
    USB_XFER_ISOCH   = 1,
    USB_XFER_BULK    = 2,
    USB_XFER_INT     = 3
} usb_xfer_type_t;

typedef enum {
    USB_DIR_HOST_TO_DEVICE = 0,
    USB_DIR_DEVICE_TO_HOST = 1
} usb_xfer_dir_t;

typedef enum {
    USB_XFER_OK = 0,
    USB_XFER_STALL,
    USB_XFER_BABBLE,
    USB_XFER_ERROR,       /* transaction / CRC / timeout */
    USB_XFER_TIMEOUT,
    USB_XFER_NODEV,
    USB_XFER_NOMEM,
    USB_XFER_INVAL,
    USB_XFER_SHORT        /* short packet accepted as success when allowed */
} usb_xfer_status_t;

typedef enum {
    USB_DEV_ATTACHED = 0,
    USB_DEV_DEFAULT,
    USB_DEV_ADDRESSED,
    USB_DEV_CONFIGURED,
    USB_DEV_SUSPENDED,
    USB_DEV_DETACHED
} usb_dev_state_t;

struct usb_hcd;
struct usb_device;
struct usb_interface;
struct usb_endpoint;
struct usb_pipe;
struct usb_xfer;

#endif
