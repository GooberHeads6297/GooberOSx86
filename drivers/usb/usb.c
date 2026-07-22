#include "usb.h"
#include "host/host.h"
#include "host/baytrail_usb.h"
#include "core/enumeration.h"
#include "core/usb_core.h"
#include "hid/hid.h"
#include "storage/msc.h"
#include "../input/input.h"
#include "../io/io.h"
#include "../timer/timer.h"
#include "../pci/pci.h"
#include "../diagnostics/driver_log.h"
#include "../../kernel.h"
#include <stddef.h>

extern void print(const char* str);

/* Tiny strcmp; we can't pull in libc here. */
static int usb_str_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/*
 * Derive the USB host-controller safety level from the unified boot config
 * (parsed once in kernel.c) instead of re-walking the raw cmdline. cfg->usb
 * holds the already-parsed gooberos.usb= value; boot_safe_mode() is the
 * master "compatibility mode" switch.
 *
 *   0 = full, 1 = safe (skip xHCI), 2 = minimal (UHCI/OHCI only), 3 = off
 */
static int usb_safety_from_config(void) {
    const boot_config_t* cfg = boot_get_config();
    if (!cfg) return boot_safe_mode() ? 2 : 0;

    const char* u = cfg->usb;
    if (u && u[0]) {
        if (usb_str_eq(u, "off"))     return 3;
        if (usb_str_eq(u, "minimal")) return 2;
        if (usb_str_eq(u, "safe"))    return 1;
        if (usb_str_eq(u, "full"))    return 0;
        return 0;
    }

    /*
     * No explicit gooberos.usb=. In compatibility/safe mode, avoid the
     * EHCI/xHCI BIOS-handoff quirks entirely and stay on UHCI/OHCI.
     */
    if (boot_safe_mode()) return 2;
    return 0;
}

/* Hotplug bookkeeping.
 *
 * last_port_mask : cached connected-bit-per-port snapshot used as a fallback
 *                  when the controller's PORTSC change-status bit is masked
 *                  or already cleared at the time we poll.
 * hotplug_next_check : timer_ticks() the next poll is allowed to run.
 *                  Capped at ~50 ticks (~500 ms) so the post-boot path runs
 *                  at most every half-second; the actual MMIO read per port
 *                  is a single dword so cost is negligible even if we tighten
 *                  this later.
 * hotplug_enabled : copy of the gooberos.usb.hotplug= cmdline switch. When
 *                  zero the polling path is a no-op.
 * port_addr      : per-port "active USB address" so disconnect knows which
 *                  device to detach. 0 == port has no live device.
 */
#define HOTPLUG_MAX_PORTS 16
static uint16_t last_port_mask = 0;
static uint32_t hotplug_next_check = 0;
static int hotplug_initialized = 0;
static int hotplug_enabled = 0;
static uint8_t port_addr[HOTPLUG_MAX_PORTS];
static uint8_t port_kind[HOTPLUG_MAX_PORTS]; /* 0=none, 1=HID, 2=MSC */
static uint32_t port_debounce_until[HOTPLUG_MAX_PORTS];
#define HOTPLUG_KIND_NONE 0
#define HOTPLUG_KIND_HID  1
#define HOTPLUG_KIND_MSC  2
#define HOTPLUG_DEBOUNCE_TICKS 20  /* ~200 ms at 100 Hz */

/* Serial debug output via Bochs port 0xE9 */
static void usb_serial(const char* s) {
    while (*s) { outb(0xE9, *s++); }
}

/* Replacement for print() that also goes to serial port */
static void usb_print(const char* s) {
    driver_log(s);
    print(s);
    usb_serial(s);
}

static int usb_initialized = 0;
static int usb_stack_is_new = 1;
/* 1 when usb_init intentionally skipped host bring-up (Braswell desktop-first). */
static int usb_poll_disabled = 0;

/*
 * Acer R3-131T / Braswell: detect 8086:22B5 via the safe PCI walk only.
 * Any further USB init (print sink mid-line, xHCI MMIO, BYT companion) has
 * hard-stalled this board; skip the whole stage so desktop boot can finish.
 * Opt back in later with a hardened probe path.
 */
