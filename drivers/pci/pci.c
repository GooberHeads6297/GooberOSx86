#include "pci.h"
#include "../io/io.h"
#include "../video/vga.h"
#include "../diagnostics/driver_log.h"
#include "../../lib/string.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

extern void print(const char* str);

uint8_t pci_read_config_byte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t dword = pci_read_config_dword(bus, slot, func, offset);
    uint8_t shift = (uint8_t)((offset & 3U) * 8U);
    return (uint8_t)((dword >> shift) & 0xFFU);
}

uint16_t pci_read_config_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t dword = pci_read_config_dword(bus, slot, func, offset);
    uint8_t shift = (uint8_t)((offset & 2U) * 8U);
    return (uint16_t)((dword >> shift) & 0xFFFFU);
}

uint32_t pci_read_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address;
    uint32_t lbus  = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;
    
    address = (uint32_t)((lbus << 16) | (lslot << 11) | (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    uint32_t current = pci_read_config_dword(bus, slot, func, offset);
    uint8_t shift = (uint8_t)((offset & 2U) * 8U);
    uint32_t mask = (uint32_t)0xFFFF << shift;
    uint32_t updated = (current & ~mask) | ((uint32_t)value << shift);
    pci_write_config_dword(bus, slot, func, offset, updated);
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

int pci_read_mmio_bar(uint8_t bus, uint8_t slot, uint8_t func, uint8_t bar_index,
                      uint64_t* out_addr) {
    uint8_t off;
    uint32_t lo;
    uint32_t hi;

    if (bar_index > 5 || !out_addr) return 0;
    off = (uint8_t)(0x10 + bar_index * 4U);
    lo = pci_read_config_dword(bus, slot, func, off);
    if (lo == 0 || lo == 0xFFFFFFFFU) return 0;
    if (lo & 0x1U) return 0; /* I/O space */

    if (lo & 0x4U) {
        hi = pci_read_config_dword(bus, slot, func, (uint8_t)(off + 4U));
        *out_addr = ((uint64_t)hi << 32) | ((uint64_t)lo & ~0xFULL);
        return 1;
    }

    *out_addr = (uint64_t)(lo & ~0xFULL);
    return 1;
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
                            out[found].bar1 = pci_read_config_dword(bus, slot, func, 0x14);
                        }
                        found++;
                    }
                }
            }
        }
    }
    return found;
}

static int pci_is_intel_lpss_i2c(uint16_t vendor_id, uint16_t device_id,
                                 uint8_t base_class, uint8_t sub_class) {
    if (vendor_id == 0x8086) {
        /* Bay Trail LPSS I2C controller IDs are commonly 0x0F41..0x0F45. */
        if (device_id >= 0x0F41 && device_id <= 0x0F45) return 1;
        /* Many Intel LPSS I2C devices report as "serial bus, other". */
        if (base_class == 0x0C && sub_class == 0x80) return 1;
    }
    return 0;
}

int pci_find_i2c_controllers(i2c_pci_controller_t* out, int max_out) {
    int found = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t id = pci_read_config_dword(bus, slot, func, 0);
                uint16_t vendor_id = (uint16_t)(id & 0xFFFF);
                uint16_t device_id = (uint16_t)((id >> 16) & 0xFFFF);
                if (vendor_id == 0xFFFF) continue;

                uint32_t class_code = pci_read_config_dword(bus, slot, func, 0x08);
                uint8_t base_class = (class_code >> 24) & 0xFF;
                uint8_t sub_class = (class_code >> 16) & 0xFF;

                if (!pci_is_intel_lpss_i2c(vendor_id, device_id, base_class, sub_class)) {
                    continue;
                }

                if (out && found < max_out) {
                    out[found].bus = (uint8_t)bus;
                    out[found].slot = slot;
                    out[found].func = func;
                    out[found].vendor_id = vendor_id;
                    out[found].device_id = device_id;
                    out[found].bar0 = pci_read_config_dword(bus, slot, func, 0x10);
                    out[found].bar1 = pci_read_config_dword(bus, slot, func, 0x14);
                }
                found++;
            }
        }
    }
    return found;
}

int pci_find_display_controllers(pci_display_device_t* out, int max_out) {
    int found = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vendor_id = pci_read_config_dword(bus, slot, func, 0) & 0xFFFF;
                if (vendor_id == 0xFFFF) continue;

                uint32_t class_code = pci_read_config_dword(bus, slot, func, 0x08);
                uint8_t base_class = (class_code >> 24) & 0xFF;
                if (base_class != 0x03) continue; /* display controller */

                if (out && found < max_out) {
                    uint32_t id = pci_read_config_dword(bus, slot, func, 0x00);
                    out[found].bus = (uint8_t)bus;
                    out[found].slot = slot;
                    out[found].func = func;
                    out[found].sub_class = (class_code >> 16) & 0xFF;
                    out[found].prog_if = (class_code >> 8) & 0xFF;
                    out[found].vendor_id = (uint16_t)(id & 0xFFFF);
                    out[found].device_id = (uint16_t)((id >> 16) & 0xFFFF);
                    for (int b = 0; b < 6; b++) {
                        out[found].bar[b] =
                            pci_read_config_dword(bus, slot, func, 0x10 + (uint8_t)(b * 4));
                    }
                }
                found++;
            }
        }
    }
    return found;
}

