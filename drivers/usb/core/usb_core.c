/*
 * New USB core: controller-independent device lifecycle + class binding.
 *
 * Controllers remain online regardless of whether a pointer was found.
 * Enumeration walks descriptors and binds class drivers independently.
 *
 * Hotplug MUST stay non-blocking for the GUI pump: failed boot-time EP0 on
 * a CCS port left "connected && !existing", and the desktop's usb_poll()
 * retried full reset+EP0 every ~500 ms — that is the Lenovo "GUI freeze"
 * while text console (rare usb_poll) still felt fine.
 */

#include "usb_core.h"
#include "../usb.h"
#include "../host/baytrail_usb.h"
#include "../../timer/timer.h"
#include "../../io/io.h"
#include "../../diagnostics/driver_log.h"
#include "../../input/input.h"
#include "kernel.h"

extern void print(const char* str);

#define CORE_SCAN_BUDGET_MS      4500U
#define CORE_PORT_BUDGET_MS       700U  /* HS phantoms / quick fail */
#define CORE_PORT_BUDGET_LSFS_MS 2200U  /* Lenovo LS mouse through config+HID */
#define CORE_FAIL_COOLDOWN_TICKS  500U  /* ~5 s at 100 Hz before retry */
#define CORE_MAX_PORTS            16

static int g_core_up = 0;
static int g_hotplug_enabled = 1;
static int g_next_addr = 1;
static int g_has_pointer = 0;
static int g_has_keyboard = 0;
static uint8_t g_scratch[256];

/* Per-HCD per-port: don't re-enum a port that just failed until cool-down
 * expires or the device actually disconnects (CCS drops). */
static uint32_t g_port_cool_until[USB_HCD_MAX][CORE_MAX_PORTS];
static uint16_t g_port_seen_mask[USB_HCD_MAX];

static void core_log(const char* s) {
    driver_log(s);
    print(s);
    while (*s) outb(0xE9, *s++);
}

static void core_log_u8(const char* prefix, int n) {
    char b[4];
    core_log(prefix);
    if (n >= 100) {
        b[0] = '0' + (n / 100) % 10;
        b[1] = '0' + (n / 10) % 10;
        b[2] = '0' + n % 10;
        b[3] = 0;
    } else if (n >= 10) {
        b[0] = '0' + n / 10;
        b[1] = '0' + n % 10;
        b[2] = 0;
    } else {
        b[0] = '0' + n;
        b[1] = 0;
    }
    core_log(b);
}

static int ctrl_once(usb_dev_t* dev, uint8_t bm, uint8_t req, uint16_t val,
                     uint16_t idx, uint16_t len, uint8_t* data, int din) {
    uint8_t setup[8];
    if (!dev || !dev->hcd || !dev->hcd->ops || !dev->hcd->ops->control)
        return -1;
    setup[0] = bm;
    setup[1] = req;
    setup[2] = (uint8_t)(val & 0xFF);
    setup[3] = (uint8_t)(val >> 8);
    setup[4] = (uint8_t)(idx & 0xFF);
    setup[5] = (uint8_t)(idx >> 8);
    setup[6] = (uint8_t)(len & 0xFF);
    setup[7] = (uint8_t)(len >> 8);
    return dev->hcd->ops->control(dev->hcd, dev, setup, data, len, din);
}

/* One soft retry for flaky Bay Trail EP0 (config descriptor especially). */
static int ctrl(usb_dev_t* dev, uint8_t bm, uint8_t req, uint16_t val,
                uint16_t idx, uint16_t len, uint8_t* data, int din) {
    int ret = ctrl_once(dev, bm, req, val, idx, len, data, din);
    if (ret == 0) return 0;
    /* Slot may already be abandoned; retry only if HCD still has it. */
    if (!dev->hcd->ops->healthy || !dev->hcd->ops->healthy(dev->hcd))
        return ret;
    timer_busy_wait_ms(20);
    return ctrl_once(dev, bm, req, val, idx, len, data, din);
}

