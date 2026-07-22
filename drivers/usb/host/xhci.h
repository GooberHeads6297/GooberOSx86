#ifndef USB_XHCI_H
#define USB_XHCI_H

#include <stdint.h>
#include "../../pci/pci.h"

int  xhci_init(const usb_pci_controller_t* controller);
void xhci_poll(void);
int  xhci_controller_healthy(void);
int  xhci_port_count(void);
int  xhci_port_connected(int port);
int  xhci_port_low_speed(int port);
void xhci_port_reset(int port);
int  xhci_port_change_pending(int port);
void xhci_port_change_ack(int port);
int  xhci_control_transfer(uint8_t dev_addr, uint8_t endpoint,
                           uint8_t* setup_pkt, uint8_t* data, uint16_t data_len,
                           int direction_in);
int  xhci_schedule_interrupt(uint8_t dev_addr, uint8_t endpoint,
                             uint16_t max_packet, uint8_t interval_frames);
void xhci_remove_interrupt(void);
int  xhci_interrupt_active(void);
uint8_t* xhci_get_report(int* ready);
void xhci_ack_report(void);

#endif
