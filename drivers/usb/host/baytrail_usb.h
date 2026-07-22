#ifndef BAYTRAIL_USB_H
#define BAYTRAIL_USB_H

#include <stdint.h>

/*
 * Bay Trail (Atom Z36xxx/Z37xxx) USB routing helpers.
 *
 * On these SoCs USB 2.0 ports can sit on a companion EHCI (8086:0F34 at
 * 0:1d.0) even when that function is hidden from PCI via PMC FUNC_DIS.EHCI_DIS.
 * XUSB2PR=0 means "ports owned by EHCI"; our xHCI-only enum then sees CCS
 * phantoms and EP0 cc=4 forever.
 *
 * Call before the USB host PCI scan: attempt USB2→xHCI route under UPRWC,
 * then unhide EHCI only when routing stays locked.
 */
void baytrail_usb_prepare_companion(void);

/*
 * Apply Bay Trail PDO clear + PRM/PR route for an xHCI PCI function.
 * Holds UPRWC.WR_EN across the writes. Returns 1 if XUSB2PR readback != 0.
 *
 * Must be called again AFTER xHCI HCRESET: reset can clear XUSB2PR back to 0,
 * leaving CCS phantoms on xHCI while the real USB2 mouse stays on EHCI.
 */
int baytrail_usb_route_to_xhci(uint8_t bus, uint8_t slot, uint8_t func);

/* Re-read PCI XUSB2PR/PDO into sticky status (after HCRESET / bring-up). */
void baytrail_usb_refresh_route_status(uint8_t bus, uint8_t slot, uint8_t func);

/* 1 if last prepare saw Bay Trail xHCI (8086:0F35 family). */
int baytrail_usb_is_soc(void);

/* 1 if last prepare saw Braswell xHCI (8086:22B5); BYT PMC/PHY skipped. */
int baytrail_usb_is_braswell(void);

/* Sticky route / companion status for devices/driverlog (greppable USB2ROUTE). */
void baytrail_usb_print_status(void (*write)(const char*));
uint32_t baytrail_usb_xusb2pr(void);
uint32_t baytrail_usb_usb2pdo(void);
int baytrail_usb_usb2_on_xhci(void);
int baytrail_usb_usb2_route_locked(void);
void baytrail_usb_note_route(uint32_t xusb2pr, uint32_t xusb2prm, int wrote);

/* 1 if a UHCI or OHCI companion is present on PCI (Bay Trail usually has none). */
int baytrail_usb_has_ls_companion(void);

/* Prefer xHCI over EHCI when USB2 was successfully routed to xHCI. */
int baytrail_usb_prefer_xhci(void);

/*
 * Optional Bay Trail vendor PHY / MMIO / clock-gating script.
 * Call after xHCI HCRESET/CNR clears and before programming rings / RUN.
 *
 * Speculative USB2/USB3 PHY IOSF writes, 0x80e0 pulse, and PCI clock-gating
 * default ON for 8086:0F35 once routing works (Lenovo EP0 still needs them).
 * Disable with gooberos.usb.byt.phy=off if a board misbehaves.
 */
void baytrail_xhci_hc_bringup(volatile uint8_t* mmio_bar,
                              uint8_t bus, uint8_t slot, uint8_t func);

/* 1 if gooberos.usb.byt.phy=on (speculative PHY/MMIO scripts allowed). */
int  baytrail_usb_phy_quirks_enabled(void);
void baytrail_usb_set_phy_quirks(int enabled);

#endif