static int parse_config(usb_dev_t* dev, uint8_t* buf, int len) {
    uint8_t* p = buf;
    uint8_t* end = buf + len;
    usb_interface_t* cur = 0;

    if (len < 9) return -1;
    {
        usb_config_descriptor_t* cfg = (usb_config_descriptor_t*)buf;
        dev->config_value = cfg->bConfigurationValue;
        p += cfg->bLength;
    }

    while (p + 2 <= end) {
        uint8_t dlen = p[0];
        uint8_t dtype = p[1];
        if (dlen < 2 || p + dlen > end) break;

        if (dtype == USB_DT_INTERFACE && dlen >= 9) {
            usb_interface_descriptor_t* id = (usb_interface_descriptor_t*)p;
            if (dev->num_interfaces >= USB_CORE_MAX_INTERFACES) {
                cur = 0;
            } else {
                cur = &dev->ifaces[dev->num_interfaces++];
                cur->number = id->bInterfaceNumber;
                cur->alt = id->bAlternateSetting;
                cur->class_code = id->bInterfaceClass;
                cur->subclass = id->bInterfaceSubClass;
                cur->protocol = id->bInterfaceProtocol;
                cur->num_endpoints = 0;
                cur->class_priv = 0;
                cur->class_bound = 0;
            }
        } else if (dtype == USB_DT_ENDPOINT && dlen >= 7 && cur) {
            usb_endpoint_descriptor_t* ep = (usb_endpoint_descriptor_t*)p;
            if (cur->num_endpoints < USB_CORE_MAX_ENDPOINTS) {
                usb_endpoint_desc_t* e = &cur->ep[cur->num_endpoints++];
                e->address = ep->bEndpointAddress;
                e->attributes = ep->bmAttributes;
                e->max_packet = ep->wMaxPacketSize;
                e->interval = ep->bInterval;
                e->present = 1;
            }
        }
        p += dlen;
    }
    return 0;
}

static void mark_port_cooldown(usb_hcd_t* hcd, int port) {
    if (!hcd || port < 0 || port >= CORE_MAX_PORTS) return;
    g_port_cool_until[hcd->id][port] = timer_ticks() + CORE_FAIL_COOLDOWN_TICKS;
}

static int port_in_cooldown(usb_hcd_t* hcd, int port) {
    if (!hcd || port < 0 || port >= CORE_MAX_PORTS) return 0;
    return (int32_t)(timer_ticks() - g_port_cool_until[hcd->id][port]) < 0;
}

static void clear_port_cooldown(usb_hcd_t* hcd, int port) {
    if (!hcd || port < 0 || port >= CORE_MAX_PORTS) return;
    g_port_cool_until[hcd->id][port] = 0;
}

