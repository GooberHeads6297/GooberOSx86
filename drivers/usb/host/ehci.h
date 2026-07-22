#ifndef USB_EHCI_H
#define USB_EHCI_H

#include <stdint.h>
#include "../../pci/pci.h"

int  ehci_init(const usb_pci_controller_t* controller);
void ehci_poll(void);
int  ehci_controller_healthy(void);
int  ehci_port_count(void);
int  ehci_port_connected(int port);
int  ehci_port_low_speed(int port);
int  ehci_port_owned_by_companion(int port);
void ehci_port_reset(int port);
int  ehci_port_change_pending(int port);
void ehci_port_change_ack(int port);
int  ehci_control_transfer(uint8_t dev_addr, uint8_t endpoint,
                           uint8_t* setup_pkt, uint8_t* data, uint16_t data_len,
                           int direction_in);
int  ehci_schedule_interrupt(uint8_t dev_addr, uint8_t endpoint,
                             uint16_t max_packet, uint8_t interval_frames);
void ehci_remove_interrupt(void);
int  ehci_interrupt_active(void);
uint8_t* ehci_get_report(int* ready);
void ehci_ack_report(void);

#endif
