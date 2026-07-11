#include "enumeration.h"
#include <stddef.h>
#include "../host/host.h"
#include "../hid/hid.h"
#include "../usb.h"
#include "../../timer/timer.h"
#include "../../io/io.h"
#include "../../diagnostics/driver_log.h"

extern void print(const char* str);

static void enum_serial(const char* s) {
    while (*s) { outb(0xE9, *s++); }
}
static void enum_print(const char* s) {
    driver_log(s);
    print(s);
    enum_serial(s);
}

/*
 * Print "<prefix>port=N attempt=M/T<suffix>" with no allocation. Used by the
 * per-port retry logger so a flaky-vs-dead port is distinguishable in the
 * serial log without building a printf in the kernel.
 */
static void enum_print_retry(int port, int attempt, int total,
                             const char* suffix) {
    char buf[6];
    int n;
    enum_print("USB enum: port=");
    n = port;
    if (n >= 10) { buf[0] = '0' + n / 10; buf[1] = '0' + (n % 10); buf[2] = 0; }
    else         { buf[0] = '0' + n; buf[1] = 0; }
    enum_print(buf);
    enum_print(" reset-retry attempt=");
    n = attempt;
    if (n >= 10) { buf[0] = '0' + n / 10; buf[1] = '0' + (n % 10); buf[2] = 0; }
    else         { buf[0] = '0' + n; buf[1] = 0; }
    enum_print(buf);
    enum_print("/");
    n = total;
    if (n >= 10) { buf[0] = '0' + n / 10; buf[1] = '0' + (n % 10); buf[2] = 0; }
    else         { buf[0] = '0' + n; buf[1] = 0; }
    enum_print(buf);
    if (suffix) enum_print(suffix);
}

#define MAX_DEVICES 8

/*
 * Bounded enumeration budgets, all driven off the IRQ-independent TSC clock
 * (timer_deadline_ms) so an SMM storm that freezes IRQ0 can never make any of
 * them infinite:
 *   - per-port:        wrap each host_port_reset()+enumerate_device() attempt
 *   - per-controller:  bring-up budget for the whole port scan (~300 ticks)
 *   - per-scan:        hard wall-time cap for one usb_enumerate_devices() call
 * Plus a Linux-style stable-connect debounce and per-port reset retries.
 */
#define ENUM_PORT_BUDGET_MS       700U   /* reset+enumerate cap per port      */
#define ENUM_CTRL_BUDGET_MS      1500U   /* controller bring-up cap           */
#define ENUM_SCAN_BUDGET_MS      2500U   /* hard cap for one scan             */
#define ENUM_PORT_RETRIES        2       /* reset/enumerate attempts/port     */
#define ENUM_DEBOUNCE_POLL_MS    20U     /* connect poll interval             */
#define ENUM_DEBOUNCE_STABLE_MS  60U     /* connection must persist this long */
#define ENUM_DEBOUNCE_CAP_MS     600U    /* total debounce window cap         */

static usb_device_t devices[MAX_DEVICES];
static int device_count = 0;
static int current_address = 1;
static uint32_t hid_control_next_poll = 0;
static int hid_control_logged = 0;
static int hid_interrupt_logged = 0;

static uint8_t buf_scratch[256];

/* Bounded fixed delay (TSC clock + iteration ceiling) -- see timer.c. */
static void tiny_delay_ms(uint32_t ms) {
    timer_busy_wait_ms(ms);
}

/* ---- Build a SETUP packet and do a control transfer ---- */
static int do_ctrl(uint8_t dev_addr, uint8_t bmReqType, uint8_t bRequest,
                   uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                   uint8_t* data, int direction_in) {
    uint8_t setup[8];
    setup[0] = bmReqType;
    setup[1] = bRequest;
    setup[2] = wValue & 0xFF;
    setup[3] = (wValue >> 8) & 0xFF;
    setup[4] = wIndex & 0xFF;
    setup[5] = (wIndex >> 8) & 0xFF;
    setup[6] = wLength & 0xFF;
    setup[7] = (wLength >> 8) & 0xFF;
    return host_control_transfer(dev_addr, 0, setup, data, wLength, direction_in);
}

