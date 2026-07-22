#include "host.h"
#include "uhci.h"
#include "ohci.h"
#include "ehci.h"
#include "xhci.h"
#include "baytrail_usb.h"
#include "../../io/io.h"
#include "../../diagnostics/driver_log.h"

extern void print(const char* str);

#define USB_HOST_LOCK_POINTER_ENUM 0x00000001U
#define USB_HOST_LOCK_RUNTIME      0x00000002U

/* Controller type constants */
#define HCTRL_NONE  0xFF
#define HCTRL_UHCI  0x00
#define HCTRL_OHCI  0x10
#define HCTRL_EHCI  0x20
#define HCTRL_XHCI  0x30

static usb_host_state_t host_state;
static uint8_t host_controller_type = HCTRL_NONE;

/*
 * Per-controller fault latch (generalized from xHCI's xhci_fault poisoning).
 *
 * Once ANY active controller wedges or faults -- a transfer times out, a
 * port reset never completes, the controller reports a host system error,
 * etc. -- we set host_faulted. From that point on every dispatch
 * (host_port_reset / host_control_transfer / host_schedule_interrupt) short-
 * circuits immediately instead of stacking more work on a dead controller,
 * and host_controller_healthy() reports false so the enumeration loop bails
 * and usb_host_try_next_candidate() moves on to the next controller. Cleared
 * on scan reset and whenever a fresh candidate comes online.
 */
static int host_faulted = 0;
static int host_recovered_flag = 0;

/* Scan state for the pointer enumeration loop. */
#define USB_HOST_MAX_CONTROLLERS 8
static usb_pci_controller_t scan_controllers[USB_HOST_MAX_CONTROLLERS];
static int scan_count = 0;
static uint8_t scan_tried[USB_HOST_MAX_CONTROLLERS];
static int scan_pass = 0;

/*
 * Safety level. Set from the kernel cmdline before usb_host_scan_reset()
 *   0 = normal: try every supported controller
 *   1 = safe: skip xHCI (our xHCI path is still narrow on some real chipsets)
 *   2 = minimal: only UHCI/OHCI (avoids EHCI/xHCI BIOS quirks entirely)
 *   3 = off: refuse to bring up any controller
 */
static int host_safety_level = 0;

/*
 * The "[usb] host: priority order ..." banner is printed once per scan so
 * a fallback test can grep a single line for the structured fallback chain.
 * Reset whenever usb_host_scan_reset() runs.
 */
static int order_logged = 0;

typedef struct {
    uint8_t prog_if;
    const char* name;
    int (*init)(const usb_pci_controller_t* controller);
} usb_host_driver_t;

static const usb_host_driver_t host_drivers[] = {
    { HCTRL_EHCI, "EHCI", ehci_init },
    { HCTRL_XHCI, "xHCI", xhci_init },
    { HCTRL_OHCI, "OHCI", ohci_init },
    { HCTRL_UHCI, "UHCI", uhci_init }
};

/* Default: EHCI then xHCI (companion handoff). Bay Trail with XUSB2PR!=0
 * flips to xHCI-first via baytrail_usb_prefer_xhci(). */
static const uint8_t host_order_default[] = {
    HCTRL_EHCI, HCTRL_XHCI, HCTRL_OHCI, HCTRL_UHCI
};
static const uint8_t host_order_xhci_first[] = {
    HCTRL_XHCI, HCTRL_EHCI, HCTRL_OHCI, HCTRL_UHCI
};

/* ---- Serial debug helpers ----
 *
 * host_serial routes through print() (panel + COM1 + 0xE9 via the active
 * print sink) AND port 0xE9 directly so messages are visible both on the
 * VGA/VESA console and in `-serial file:` captures on QEMU. The earlier
 * variant only wrote to 0xE9, which made the host fallback-chain
 * diagnostics invisible on `-serial file:` setups (only `-debugcon`
 * captures port 0xE9). */
