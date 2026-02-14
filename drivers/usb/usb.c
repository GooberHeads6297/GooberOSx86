#include "usb.h"
#include "host/host.h"
#include "core/enumeration.h"
#include "hid/hid.h"
#include "../input/input.h"

extern void print(const char* str);

static int usb_initialized = 0;

void usb_init(void) {
    if (usb_initialized) return;

    usb_host_init();
    usb_hid_init();
    usb_enumerate_devices();

    if (usb_hid_has_pointer_device()) {
        print("USB HID pointer ready.\n");
    } else {
        input_set_usb_pointer_active(0);
        print("USB HID pointer not available, using PS/2 fallback.\n");
    }

    usb_initialized = 1;
}

void usb_poll(void) {
    if (!usb_initialized) return;
    usb_host_poll();
    if (!usb_host_is_healthy() && usb_hid_has_pointer_device()) {
        usb_hid_register_boot_pointer(0, 0);
        input_set_usb_pointer_active(0);
        print("USB poll: pointer soft-disabled, PS/2 fallback active.\n");
    }
}

int usb_has_pointer_device(void) {
    return usb_hid_has_pointer_device();
}

int usb_has_touchpad_device(void) {
    return usb_hid_has_touchpad_device();
}