/* ---- Read 8 bytes of device descriptor to get max packet size ---- */
static int get_device_desc_short(uint8_t addr, usb_device_descriptor_t* desc) {
    for (int attempt = 0; attempt < 3; attempt++) {
        int ret = do_ctrl(addr,
                          USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                          USB_REQ_GET_DESCRIPTOR,
                          0x0100,
                          0,
                          8,
                          (uint8_t*)desc, 1);
        if (ret == 0) return 0;
        tiny_delay_ms(20);
    }
    return -1;
}

/* ---- Read full device descriptor ---- */
static int get_device_desc_full(uint8_t addr, usb_device_descriptor_t* desc) {
    for (int attempt = 0; attempt < 3; attempt++) {
        int ret = do_ctrl(addr,
                          USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                          USB_REQ_GET_DESCRIPTOR,
                          0x0100,
                          0,
                          sizeof(usb_device_descriptor_t),
                          (uint8_t*)desc, 1);
        if (ret == 0) return 0;
        tiny_delay_ms(20);
    }
    return -1;
}

/* ---- Set device address ---- */
static int set_address(uint8_t old_addr, uint8_t new_addr) {
    int ret = do_ctrl(old_addr,
                      USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                      USB_REQ_SET_ADDRESS,
                      new_addr,
                      0,
                      0,
                      NULL, 0);
    if (ret != 0) return -1;
    tiny_delay_ms(50);  /* USB 2.0 SET_ADDRESS recovery (>= 2 ms; be generous) */
    return 0;
}

/* ---- Get configuration descriptor ---- */
static int get_config_desc(uint8_t addr, uint8_t* buf, uint16_t max_len) {
    usb_config_descriptor_t cfg_hdr;
    int ret = do_ctrl(addr,
                      USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                      USB_REQ_GET_DESCRIPTOR,
                      0x0200,
                      0,
                      9,
                      (uint8_t*)&cfg_hdr, 1);
    if (ret != 0) return -1;

    uint16_t total = cfg_hdr.wTotalLength;
    if (total > max_len) total = max_len;

    ret = do_ctrl(addr,
                  USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                  USB_REQ_GET_DESCRIPTOR,
                  0x0200,
                  0,
                  total,
                  buf, 1);
    if (ret != 0) return -1;
    return total;
}

/* ---- Set configuration ---- */
static int set_configuration(uint8_t addr, uint8_t config_value) {
    return do_ctrl(addr,
                   USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                   USB_REQ_SET_CONFIGURATION,
                   config_value,
                   0,
                   0,
                   NULL, 0);
}

/* ---- HID: Set Protocol (boot) ---- */
static int hid_set_protocol(uint8_t addr, uint8_t interface) {
    return do_ctrl(addr,
                   USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                   USB_HID_REQ_SET_PROTOCOL,
                   0,
                   interface,
                   0,
                   NULL, 0);
}

/* ---- HID: Set Idle ---- */
static int hid_set_idle(uint8_t addr, uint8_t interface, uint8_t duration) {
    return do_ctrl(addr,
                   USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                   USB_HID_REQ_SET_IDLE,
                   (duration << 8) | 0,
                   interface,
                   0,
                   NULL, 0);
}

/*
 * Per-port budget check used BETWEEN control transfers inside
 * enumerate_device(). This is the gap the old code had: the per-scan budget
 * was only checked at the top of the port loop, so one slow port could run
 * unbounded. Returns nonzero when the port's time (or the controller) is up.
 */
static int port_budget_blown(uint64_t port_deadline) {
    return host_controller_faulted() || timer_deadline_expired(port_deadline);
}

static int vid_is_touchpad(uint16_t vendor_id) {
    /* ELAN, Synaptics, ALPS, and Cypress commonly ship laptop touchpads. */
    return vendor_id == 0x04F3 || vendor_id == 0x06CB ||
           vendor_id == 0x044E || vendor_id == 0x04B4;
}

