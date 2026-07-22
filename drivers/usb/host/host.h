#ifndef USB_HOST_H
#define USB_HOST_H

#include <stdint.h>
#include "../../pci/pci.h"

typedef struct {
    usb_pci_controller_t controller;
    int active;
    uint32_t feature_locks;
} usb_host_state_t;

void usb_host_init(void);
void usb_host_poll(void);
int usb_host_ready(void);
uint8_t usb_host_controller_type(void);
const char* usb_host_controller_name(void);
int usb_host_is_healthy(void);
int usb_host_pointer_enumeration_allowed(void);

/*
 * Pointer enumeration scanning helpers.
 *
 * usb_host_scan_reset() rescans the PCI bus and clears scan state.
 * usb_host_try_next_candidate() initializes the next controller in priority
 *   order (xHCI/EHCI/OHCI/UHCI) and returns 1 if it came online, 0 once all
 *   candidates have been exhausted.
 * usb_host_promote_active() locks the currently selected controller as the
 *   active one (called after enumeration found a working pointer device).
 */
void usb_host_scan_reset(void);
int  usb_host_try_next_candidate(void);
void usb_host_promote_active(void);

/*
 * Safety level (cmdline gooberos.usb=). Filters which host controllers are
 * tried during scan, so that buggy chipsets can still boot via PS/2:
 *   0 = full (default), 1 = safe (skip xHCI),
 *   2 = minimal (only UHCI/OHCI), 3 = off (skip USB entirely).
 */
void usb_host_set_safety(int level);
int  usb_host_safety_level(void);

/* ---- Controller-agnostic dispatch (enumeration.c uses these) ---- */
int  host_controller_healthy(void);

/*
 * Per-controller fault isolation. host_controller_faulted() reports whether
 * the active controller has wedged/faulted; once it has, all dispatch short-
 * circuits to -1. host_mark_faulted() lets the enumeration layer poison a
 * controller after a per-port budget blow-out so the scan abandons it.
 */
int  host_controller_faulted(void);
void host_mark_faulted(void);
int  host_port_count(void);
int  host_port_companion_owned(int port);
int  host_port_connected(int port);
int  host_port_low_speed(int port);
void host_port_reset(int port);
int  host_port_change_pending(int port);
void host_port_change_ack(int port);
int  host_control_transfer(uint8_t dev_addr, uint8_t endpoint,
                           uint8_t* setup_pkt, uint8_t* data, uint16_t data_len,
                           int direction_in);
int  host_schedule_interrupt(uint8_t dev_addr, uint8_t endpoint,
                             uint16_t max_packet, uint8_t interval_frames);
void host_remove_interrupt(void);
int  host_interrupt_active(void);
uint8_t* host_get_report(int* ready);
void host_ack_report(void);

#endif
