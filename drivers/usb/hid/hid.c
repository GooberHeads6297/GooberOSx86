#include "hid.h"
#include "../../input/input.h"

static int pointer_present = 0;
static int touchpad_present = 0;

void usb_hid_init(void) {
    pointer_present = 0;
    touchpad_present = 0;
}

void usb_hid_register_boot_pointer(uint8_t present, uint8_t is_touchpad) {
    pointer_present = present ? 1 : 0;
    touchpad_present = (present && is_touchpad) ? 1 : 0;
}

void usb_hid_handle_boot_report(const uint8_t* report, uint8_t length) {
    if (!report || length < 3 || !pointer_present) return;

    // Many touchpads expose mouse-compatible packets plus extra bytes.
    if (length > 4) {
        touchpad_present = 1;
    }

    uint8_t buttons = report[0] & 0x07;
    int dx = (int)((int8_t)report[1]);
    int dy = (int)(-((int8_t)report[2]));
    int8_t wheel = 0;

    if (length >= 4) {
        wheel = (int8_t)report[3];
    }

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