/* ---- Enumerate a single device on a given port ---- */
static int enumerate_device(int port, int low_speed, uint64_t port_deadline) {
    if (device_count >= MAX_DEVICES) return -1;
    if (current_address >= 127) return -1;

    uint8_t addr = current_address++;
    usb_device_descriptor_t dd;
    int ret;

    if (port_budget_blown(port_deadline)) { current_address--; return -1; }
    ret = get_device_desc_short(0, &dd);
    if (ret != 0) {
        enum_print("USB enum: short desc failed.\n");
        current_address--;  /* reclaim unused address */
        return -1;
    }
    uint8_t max_pkt = dd.bMaxPacketSize0;
    if (max_pkt == 0) max_pkt = 8;

    if (port_budget_blown(port_deadline)) { current_address--; return -1; }
    ret = set_address(0, addr);
    if (ret != 0) {
        enum_print("USB enum: set_address failed.\n");
        current_address--;
        return -1;
    }

    if (port_budget_blown(port_deadline)) { current_address--; return -1; }
    ret = get_device_desc_full(addr, &dd);
    if (ret != 0) {
        enum_print("USB enum: full desc failed.\n");
        current_address--;
        return -1;
    }

    if (port_budget_blown(port_deadline)) { current_address--; return -1; }
    ret = get_config_desc(addr, buf_scratch, sizeof(buf_scratch));
    if (ret <= 0) {
        enum_print("USB enum: config desc failed.\n");
        current_address--;
        return -1;
    }

    usb_interface_descriptor_t* intf = NULL;
    usb_endpoint_descriptor_t* mouse_ep_in = NULL;
    usb_endpoint_descriptor_t* keyboard_ep_in = NULL;
    int found_hid_mouse = 0;
    int found_hid_keyboard = 0;
    int config_value = 0;
    int mouse_interface_number = 0;
    int keyboard_interface_number = 0;
    int active_hid_protocol = 0;

    uint8_t* p = buf_scratch;
    uint8_t* end = p + ret;

    if (ret >= (int)sizeof(usb_config_descriptor_t)) {
        usb_config_descriptor_t* cfg = (usb_config_descriptor_t*)p;
        config_value = cfg->bConfigurationValue;
        p += cfg->bLength;

        while (p < end) {
            uint8_t len = p[0];
            uint8_t type = p[1];
            if (len == 0) break;
            if (p + len > end) break;

            if (type == USB_DT_INTERFACE && len >= 9) {
                intf = (usb_interface_descriptor_t*)p;
                if (intf->bInterfaceClass == USB_CLASS_HID &&
                    intf->bInterfaceSubClass == USB_HID_SUBCLASS_BOOT &&
                    intf->bInterfaceProtocol == USB_HID_PROTOCOL_MOUSE) {
                    found_hid_mouse = 1;
                    active_hid_protocol = USB_HID_PROTOCOL_MOUSE;
                    mouse_interface_number = intf->bInterfaceNumber;
                } else if (intf->bInterfaceClass == USB_CLASS_HID &&
                           intf->bInterfaceSubClass == USB_HID_SUBCLASS_BOOT &&
                           intf->bInterfaceProtocol == USB_HID_PROTOCOL_KEYBOARD) {
                    found_hid_keyboard = 1;
                    active_hid_protocol = USB_HID_PROTOCOL_KEYBOARD;
                    keyboard_interface_number = intf->bInterfaceNumber;
                } else {
                    active_hid_protocol = 0;
                }
            }

            if (type == USB_DT_ENDPOINT && len >= 7 && active_hid_protocol) {
                usb_endpoint_descriptor_t* ep = (usb_endpoint_descriptor_t*)p;
                if ((ep->bEndpointAddress & 0x80) &&
                    (ep->bmAttributes & 0x03) == 3) {
                    if (active_hid_protocol == USB_HID_PROTOCOL_MOUSE)
                        mouse_ep_in = ep;
                    else if (active_hid_protocol == USB_HID_PROTOCOL_KEYBOARD)
                        keyboard_ep_in = ep;
                }
            }

            p += len;
        }
    }

    int chosen_protocol = 0;
    usb_endpoint_descriptor_t* ep_in = NULL;
    int interface_number = 0;
    if (found_hid_mouse && mouse_ep_in) {
        chosen_protocol = USB_HID_PROTOCOL_MOUSE;
        ep_in = mouse_ep_in;
        interface_number = mouse_interface_number;
    } else if (found_hid_keyboard && keyboard_ep_in) {
        chosen_protocol = USB_HID_PROTOCOL_KEYBOARD;
        ep_in = keyboard_ep_in;
        interface_number = keyboard_interface_number;
    }

    if (!chosen_protocol || !ep_in) {
        if (intf && intf->bInterfaceClass == USB_CLASS_HID) {
            enum_print("USB enum: HID device is not boot mouse/keyboard compatible.\n");
        } else {
            enum_print("USB enum: no HID boot mouse/keyboard.\n");
        }
        current_address--;  /* reclaim: device never configured */
        return -1;
    }

    if (port_budget_blown(port_deadline)) { current_address--; return -1; }
    ret = set_configuration(addr, config_value);
    if (ret != 0) {
        enum_print("USB enum: set_config failed.\n");
        current_address--;
        return -1;
    }

    if (port_budget_blown(port_deadline)) { current_address--; return -1; }
    ret = hid_set_protocol(addr, interface_number);
    if (ret != 0) {
        enum_print("USB enum: set_protocol failed.\n");
        current_address--;
        return -1;
    }

    hid_set_idle(addr, interface_number, 0);

    /* Fill device entry */
    usb_device_t* dev = &devices[device_count++];
    dev->address = addr;
    dev->port = port;
    dev->speed = low_speed;
    dev->configured = 1;
    dev->class_code = USB_CLASS_HID;
    dev->subclass = USB_HID_SUBCLASS_BOOT;
    dev->protocol = (uint8_t)chosen_protocol;
    dev->interface_number = (uint8_t)interface_number;
    dev->max_packet_size = max_pkt;
    dev->ep_in = ep_in->bEndpointAddress;
    dev->ep_out = 0;
    dev->ep_in_max_pkt = ep_in->wMaxPacketSize;
    dev->ep_in_interval = ep_in->bInterval;

    /* Schedule interrupt IN for HID reports */
    ret = host_schedule_interrupt(addr, ep_in->bEndpointAddress & 0x7F,
                                  ep_in->wMaxPacketSize, ep_in->bInterval);
    if (ret != 0) {
        enum_print("USB enum: interrupt schedule failed.\n");
        if (device_count > 0) device_count--;
        current_address--;  /* reclaim: device entry rolled back */
        return -1;
    }

    if (chosen_protocol == USB_HID_PROTOCOL_MOUSE) {
        uint8_t is_touchpad = vid_is_touchpad(dd.idVendor) ? 1 : 0;
        usb_hid_register_boot_pointer_detail(1, is_touchpad, (uint8_t)port, addr,
                                             ep_in->bEndpointAddress,
                                             ep_in->wMaxPacketSize,
                                             ep_in->bInterval);
        enum_print("USB HID boot mouse enumerated.\n");
        return USB_HID_PROTOCOL_MOUSE;
    }

    usb_hid_register_boot_keyboard_detail(1, (uint8_t)port, addr,
                                          ep_in->bEndpointAddress,
                                          ep_in->wMaxPacketSize,
                                          ep_in->bInterval);
    enum_print("USB HID boot keyboard enumerated.\n");
    return USB_HID_PROTOCOL_KEYBOARD;
}