static int usb_pci_is_braswell_xhci(void) {
    usb_pci_controller_t ctrls[8];
    int n = pci_find_usb_controllers(ctrls, 8);
    int i;
    for (i = 0; i < n && i < 8; i++) {
        if (ctrls[i].vendor_id == 0x8086U && ctrls[i].device_id == 0x22B5U &&
            ctrls[i].prog_if == 0x30)
            return 1;
    }
    return 0;
}

static int usb_stack_new_from_config(void) {
    const boot_config_t* cfg = boot_get_config();
    if (!cfg || !cfg->usb_stack[0]) return 1; /* default: new */
    if (usb_str_eq(cfg->usb_stack, "legacy")) return 0;
    return 1;
}

void usb_init(void) {
    if (usb_initialized) return;

    if (usb_pci_is_braswell_xhci()) {
        driver_log_line("USB: Braswell 8086:22B5 -- stage skipped (desktop-first)");
        input_set_usb_pointer_active(0);
        usb_initialized = 1;
        usb_poll_disabled = 1;
        usb_stack_is_new = 0;
        return;
    }

    usb_print("USB: init...\n");

    /*
     * Calibrate the IRQ-independent TSC clock now, while IRQ0 is still being
     * delivered (before any USB port I/O could trigger an SMM storm). All USB
     * busy-waits use this clock so they can't wedge if IRQ0 later freezes.
     */
    timer_calibrate_tsc();

    int safety = usb_safety_from_config();
    usb_host_set_safety(safety);
    if (safety >= 3) {
        usb_print("USB: disabled by gooberos.usb=off; using PS/2 only.\n");
        input_set_usb_pointer_active(0);
        usb_initialized = 1;
        return;
    }

    {
        const boot_config_t* cfg = boot_get_config();
        baytrail_usb_set_phy_quirks(cfg && cfg->usb_byt_phy);
    }

    usb_stack_is_new = usb_stack_new_from_config();
    if (usb_stack_is_new) {
        /*
         * New stack currently ships an xHCI HCD. When cmdline asks for
         * safe/minimal (skip xHCI / UHCI-OHCI only), fall back to legacy
         * so companion controllers remain reachable.
         */
        if (safety >= 1) {
            usb_print("USB: stack=new unavailable for gooberos.usb=safe|minimal; "
                      "using legacy host path.\n");
            usb_stack_is_new = 0;
        } else {
            usb_print("USB: stack=new (HCD registry + class drivers)\n");
            usb_hid_init();
            usb_core_init();
            usb_initialized = 1;
            usb_host_print_usb2_route(usb_print);
            return;
        }
    }

    usb_print("USB: stack=legacy (singleton host fallback)\n");
    usb_hid_init();
    usb_host_scan_reset();

    /*
     * Try each USB host controller in priority order. If a controller comes
     * up but enumeration can't find a boot HID device, soft-fail to the next one.
     * This keeps EHCI/OHCI/UHCI usable as a fallback if xHCI is partial, and
     * still lets us land on PS/2 when no controller works at all.
     */
    int found_pointer = 0;
    int found_keyboard = 0;
    int scan_round;
    int first_round_healthy = 0;

    /*
     * Second full scan only if the first left the host unhealthy/faulted.
     * Re-scanning every empty enum doubles port-0 Transaction Error pain on
     * Bay Trail and can push into a PCI hang before the software watchdog.
     */
    for (scan_round = 0; scan_round < 2 && !found_pointer; scan_round++) {
        if (scan_round > 0) {
            if (first_round_healthy) {
                usb_print("USB: skipping re-scan (first pass finished healthy).\n");
                break;
            }
            usb_print("USB: re-scanning hosts once after empty/faulted enum...\n");
            usb_hid_init();
            usb_host_scan_reset();
        }

        while (usb_host_try_next_candidate()) {
            usb_print("USB host candidate: ");
            usb_print(usb_host_controller_name());
            usb_print("\n");
            usb_enumerate_devices();
            if (usb_hid_has_pointer_device()) {
                found_pointer = 1;
                break;
            }
            if (usb_hid_has_keyboard_device()) {
                found_keyboard = 1;
                usb_print("USB enum: keyboard present; continuing host fallback chain for pointer.\n");
                continue;
            }
            /* Keep this host if MSC came up — do not tear down for HID-only fallback. */
            if (usb_msc_is_attached()) {
                usb_print("USB enum: MSC attached on this host; stopping host fallback.\n");
                break;
            }
            usb_print("USB enum: candidate had no boot HID input, trying next host.\n");
        }

        if (scan_round == 0)
            first_round_healthy = usb_host_ready() && usb_host_is_healthy();

        if (found_pointer || usb_msc_is_attached()) break;
    }

    if (found_pointer) {
        usb_host_promote_active();
        input_set_usb_pointer_active(1);
        usb_print("USB HID pointer ready.\n");
    } else if (found_keyboard) {
        usb_host_promote_active();
        input_set_usb_pointer_active(0);
        usb_print("USB HID keyboard ready, using PS/2 pointer fallback.\n");
    } else if (usb_msc_is_attached()) {
        usb_host_promote_active();
        input_set_usb_pointer_active(0);
        usb_print("USB MSC ready (no HID pointer; PS/2 fallback).\n");
    } else {
        input_set_usb_pointer_active(0);
        usb_print("USB HID pointer not available, using PS/2 fallback.\n");
    }

    /* Seed the hotplug mask so we don't re-enumerate ports we already scanned. */
    last_port_mask = 0;
    int ports = host_port_count();
    if (ports > HOTPLUG_MAX_PORTS) ports = HOTPLUG_MAX_PORTS;
    for (int i = 0; i < HOTPLUG_MAX_PORTS; i++) port_addr[i] = 0;
    for (int i = 0; i < ports; i++) {
        if (host_port_connected(i)) last_port_mask |= (uint16_t)(1u << i);
        /* Also clear the connect-status-change latch so the first poll
         * doesn't re-fire on a state we already saw at boot. */
        host_port_change_ack(i);
    }
    /*
     * Boot-time enumeration found at most one HID device. If it landed on a
     * port we know which one; remember its address so disconnect later
     * detaches the right entry.
     */
    if (usb_hid_has_pointer_device() || usb_hid_has_keyboard_device()) {
        const usb_device_t* dev = usb_get_device(0);
        if (dev && dev->port < HOTPLUG_MAX_PORTS) {
            port_addr[dev->port] = dev->address ? dev->address : 1;
        }
    }
    hotplug_initialized = 1;
    hotplug_next_check = timer_ticks() + 5;  /* first scan happens promptly */

    /* gooberos.usb.hotplug=off disables the polling path entirely. */
    {
        const boot_config_t* cfg = boot_get_config();
        hotplug_enabled = (cfg && cfg->usb_hotplug) ? 1 : 0;
        if (!hotplug_enabled) {
            usb_print("[usb] hotplug: disabled by gooberos.usb.hotplug=off; "
                      "post-boot port polling skipped.\n");
        } else {
            usb_print("[usb] hotplug: enabled (polling root-hub PORTSC for "
                      "connect/disconnect events).\n");
        }
    }

    usb_initialized = 1;
    /* Always emit sticky route line at end of init (driverlog may trim). */
    usb_host_print_usb2_route(usb_print);
}

