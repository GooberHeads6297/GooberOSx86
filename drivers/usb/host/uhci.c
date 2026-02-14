#include "uhci.h"
#include "../../io/io.h"

extern void print(const char* str);

static uint16_t uhci_io_base = 0;
static int uhci_ready = 0;

int uhci_init(const usb_pci_controller_t* controller) {
    if (!controller) return 0;

    if ((controller->bar0 & 0x1) == 0) {
        print("UHCI BAR0 is not IO-mapped.\n");
        return 0;
    }

    uhci_io_base = (uint16_t)(controller->bar0 & 0xFFFC);
    if (uhci_io_base == 0) {
        print("UHCI invalid IO base.\n");
        return 0;
    }

    // Host Controller Reset.
    outw(uhci_io_base + 0x00, 0x0002);
    for (volatile int i = 0; i < 100000; i++);
    // Clear status bits.
    outw(uhci_io_base + 0x02, 0xFFFF);
    // Run/Stop bit set.
    outw(uhci_io_base + 0x00, 0x0001);

    uhci_ready = 1;
    print("UHCI initialized.\n");
    return 1;
}

void uhci_poll(void) {
    if (!uhci_ready) return;
    // Placeholder poll path. Future work hooks transfer completion here.
}
