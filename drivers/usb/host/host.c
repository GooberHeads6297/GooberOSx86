#include "host.h"
#include "uhci.h"

extern void print(const char* str);

static usb_host_state_t host_state;
static uint8_t host_controller_type = 0xFF;

void usb_host_init(void) {
    usb_pci_controller_t controllers[8];
    int found = pci_find_usb_controllers(controllers, 8);

    host_state.active = 0;
    host_controller_type = 0xFF;

    if (found <= 0) {
        print("USB host: no controllers.\n");
        return;
    }

    for (int i = 0; i < found && i < 8; i++) {
        if (controllers[i].prog_if == 0x00) {
            if (uhci_init(&controllers[i])) {
                host_state.controller = controllers[i];
                host_state.active = 1;
                host_controller_type = controllers[i].prog_if;
                print("USB host: UHCI online.\n");
                return;
            }
        }
    }

    print("USB host: no supported controller initialized.\n");
}

void usb_host_poll(void) {
    if (!host_state.active) return;
    if (host_controller_type == 0x00) {
        uhci_poll();
    }
}

int usb_host_ready(void) {
    return host_state.active;
}

uint8_t usb_host_controller_type(void) {
    return host_controller_type;
}
