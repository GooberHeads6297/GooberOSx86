#include "pci.h"
#include "../io/io.h"
#include "../video/vga.h"
#include "../../lib/string.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

extern void print(const char* str);

uint32_t pci_read_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address;
    uint32_t lbus  = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;
    
    address = (uint32_t)((lbus << 16) | (lslot << 11) | (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address;
    uint32_t lbus  = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;
    address = (uint32_t)((lbus << 16) | (lslot << 11) | (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

static const char* usb_prog_if_name(uint8_t prog_if) {
    if (prog_if == 0x00) return "UHCI";
    if (prog_if == 0x10) return "OHCI";
    if (prog_if == 0x20) return "EHCI";
    if (prog_if == 0x30) return "XHCI";
    return "Unknown";
}

int pci_find_usb_controllers(usb_pci_controller_t* out, int max_out) {
    int found = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vendor_id = pci_read_config_dword(bus, slot, func, 0) & 0xFFFF;
                if (vendor_id != 0xFFFF) {
                    uint32_t class_code = pci_read_config_dword(bus, slot, func, 0x08);
                    uint8_t base_class = (class_code >> 24) & 0xFF;
                    uint8_t sub_class = (class_code >> 16) & 0xFF;
                    uint8_t prog_if = (class_code >> 8) & 0xFF;

                    if (base_class == 0x0C && sub_class == 0x03) {
                        if (out && found < max_out) {
                            uint32_t id = pci_read_config_dword(bus, slot, func, 0x00);
                            out[found].bus = bus;
                            out[found].slot = slot;
                            out[found].func = func;
                            out[found].prog_if = prog_if;
                            out[found].vendor_id = (uint16_t)(id & 0xFFFF);
                            out[found].device_id = (uint16_t)((id >> 16) & 0xFFFF);
                            out[found].bar0 = pci_read_config_dword(bus, slot, func, 0x10);
                        }
                        found++;
                    }
                }
            }
        }
    }
    return found;
}

static void pci_check_usb(void) {
    usb_pci_controller_t controllers[8];
    int found = pci_find_usb_controllers(controllers, 8);

    print("Scanning PCI bus for USB controllers...\n");
    if (found <= 0) {
        print("No USB controllers found.\n");
        return;
    }

    for (int i = 0; i < found && i < 8; i++) {
        char buf[16];
        print("  Found USB Controller at ");
        itoa((int)controllers[i].bus, buf, 10); print(buf); print(":");
        itoa((int)controllers[i].slot, buf, 10); print(buf); print(":");
        itoa((int)controllers[i].func, buf, 10); print(buf);
        print(" (Type: ");
        print(usb_prog_if_name(controllers[i].prog_if));
        print(")\n");
    }

    print("USB hardware detected.\n");
}

void pci_init(void) {
    pci_check_usb();
}
