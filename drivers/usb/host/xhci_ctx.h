#ifndef XHCI_CTX_H
#define XHCI_CTX_H

#include <stdint.h>
#include "xhci_regs.h"

/*
 * Map PORTSC Protocol Speed Identifier to Slot Context Speed (SPD).
 * With the default USB2 mapping both use 1=FS, 2=LS, 3=HS, 4=SS.
 * When Supported Protocol capabilities remapped PSI values, pass the
 * resolved Slot SPD through here after capability parse.
 */
uint32_t xhci_psi_to_slot_speed(uint32_t psi);

/* Default EP0 max packet size for a Slot Context speed value. */
uint16_t xhci_default_ep0_mps(uint32_t slot_speed);

/* Encode Slot Context dword0 speed + context-entries fields. */
uint32_t xhci_slot_ctx_dword0(uint32_t slot_speed, uint32_t context_entries);

#endif