/*
 * Hotplug scan (post-boot, additive).
 *
 * Polls the active host controller's PORTSC every ~500 ms. The actual port
 * read is a single MMIO dword (host_port_change_pending + host_port_connected)
 * so the cost at 60 Hz polling is dominated by the timer_ticks compare, NOT
 * by hardware access. We honor the connect-status-change latch when the
 * controller exposes one (xHCI/EHCI/UHCI/OHCI all do) and ack it after
 * reading; we ALSO compare against last_port_mask so a missed transition
 * (e.g. quick unplug-then-replug between polls) is still caught the next
 * tick.
 *
 * On detected connect:
 *   1. usb_enumerate_port_hotplug runs the same reset+address+desc+config
 *      chain the boot-time path uses, scoped to ONE port.
 *   2. usb_hid_attach() emits the canonical "[usb] hotplug: port-N connect,
 *      addr=A, class=HID, attaching to HID driver" line.
 *
 * On detected disconnect:
 *   1. usb_hid_detach() emits the canonical "[usb] hotplug: port-N
 *      disconnect, detaching HID driver" line and tears down HID state.
 *   2. input_remove_device drops any queued events from the now-vanished
 *      device so a stale move/button never pops out post-detach.
 *   3. usb_enumeration_release_active() clears the active interrupt
 *      endpoint + frees the address counter, leaving the bus in a clean
 *      state for the next plug.
 *
 * Disabled entirely when gooberos.usb.hotplug=off (or gooberos.usb=off,
 * which short-circuits earlier in usb_init() before hotplug_initialized is
 * set).
 */
