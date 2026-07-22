#ifndef USB_UHCI_H
#define USB_UHCI_H

#include <stdint.h>
#include <stddef.h>
#include "../../pci/pci.h"

/* ---- UHCI Transfer Descriptor (16 bytes) ---- */
typedef struct __attribute__((packed)) uhci_td {
    uint32_t link;
    uint32_t status;
    uint32_t token;
    uint32_t buffer;
} uhci_td_t;

/* Link pointer bits */
#define UHCI_LINK_TERMINATE     (1 << 0)
#define UHCI_LINK_QH            (1 << 1)
#define UHCI_LINK_DEPTH         (1 << 2)
#define UHCI_LINK_VF            (1 << 3)

/* Control/Status bits */
#define UHCI_TD_IOS             (1 << 0)
#define UHCI_TD_LS              (1 << 1)
#define UHCI_TD_ERR_MASK        (3 << 2)
#define UHCI_TD_ERR(x)          (((x) & 3) << 2)
#define UHCI_TD_ACTIVE          (1 << 7)
#define UHCI_TD_STALLED         (1 << 8)
#define UHCI_TD_BUFERR          (1 << 9)
#define UHCI_TD_BABBLE          (1 << 10)
#define UHCI_TD_NAK             (1 << 11)
#define UHCI_TD_TIMEOUT         (1 << 12)
#define UHCI_TD_ACTLEN_SHIFT    16
#define UHCI_TD_ACTLEN_MASK     (0x7FF << 16)
#define UHCI_TD_SPD             (1 << 27)

/* Token bits */
#define UHCI_TOKEN_PID_SETUP    0x2D
#define UHCI_TOKEN_PID_IN       0x69
#define UHCI_TOKEN_PID_OUT      0xE1
#define UHCI_TOKEN_DEVADDR_SHIFT 8
#define UHCI_TOKEN_ENDP_SHIFT   15
#define UHCI_TOKEN_TOGGLE_SHIFT 19
#define UHCI_TOKEN_TOGGLE_DATA0  (0 << 19)
#define UHCI_TOKEN_TOGGLE_DATA1  (1 << 19)

/* ---- UHCI Queue Head (8 bytes) ---- */
typedef struct __attribute__((packed)) uhci_qh {
    uint32_t head_link;
    uint32_t element;
} uhci_qh_t;

/* UHCI functions */
int  uhci_init(const usb_pci_controller_t* controller);
void uhci_poll(void);
int  uhci_controller_healthy(void);
int  uhci_pointer_enumeration_allowed(void);

/* Port operations */
int  uhci_port_connected(int port);
int  uhci_port_low_speed(int port);
void uhci_port_reset(int port);
int  uhci_port_change_pending(int port);
void uhci_port_change_ack(int port);

/* UHCI transfer API */
int  uhci_control_transfer(uint8_t dev_addr, uint8_t endpoint,
                           uint8_t* setup_pkt, uint8_t* data, uint16_t data_len,
                           int direction_in);
int  uhci_schedule_interrupt(uint8_t dev_addr, uint8_t endpoint,
                             uint16_t max_packet, uint8_t interval_frames);
void uhci_remove_interrupt(void);
int  uhci_interrupt_active(void);

/* Report retrieval (INTERRUPT IN) */
uint8_t* uhci_get_report(int* ready);
void uhci_ack_report(void);

#endif