static void host_serial(const char* s) {
    driver_log(s);
    print(s);
    const char* p = s;
    while (*p) { outb(0xE9, *p++); }
}
static void host_serial_u8(uint8_t v) {
    char buf[4];
    if (v >= 100) { buf[0]='0'+v/100; v%=100; buf[1]='0'+v/10; buf[2]='0'+(v%10); buf[3]=0; }
    else if (v >= 10) { buf[0]='0'+v/10; buf[1]='0'+(v%10); buf[2]=0; }
    else { buf[0]='0'+v; buf[1]=0; }
    host_serial(buf);
}
static void host_serial_hex32(uint32_t v) {
    char buf[11]; buf[0]='0'; buf[1]='x';
    for (int i=0; i<8; i++) {
        int d = (v >> (28 - i*4)) & 0xF;
        buf[2+i] = d < 10 ? '0'+d : 'A'+d-10;
    }
    buf[10]=0;
    host_serial(buf);
}

static void host_lock(uint32_t lock_bits, const char* msg) {
    if ((host_state.feature_locks & lock_bits) == lock_bits) return;
    host_state.feature_locks |= lock_bits;
    if (msg) { print(msg); host_serial(msg); }
}

/* Returns the active controller's own health latch (no host_faulted gate). */
static int active_controller_healthy(void) {
    if (!host_state.active || host_controller_type == HCTRL_NONE) return 0;
    if (host_controller_type == HCTRL_UHCI) return uhci_controller_healthy();
    if (host_controller_type == HCTRL_OHCI) return ohci_controller_healthy();
    if (host_controller_type == HCTRL_EHCI) return ehci_controller_healthy();
    if (host_controller_type == HCTRL_XHCI) return xhci_controller_healthy();
    return 0;
}

/* Latch the host-layer fault if the active controller has gone unhealthy. */
static void host_note_health(void) {
    if (!active_controller_healthy()) host_faulted = 1;
}

void host_mark_faulted(void) {
    host_faulted = 1;
}

int host_controller_faulted(void) {
    return host_faulted;
}

static const usb_host_driver_t* driver_for_prog_if(uint8_t prog_if) {
    for (uint32_t i = 0; i < sizeof(host_drivers) / sizeof(host_drivers[0]); i++) {
        if (host_drivers[i].prog_if == prog_if)
            return &host_drivers[i];
    }
    return NULL;
}

void usb_host_set_safety(int level) {
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    host_safety_level = level;
    host_serial("USB host: safety level=");
    host_serial_u8((uint8_t)level);
    host_serial("\n");
}

int usb_host_safety_level(void) {
    return host_safety_level;
}

static int safety_allows_prog_if(uint8_t prog_if) {
    switch (host_safety_level) {
        case 0: return 1;
        case 1: return prog_if != HCTRL_XHCI;
        case 2: return prog_if == HCTRL_UHCI || prog_if == HCTRL_OHCI;
        case 3: return 0;
        default: return 1;
    }
}

void usb_host_scan_reset(void) {
    host_state.active = 0;
    host_state.feature_locks = 0;
    host_controller_type = HCTRL_NONE;
    host_faulted = 0;
    scan_count = 0;
    order_logged = 0;  /* re-emit fallback priority order on each fresh scan */
    if (host_safety_level >= 3) {
        host_serial("USB host: scan skipped (usb=off).\n");
        for (int i = 0; i < USB_HOST_MAX_CONTROLLERS; i++) scan_tried[i] = 0;
        scan_pass = 0;
        return;
    }
    /* Unhide Bay Trail EHCI before the PCI class scan (XUSB2PR=0 path). */
    baytrail_usb_prepare_companion();
    scan_count = pci_find_usb_controllers(scan_controllers, USB_HOST_MAX_CONTROLLERS);
    if (scan_count < 0) scan_count = 0;
    if (scan_count > USB_HOST_MAX_CONTROLLERS) scan_count = USB_HOST_MAX_CONTROLLERS;
    for (int i = 0; i < USB_HOST_MAX_CONTROLLERS; i++) scan_tried[i] = 0;
    scan_pass = 0;

    if (scan_count <= 0) {
        host_serial("USB host: no controllers detected.\n");
    } else {
        host_serial("USB host: scan ready, controllers=");
        host_serial_u8((uint8_t)scan_count); host_serial("\n");
    }
}