/* Returns 0 on success, -1 on failure. Sets *out_hid_pointer if mouse bound. */
static int enumerate_port(usb_hcd_t* hcd, int port, int* out_hid_pointer) {
    usb_dev_t* dev;
    usb_speed_t speed;
    usb_device_descriptor_t dd;
    uint8_t addr;
    int cfg_len;
    int i;
    uint64_t port_deadline;

    if (out_hid_pointer) *out_hid_pointer = 0;
    if (!hcd || !hcd->ops) return -1;
    if (!hcd->ops->port_connected(hcd, port)) return -1;

    speed = (usb_speed_t)hcd->ops->port_speed(hcd, port);
    /* Speed 0 with CCS often means an unusable phantom on Bay Trail. */
    if (speed == USB_SPEED_UNKNOWN) {
        core_log("USB-CORE: skip port (unknown speed)\n");
        mark_port_cooldown(hcd, port);
        return -1;
    }

    if (usb_dev_by_port(hcd, port))
        return 0;

    core_log_u8("USB-CORE: reset port=", port);
    core_log("\n");
    if (hcd->ops->port_reset(hcd, port) != 0) {
        core_log("USB-CORE: port reset failed\n");
        mark_port_cooldown(hcd, port);
        return -1;
    }

    /* Re-read speed after reset — Lenovo PORTSC often settles late. */
    speed = (usb_speed_t)hcd->ops->port_speed(hcd, port);

    /*
     * Bay Trail xHCI often latches CCS phantoms that come up as High-speed
     * after PR. Real wired mice on this board are LS/FS. Skip HS/SS here so
     * we spend the USB watchdog budget on the AmazonBasics LS port.
     */
    if (baytrail_usb_is_soc() &&
        (speed == USB_SPEED_HIGH || speed == USB_SPEED_SUPER)) {
        core_log("USB-CORE: skip Bay Trail HS/SS root phantom\n");
        if (hcd->ops->device_disable) hcd->ops->device_disable(hcd, NULL);
        mark_port_cooldown(hcd, port);
        return -1;
    }

    {
        uint32_t budget = CORE_PORT_BUDGET_MS;
        if (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL)
            budget = CORE_PORT_BUDGET_LSFS_MS;
        port_deadline = timer_deadline_ms(budget);
    }

    dev = usb_dev_alloc(hcd, port, speed);
    if (!dev) {
        mark_port_cooldown(hcd, port);
        return -1;
    }

    if (hcd->ops->device_enable && hcd->ops->device_enable(hcd, dev) != 0) {
        core_log("USB-CORE: device_enable failed\n");
        usb_dev_free(dev);
        mark_port_cooldown(hcd, port);
        return -1;
    }

    if (ctrl(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
             USB_REQ_GET_DESCRIPTOR, 0x0100, 0, 8, (uint8_t*)&dd, 1) != 0) {
        core_log("USB-CORE: short device desc failed\n");
        if (hcd->ops->device_disable) hcd->ops->device_disable(hcd, dev);
        usb_dev_free(dev);
        mark_port_cooldown(hcd, port);
        return -1;
    }
    if (dd.bMaxPacketSize0)
        dev->ep0_mps = dd.bMaxPacketSize0;

    if (timer_deadline_expired(port_deadline) || g_next_addr >= 127) {
        if (hcd->ops->device_disable) hcd->ops->device_disable(hcd, dev);
        usb_dev_free(dev);
        mark_port_cooldown(hcd, port);
        return -1;
    }
    addr = (uint8_t)g_next_addr++;

    if (hcd->ops->device_address(hcd, dev, addr) != 0) {
        core_log("USB-CORE: address failed\n");
        if (hcd->ops->device_disable) hcd->ops->device_disable(hcd, dev);
        usb_dev_free(dev);
        g_next_addr--;
        mark_port_cooldown(hcd, port);
        return -1;
    }

    if (ctrl(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
             USB_REQ_GET_DESCRIPTOR, 0x0100, 0,
             (uint16_t)sizeof(dd), (uint8_t*)&dd, 1) != 0) {
        core_log("USB-CORE: full device desc failed\n");
        if (hcd->ops->device_disable) hcd->ops->device_disable(hcd, dev);
        usb_dev_free(dev);
        mark_port_cooldown(hcd, port);
        return -1;
    }
    dev->id_vendor = dd.idVendor;
    dev->id_product = dd.idProduct;

    {
        usb_config_descriptor_t hdr;
        if (ctrl(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                 USB_REQ_GET_DESCRIPTOR, 0x0200, 0, 9, (uint8_t*)&hdr, 1) != 0) {
            core_log("USB-CORE: config hdr failed\n");
            if (hcd->ops->device_disable) hcd->ops->device_disable(hcd, dev);
            usb_dev_free(dev);
            mark_port_cooldown(hcd, port);
            return -1;
        }
        cfg_len = hdr.wTotalLength;
        if (cfg_len > (int)sizeof(g_scratch)) cfg_len = (int)sizeof(g_scratch);
        if (ctrl(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                 USB_REQ_GET_DESCRIPTOR, 0x0200, 0, (uint16_t)cfg_len,
                 g_scratch, 1) != 0) {
            core_log("USB-CORE: config desc failed\n");
            if (hcd->ops->device_disable) hcd->ops->device_disable(hcd, dev);
            usb_dev_free(dev);
            mark_port_cooldown(hcd, port);
            return -1;
        }
    }

    if (parse_config(dev, g_scratch, cfg_len) != 0) {
        if (hcd->ops->device_disable) hcd->ops->device_disable(hcd, dev);
        usb_dev_free(dev);
        mark_port_cooldown(hcd, port);
        return -1;
    }

    if (ctrl(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
             USB_REQ_SET_CONFIGURATION, dev->config_value, 0, 0, 0, 0) != 0) {
        core_log("USB-CORE: SET_CONFIGURATION failed\n");
        if (hcd->ops->device_disable) hcd->ops->device_disable(hcd, dev);
        usb_dev_free(dev);
        mark_port_cooldown(hcd, port);
        return -1;
    }
    dev->state = USB_DEV_CONFIGURED;

    i = usb_class_bind_device(dev);
    if (i <= 0) {
        core_log("USB-CORE: no class driver claimed interfaces\n");
        /* Keep the device so hotplug does not re-reset this port every frame. */
    } else {
        core_log("USB-CORE: class driver(s) bound\n");
        if (usb_core_has_pointer() && out_hid_pointer)
            *out_hid_pointer = 1;
    }
    clear_port_cooldown(hcd, port);
    return 0;
}