static void usb_hotplug_scan(void) {
    if (!hotplug_initialized || !hotplug_enabled) return;
    if (!usb_host_ready() || !usb_host_is_healthy()) return;
    if ((int32_t)(timer_ticks() - hotplug_next_check) < 0) return;
    hotplug_next_check = timer_ticks() + 50;

    int ports = host_port_count();
    if (ports <= 0) return;
    if (ports > HOTPLUG_MAX_PORTS) ports = HOTPLUG_MAX_PORTS;

    uint16_t mask = 0;
    uint16_t change_mask = 0;
    for (int i = 0; i < ports; i++) {
        if (host_port_connected(i)) mask |= (uint16_t)(1u << i);
        if (host_port_change_pending(i)) {
            change_mask |= (uint16_t)(1u << i);
            host_port_change_ack(i);
        }
    }

    /* Combine hardware-latched changes with the cached-mask diff so a
     * change-bit that was already cleared doesn't make us miss the event. */
    uint16_t diff = (uint16_t)(mask ^ last_port_mask);
    uint16_t to_examine = (uint16_t)(diff | change_mask);
    if (!to_examine) return;

    for (int i = 0; i < ports; i++) {
        uint16_t bit = (uint16_t)(1u << i);
        if (!(to_examine & bit)) continue;

        /* Debounce CSC chatter (~100-200 ms). */
        if ((int32_t)(timer_ticks() - port_debounce_until[i]) < 0) {
            continue;
        }

        int now_connected  = (mask & bit) != 0;
        int was_connected  = (last_port_mask & bit) != 0;

        if (now_connected && !was_connected) {
            uint8_t addr = 0;
            int proto = usb_enumerate_port_hotplug(i, &addr);
            port_debounce_until[i] = timer_ticks() + HOTPLUG_DEBOUNCE_TICKS;
            if (proto == USB_HID_PROTOCOL_MOUSE ||
                proto == USB_HID_PROTOCOL_KEYBOARD) {
                if (i < HOTPLUG_MAX_PORTS) {
                    port_addr[i] = addr ? addr : 1;
                    port_kind[i] = HOTPLUG_KIND_HID;
                }
                /* Single-device: MSC cannot share the xHCI slot with HID. */
                if (usb_msc_is_attached()) usb_msc_detach_all();
                usb_hid_attach(i, addr ? addr : 1, proto);
            } else if (proto == USB_ENUM_PROTOCOL_MSC) {
                if (i < HOTPLUG_MAX_PORTS) {
                    port_addr[i] = addr ? addr : 1;
                    port_kind[i] = HOTPLUG_KIND_MSC;
                }
                usb_print("[usb] hotplug: port-");
                {
                    char b[3];
                    if (i >= 10) { b[0] = '0' + i / 10; b[1] = '0' + i % 10; b[2] = 0; }
                    else         { b[0] = '0' + i; b[1] = 0; }
                    usb_print(b);
                }
                usb_print(" connect, MSC attached.\n");
            } else if (proto < 0) {
                usb_print("[usb] hotplug: enumerate aborted "
                          "(controller faulted).\n");
            } else {
                usb_print("[usb] hotplug: port-");
                {
                    char b[3];
                    if (i >= 10) { b[0] = '0' + i / 10; b[1] = '0' + i % 10; b[2] = 0; }
                    else         { b[0] = '0' + i; b[1] = 0; }
                    usb_print(b);
                }
                usb_print(" connect, no boot-HID/MSC device found.\n");
            }
        } else if (!now_connected && was_connected) {
            port_debounce_until[i] = timer_ticks() + HOTPLUG_DEBOUNCE_TICKS;
            if (port_kind[i] == HOTPLUG_KIND_MSC ||
                (usb_msc_is_attached() && usb_msc_attached_port() == i)) {
                usb_msc_detach(i);
            } else {
                usb_hid_detach(i);
                input_remove_device(INPUT_DEVICE_USB_MOUSE);
                input_remove_device(INPUT_DEVICE_USB_TOUCHPAD);
            }
            usb_enumeration_release_active();
            if (i < HOTPLUG_MAX_PORTS) {
                port_addr[i] = 0;
                port_kind[i] = HOTPLUG_KIND_NONE;
            }
        } else if (now_connected && was_connected && (change_mask & bit)) {
            usb_print("[usb] hotplug: port reset detected, re-enumerating.\n");
            port_debounce_until[i] = timer_ticks() + HOTPLUG_DEBOUNCE_TICKS;
            if (port_kind[i] == HOTPLUG_KIND_MSC)
                usb_msc_detach(i);
            else {
                usb_hid_detach(i);
                input_remove_device(INPUT_DEVICE_USB_MOUSE);
                input_remove_device(INPUT_DEVICE_USB_TOUCHPAD);
            }
            usb_enumeration_release_active();
            uint8_t addr = 0;
            int proto = usb_enumerate_port_hotplug(i, &addr);
            if (proto == USB_HID_PROTOCOL_MOUSE ||
                proto == USB_HID_PROTOCOL_KEYBOARD) {
                if (i < HOTPLUG_MAX_PORTS) {
                    port_addr[i] = addr ? addr : 1;
                    port_kind[i] = HOTPLUG_KIND_HID;
                }
                usb_hid_attach(i, addr ? addr : 1, proto);
            } else if (proto == USB_ENUM_PROTOCOL_MSC) {
                if (i < HOTPLUG_MAX_PORTS) {
                    port_addr[i] = addr ? addr : 1;
                    port_kind[i] = HOTPLUG_KIND_MSC;
                }
            } else if (i < HOTPLUG_MAX_PORTS) {
                port_addr[i] = 0;
                port_kind[i] = HOTPLUG_KIND_NONE;
            }
        }
    }

    last_port_mask = mask;
}