/* ---- Main enumeration entry point ---- */
void usb_enumerate_devices(void) {
    enum_print("USB enum: probing ports...\n");

    device_count = 0;
    current_address = 1;
    hid_control_next_poll = 0;
    hid_control_logged = 0;
    hid_interrupt_logged = 0;

    if (!host_controller_healthy()) {
        enum_print("USB enum: controller not healthy.\n");
        usb_hid_register_boot_pointer(0, 0);
        return;
    }

    int found_pointer = 0;
    int found_keyboard = 0;
    int ports = host_port_count();
    if (ports <= 0 || ports > 32) ports = 8;

    {
        char c[3]; int n = ports;
        if (n >= 10) { c[0] = '0' + n / 10; c[1] = '0' + n % 10; c[2] = 0; }
        else         { c[0] = '0' + n; c[1] = 0; }
        enum_print("USB enum: host reports ");
        enum_print(c);
        enum_print(" port(s).\n");
    }

    /*
     * Stable-connect debounce (Linux-style).
     *
     * On real hardware (especially Intel xHCI) CCS can take 50-500 ms to
     * stabilize after the controller is started, and a freshly inserted plug
     * bounces. We poll every 25 ms and require a connection to remain present
     * for a stable 100 ms window before trusting it, giving up after a 2000 ms
     * cap. Every wait is bounded by the TSC clock, so this can never hang.
     */
    int connected = 0;
    {
        uint64_t debounce_cap = timer_deadline_ms(ENUM_DEBOUNCE_CAP_MS);
        uint32_t stable_ms = 0;
        while (!timer_deadline_expired(debounce_cap)) {
            int any = 0;
            for (int port = 0; port < ports; port++) {
                if (host_port_connected(port)) { any = 1; break; }
            }
            if (any) {
                stable_ms += ENUM_DEBOUNCE_POLL_MS;
                if (stable_ms >= ENUM_DEBOUNCE_STABLE_MS) break;
            } else {
                stable_ms = 0;
            }
            tiny_delay_ms(ENUM_DEBOUNCE_POLL_MS);
        }
        for (int port = 0; port < ports; port++) {
            if (host_port_connected(port)) connected++;
        }
    }

    for (int port = 0; port < ports; port++) {
        if (host_port_connected(port)) {
            char c[3];
            if (port >= 10) { c[0] = '0' + port / 10; c[1] = '0' + port % 10; c[2] = 0; }
            else            { c[0] = '0' + port; c[1] = 0; }
            enum_print("USB enum: port ");
            enum_print(c);
            enum_print(" reports a device connected.\n");
        }
    }
    if (connected == 0) {
        enum_print("USB enum: no ports report a connected device after debounce (check port routing/cabling).\n");
    }

    /*
     * Two cooperating wall-time budgets, both on the TSC clock:
     *   - scan_deadline: hard cap for this whole usb_enumerate_devices() call.
     *   - ctrl_deadline: per-controller bring-up budget (tighter); whichever
     *     fires first stops the port loop. Each port additionally gets its own
     *     per-port budget (re-armed per retry) checked between transfers, so a
     *     single wedged port can no longer run unbounded.
     */
    uint64_t scan_deadline = timer_deadline_ms(ENUM_SCAN_BUDGET_MS);
    uint64_t ctrl_deadline = timer_deadline_ms(ENUM_CTRL_BUDGET_MS);

    for (int port = 0; port < ports && !found_pointer; port++) {
        if (host_controller_faulted()) {
            enum_print("USB enum: controller faulted, abandoning scan.\n");
            break;
        }
        if (timer_deadline_expired(scan_deadline) ||
            timer_deadline_expired(ctrl_deadline)) {
            enum_print("USB enum: bring-up/scan time budget exceeded, stopping.\n");
            break;
        }
        if (!host_port_connected(port)) continue;
        if (host_port_companion_owned(port)) {
            enum_print("USB enum: EHCI port handed to companion.\n");
            continue;
        }

        { char c[3];
          if (port >= 10) { c[0] = '0' + port / 10; c[1] = '0' + port % 10; c[2] = 0; }
          else            { c[0] = '0' + port; c[1] = 0; }
          enum_print("USB enum: enumerating port "); enum_print(c); enum_print("\n"); }

        /*
         * Per-port reset + enumerate with a few retries, then mark this port
         * dead and continue. Each attempt gets a fresh per-port budget; the
         * controller/scan budgets bound the total number of retries.
         *
         * Every attempt logs a canonical "USB enum: port=N reset-retry
         * attempt=M/T" line so a human can tell "this port is flaky" (1 or 2
         * retries before success) from "this port is dead" (T retries all
         * failed).
         */
        int enum_result = 0;
        int attempts_used = 0;
        for (int attempt = 1; attempt <= ENUM_PORT_RETRIES; attempt++) {
            if (host_controller_faulted() ||
                timer_deadline_expired(scan_deadline) ||
                timer_deadline_expired(ctrl_deadline)) {
                break;
            }
            attempts_used = attempt;
            enum_print_retry(port, attempt, ENUM_PORT_RETRIES, ": resetting\n");
            uint64_t port_deadline = timer_deadline_ms(ENUM_PORT_BUDGET_MS);

            int low_speed = host_port_low_speed(port);
            host_port_reset(port);
            if (host_controller_faulted()) break;
            if (host_port_companion_owned(port)) {
                enum_print("USB enum: port requires companion controller.\n");
                break;
            }

            enum_result = enumerate_device(port, low_speed, port_deadline);
            if (enum_result > 0) {
                enum_print_retry(port, attempt, ENUM_PORT_RETRIES,
                                 ": SUCCESS\n");
                break;
            }
            enum_print_retry(port, attempt, ENUM_PORT_RETRIES,
                             ": FAILED, will retry if budget allows\n");
        }
        if (enum_result <= 0 && attempts_used >= ENUM_PORT_RETRIES) {
            enum_print_retry(port, attempts_used, ENUM_PORT_RETRIES,
                             ": dead port (all retries exhausted)\n");
        }

        if (enum_result == USB_HID_PROTOCOL_MOUSE) {
            found_pointer = 1;
            /*
             * Stop as soon as we find one HID boot mouse. Our xHCI driver
             * only tracks a single active slot/EP0 ring, so continuing past
             * the first hit would clobber its state on subsequent ports.
             */
            enum_print("USB enum: boot mouse found, stopping scan.\n");
        } else if (enum_result == USB_HID_PROTOCOL_KEYBOARD) {
            found_keyboard = 1;
            /*
             * Keep scanning for a boot mouse. On laptops it is common for a
             * keyboard-like HID interface/controller path to enumerate before
             * the external mouse, and stopping here leaves the GUI pointer
             * stuck even though a mouse is present. If no mouse is found by the
             * end of the scan, this keyboard remains the fallback HID device.
             */
            enum_print("USB enum: boot keyboard found, continuing scan for mouse.\n");
        } else {
            /* Port exhausted its retries: mark dead and move on. */
            enum_print("USB enum: port gave up, marking dead and continuing.\n");
        }
    }

    if (found_pointer) {
        /* enumerate_device() already registered the pointer with endpoint
         * details and touchpad classification. Do not clobber that state here. */
        enum_print("USB enum: pointer device ready.\n");
    } else if (found_keyboard) {
        usb_hid_register_boot_pointer(0, 0);
        enum_print("USB enum: keyboard device ready, PS/2 pointer fallback.\n");
    } else {
        usb_hid_register_boot_pointer(0, 0);
        usb_hid_register_boot_keyboard(0);
        enum_print("USB enum: no pointer device found.\n");
    }
}

