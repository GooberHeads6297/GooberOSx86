#ifndef XHCI_PORT_H
#define XHCI_PORT_H

#include <stdint.h>
#include "xhci_regs.h"

/*
 * Spec-correct PORTSC write helpers.
 *
 * PED is RW1CS: writing the observed '1' clears Port Enabled (disables).
 * Change bits are RW1C. PR/WPR are RW1S. Never RMW the raw register.
 */

/* Preserve power/wake RWS bits; strip PED, PR, PLS, LWS, and change bits. */
uint32_t xhci_port_state_to_neutral(uint32_t portsc);

/* Build a write value: neutral(state) | set_bits (typically one change bit or PP/PR). */
uint32_t xhci_port_write_value(uint32_t portsc, uint32_t set_bits);

/* PORTSC[13:10] protocol speed identifier. */
static inline uint32_t xhci_portsc_psi(uint32_t portsc) {
    return (portsc & XHCI_PORT_SPEED_MASK) >> XHCI_PORT_SPEED_SHIFT;
}

/* Host-testable: return 1 if value would clear PED when current PED is set. */
int xhci_port_write_clears_ped(uint32_t current, uint32_t write_val);

#endif
