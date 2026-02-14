#include "enumeration.h"
#include "../host/host.h"
#include "../hid/hid.h"

extern void print(const char* str);

void usb_enumerate_devices(void) {
    // Default to disabled until host/controller sanity checks pass.
    usb_hid_register_boot_pointer(0, 0);

    if (!usb_host_ready()) {
        print("USB enum: host not ready.\n");
        return;
    }
    if (!usb_host_is_healthy()) {
        print("USB enum: host unhealthy, skipping pointer enable.\n");
        return;
    }
    if (!usb_host_pointer_enumeration_allowed()) {
        print("USB enum: pointer path locked, using PS/2 fallback.\n");
        return;
    }

    // Minimal bootstrap enumeration:
    // - On this kernel stage we do not yet walk full descriptor trees.
    // - Expose a boot-protocol pointer path for mouse-compatible reports.
    print("USB enum: probing boot HID pointer interfaces...\n");
    usb_hid_register_boot_pointer(1, 0);
}
