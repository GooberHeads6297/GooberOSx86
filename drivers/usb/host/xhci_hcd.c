/*
 * xHCI HCD ops adapter for the redesigned USB core.
 *
 * Uses the modular PORTSC/context helpers and the production xhci_* engine
 * (rings, Address Device, transfers). Controllers stay registered with the
 * HCD registry independent of HID pointer discovery.
 */

#include "xhci_hcd.h"
#include "xhci.h"
#include "xhci_ctx.h"
#include "../core/usb_types.h"
#include "../../timer/timer.h"

static int xhci_hcd_probe(usb_hcd_t* hcd, const usb_pci_controller_t* pci) {
    (void)hcd;
    if (!pci) return -1;
    if (!xhci_init(pci)) return -1;
    return 0;
}

static void xhci_hcd_shutdown(usb_hcd_t* hcd) {
    (void)hcd;
    xhci_abandon_slot();
}

static void xhci_hcd_poll(usb_hcd_t* hcd) {
    (void)hcd;
    xhci_poll();
}

static int xhci_hcd_healthy(usb_hcd_t* hcd) {
    (void)hcd;
    return xhci_controller_healthy();
}

static int xhci_hcd_port_count(usb_hcd_t* hcd) {
    (void)hcd;
    return xhci_port_count();
}

static int xhci_hcd_port_connected(usb_hcd_t* hcd, int port) {
    (void)hcd;
    return xhci_port_connected(port);
}

static int xhci_hcd_port_speed(usb_hcd_t* hcd, int port) {
    int psi;
    (void)hcd;
    psi = xhci_port_protocol_speed(port);
    switch (xhci_psi_to_slot_speed((uint32_t)psi)) {
        case 1: return USB_SPEED_FULL;
        case 2: return USB_SPEED_LOW;
        case 3: return USB_SPEED_HIGH;
        case 4: return USB_SPEED_SUPER;
        default: return USB_SPEED_UNKNOWN;
    }
}

static int xhci_hcd_port_reset(usb_hcd_t* hcd, int port) {
    (void)hcd;
    /* Existing xhci_port_reset does PR + Enable Slot + Address Device (BSR=1). */
    return xhci_port_reset(port);
}

static int xhci_hcd_port_change_pending(usb_hcd_t* hcd, int port) {
    (void)hcd;
    return xhci_port_change_pending(port);
}

static void xhci_hcd_port_change_ack(usb_hcd_t* hcd, int port) {
    (void)hcd;
    xhci_port_change_ack(port);
}

static int xhci_hcd_device_enable(usb_hcd_t* hcd, usb_dev_t* dev) {
    (void)hcd;
    if (!dev) return -1;
    /* Slot already activated by port_reset in this stack. */
    if (!xhci_has_active_slot()) return -1;
    if (xhci_port_protocol_speed(dev->port) <= 0) return -1;
    dev->state = USB_DEV_DEFAULT;
    return 0;
}

static void xhci_hcd_device_disable(usb_hcd_t* hcd, usb_dev_t* dev) {
    (void)hcd;
    (void)dev;
    xhci_remove_interrupt();
    xhci_abandon_slot();
}

static int xhci_hcd_device_address(usb_hcd_t* hcd, usb_dev_t* dev, uint8_t addr) {
    uint8_t setup[8];
    (void)hcd;
    if (!dev) return -1;
    /* Translate SET_ADDRESS into Address Device (BSR=0) inside xhci_control. */
    setup[0] = 0x00;
    setup[1] = 0x05; /* SET_ADDRESS */
    setup[2] = addr;
    setup[3] = 0;
    setup[4] = 0;
    setup[5] = 0;
    setup[6] = 0;
    setup[7] = 0;
    if (xhci_control_transfer(0, 0, setup, 0, 0, 0) != 0) return -1;
    {
        uint8_t assigned = xhci_assigned_address();
        dev->address = assigned ? assigned : addr;
    }
    dev->state = USB_DEV_ADDRESSED;
    /* LS needs extra settle after Address Device before full GET_DESCRIPTOR. */
    timer_busy_wait_ms(dev->speed == USB_SPEED_LOW ? 50 : 15);
    return 0;
}

