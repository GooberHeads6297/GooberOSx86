#ifndef USB_DEVICE_H
#define USB_DEVICE_H

#include "usb_types.h"

#define USB_CORE_MAX_DEVICES    16
#define USB_CORE_MAX_INTERFACES 8
#define USB_CORE_MAX_ENDPOINTS  8

struct usb_hcd;

typedef struct usb_endpoint {
    uint8_t  address;      /* including direction bit */
    uint8_t  attributes;
    uint16_t max_packet;
    uint8_t  interval;
    uint8_t  present;
} usb_endpoint_desc_t;

typedef struct usb_interface {
    uint8_t  number;
    uint8_t  alt;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  protocol;
    uint8_t  num_endpoints;
    usb_endpoint_desc_t ep[USB_CORE_MAX_ENDPOINTS];
    void*    class_priv;   /* owned by class driver */
    int      class_bound;
} usb_interface_t;

/* Core device object (distinct from legacy usb_device_t in usb.h). */
typedef struct usb_dev {
    struct usb_hcd* hcd;
    uint8_t  port;         /* 0-based root hub port */
    uint8_t  address;      /* USB address 0..127 */
    usb_speed_t speed;
    usb_dev_state_t state;
    uint8_t  slot_id;      /* HC-private slot (xhCI) */
    uint16_t ep0_mps;
    uint16_t id_vendor;
    uint16_t id_product;
    uint8_t  config_value;
    uint8_t  num_interfaces;
    usb_interface_t ifaces[USB_CORE_MAX_INTERFACES];
    void*    hcd_priv;     /* per-device HCD state */
    int      in_use;
} usb_dev_t;

usb_dev_t* usb_dev_alloc(struct usb_hcd* hcd, int port, usb_speed_t speed);
void       usb_dev_free(usb_dev_t* dev);
usb_dev_t* usb_dev_by_port(struct usb_hcd* hcd, int port);
int        usb_dev_count(void);
usb_dev_t* usb_dev_get(int index);
void       usb_dev_table_reset(void);

#endif