/*
 * Prefer Low/Full-speed root ports (typical wired mice) over High/Super
 * phantoms that often CCS-latch on Bay Trail USB2 roots.
 */
static int build_port_order(usb_hcd_t* hcd, int ports, int* order, int maxn) {
    int n = 0;
    int p;
    int byt = baytrail_usb_is_soc();
    if (ports > CORE_MAX_PORTS) ports = CORE_MAX_PORTS;
    /* Pass 1: LS then FS (wired USB2 mice). */
    for (p = 0; p < ports && n < maxn; p++) {
        int spd;
        if (!hcd->ops->port_connected(hcd, p)) continue;
        spd = hcd->ops->port_speed(hcd, p);
        if (spd == USB_SPEED_LOW || spd == USB_SPEED_FULL)
            order[n++] = p;
    }
    /* Pass 2: other speeds — skip HS/SS phantoms on Bay Trail entirely. */
    if (!byt) {
        for (p = 0; p < ports && n < maxn; p++) {
            int already = 0;
            int j;
            if (!hcd->ops->port_connected(hcd, p)) continue;
            for (j = 0; j < n; j++) if (order[j] == p) { already = 1; break; }
            if (!already) order[n++] = p;
        }
    }
    return n;
}

void usb_core_enumerate_all(void) {
    int n = usb_hcd_count();
    uint64_t scan_deadline = timer_deadline_ms(CORE_SCAN_BUDGET_MS);

    for (int hi = 0; hi < n; hi++) {
        usb_hcd_t* hcd = usb_hcd_get(hi);
        int ports;
        int order[CORE_MAX_PORTS];
        int on;
        if (!hcd || !hcd->online || !hcd->healthy) continue;
        ports = hcd->ops->port_count(hcd);
        core_log("USB-CORE: scanning HCD ports\n");
        on = build_port_order(hcd, ports, order, CORE_MAX_PORTS);

        for (int oi = 0; oi < on; oi++) {
            int p = order[oi];
            int got_pointer = 0;

            if (timer_deadline_expired(scan_deadline)) {
                core_log("USB-CORE: scan budget exceeded, stopping.\n");
                return;
            }
            if (!hcd->ops->healthy || !hcd->ops->healthy(hcd)) {
                core_log("USB-CORE: HCD unhealthy, stopping scan.\n");
                return;
            }

            /* Stable-connect debounce (~40 ms). */
            {
                int stable = 1;
                for (int k = 0; k < 2; k++) {
                    timer_busy_wait_ms(20);
                    if (!hcd->ops->port_connected(hcd, p)) { stable = 0; break; }
                }
                if (!stable) continue;
            }

            (void)enumerate_port(hcd, p, &got_pointer);
            if (hcd->ops->port_change_ack)
                hcd->ops->port_change_ack(hcd, p);

            if (got_pointer || usb_core_has_pointer()) {
                core_log("USB-CORE: HID pointer bound, stopping port scan "
                         "(singleton xHCI slot).\n");
                return;
            }
        }
    }
}