static int xhci_hcd_ep0_mps(usb_hcd_t* hcd, usb_dev_t* dev, uint16_t mps) {
    uint8_t setup[8];
    uint8_t buf[8];
    (void)hcd;
    if (!dev || mps == 0) return -1;
    /*
     * Existing engine evaluates EP0 MPS on the first short GET_DESCRIPTOR.
     * Issue an 8-byte GET_DESCRIPTOR so Evaluate Context runs, then remember MPS.
     */
    setup[0] = 0x80;
    setup[1] = 0x06;
    setup[2] = 0x00;
    setup[3] = 0x01;
    setup[4] = 0;
    setup[5] = 0;
    setup[6] = 8;
    setup[7] = 0;
    if (xhci_control_transfer(dev->address, 0, setup, buf, 8, 1) != 0)
        return -1;
    if (buf[7] != 0) {
        dev->ep0_mps = buf[7];
    } else {
        dev->ep0_mps = mps;
    }
    return 0;
}

static int xhci_hcd_control(usb_hcd_t* hcd, usb_dev_t* dev,
                            uint8_t* setup8, uint8_t* data, uint16_t len,
                            int dir_in) {
    (void)hcd;
    if (!dev || !setup8) return -1;
    return xhci_control_transfer(dev->address, 0, setup8, data, len, dir_in);
}

static int xhci_hcd_configure_interrupt(usb_hcd_t* hcd, usb_dev_t* dev,
                                        uint8_t ep_addr, uint16_t mps,
                                        uint8_t interval) {
    (void)hcd;
    if (!dev) return -1;
    return xhci_schedule_interrupt(dev->address, ep_addr, mps, interval);
}

static int xhci_hcd_interrupt_poll(usb_hcd_t* hcd, usb_dev_t* dev,
                                   uint8_t* out, uint16_t max_len,
                                   uint16_t* got) {
    int ready = 0;
    uint8_t* report;
    uint16_t n;
    (void)hcd;
    (void)dev;
    report = xhci_get_report(&ready);
    if (!ready || !report || !out) {
        if (got) *got = 0;
        return 0;
    }
    n = max_len < 8 ? max_len : 8;
    for (uint16_t i = 0; i < n; i++) out[i] = report[i];
    if (got) *got = n;
    xhci_ack_report();
    return 0;
}

static void xhci_hcd_interrupt_stop(usb_hcd_t* hcd, usb_dev_t* dev) {
    (void)hcd;
    (void)dev;
    xhci_remove_interrupt();
}

const usb_hcd_ops_t usb_xhci_hcd_ops = {
    .name = "xHCI",
    .prog_if = 0x30,
    .probe = xhci_hcd_probe,
    .shutdown = xhci_hcd_shutdown,
    .poll = xhci_hcd_poll,
    .healthy = xhci_hcd_healthy,
    .port_count = xhci_hcd_port_count,
    .port_connected = xhci_hcd_port_connected,
    .port_speed = xhci_hcd_port_speed,
    .port_reset = xhci_hcd_port_reset,
    .port_change_pending = xhci_hcd_port_change_pending,
    .port_change_ack = xhci_hcd_port_change_ack,
    .device_enable = xhci_hcd_device_enable,
    .device_disable = xhci_hcd_device_disable,
    .device_address = xhci_hcd_device_address,
    .ep0_mps = xhci_hcd_ep0_mps,
    .control = xhci_hcd_control,
    .configure_interrupt = xhci_hcd_configure_interrupt,
    .interrupt_poll = xhci_hcd_interrupt_poll,
    .interrupt_stop = xhci_hcd_interrupt_stop,
};