/*
 * Structured fallback chain. host_drivers[] is { EHCI, xHCI, OHCI, UHCI } --
 * deliberately not pure descending speed because:
 *   - EHCI must be brought up first so it can release low-/full-speed ports
 *     to its companion OHCI/UHCI (port_owner handoff). Without that, USB 1
 *     mice on PCH chipsets are invisible.
 *   - xHCI is tried second so a Bay-Trail-class SoC (no companion EHCI)
 *     still gets first crack at every USB-2/3 port. Intel xHCI port-routing
 *     writes are gated on EHCI-companion presence inside xhci_init.
 *   - OHCI/UHCI catch the residual full-speed devices the EHCI handoff
 *     pushed to a companion.
 * On every init() == 0 we move to the next rung in the chain.
 */
static void log_priority_order(void) {
    if (order_logged) return;
    order_logged = 1;
    const uint8_t* order = baytrail_usb_prefer_xhci()
                               ? host_order_xhci_first : host_order_default;
    host_serial("[usb] host: priority order =");
    for (int i = 0; i < 4; i++) {
        const usb_host_driver_t* d = driver_for_prog_if(order[i]);
        if (!d) continue;
        host_serial(" ");
        host_serial(d->name);
        if (!safety_allows_prog_if(d->prog_if))
            host_serial("(skipped-by-safety)");
    }
    host_serial("\n");
}

int usb_host_try_next_candidate(void) {
    const uint8_t* order = baytrail_usb_prefer_xhci()
                               ? host_order_xhci_first : host_order_default;

    log_priority_order();

    while (scan_pass < 4) {
        const usb_host_driver_t* pass_driver = driver_for_prog_if(order[scan_pass]);
        if (!pass_driver) {
            scan_pass++;
            continue;
        }
        if (!safety_allows_prog_if(pass_driver->prog_if)) {
            host_serial("[usb] host: skip ");
            host_serial(pass_driver->name);
            host_serial(" (gooberos.usb safety filter).\n");
            scan_pass++;
            continue;
        }
        for (int i = 0; i < scan_count; i++) {
            if (scan_tried[i]) continue;
            if (scan_controllers[i].prog_if != pass_driver->prog_if) continue;
            scan_tried[i] = 1;

            host_serial("[usb] host: try ");
            host_serial(pass_driver->name); host_serial(" at ");
            host_serial_u8(scan_controllers[i].bus); host_serial(":");
            host_serial_u8(scan_controllers[i].slot); host_serial(":");
            host_serial_u8(scan_controllers[i].func);
            host_serial(" bar0="); host_serial_hex32(scan_controllers[i].bar0);
            host_serial("\n");

            if (pass_driver->init(&scan_controllers[i])) {
                host_controller_type = pass_driver->prog_if;
                host_state.controller = scan_controllers[i];
                host_state.active = 1;
                host_state.feature_locks = 0;
                host_faulted = 0;
                host_serial("[usb] host: selected ");
                host_serial(pass_driver->name);
                host_serial(" -- candidate online.\n");
                return 1;
            }

            host_serial("[usb] host: ");
            host_serial(pass_driver->name);
            host_serial(" init failed, advancing fallback chain.\n");
        }
        scan_pass++;
    }

    /*
     * Fallback chain exhausted. Keep a healthy active controller for hot-plug,
     * but drop a poisoned one (Bay Trail has only xHCI — keeping a faulted
     * controller just soft-locks USB and blocks PS/2 clarity).
     */
    if (host_state.active) {
        if (host_faulted || !active_controller_healthy()) {
            host_serial("[usb] host: fallback chain exhausted, "
                        "dropping faulted ");
            host_serial(usb_host_controller_name());
            host_serial(".\n");
            host_state.active = 0;
            host_controller_type = HCTRL_NONE;
        } else {
            host_serial("[usb] host: fallback chain exhausted, "
                        "keeping previously-selected ");
            host_serial(usb_host_controller_name());
            host_serial(" as active.\n");
        }
    } else {
        host_controller_type = HCTRL_NONE;
        host_serial("[usb] host: fallback chain exhausted, "
                    "no controller selected.\n");
    }
    return 0;
}