void usb_core_hotplug_poll(void) {
    static uint32_t next_check = 0;
    int n;
    if (!g_core_up || !g_hotplug_enabled) return;
    if ((int32_t)(timer_ticks() - next_check) < 0) return;
    next_check = timer_ticks() + 50; /* ~500 ms */

    n = usb_hcd_count();
    for (int hi = 0; hi < n; hi++) {
        usb_hcd_t* hcd = usb_hcd_get(hi);
        int ports;
        uint16_t mask = 0;
        if (!hcd || !hcd->online || !hcd->healthy) continue;
        if (hcd->ops->healthy && !hcd->ops->healthy(hcd)) continue;
        ports = hcd->ops->port_count(hcd);
        if (ports > CORE_MAX_PORTS) ports = CORE_MAX_PORTS;

        for (int p = 0; p < ports; p++) {
            int connected = hcd->ops->port_connected(hcd, p);
            int pending = hcd->ops->port_change_pending
                              ? hcd->ops->port_change_pending(hcd, p) : 0;
            usb_dev_t* existing = usb_dev_by_port(hcd, p);
            int was = (g_port_seen_mask[hcd->id] & (1u << p)) != 0;

            if (connected) mask |= (uint16_t)(1u << p);

            if (pending && hcd->ops->port_change_ack)
                hcd->ops->port_change_ack(hcd, p);

            if (!connected && existing) {
                usb_class_unbind_device(existing);
                if (hcd->ops->device_disable)
                    hcd->ops->device_disable(hcd, existing);
                usb_dev_free(existing);
                clear_port_cooldown(hcd, p);
                continue;
            }

            if (!connected) {
                clear_port_cooldown(hcd, p);
                continue;
            }

            /*
             * Only enumerate on a fresh connect edge (or CSC) AND when not
             * cooling down after a prior EP0/reset failure. Never hammer
             * reset+EP0 from the desktop frame loop.
             */
            if (existing) continue;
            if (port_in_cooldown(hcd, p)) continue;
            if (!pending && was) continue; /* still connected; already tried */

            {
                int got = 0;
                core_log_u8("USB-CORE: hotplug enum port=", p);
                core_log("\n");
                (void)enumerate_port(hcd, p, &got);
                if (got || usb_core_has_pointer())
                    input_set_usb_pointer_active(1);
            }
        }
        g_port_seen_mask[hcd->id] = mask;
    }
}

