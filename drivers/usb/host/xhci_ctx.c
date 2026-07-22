#include "xhci_ctx.h"

uint32_t xhci_psi_to_slot_speed(uint32_t psi) {
    switch (psi) {
        case XHCI_PORT_PSI_FS: return 1U; /* Full-speed  */
        case XHCI_PORT_PSI_LS: return 2U; /* Low-speed   */
        case XHCI_PORT_PSI_HS: return 3U; /* High-speed  */
        case XHCI_PORT_PSI_SS: return 4U; /* SuperSpeed  */
        default: return psi & 0xFU;
    }
}

uint16_t xhci_default_ep0_mps(uint32_t slot_speed) {
    switch (slot_speed) {
        case 4: return 512; /* SuperSpeed */
        case 3: return 64;  /* High-speed */
        case 1: return 8;   /* Full-speed default at address 0 */
        case 2: return 8;   /* Low-speed */
        default: return 8;
    }
}

uint32_t xhci_slot_ctx_dword0(uint32_t slot_speed, uint32_t context_entries) {
    return ((context_entries & 0x1FU) << 27) | ((slot_speed & 0xFU) << 20);
}
