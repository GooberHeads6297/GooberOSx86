#include "hid.h"
#include "../usb.h"
#include "../../input/input.h"
#include "../../keyboard/keyboard.h"
#include "../../io/io.h"

extern void print(const char* str);

static int pointer_present = 0;
static int touchpad_present = 0;
static int keyboard_present = 0;
static uint8_t last_keys[6];
static uint32_t pointer_report_count = 0;
static uint32_t keyboard_report_count = 0;
static int logged_left_click = 0;
static int logged_right_click = 0;
static int logged_wheel = 0;

typedef struct {
    uint8_t present;
    uint8_t is_touchpad;
    uint8_t port;
    uint8_t address;
    uint8_t endpoint;
    uint16_t max_packet;
    uint8_t interval;
} hid_boot_endpoint_state_t;

static hid_boot_endpoint_state_t pointer_state;
static hid_boot_endpoint_state_t keyboard_state;

/* Single output sink shared by attach/detach so the hot-plug log format is
 * canonical across all callers (boot scan + hot-plug poll). */
static void hid_serial(const char* s) { while (*s) outb(0xE9, *s++); }
static void hid_print(const char* s) { print(s); hid_serial(s); }

static void hid_print_int(int n) {
    char tmp[12];
    char buf[12];
    int i = 0;
    int j = 0;
    if (n < 0) { hid_print("-"); n = -n; }
    if (n == 0) {
        hid_print("0");
        return;
    }
    while (n > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (n % 10));
        n /= 10;
    }
    while (i > 0 && j < (int)sizeof(buf) - 1) {
        buf[j++] = tmp[--i];
    }
    buf[j] = 0;
    hid_print(buf);
}

static void hid_print_endpoint(const char* label, const hid_boot_endpoint_state_t* s) {
    hid_print("[usb-hid] ");
    hid_print(label);
    hid_print(": port=");
    hid_print_int((int)s->port);
    hid_print(" addr=");
    hid_print_int((int)s->address);
    hid_print(" ep=");
    hid_print_int((int)s->endpoint);
    hid_print(" maxpkt=");
    hid_print_int((int)s->max_packet);
    hid_print(" interval=");
    hid_print_int((int)s->interval);
    hid_print(s->is_touchpad ? " touchpad=1\n" : " touchpad=0\n");
}

