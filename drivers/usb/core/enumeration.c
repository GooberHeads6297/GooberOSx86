#include "enumeration.h"
#include "../host/host.h"
#include "../hid/hid.h"

extern void print(const char* str);

void usb_enumerate_devices(void) {
    if (!usb_host_ready()) {
        print("USB enum: host not ready.\n");
        return;
    }

    // Current bootstrap stage: host bring-up only.
    // We do not yet enumerate real HID interfaces here.
    print("USB enum: host online, HID enumeration not fully implemented.\n");
    usb_hid_register_boot_pointer(0, 0);
}