/*
 * Hot-plug single-port enumeration.
 *
 * Called from usb.c::usb_hotplug_scan() when the host's port-status-change
 * latch fires for a freshly connected device. We deliberately don't reset
 * device_count: the host drivers track ONE active interrupt endpoint and ONE
 * device slot, so re-running enumeration on a freshly connected port over
 * the existing slot is the right thing on the boot-mouse-only path. Returns:
 *   USB_HID_PROTOCOL_MOUSE     -> attached mouse, addr returned via *out_addr
 *   USB_HID_PROTOCOL_KEYBOARD  -> attached keyboard
 *   0                          -> no HID found / non-bootable / dead port
 *  -1                          -> controller faulted, abandon port
 *
 * Mirrors the per-port retry + budget logic of usb_enumerate_devices() but
 * scoped to ONE port -- the goal is to keep the existing boot-time path
 * unchanged.
 */
int usb_enumerate_port_hotplug(int port, uint8_t* out_addr) {
    if (out_addr) *out_addr = 0;

    if (!host_controller_healthy()) {
        enum_print("USB hotplug-enum: controller not healthy.\n");
        return -1;
    }
    if (port < 0 || port >= host_port_count()) return 0;
    if (!host_port_connected(port)) return 0;
    if (host_port_companion_owned(port)) {
        enum_print("USB hotplug-enum: port routed to companion controller.\n");
        return 0;
    }

    /*
     * Reset device_count + current_address back to 1 -- the existing host
     * drivers track a single active slot/EP, so the previously-attached
     * device's state is already torn down by the disconnect path before
     * this is called.
     */
    device_count = 0;
    current_address = 1;
    host_remove_interrupt();

    /* Per-port budget chain identical to the boot-time scan. */
    uint64_t scan_deadline = timer_deadline_ms(ENUM_SCAN_BUDGET_MS);

    int enum_result = 0;
    for (int attempt = 1; attempt <= ENUM_PORT_RETRIES; attempt++) {
        if (host_controller_faulted() ||
            timer_deadline_expired(scan_deadline)) break;

        enum_print_retry(port, attempt, ENUM_PORT_RETRIES,
                         ": hotplug resetting\n");
        uint64_t port_deadline = timer_deadline_ms(ENUM_PORT_BUDGET_MS);

        int low_speed = host_port_low_speed(port);
        host_port_reset(port);
        if (host_controller_faulted()) return -1;
        if (host_port_companion_owned(port)) {
            enum_print("USB hotplug-enum: port now routed to companion.\n");
            return 0;
        }

        enum_result = enumerate_device(port, low_speed, port_deadline);
        if (enum_result > 0) {
            enum_print_retry(port, attempt, ENUM_PORT_RETRIES,
                             ": hotplug SUCCESS\n");
            break;
        }
        enum_print_retry(port, attempt, ENUM_PORT_RETRIES,
                         ": hotplug FAILED, retrying if budget allows\n");
    }

    if (enum_result == USB_HID_PROTOCOL_MOUSE) {
        if (out_addr && device_count > 0) *out_addr = devices[device_count - 1].address;
        return USB_HID_PROTOCOL_MOUSE;
    }
    if (enum_result == USB_HID_PROTOCOL_KEYBOARD) {
        if (out_addr && device_count > 0) *out_addr = devices[device_count - 1].address;
        return USB_HID_PROTOCOL_KEYBOARD;
    }
    return 0;
}

