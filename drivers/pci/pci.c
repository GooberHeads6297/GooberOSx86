#include "pci.h"
#include "../io/io.h"
#include "../video/vga.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

extern void print(const char* str);
extern char* itoa(int value, char* str, int base);

static uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address;
    uint32_t lbus  = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;
    
    address = (uint32_t)((lbus << 16) | (lslot << 11) | (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_check_usb(void) {
    print("Scanning PCI bus for USB controllers...\n");
    int usb_found = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vendor_id = pci_read(bus, slot, func, 0) & 0xFFFF;
                if (vendor_id != 0xFFFF) {
                    uint32_t class_code = pci_read(bus, slot, func, 0x08);
                    uint8_t base_class = (class_code >> 24) & 0xFF;
                    uint8_t sub_class = (class_code >> 16) & 0xFF;
                    uint8_t prog_if = (class_code >> 8) & 0xFF;

                    // Class 0x0C = Serial Bus Controller
                    // Subclass 0x03 = USB Controller
                    if (base_class == 0x0C && sub_class == 0x03) {
                        usb_found++;
                        print("  Found USB Controller at ");
                        char buf[16];
                        itoa(bus, buf, 10); print(buf); print(":");
                        itoa(slot, buf, 10); print(buf); print(":");
                        itoa(func, buf, 10); print(buf);
                        
                        print(" (Type: ");
                        if (prog_if == 0x00) print("UHCI");
                        else if (prog_if == 0x10) print("OHCI");
                        else if (prog_if == 0x20) print("EHCI");
                        else if (prog_if == 0x30) print("XHCI");
                        else print("Unknown");
                        print(")\n");
                    }
                }
            }
        }
    }

    if (usb_found) {
        print("USB hardware detected. Enabling legacy emulation...\n");
        // In a real OS, we would initialize the controller here.
        // For now, we rely on BIOS legacy support which is standard.
    } else {
        print("No USB controllers found.\n");
    }
}

void pci_init(void) {
    pci_check_usb();
}