void usb_host_promote_active(void) {
    if (host_state.active) {
        print("USB host: selected ");
        print(usb_host_controller_name());
        print(".\n");
    }
}

int usb_host_consumed_recovery(void) {
    int v = host_recovered_flag;
    host_recovered_flag = 0;
    return v;
}

static int usb_host_recover_next(const char* why) {
    host_serial(why);
    print(why);
    host_state.active = 0;
    host_faulted = 1;
    host_controller_type = HCTRL_NONE;
    if (usb_host_try_next_candidate()) {
        host_recovered_flag = 1;
        usb_host_promote_active();
        return 1;
    }
    return 0;
}

void usb_host_init(void) {
    usb_host_scan_reset();
    if (usb_host_try_next_candidate()) {
        usb_host_promote_active();
    } else {
        host_serial("USB host: no supported controller initialized.\n");
        print("USB host: no supported controller initialized.\n");
    }
}

void usb_host_poll(void) {
    if (!host_state.active) return;

    if (host_controller_type == HCTRL_UHCI) {
        uhci_poll();
        if (!uhci_controller_healthy()) {
            if (!usb_host_recover_next(
                    "USB host: UHCI runtime error, trying next controller.\n")) {
                host_lock(USB_HOST_LOCK_RUNTIME | USB_HOST_LOCK_POINTER_ENUM,
                          "USB host: runtime error, USB features soft-locked.\n");
            }
        }
    } else if (host_controller_type == HCTRL_OHCI) {
        ohci_poll();
        if (!ohci_controller_healthy()) {
            if (!usb_host_recover_next(
                    "USB host: OHCI runtime error, trying next controller.\n")) {
                host_lock(USB_HOST_LOCK_RUNTIME | USB_HOST_LOCK_POINTER_ENUM,
                          "USB host: runtime error, USB features soft-locked.\n");
            }
        }
    } else if (host_controller_type == HCTRL_EHCI) {
        ehci_poll();
        if (!ehci_controller_healthy()) {
            if (!usb_host_recover_next(
                    "USB host: EHCI runtime error, trying next controller.\n")) {
                host_lock(USB_HOST_LOCK_RUNTIME | USB_HOST_LOCK_POINTER_ENUM,
                          "USB host: EHCI runtime error, USB features soft-locked.\n");
            }
        }
    } else if (host_controller_type == HCTRL_XHCI) {
        xhci_poll();
        if (!xhci_controller_healthy()) {
            if (!usb_host_recover_next(
                    "USB host: xHCI runtime error, trying next controller.\n")) {
                host_lock(USB_HOST_LOCK_RUNTIME | USB_HOST_LOCK_POINTER_ENUM,
                          "USB host: xHCI runtime error, USB features soft-locked.\n");
            }
        }
    }
}

int usb_host_ready(void) {
    return host_state.active;
}

uint8_t usb_host_controller_type(void) {
    return host_controller_type;
}

const char* usb_host_controller_name(void) {
    const usb_host_driver_t* driver = driver_for_prog_if(host_controller_type);
    return driver ? driver->name : "None";
}

int usb_host_is_healthy(void) {
    if (!host_state.active) return 0;
    return (host_state.feature_locks & USB_HOST_LOCK_RUNTIME) == 0;
}

int usb_host_pointer_enumeration_allowed(void) {
    if (!host_state.active) return 0;
    return (host_state.feature_locks & USB_HOST_LOCK_POINTER_ENUM) == 0;
}

/* ---- Controller-agnostic dispatch ---- */
int host_controller_healthy(void) {
    if (host_faulted) return 0;
    return active_controller_healthy();
}

int host_port_count(void) {
    if (host_controller_type == HCTRL_UHCI) return 2;
    if (host_controller_type == HCTRL_OHCI) return ohci_port_count();
    if (host_controller_type == HCTRL_EHCI) return ehci_port_count();
    if (host_controller_type == HCTRL_XHCI) return xhci_port_count();
    return 0;
}