static const char hid_key_ascii[128] = {
    [0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd',
    [0x08] = 'e', [0x09] = 'f', [0x0A] = 'g', [0x0B] = 'h',
    [0x0C] = 'i', [0x0D] = 'j', [0x0E] = 'k', [0x0F] = 'l',
    [0x10] = 'm', [0x11] = 'n', [0x12] = 'o', [0x13] = 'p',
    [0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't',
    [0x18] = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x',
    [0x1C] = 'y', [0x1D] = 'z',
    [0x1E] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4',
    [0x22] = '5', [0x23] = '6', [0x24] = '7', [0x25] = '8',
    [0x26] = '9', [0x27] = '0',
    [0x28] = '\n', [0x29] = 0x1B, [0x2A] = '\b', [0x2B] = '\t',
    [0x2C] = ' ', [0x2D] = '-', [0x2E] = '=', [0x2F] = '[',
    [0x30] = ']', [0x31] = '\\', [0x33] = ';', [0x34] = '\'',
    [0x35] = '`', [0x36] = ',', [0x37] = '.', [0x38] = '/',
};

static const char hid_key_ascii_shift[128] = {
    [0x04] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D',
    [0x08] = 'E', [0x09] = 'F', [0x0A] = 'G', [0x0B] = 'H',
    [0x0C] = 'I', [0x0D] = 'J', [0x0E] = 'K', [0x0F] = 'L',
    [0x10] = 'M', [0x11] = 'N', [0x12] = 'O', [0x13] = 'P',
    [0x14] = 'Q', [0x15] = 'R', [0x16] = 'S', [0x17] = 'T',
    [0x18] = 'U', [0x19] = 'V', [0x1A] = 'W', [0x1B] = 'X',
    [0x1C] = 'Y', [0x1D] = 'Z',
    [0x1E] = '!', [0x1F] = '@', [0x20] = '#', [0x21] = '$',
    [0x22] = '%', [0x23] = '^', [0x24] = '&', [0x25] = '*',
    [0x26] = '(', [0x27] = ')',
    [0x28] = '\n', [0x29] = 0x1B, [0x2A] = '\b', [0x2B] = '\t',
    [0x2C] = ' ', [0x2D] = '_', [0x2E] = '+', [0x2F] = '{',
    [0x30] = '}', [0x31] = '|', [0x33] = ':', [0x34] = '"',
    [0x35] = '~', [0x36] = '<', [0x37] = '>', [0x38] = '?',
};

void usb_hid_init(void) {
    pointer_present = 0;
    touchpad_present = 0;
    keyboard_present = 0;
    pointer_state.present = 0;
    keyboard_state.present = 0;
    pointer_report_count = 0;
    keyboard_report_count = 0;
    logged_left_click = 0;
    logged_right_click = 0;
    logged_wheel = 0;
    for (int i = 0; i < 6; i++) last_keys[i] = 0;
}

void usb_hid_register_boot_pointer(uint8_t present, uint8_t is_touchpad) {
    usb_hid_register_boot_pointer_detail(present, is_touchpad, 0, 0, 0, 0, 0);
}

void usb_hid_register_boot_pointer_detail(uint8_t present, uint8_t is_touchpad,
                                          uint8_t port, uint8_t address,
                                          uint8_t endpoint, uint16_t max_packet,
                                          uint8_t interval) {
    pointer_present = present ? 1 : 0;
    touchpad_present = (present && is_touchpad) ? 1 : 0;
    pointer_state.present = pointer_present;
    pointer_state.is_touchpad = touchpad_present;
    pointer_state.port = port;
    pointer_state.address = address;
    pointer_state.endpoint = endpoint;
    pointer_state.max_packet = max_packet;
    pointer_state.interval = interval;
    if (pointer_present) {
        pointer_report_count = 0;
        hid_print_endpoint("boot pointer attached", &pointer_state);
        input_set_usb_pointer_active(1);
        input_set_usb_pointer_kind(touchpad_present);
    }
}

void usb_hid_register_boot_keyboard(uint8_t present) {
    usb_hid_register_boot_keyboard_detail(present, 0, 0, 0, 0, 0);
}

void usb_hid_register_boot_keyboard_detail(uint8_t present, uint8_t port,
                                           uint8_t address, uint8_t endpoint,
                                           uint16_t max_packet, uint8_t interval) {
    keyboard_present = present ? 1 : 0;
    keyboard_state.present = keyboard_present;
    keyboard_state.is_touchpad = 0;
    keyboard_state.port = port;
    keyboard_state.address = address;
    keyboard_state.endpoint = endpoint;
    keyboard_state.max_packet = max_packet;
    keyboard_state.interval = interval;
    if (!keyboard_present) {
        for (int i = 0; i < 6; i++) last_keys[i] = 0;
    } else {
        keyboard_report_count = 0;
        hid_print_endpoint("boot keyboard attached", &keyboard_state);
    }
}

static int key_was_down(uint8_t key) {
    for (int i = 0; i < 6; i++) {
        if (last_keys[i] == key) return 1;
    }
    return 0;
}

static int key_is_down_now(const uint8_t* report, uint8_t key) {
    int i;
    for (i = 2; i < 8; i++) {
        if (report[i] == key) return 1;
    }
    return 0;
}

static char hid_usage_to_char(uint8_t key, int shifted) {
    char c = 0;
    if (key < 128)
        c = shifted ? hid_key_ascii_shift[key] : hid_key_ascii[key];
    if (key == 0x4F) c = KEY_RIGHT;
    else if (key == 0x50) c = KEY_LEFT;
    else if (key == 0x51) c = KEY_DOWN;
    else if (key == 0x52) c = KEY_UP;
    else if (key >= 0x3A && key <= 0x45) c = (char)(KEY_F1 + (key - 0x3A));
    return c;
}

static void usb_hid_handle_keyboard_report(const uint8_t* report, uint8_t length) {
    if (!report || length < 8 || !keyboard_present) return;
    uint8_t modifiers = report[0];
    int shifted = (modifiers & ((1U << 1) | (1U << 5))) != 0;
    int i;
    if (keyboard_report_count == 0) {
        hid_print("[usb-hid] first keyboard interrupt report received.\n");
    }
    keyboard_report_count++;

    /* Releases: clear held state for usages that left the 6-key slot. */
    for (i = 0; i < 6; i++) {
        uint8_t key = last_keys[i];
        char c0, c1;
        if (key == 0 || key_is_down_now(report, key)) continue;
        c0 = hid_usage_to_char(key, 0);
        c1 = hid_usage_to_char(key, 1);
        if (c0) keyboard_release_char(c0);
        if (c1 && c1 != c0) keyboard_release_char(c1);
    }

    for (i = 2; i < 8; i++) {
        uint8_t key = report[i];
        char c;
        if (key == 0 || key_was_down(key)) continue;
        c = hid_usage_to_char(key, shifted);
        if (c) keyboard_inject_char(c);
    }

    for (i = 0; i < 6; i++) last_keys[i] = report[2 + i];
}

void usb_hid_handle_boot_report_ex(int protocol, const uint8_t* report,
                                   uint8_t length) {
    if (!report) return;

    if (protocol == USB_HID_PROTOCOL_KEYBOARD) {
        if (keyboard_present)
            usb_hid_handle_keyboard_report(report, length);
        return;
    }

    /* Mouse / generic pointer (protocol mouse or 0 treated as pointer). */
    if (length < 3 || !pointer_present) return;

    /*
     * Absolute HID tablets (VirtualBox USB Tablet, many USB 2/3 pointers)
     * send 16-bit X/Y. Only consider absolute when the report is longer than
     * a boot relative mouse packet (3–4 bytes) so real boot mice stay relative.
     */
    if (length >= 5) {
        int off = 0;
        uint8_t abs_buttons;
        uint16_t ax;
        uint16_t ay;

        if (length >= 6 && report[0] >= 1 && report[0] <= 15 &&
            (report[1] & 0xF8) == 0) {
            off = 1;
        }
        abs_buttons = (uint8_t)(report[off] & 0x07);
        ax = (uint16_t)report[off + 1] | ((uint16_t)report[off + 2] << 8);
        ay = (uint16_t)report[off + 3] | ((uint16_t)report[off + 4] << 8);
        if ((report[off + 2] != 0 || report[off + 4] != 0) &&
            ax <= 32767U && ay <= 32767U) {
            if (pointer_report_count == 0) {
                hid_print("[usb-hid] first absolute pointer report: x=");
                hid_print_int((int)ax);
                hid_print(" y=");
                hid_print_int((int)ay);
                hid_print("\n");
            }
            pointer_report_count++;
            input_report_pointer_absolute_scaled(
                touchpad_present ? INPUT_DEVICE_USB_TOUCHPAD : INPUT_DEVICE_USB_MOUSE,
                (int)ax, (int)ay, 32767, 32767, abs_buttons, 0);
            return;
        }
    }

    /*
     * Boot-protocol mouse report layout (we force boot protocol during
     * enumeration, so there is never a leading report-ID byte):
     *   byte 0: buttons (bit0 left, bit1 right, bit2 middle)
     *   byte 1: signed X delta
     *   byte 2: signed Y delta (positive = up)
     *   byte 3: optional signed wheel delta
     * Screen Y grows downward, so invert the reported Y delta.
     */
    uint8_t buttons = report[0] & 0x07;
    int dx = (int)((int8_t)report[1]);
    int dy = -(int)((int8_t)report[2]);
    int8_t wheel = (length >= 4) ? (int8_t)report[3] : 0;

    if (pointer_report_count == 0) {
        hid_print("[usb-hid] first pointer interrupt report: dx=");
        hid_print_int(dx);
        hid_print(" dy=");
        hid_print_int(dy);
        hid_print(" buttons=");
        hid_print_int((int)buttons);
        hid_print("\n");
    }
    if (!logged_left_click && (buttons & 0x01)) {
        logged_left_click = 1;
        hid_print("[usb-hid] first LEFT click confirmed.\n");
    }
    if (!logged_right_click && (buttons & 0x02)) {
        logged_right_click = 1;
        hid_print("[usb-hid] first RIGHT click confirmed.\n");
    }
    if (!logged_wheel && wheel != 0) {
        logged_wheel = 1;
        hid_print("[usb-hid] first WHEEL scroll confirmed wheel=");
        hid_print_int((int)wheel);
        hid_print("\n");
    }
    pointer_report_count++;

    input_report_pointer_delta(
        touchpad_present ? INPUT_DEVICE_USB_TOUCHPAD : INPUT_DEVICE_USB_MOUSE,
        dx,
        dy,
        buttons,
        wheel);
}

void usb_hid_handle_boot_report(const uint8_t* report, uint8_t length) {
    /*
     * Legacy auto path (single active interrupt pipe): prefer pointer when
     * both are registered so a bound keyboard does not steal mouse packets.
     * Prefer callers that know the source protocol via _ex().
     */
    if (pointer_present)
        usb_hid_handle_boot_report_ex(USB_HID_PROTOCOL_MOUSE, report, length);
    else if (keyboard_present)
        usb_hid_handle_boot_report_ex(USB_HID_PROTOCOL_KEYBOARD, report, length);
}

int usb_hid_has_pointer_device(void) {
    return pointer_present;
}

int usb_hid_has_touchpad_device(void) {
    return touchpad_present;
}

int usb_hid_has_keyboard_device(void) {
    return keyboard_present;
}

int usb_hid_keyboard_active(void) {
    return keyboard_present && keyboard_report_count > 0;
}

void usb_hid_attach(int port, uint8_t address, int protocol) {
    if (protocol == USB_HID_PROTOCOL_MOUSE) {
        /* enumerate_device() already registered endpoint/address details. */
        input_set_usb_pointer_active(1);
        input_set_usb_pointer_kind(touchpad_present);
        hid_print("[usb] hotplug: port-");
        hid_print_int(port);
        hid_print(" connect, addr=");
        hid_print_int((int)address);
        hid_print(", class=HID, protocol=mouse, attaching to HID driver\n");
    } else if (protocol == USB_HID_PROTOCOL_KEYBOARD) {
        hid_print("[usb] hotplug: port-");
        hid_print_int(port);
        hid_print(" connect, addr=");
        hid_print_int((int)address);
        hid_print(", class=HID, protocol=keyboard, attaching to HID driver\n");
    } else {
        hid_print("[usb] hotplug: port-");
        hid_print_int(port);
        hid_print(" connect, addr=");
        hid_print_int((int)address);
        hid_print(", class=HID, protocol=other (no boot driver)\n");
    }
}

void usb_hid_detach(int port) {
    int had_pointer = pointer_present;
    int had_keyboard = keyboard_present;
    int was_touchpad = touchpad_present;
    if (had_pointer || had_keyboard) {
        hid_print("[usb] hotplug: port-");
        hid_print_int(port);
        hid_print(" disconnect, detaching HID driver\n");
    }
    if (had_pointer) {
        usb_hid_register_boot_pointer(0, 0);
        /* Drop queued USB events + synthesize button-ups. */
        input_remove_device(was_touchpad ? INPUT_DEVICE_USB_TOUCHPAD
                                          : INPUT_DEVICE_USB_MOUSE);
        input_remove_device(INPUT_DEVICE_USB_MOUSE);
        input_remove_device(INPUT_DEVICE_USB_TOUCHPAD);
    }
    if (had_keyboard) {
        usb_hid_register_boot_keyboard(0);
    }
}