void usb_enumeration_release_active(void) {
    /*
     * Hot-unplug: tear down the active interrupt endpoint, drop the device
     * registry entry, and reset the address counter so a re-plug starts
     * clean. The HID layer's pointer/keyboard "present" flags are reset
     * separately by the caller (usb.c) via usb_hid_register_*.
     */
    host_remove_interrupt();
    device_count = 0;
    current_address = 1;
    hid_control_next_poll = 0;
    hid_control_logged = 0;
    hid_interrupt_logged = 0;
}

int usb_get_device_count(void) {
    return device_count;
}

const usb_device_t* usb_get_device(int index) {
    if (index < 0 || index >= device_count) return NULL;
    return &devices[index];
}

void usb_process_hid_reports(void) {
    int ready;
    uint8_t* report = host_get_report(&ready);
    if (ready > 0) {
        if (!hid_interrupt_logged) {
            enum_print("USB HID: interrupt report path is live.\n");
            hid_interrupt_logged = 1;
        }
        /*
         * The host drivers expose data-ready as a flag, not a byte count, so
         * pass the boot-protocol report length for the active device class:
         * boot keyboards always send 8 bytes; boot mice send 3 bytes plus an
         * optional wheel byte. Host report buffers are cleared on re-arm, so a
         * 4-byte mouse window safely reads 0 for the wheel when absent.
         */
        uint8_t length = (usb_hid_has_keyboard_device() && !usb_hid_has_pointer_device())
                             ? 8 : 4;
        usb_hid_handle_boot_report(report, length);
        host_ack_report();
    } else if (ready < 0) {
        /* Re-arm on stall/error */
        host_ack_report();
    }

    /*
     * Real-hardware fallback: some controller paths enumerate a boot mouse but
     * never deliver periodic interrupt completions. Poll HID GET_REPORT over
     * endpoint 0 at a low rate. This is slower than interrupts, but endpoint 0
     * is the path that already worked for descriptors/configuration, so it is a
     * good rescue path for cheap mice and a useful diagnostic on bare metal.
     */
    if (!hid_interrupt_logged && usb_hid_has_pointer_device() && device_count > 0) {
        usb_device_t* dev = NULL;
        for (int i = 0; i < device_count; i++) {
            if (devices[i].protocol == USB_HID_PROTOCOL_MOUSE) {
                dev = &devices[i];
                break;
            }
        }
        if (dev) {
            uint32_t now = timer_ticks();
            if ((int32_t)(now - hid_control_next_poll) >= 0) {
                uint8_t ctrl_report[8];
                uint8_t len = dev->ep_in_max_pkt;
                if (len < 3) len = 3;
                if (len > sizeof(ctrl_report)) len = sizeof(ctrl_report);
                for (uint8_t i = 0; i < sizeof(ctrl_report); i++) ctrl_report[i] = 0;
                int ret = do_ctrl(dev->address,
                                  USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                                  USB_HID_REQ_GET_REPORT,
                                  0x0100,
                                  dev->interface_number,
                                  len,
                                  ctrl_report,
                                  1);
                if (ret == 0) {
                    if (!hid_control_logged) {
                        enum_print("USB HID: control GET_REPORT fallback is live.\n");
                        hid_control_logged = 1;
                    }
                    usb_hid_handle_boot_report(ctrl_report, len);
                    hid_control_next_poll = now + 2; /* ~50 Hz on 100 Hz PIT */
                } else {
                    if (!hid_control_logged) {
                        enum_print("USB HID: control GET_REPORT fallback failed once.\n");
                        hid_control_logged = 1;
                    }
                    hid_control_next_poll = now + 25;
                }
            }
        }
    }
}
