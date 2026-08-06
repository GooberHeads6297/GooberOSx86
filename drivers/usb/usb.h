#ifndef USB_H
#define USB_H

#include <stdint.h>
#include <stddef.h>

void usb_init(void);
void usb_poll(void);
int usb_has_pointer_device(void);
int usb_has_touchpad_device(void);
int usb_has_keyboard_device(void);

/* USB standard request codes */
#define USB_REQ_GET_STATUS         0x00
#define USB_REQ_CLEAR_FEATURE      0x01
#define USB_REQ_SET_FEATURE        0x03
#define USB_REQ_SET_ADDRESS        0x05
#define USB_REQ_GET_DESCRIPTOR     0x06
#define USB_REQ_SET_DESCRIPTOR     0x07
#define USB_REQ_GET_CONFIGURATION  0x08
#define USB_REQ_SET_CONFIGURATION  0x09
#define USB_REQ_GET_INTERFACE      0x0A
#define USB_REQ_SET_INTERFACE      0x0B
#define USB_REQ_SYNCH_FRAME        0x0C

/* HID class requests */
#define USB_HID_REQ_GET_REPORT     0x01
#define USB_HID_REQ_GET_IDLE       0x02
#define USB_HID_REQ_GET_PROTOCOL   0x03
#define USB_HID_REQ_SET_REPORT     0x09
#define USB_HID_REQ_SET_IDLE       0x0A
#define USB_HID_REQ_SET_PROTOCOL   0x0B

/* Descriptor types */
#define USB_DT_DEVICE              1
#define USB_DT_CONFIG              2
#define USB_DT_STRING              3
#define USB_DT_INTERFACE           4
#define USB_DT_ENDPOINT            5
#define USB_DT_HID                 0x21
#define USB_DT_REPORT              0x22

/* Class codes */
#define USB_CLASS_PER_INTERFACE    0
#define USB_CLASS_COMM             2
#define USB_CLASS_HID              3
#define USB_CLASS_MASS_STORAGE     0x08

/* HID subclass / protocol */
#define USB_HID_SUBCLASS_BOOT      1
#define USB_HID_PROTOCOL_KEYBOARD  1
#define USB_HID_PROTOCOL_MOUSE     2

/* MSC (Bulk-Only Transport) subclass / protocol */
#define USB_MSC_SUBCLASS_SCSI      0x06
#define USB_MSC_PROTOCOL_BOT       0x50
/* Enumeration return tag for BOT MSC (distinct from HID protocol values). */
#define USB_ENUM_PROTOCOL_MSC      0x50

/* Request type: direction bits */
#define USB_DIR_OUT                0
#define USB_DIR_IN                 0x80
#define USB_TYPE_STANDARD          (0 << 5)
#define USB_TYPE_CLASS             (1 << 5)
#define USB_RECIP_DEVICE           (0 << 0)
#define USB_RECIP_INTERFACE        (1 << 0)
#define USB_RECIP_ENDPOINT         (2 << 0)

/* PID codes */
#define USB_PID_OUT                0xE1
#define USB_PID_IN                 0x69
#define USB_PID_SETUP              0x2D
#define USB_PID_DATA0              0xC3
#define USB_PID_DATA1              0x4B

#define USB_MAX_DEVICES            16
#define USB_ENUM_RETRIES           3

/* Standard device descriptor */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} usb_device_descriptor_t;

/* Standard configuration descriptor */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} usb_config_descriptor_t;

/* Standard interface descriptor */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} usb_interface_descriptor_t;

/* Standard endpoint descriptor */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} usb_endpoint_descriptor_t;

/* HID descriptor */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdHID;
    uint8_t  bCountryCode;
    uint8_t  bNumDescriptors;
    uint8_t  bReportDescriptorType;
    uint16_t wReportDescriptorLength;
} usb_hid_descriptor_t;

/* USB device structure */
typedef struct {
    uint8_t  address;
    uint8_t  port;
    uint8_t  speed;          /* 0 = full, 1 = low */
    uint8_t  configured;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  protocol;
    uint8_t  interface_number;
    uint8_t  max_packet_size;
    uint8_t  ep_in;          /* IN endpoint address for interrupt */
    uint8_t  ep_out;         /* OUT endpoint address */
    uint16_t ep_in_max_pkt;
    uint8_t  ep_in_interval;
} usb_device_t;

#endif