void usb_core_init(void) {
    const boot_config_t* cfg;
    if (g_core_up) return;

    core_log("USB-CORE: init (gooberos.usb.stack=new)\n");
    timer_calibrate_tsc();

    usb_dev_table_reset();
    usb_hcd_registry_reset();
    usb_hcd_register_builtins();
    usb_class_registry_init();
    usb_class_hid_register();

    g_next_addr = 1;
    g_has_pointer = 0;
    g_has_keyboard = 0;
    for (int i = 0; i < USB_HCD_MAX; i++) {
        g_port_seen_mask[i] = 0;
        for (int p = 0; p < CORE_MAX_PORTS; p++)
            g_port_cool_until[i][p] = 0;
    }

    cfg = boot_get_config();
    g_hotplug_enabled = (cfg && cfg->usb_hotplug) ? 1 : 0;

    baytrail_usb_prepare_companion();

    /*
     * Braswell (Acer R3-131T): first xHCI capability MMIO read can hard-stall
     * when the controller power well is down. Default defers probe so boot
     * reaches the desktop. gooberos.usb=on|full opts into watchdog-gated
     * bring-up (still skips BYT PMC/PHY route in xhci/baytrail_usb).
     */
    if (baytrail_usb_is_braswell()) {
        int opt_in = 0;
        if (cfg && cfg->usb[0]) {
            const char* u = cfg->usb;
            opt_in = (u[0] == 'o' && u[1] == 'n' && u[2] == '\0') ||
                     (u[0] == 'f' && u[1] == 'u' && u[2] == 'l' &&
                      u[3] == 'l' && u[4] == '\0');
        }
        if (!opt_in) {
            core_log("USB-CORE: Braswell xHCI -- deferred "
                     "(set gooberos.usb=on to probe).\n");
            input_set_usb_pointer_active(0);
            g_core_up = 1;
            return;
        }
        core_log("USB-CORE: Braswell xHCI -- cmdline opt-in probe.\n");
    }

    if (baytrail_usb_is_soc() && !baytrail_usb_usb2_on_xhci()) {
        core_log("USB-CORE: Bay Trail XUSB2PR=0 — skipping USB2 port enum "
                 "(mouse owned by companion EHCI; xHCI EP0 will time out).\n");
    }

    if (usb_hcd_probe_all() <= 0) {
        core_log("USB-CORE: no HCD came online\n");
        g_core_up = 1;
        return;
    }

    usb_core_enumerate_all();

    /* Seed seen-mask so hotplug does not re-hit boot-failed CCS ports. */
    {
        int n = usb_hcd_count();
        for (int hi = 0; hi < n; hi++) {
            usb_hcd_t* hcd = usb_hcd_get(hi);
            int ports;
            uint16_t mask = 0;
            if (!hcd || !hcd->online) continue;
            ports = hcd->ops->port_count(hcd);
            if (ports > CORE_MAX_PORTS) ports = CORE_MAX_PORTS;
            for (int p = 0; p < ports; p++) {
                if (hcd->ops->port_connected(hcd, p))
                    mask |= (uint16_t)(1u << p);
                /* Boot failures already set cooldown; also cool any leftover CCS. */
                if ((mask & (1u << p)) && !usb_dev_by_port(hcd, p))
                    mark_port_cooldown(hcd, p);
                if (hcd->ops->port_change_ack)
                    hcd->ops->port_change_ack(hcd, p);
            }
            g_port_seen_mask[hcd->id] = mask;
        }
    }

    g_has_pointer = usb_core_has_pointer();
    g_has_keyboard = usb_core_has_keyboard();

    if (g_has_pointer) {
        input_set_usb_pointer_active(1);
        core_log("USB HID pointer ready.\n");
    } else if (g_has_keyboard) {
        input_set_usb_pointer_active(0);
        core_log("USB HID keyboard ready, using PS/2 pointer fallback.\n");
    } else {
        input_set_usb_pointer_active(0);
        core_log("USB HID pointer not available, using PS/2 fallback.\n");
    }

    if (!g_hotplug_enabled)
        core_log("USB-CORE: hotplug disabled (gooberos.usb.hotplug=off)\n");

    g_core_up = 1;
}

void usb_core_poll(void) {
    if (!g_core_up) return;
    /* Drain HC events + HID reports every frame; hotplug is rate-limited
     * and must not do heavy work unless a real connect edge appears. */
    usb_hcd_poll_all();
    usb_class_poll_all();
    usb_core_hotplug_poll();
}

int usb_core_has_pointer(void) {
    extern int usb_hid_has_pointer_device(void);
    return usb_hid_has_pointer_device();
}

int usb_core_has_keyboard(void) {
    extern int usb_hid_has_keyboard_device(void);
    return usb_hid_has_keyboard_device();
}