void usb_poll(void) {
    if (!usb_initialized || usb_poll_disabled) return;

    if (usb_stack_is_new) {
        usb_core_poll();
        return;
    }

    usb_host_poll();

    if (usb_host_consumed_recovery()) {
        usb_hid_register_boot_pointer(0, 0);
        usb_hid_register_boot_keyboard(0);
        input_set_usb_pointer_active(0);
        input_remove_device(INPUT_DEVICE_USB_MOUSE);
        input_remove_device(INPUT_DEVICE_USB_TOUCHPAD);
        usb_print("USB: re-enumerating HID after host recovery...\n");
        usb_enumerate_devices();
        if (usb_hid_has_pointer_device())
            input_set_usb_pointer_active(1);
    } else if (!usb_host_is_healthy()) {
        if (usb_hid_has_pointer_device() || usb_hid_has_keyboard_device()) {
            usb_hid_register_boot_pointer(0, 0);
            usb_hid_register_boot_keyboard(0);
            input_set_usb_pointer_active(0);
            input_remove_device(INPUT_DEVICE_USB_MOUSE);
            input_remove_device(INPUT_DEVICE_USB_TOUCHPAD);
            print("USB poll: HID soft-disabled, PS/2 fallback.\n");
        }
        return;
    }

    /* Process HID reports from interrupt transfers */
    usb_process_hid_reports();

    /* Detect connect/disconnect on root-hub ports without blocking. */
    usb_hotplug_scan();
}

int usb_has_pointer_device(void) {
    if (usb_stack_is_new) return usb_core_has_pointer();
    return usb_hid_has_pointer_device();
}

int usb_has_touchpad_device(void) {
    return usb_hid_has_touchpad_device();
}
