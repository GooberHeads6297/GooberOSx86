/*
 * Host-native unit tests: PORTSC neutralization and Slot Context helpers.
 * Build/run: gcc -Wall -Wextra -o test_xhci_port tests/usb/test_xhci_port.c \
 *              drivers/usb/host/xhci_port.c drivers/usb/host/xhci_ctx.c && ./test_xhci_port
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "../../drivers/usb/host/xhci_port.h"
#include "../../drivers/usb/host/xhci_ctx.h"

static int fails = 0;

static void expect(int cond, const char* msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        fails++;
    } else {
        printf("ok: %s\n", msg);
    }
}

int main(void) {
    uint32_t enabled = XHCI_PORT_CCS | XHCI_PORT_PED | XHCI_PORT_PP |
                       XHCI_PORT_PRC | (1U << 10); /* FS speed */

    /* Neutral must never include PED. */
    uint32_t neu = xhci_port_state_to_neutral(enabled);
    expect((neu & XHCI_PORT_PED) == 0, "neutral clears PED");
    expect((neu & XHCI_PORT_PRC) == 0, "neutral clears PRC");
    expect((neu & XHCI_PORT_PP) != 0, "neutral keeps PP");

    /* Ack PRC while PED=1 must not write PED back. */
    uint32_t wr = xhci_port_write_value(enabled, XHCI_PORT_PRC);
    expect((wr & XHCI_PORT_PRC) != 0, "PRC ack sets PRC write-1-to-clear");
    expect((wr & XHCI_PORT_PED) == 0, "PRC ack does not echo PED");
    expect(!xhci_port_write_clears_ped(enabled, wr),
           "PRC ack write does not clear PED");

    /* Legacy buggy pattern (for regression documentation). */
    uint32_t buggy = (enabled & ~0x00FE0000U) | XHCI_PORT_PRC;
    expect(xhci_port_write_clears_ped(enabled, buggy),
           "legacy RMW pattern would clear PED (documented)");

    /* Slot speed mapping uses xHCI default PSI encoding. */
    expect(xhci_psi_to_slot_speed(1) == 1, "FS PSI -> Slot FS");
    expect(xhci_psi_to_slot_speed(2) == 2, "LS PSI -> Slot LS");
    expect(xhci_default_ep0_mps(1) == 8, "FS EP0 default 8");
    expect(xhci_default_ep0_mps(3) == 64, "HS EP0 default 64");
    expect(((xhci_slot_ctx_dword0(2, 1) >> 20) & 0xFU) == 2,
           "slot ctx encodes LS speed");

    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    printf("All PORTSC/context tests passed.\n");
    return 0;
}