int host_port_companion_owned(int port) {
    if (host_controller_type == HCTRL_EHCI)
        return ehci_port_owned_by_companion(port);
    return 0;
}

int host_port_connected(int port) {
    if (host_controller_type == HCTRL_UHCI) return uhci_port_connected(port);
    if (host_controller_type == HCTRL_OHCI) return ohci_port_connected(port);
    if (host_controller_type == HCTRL_EHCI) return ehci_port_connected(port);
    if (host_controller_type == HCTRL_XHCI) return xhci_port_connected(port);
    return 0;
}

int host_port_low_speed(int port) {
    if (host_controller_type == HCTRL_UHCI) return uhci_port_low_speed(port);
    if (host_controller_type == HCTRL_OHCI) return ohci_port_low_speed(port);
    if (host_controller_type == HCTRL_EHCI) return ehci_port_low_speed(port);
    if (host_controller_type == HCTRL_XHCI) return xhci_port_low_speed(port);
    return 0;
}

int host_port_protocol_speed(int port) {
    if (host_controller_type == HCTRL_XHCI) return xhci_port_protocol_speed(port);
    if (host_port_low_speed(port)) return 2; /* LS */
    if (host_port_connected(port)) return 3; /* treat as HS for ranking */
    return 0;
}

int host_port_reset(int port) {
    int ret = 0;
    if (host_faulted) return -1;  /* controller wedged: do not poke it further */
    if (host_controller_type == HCTRL_UHCI) uhci_port_reset(port);
    else if (host_controller_type == HCTRL_OHCI) ohci_port_reset(port);
    else if (host_controller_type == HCTRL_EHCI) ehci_port_reset(port);
    else if (host_controller_type == HCTRL_XHCI) ret = xhci_port_reset(port);
    else ret = -1;
    host_note_health();
    return ret;
}

int host_has_active_slot(void) {
    if (host_faulted) return 0;
    if (host_controller_type == HCTRL_XHCI) return xhci_has_active_slot();
    /* UHCI/OHCI/EHCI do not track a slot id; treat as always-ready after reset. */
    return 1;
}

int host_ep0_soft_fail_pending(void) {
    if (host_controller_type == HCTRL_XHCI) return xhci_ep0_soft_fail_pending();
    return 0;
}

void host_clear_ep0_soft_fail(void) {
    if (host_controller_type == HCTRL_XHCI) xhci_clear_ep0_soft_fail();
}

void host_abandon_slot(void) {
    if (host_controller_type == HCTRL_XHCI) xhci_abandon_slot();
}

/*
 * Hot-plug change-bit dispatch. host_port_change_pending() does ONE MMIO read
 * per call (at most 32 ports x 50ms = 640 reads/sec, all single-dword), so
 * polling at 60+ Hz inside vesa_desktop_main_loop / x64_repl_pump_mouse is
 * cheap. host_port_change_ack() clears the connect-status-change latch on
 * the active controller's port. Both short-circuit when the active
 * controller has faulted, so a wedged xHCI never gets re-poked.
 */
int host_port_change_pending(int port) {
    if (host_faulted || !host_state.active) return 0;
    if (host_controller_type == HCTRL_UHCI) return uhci_port_change_pending(port);
    if (host_controller_type == HCTRL_OHCI) return ohci_port_change_pending(port);
    if (host_controller_type == HCTRL_EHCI) return ehci_port_change_pending(port);
    if (host_controller_type == HCTRL_XHCI) return xhci_port_change_pending(port);
    return 0;
}

void host_port_change_ack(int port) {
    if (host_faulted || !host_state.active) return;
    if (host_controller_type == HCTRL_UHCI) uhci_port_change_ack(port);
    else if (host_controller_type == HCTRL_OHCI) ohci_port_change_ack(port);
    else if (host_controller_type == HCTRL_EHCI) ehci_port_change_ack(port);
    else if (host_controller_type == HCTRL_XHCI) xhci_port_change_ack(port);
}

