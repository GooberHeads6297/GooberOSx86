#include "xhci_port.h"

uint32_t xhci_port_state_to_neutral(uint32_t portsc) {
    /*
     * Keep only bits that are genuinely RWS and safe to echo. Explicitly drop:
     *  - PED (RW1CS)
     *  - PR / WPR (RW1S)
     *  - PLS / LWS (must use LWS + desired PLS together if ever needed)
     *  - all RW1C change bits
     *  - RO status bits (CCS, OCA, Port Speed, CAS, DR, ...)
     */
    return portsc & XHCI_PORT_RWS;
}

uint32_t xhci_port_write_value(uint32_t portsc, uint32_t set_bits) {
    return xhci_port_state_to_neutral(portsc) | set_bits;
}

int xhci_port_write_clears_ped(uint32_t current, uint32_t write_val) {
    if (!(current & XHCI_PORT_PED))
        return 0;
    /* Writing PED=1 while enabled clears enable (RW1CS). */
    return (write_val & XHCI_PORT_PED) != 0;
}