#define PCI_COMMAND_OFFSET 0x04
#define PCI_COMMAND_MEMORY 0x0002
#define PCI_COMMAND_IO     0x0001
#define PCI_COMMAND_BUSMASTER 0x0004

void pci_enable_device(uint8_t bus, uint8_t slot, uint8_t func) {
    uint8_t cap_ptr;
    uint16_t cmd;

    cmd = pci_read_config_word(bus, slot, func, PCI_COMMAND_OFFSET);
    cmd = (uint16_t)(cmd | PCI_COMMAND_MEMORY | PCI_COMMAND_IO | PCI_COMMAND_BUSMASTER);
    pci_write_config_word(bus, slot, func, PCI_COMMAND_OFFSET, cmd);

    cap_ptr = pci_read_config_byte(bus, slot, func, 0x34);
    for (int guard = 0; guard < 48 && cap_ptr >= 0x40; guard++) {
        uint8_t cap_id = pci_read_config_byte(bus, slot, func, cap_ptr);
        if (cap_id == 0x01) {
            uint16_t pmcsr = pci_read_config_word(bus, slot, func, (uint8_t)(cap_ptr + 4));
            pmcsr = (uint16_t)(pmcsr & ~0x0003U);
            pci_write_config_word(bus, slot, func, (uint8_t)(cap_ptr + 4), pmcsr);
            break;
        }
        cap_ptr = pci_read_config_byte(bus, slot, func, (uint8_t)(cap_ptr + 1));
        if (cap_ptr == 0) break;
    }
}

static int pci_is_intel_sdhci_id(uint16_t vendor_id, uint16_t device_id) {
    if (vendor_id != 0x8086U) return 0;
    return device_id == 0x0F14U || device_id == 0x0F15U || device_id == 0x0F16U ||
           device_id == 0x119A || device_id == 0x119B;
}

static int pci_is_sdhci_class(uint8_t base_class, uint8_t sub_class) {
    return base_class == 0x08 && sub_class == 0x05;
}

int pci_find_sdhci_controllers(sdhci_pci_controller_t* out, int max_out) {
    int found = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t id = pci_read_config_dword((uint8_t)bus, slot, func, 0x00);
                uint16_t vendor_id = (uint16_t)(id & 0xFFFFU);
                uint16_t device_id = (uint16_t)((id >> 16) & 0xFFFFU);
                uint8_t base_class;
                uint8_t sub_class;
                uint8_t prog_if;

                if (vendor_id == 0xFFFFU || vendor_id == 0x0000U) continue;

                base_class = pci_read_config_byte((uint8_t)bus, slot, func, 0x0B);
                sub_class = pci_read_config_byte((uint8_t)bus, slot, func, 0x0A);
                prog_if = pci_read_config_byte((uint8_t)bus, slot, func, 0x09);

                if (!pci_is_sdhci_class(base_class, sub_class) &&
                    !pci_is_intel_sdhci_id(vendor_id, device_id))
                    continue;

                if (out && found < max_out) {
                    out[found].bus = (uint8_t)bus;
                    out[found].slot = slot;
                    out[found].func = func;
                    out[found].class_code = base_class;
                    out[found].sub_class = sub_class;
                    out[found].prog_if = prog_if;
                    out[found].vendor_id = vendor_id;
                    out[found].device_id = device_id;
                    {
                        uint64_t bar64 = 0;
                        if (pci_read_mmio_bar((uint8_t)bus, slot, func, 0, &bar64))
                            out[found].bar0 = (uint32_t)bar64;
                        else
                            out[found].bar0 = pci_read_config_dword((uint8_t)bus, slot, func, 0x10);
                    }
                }
                found++;
            }
        }
    }
    return found;
}

static void pci_check_usb(void) {
    usb_pci_controller_t controllers[8];
    int found = pci_find_usb_controllers(controllers, 8);

    print("Scanning PCI bus for USB controllers...\n");
    driver_log_line("[pci] scanning for USB controllers.");
    if (found <= 0) {
        print("No USB controllers found.\n");
        driver_log_line("[pci] no USB controllers found.");
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
        driver_log("[pci] USB ");
        driver_log(usb_prog_if_name(controllers[i].prog_if));
        driver_log(" controller at ");
        driver_log_u32(controllers[i].bus);
        driver_log(":");
        driver_log_u32(controllers[i].slot);
        driver_log(":");
        driver_log_u32(controllers[i].func);
        driver_log(" vendor=");
        driver_log_hex32(controllers[i].vendor_id);
        driver_log(" device=");
        driver_log_hex32(controllers[i].device_id);
        driver_log("\n");
    }

    print("USB hardware detected.\n");
    driver_log_line("[pci] USB hardware detected.");
}

void pci_init(void) {
    /*
     * NON-DESTRUCTIVE PCI scan.
     *
     * The previous implementation wrote 0xFFFFFFFF to BAR0 of every USB
     * controller to size the BAR. On real hardware that share USB controllers
     * with BIOS legacy USB SMI emulation (most pre-UEFI laptops, including
     * Lenovo 2013-era), poking the BAR while SMM still owns the controller
     * triggers an SMI storm or freezes the chipset. We rely on the BIOS to
     * have already assigned BARs and just enumerate the bus passively here.
     */
    pci_check_usb();
}