int host_control_transfer(uint8_t dev_addr, uint8_t endpoint,
                          uint8_t* setup_pkt, uint8_t* data, uint16_t data_len,
                          int direction_in) {
    if (host_faulted) return -1;
    int ret = -1;
    if (host_controller_type == HCTRL_UHCI)
        ret = uhci_control_transfer(dev_addr, endpoint, setup_pkt, data, data_len, direction_in);
    else if (host_controller_type == HCTRL_OHCI)
        ret = ohci_control_transfer(dev_addr, endpoint, setup_pkt, data, data_len, direction_in);
    else if (host_controller_type == HCTRL_EHCI)
        ret = ehci_control_transfer(dev_addr, endpoint, setup_pkt, data, data_len, direction_in);
    else if (host_controller_type == HCTRL_XHCI)
        ret = xhci_control_transfer(dev_addr, endpoint, setup_pkt, data, data_len, direction_in);
    host_note_health();
    return ret;
}

int host_bulk_transfer(uint8_t endpoint, uint8_t* data, uint16_t data_len,
                       int direction_in) {
    if (host_faulted) return -1;
    int ret = -1;
    if (host_controller_type == HCTRL_XHCI)
        ret = xhci_bulk_transfer(endpoint, data, data_len, direction_in);
    host_note_health();
    return ret;
}

int host_configure_bulk_eps(uint8_t ep_out, uint8_t ep_in,
                            uint16_t mps_out, uint16_t mps_in) {
    if (host_faulted) return -1;
    int ret = -1;
    if (host_controller_type == HCTRL_XHCI)
        ret = xhci_configure_bulk_eps(ep_out, ep_in, mps_out, mps_in);
    host_note_health();
    return ret;
}

int host_schedule_interrupt(uint8_t dev_addr, uint8_t endpoint,
                            uint16_t max_packet, uint8_t interval_frames) {
    if (host_faulted) return -1;
    int ret = -1;
    if (host_controller_type == HCTRL_UHCI)
        ret = uhci_schedule_interrupt(dev_addr, endpoint, max_packet, interval_frames);
    else if (host_controller_type == HCTRL_OHCI)
        ret = ohci_schedule_interrupt(dev_addr, endpoint, max_packet, interval_frames);
    else if (host_controller_type == HCTRL_EHCI)
        ret = ehci_schedule_interrupt(dev_addr, endpoint, max_packet, interval_frames);
    else if (host_controller_type == HCTRL_XHCI)
        ret = xhci_schedule_interrupt(dev_addr, endpoint, max_packet, interval_frames);
    host_note_health();
    return ret;
}

void host_remove_interrupt(void) {
    if (host_controller_type == HCTRL_UHCI) uhci_remove_interrupt();
    else if (host_controller_type == HCTRL_OHCI) ohci_remove_interrupt();
    else if (host_controller_type == HCTRL_EHCI) ehci_remove_interrupt();
    else if (host_controller_type == HCTRL_XHCI) xhci_remove_interrupt();
}

int host_interrupt_active(void) {
    if (host_controller_type == HCTRL_UHCI) return uhci_interrupt_active();
    if (host_controller_type == HCTRL_OHCI) return ohci_interrupt_active();
    if (host_controller_type == HCTRL_EHCI) return ehci_interrupt_active();
    if (host_controller_type == HCTRL_XHCI) return xhci_interrupt_active();
    return 0;
}

uint8_t* host_get_report(int* ready) {
    if (host_controller_type == HCTRL_UHCI) return uhci_get_report(ready);
    if (host_controller_type == HCTRL_OHCI) return ohci_get_report(ready);
    if (host_controller_type == HCTRL_EHCI) return ehci_get_report(ready);
    if (host_controller_type == HCTRL_XHCI) return xhci_get_report(ready);
    if (ready) *ready = 0;
    return NULL;
}

void host_ack_report(void) {
    if (host_controller_type == HCTRL_UHCI) uhci_ack_report();
    else if (host_controller_type == HCTRL_OHCI) ohci_ack_report();
    else if (host_controller_type == HCTRL_EHCI) ehci_ack_report();
    else if (host_controller_type == HCTRL_XHCI) xhci_ack_report();
}

void usb_host_print_usb2_route(void (*write)(const char*)) {
    baytrail_usb_print_status(write);
}
