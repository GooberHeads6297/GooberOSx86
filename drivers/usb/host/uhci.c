#include "uhci.h"
#include "../../io/io.h"

extern void print(const char* str);

#define UHCI_REG_USBCMD 0x00
#define UHCI_REG_USBSTS 0x02
#define UHCI_REG_PORTSC1 0x10
#define UHCI_REG_PORTSC2 0x12

#define UHCI_CMD_RUNSTOP 0x0001
#define UHCI_CMD_HCRESET 0x0002

#define UHCI_STS_USBINT 0x0001
#define UHCI_STS_ERROR_INTERRUPT 0x0002
#define UHCI_STS_RESUME_DETECT 0x0004
#define UHCI_STS_HOST_SYSTEM_ERR 0x0008
#define UHCI_STS_PROCESS_ERR 0x0010
#define UHCI_STS_HCHALTED 0x0020

static uint16_t uhci_io_base = 0;
static int uhci_ready = 0;
static int uhci_fault_latched = 0;
static int uhci_pointer_probe_ok = 0;

static int uhci_wait_cmd_clear(uint16_t mask, uint32_t spins) {
    while (spins--) {
        if ((inw(uhci_io_base + UHCI_REG_USBCMD) & mask) == 0) return 1;
    }
    return 0;
}

static int uhci_wait_status_clear(uint16_t mask, uint32_t spins) {
    while (spins--) {
        if ((inw(uhci_io_base + UHCI_REG_USBSTS) & mask) == 0) return 1;
    }
    return 0;
}

static int uhci_port_register_sane(uint16_t reg) {
    uint16_t v = inw(uhci_io_base + reg);
    return v != 0xFFFF;
}

int uhci_init(const usb_pci_controller_t* controller) {
    if (!controller) return 0;
    uhci_ready = 0;
    uhci_fault_latched = 0;
    uhci_pointer_probe_ok = 0;

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
    outw(uhci_io_base + UHCI_REG_USBCMD, UHCI_CMD_HCRESET);
    if (!uhci_wait_cmd_clear(UHCI_CMD_HCRESET, 200000U)) {
        print("UHCI reset timeout.\n");
        return 0;
    }

    // Clear status bits.
    outw(uhci_io_base + UHCI_REG_USBSTS, 0xFFFF);
    // Run/Stop bit set.
    outw(uhci_io_base + UHCI_REG_USBCMD, UHCI_CMD_RUNSTOP);
    if (!uhci_wait_status_clear(UHCI_STS_HCHALTED, 200000U)) {
        print("UHCI failed to leave halted state.\n");
        return 0;
    }

    {
        uint16_t status = inw(uhci_io_base + UHCI_REG_USBSTS);
        if (status & (UHCI_STS_HOST_SYSTEM_ERR | UHCI_STS_PROCESS_ERR)) {
            print("UHCI started in error state.\n");
            return 0;
        }
    }

    // Basic safety probe: if both root-port registers read invalid values,
    // lock USB pointer enumeration and rely on PS/2 fallback.
    uhci_pointer_probe_ok =
        uhci_port_register_sane(UHCI_REG_PORTSC1) ||
        uhci_port_register_sane(UHCI_REG_PORTSC2);
    if (!uhci_pointer_probe_ok) {
        print("UHCI root-port probe unstable; pointer enumeration locked.\n");
    }

    uhci_ready = 1;
    print("UHCI initialized.\n");
    return 1;
}

void uhci_poll(void) {
    if (!uhci_ready) return;
    {
        uint16_t status = inw(uhci_io_base + UHCI_REG_USBSTS);
        if (status & (UHCI_STS_HOST_SYSTEM_ERR | UHCI_STS_PROCESS_ERR)) {
            outw(uhci_io_base + UHCI_REG_USBSTS, status);
            uhci_ready = 0;
            uhci_fault_latched = 1;
            print("UHCI fault: controller error, soft-locked.\n");
            return;
        }

        if (status & UHCI_STS_HCHALTED) {
            outw(uhci_io_base + UHCI_REG_USBCMD, UHCI_CMD_RUNSTOP);
            if (!uhci_wait_status_clear(UHCI_STS_HCHALTED, 120000U)) {
                uhci_ready = 0;
                uhci_fault_latched = 1;
                print("UHCI fault: halted, soft-locked.\n");
                return;
            }
        }

        if (status & (UHCI_STS_USBINT | UHCI_STS_ERROR_INTERRUPT | UHCI_STS_RESUME_DETECT)) {
            outw(uhci_io_base + UHCI_REG_USBSTS,
                 (uint16_t)(status & (UHCI_STS_USBINT | UHCI_STS_ERROR_INTERRUPT | UHCI_STS_RESUME_DETECT)));
        }
    }
}

int uhci_controller_healthy(void) {
    return uhci_ready && !uhci_fault_latched;
}

int uhci_pointer_enumeration_allowed(void) {
    return uhci_controller_healthy() && uhci_pointer_probe_ok;
}
