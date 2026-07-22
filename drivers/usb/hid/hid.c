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

/* Single output sink shared by attach/detach so the hot-plug log format is
 * canonical across all callers (boot scan + hot-plug poll). */
static void hid_serial(const char* s) { while (*s) outb(0xE9, *s++); }
static void hid_print(const char* s) { print(s); hid_serial(s); }

static void hid_print_int(int n) {
    char buf[6];
    if (n < 0) { hid_print("-"); n = -n; }
    if (n >= 100) { buf[0] = '0' + (n / 100); buf[1] = '0' + ((n / 10) % 10); buf[2] = '0' + (n % 10); buf[3] = 0; }
    else if (n >= 10) { buf[0] = '0' + (n / 10); buf[1] = '0' + (n % 10); buf[2] = 0; }
    else { buf[0] = '0' + n; buf[1] = 0; }
    hid_print(buf);
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
    for (int i = 0; i < 6; i++) last_keys[i] = 0;
}

void usb_hid_register_boot_pointer(uint8_t present, uint8_t is_touchpad) {
    pointer_present = present ? 1 : 0;
    touchpad_present = (present && is_touchpad) ? 1 : 0;
}

void usb_hid_register_boot_keyboard(uint8_t present) {
    keyboard_present = present ? 1 : 0;
    if (!keyboard_present) {
        for (int i = 0; i < 6; i++) last_keys[i] = 0;
    }
}

static int key_was_down(uint8_t key) {
    for (int i = 0; i < 6; i++) {
        if (last_keys[i] == key) return 1;
    }
    return 0;
}

static void usb_hid_handle_keyboard_report(const uint8_t* report, uint8_t length) {
    if (!report || length < 8 || !keyboard_present) return;
    uint8_t modifiers = report[0];
    int shifted = (modifiers & ((1U << 1) | (1U << 5))) != 0;

    for (int i = 2; i < 8; i++) {
        uint8_t key = report[i];
        if (key == 0 || key_was_down(key)) continue;
        char c = (key < 128) ? (shifted ? hid_key_ascii_shift[key] : hid_key_ascii[key]) : 0;
        if (key == 0x4F) c = KEY_RIGHT;
        else if (key == 0x50) c = KEY_LEFT;
        else if (key == 0x51) c = KEY_DOWN;
        else if (key == 0x52) c = KEY_UP;
        else if (key >= 0x3A && key <= 0x45) c = KEY_F1 + (key - 0x3A);
        if (c) keyboard_inject_char(c);
    }

    for (int i = 0; i < 6; i++) last_keys[i] = report[2 + i];
}

void usb_hid_handle_boot_report(const uint8_t* report, uint8_t length) {
    if (!report) return;

    if (keyboard_present && !pointer_present) {
        usb_hid_handle_keyboard_report(report, length);
        return;
    }
    if (length < 3 || !pointer_present) return;

    /*
     * Boot-protocol mouse report layout (we force boot protocol during
     * enumeration, so there is never a leading report-ID byte):
     *   byte 0: buttons (bit0 left, bit1 right, bit2 middle)
     *   byte 1: signed X delta
     *   byte 2: signed Y delta (positive = up)
     *   byte 3: optional signed wheel delta (many mice append this even in
     *           boot mode; host buffers are cleared before each transfer so
     *           this reads 0 when the device does not send it)
     * Screen Y grows downward, so invert the reported Y delta.
     */
    uint8_t buttons = report[0] & 0x07;
    int dx = (int)((int8_t)report[1]);
    int dy = -(int)((int8_t)report[2]);
    int8_t wheel = (length >= 4) ? (int8_t)report[3] : 0;

    input_report_pointer_delta(
        touchpad_present ? INPUT_DEVICE_USB_TOUCHPAD : INPUT_DEVICE_USB_MOUSE,
        dx,
        dy,
        buttons,
        wheel);
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

void usb_hid_attach(int port, uint8_t address, int protocol) {
    if (protocol == USB_HID_PROTOCOL_MOUSE) {
        usb_hid_register_boot_pointer(1, 0);
        input_set_usb_pointer_active(1);
        hid_print("[usb] hotplug: port-");
        hid_print_int(port);
        hid_print(" connect, addr=");
        hid_print_int((int)address);
        hid_print(", class=HID, protocol=mouse, attaching to HID driver\n");
    } else if (protocol == USB_HID_PROTOCOL_KEYBOARD) {
        usb_hid_register_boot_keyboard(1);
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
    if (had_pointer || had_keyboard) {
        hid_print("[usb] hotplug: port-");
        hid_print_int(port);
        hid_print(" disconnect, detaching HID driver\n");
    }
    if (had_pointer) {
        usb_hid_register_boot_pointer(0, 0);
        input_set_usb_pointer_active(0);
    }
    if (had_keyboard) {
        usb_hid_register_boot_keyboard(0);
    }
}
