#include "ehci.h"
#include "../../io/io.h"
#include "../../timer/timer.h"

extern void print(const char* str);

#define EHCI_USBCMD       0x00
#define EHCI_USBSTS       0x04
#define EHCI_USBINTR      0x08
#define EHCI_PERIODICBASE 0x14
#define EHCI_ASYNCLIST    0x18
#define EHCI_CONFIGFLAG   0x40
#define EHCI_PORTSC_BASE  0x44

#define EHCI_CMD_RUN      (1U << 0)
#define EHCI_CMD_RESET    (1U << 1)
#define EHCI_CMD_PSE      (1U << 4)
#define EHCI_CMD_ASE      (1U << 5)
#define EHCI_STS_HALTED   (1U << 12)
#define EHCI_STS_RECL     (1U << 13)
#define EHCI_STS_FATAL    (1U << 4)
#define EHCI_PORT_CCS     (1U << 0)
#define EHCI_PORT_PE      (1U << 2)
#define EHCI_PORT_RESET   (1U << 8)
#define EHCI_PORT_LINE    (3U << 10)
#define EHCI_PORT_POWER   (1U << 12)
#define EHCI_PORT_OWNER   (1U << 13)

#define EHCI_LINK_TERMINATE 1U
#define EHCI_LINK_QH        0x2U
#define EHCI_QTD_ACTIVE     0x80U
#define EHCI_QTD_HALTED     0x40U
#define EHCI_QTD_BUFERR     0x20U
#define EHCI_QTD_BABBLE     0x10U
#define EHCI_QTD_XACTERR    0x08U
#define EHCI_QTD_MMF        0x04U
#define EHCI_QTD_STSERR     0x02U
#define EHCI_QTD_PING       0x01U
#define EHCI_QTD_PID_OUT    (0U << 8)
#define EHCI_QTD_PID_IN     (1U << 8)
#define EHCI_QTD_PID_SETUP  (2U << 8)
#define EHCI_QTD_CERR(x)    (((x) & 3U) << 10)
#define EHCI_QTD_IOC        (1U << 15)
#define EHCI_QTD_BYTES(n)   (((uint32_t)(n) & 0x7FFFU) << 16)
#define EHCI_QTD_TOGGLE     (1U << 31)

typedef struct __attribute__((packed, aligned(32))) {
    uint32_t next;
    uint32_t alt_next;
    uint32_t token;
    uint32_t buffer[5];
} ehci_qtd_t;

typedef struct __attribute__((packed, aligned(32))) {
    uint32_t horiz_link;
    uint32_t ep_char;
    uint32_t ep_cap;
    uint32_t current_qtd;
    ehci_qtd_t overlay;
    uint32_t reserved[3];
} ehci_qh_t;

static volatile uint8_t* ehci_cap = 0;
static volatile uint8_t* ehci_op = 0;
static uint32_t ehci_ports = 0;
static int ehci_ready = 0;
static int ehci_fault = 0;

/*
 * Hard iteration ceiling for every hardware-polling busy-wait. Belt-and-
 * suspenders on top of the TSC deadline so no loop can spin forever even if
 * the clock stops. Won't truncate a legitimate sub-second poll.
 */
#define EHCI_SPIN_CEILING 4000000U
static uint32_t periodic_list[1024] __attribute__((aligned(4096)));
static ehci_qh_t async_head __attribute__((aligned(32)));
static ehci_qh_t control_qh __attribute__((aligned(32)));
static ehci_qh_t intr_qh __attribute__((aligned(32)));
static ehci_qtd_t qtd_pool[16] __attribute__((aligned(32)));
static uint8_t report_buf[64] __attribute__((aligned(32)));
static int intr_active = 0;
static uint16_t intr_packet = 8;

static void ehci_serial(const char* s) {
    while (*s) outb(0xE9, *s++);
}

static void ehci_print(const char* s) {
    print(s);
    ehci_serial(s);
}

static void ehci_print_hex32(uint32_t v) {
    char buf[12];
    const char* hex = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) buf[2 + i] = hex[(v >> ((7 - i) * 4)) & 0xF];
    buf[10] = '\n'; buf[11] = '\0';
    ehci_print(buf);
}

static inline uint32_t ehci_read32(uint32_t reg) {
    return *(volatile uint32_t*)(ehci_op + reg);
}

static inline void ehci_write32(uint32_t reg, uint32_t value) {
    *(volatile uint32_t*)(ehci_op + reg) = value;
}

static void ehci_memzero(void* ptr, uint32_t bytes) {
    uint8_t* p = (uint8_t*)ptr;
    for (uint32_t i = 0; i < bytes; i++) p[i] = 0;
}

/*
 * NOTE: the timeout argument here is in 100 Hz TICKS (10 ms each), matching
 * the historical call-site constants. We convert to the IRQ-independent TSC
 * deadline internally (*10 ms) so an SMM storm that freezes IRQ0 can no
 * longer wedge these loops, and add an absolute iteration ceiling.
 */
static int ehci_wait_clear(uint32_t reg, uint32_t mask, uint32_t timeout_ticks) {
    uint64_t deadline = timer_deadline_ms(timeout_ticks * 10);
    uint32_t guard = EHCI_SPIN_CEILING;
    while (!timer_deadline_expired(deadline)) {
        if ((ehci_read32(reg) & mask) == 0) return 1;
        if (--guard == 0) break;
    }
    return 0;
}

static int ehci_wait_set(uint32_t reg, uint32_t mask, uint32_t timeout_ticks) {
    uint64_t deadline = timer_deadline_ms(timeout_ticks * 10);
    uint32_t guard = EHCI_SPIN_CEILING;
    while (!timer_deadline_expired(deadline)) {
        if ((ehci_read32(reg) & mask) == mask) return 1;
        if (--guard == 0) break;
    }
    return 0;
}

static void qtd_setup(ehci_qtd_t* qtd, uint32_t pid, void* buffer,
                      uint16_t len, uint32_t toggle, int ioc) {
    ehci_memzero(qtd, sizeof(ehci_qtd_t));
    qtd->next = EHCI_LINK_TERMINATE;
    qtd->alt_next = EHCI_LINK_TERMINATE;
    qtd->token = EHCI_QTD_ACTIVE | pid | EHCI_QTD_CERR(3) |
                 EHCI_QTD_BYTES(len) | toggle | (ioc ? EHCI_QTD_IOC : 0);
    if (buffer && len > 0) {
        uint32_t addr = (uint32_t)(uintptr_t)buffer;
        for (int i = 0; i < 5; i++)
            qtd->buffer[i] = (addr & 0xFFFFF000U) + (uint32_t)i * 0x1000U;
        qtd->buffer[0] = addr;
    }
}

static void qh_setup(ehci_qh_t* qh, uint8_t addr, uint8_t endpoint,
                     uint16_t max_packet, uint32_t first_qtd, int head) {
    ehci_memzero(qh, sizeof(ehci_qh_t));
    qh->horiz_link = head ? ((uint32_t)(uintptr_t)qh | EHCI_LINK_QH) : EHCI_LINK_TERMINATE;
    qh->ep_char = (uint32_t)(addr & 0x7F)
                | ((uint32_t)(endpoint & 0x0F) << 8)
                | (2U << 12)       /* high-speed */
                | (1U << 14)       /* data toggle from qTD */
                | (head ? (1U << 15) : 0)
                | ((uint32_t)max_packet << 16);
    qh->ep_cap = 1U << 30;         /* one transaction per microframe */
    qh->overlay.next = first_qtd;
    qh->overlay.alt_next = EHCI_LINK_TERMINATE;
}

/* timeout_ticks is in 100 Hz ticks (10 ms each); see ehci_wait_clear note. */
static int qtd_wait(ehci_qtd_t* qtd, uint32_t timeout_ticks) {
    uint64_t deadline = timer_deadline_ms(timeout_ticks * 10);
    uint32_t guard = EHCI_SPIN_CEILING;
    while (!timer_deadline_expired(deadline)) {
        uint32_t token = qtd->token;
        if (!(token & EHCI_QTD_ACTIVE)) {
            if (token & (EHCI_QTD_HALTED | EHCI_QTD_BUFERR | EHCI_QTD_BABBLE |
                         EHCI_QTD_XACTERR | EHCI_QTD_MMF | EHCI_QTD_STSERR))
                return -1;
            return 0;
        }
        if (--guard == 0) break;
    }
    return -2;
}

/*
 * Perform the EHCI BIOS-to-OS legacy handoff. On real hardware (especially
 * pre-UEFI laptops), BIOS keeps ownership of the EHCI controller for USB
 * keyboard/mouse emulation. If we touch the operational registers without
 * asking BIOS to release ownership, an SMI storm can freeze the chipset.
 *
 * Protocol (EHCI spec 2.1.7):
 *   1) Read HCCPARAMS (cap+0x08). Bits [15:8] are EECP (PCI config offset).
 *   2) Walk the PCI capability list looking for cap ID 0x01 (USB Legacy Support).
 *   3) Write bit 24 of USBLEGSUP (HC OS Owned Semaphore).
 *   4) Spin until bit 16 (HC BIOS Owned Semaphore) clears.
 *   5) Clear USBLEGCTLSTS (offset +4) to disable any SMI sources.
 */
static void ehci_bios_handoff(const usb_pci_controller_t* controller,
                              volatile uint8_t* cap_base) {
    uint32_t hccparams = *(volatile uint32_t*)(cap_base + 0x08);
    uint8_t eecp = (uint8_t)((hccparams >> 8) & 0xFF);
    int guard = 0;
    while (eecp >= 0x40 && guard++ < 16) {
        uint32_t cap = pci_read_config_dword(controller->bus, controller->slot,
                                             controller->func, eecp);
        uint8_t cap_id = (uint8_t)(cap & 0xFF);
        if (cap_id == 0x01) {
            /* USB Legacy Support cap: set HC OS Owned (bit 24) */
            uint32_t legsup = cap | (1U << 24);
            pci_write_config_dword(controller->bus, controller->slot,
                                   controller->func, eecp, legsup);
            uint64_t deadline = timer_deadline_ms(1000);
            uint32_t guard = EHCI_SPIN_CEILING;
            while (!timer_deadline_expired(deadline)) {
                legsup = pci_read_config_dword(controller->bus, controller->slot,
                                               controller->func, eecp);
                if (!(legsup & (1U << 16))) break;
                if (--guard == 0) break;
            }
            if (legsup & (1U << 16)) {
                ehci_print("EHCI BIOS did not release legacy ownership (continuing).\n");
                /* Force release by clearing BIOS Owned bit. */
                legsup &= ~(1U << 16);
                pci_write_config_dword(controller->bus, controller->slot,
                                       controller->func, eecp, legsup);
            } else {
                ehci_print("EHCI BIOS legacy handoff complete.\n");
            }
            /* Disable all SMI sources in USBLEGCTLSTS (eecp + 4). */
            pci_write_config_dword(controller->bus, controller->slot,
                                   controller->func, eecp + 4, 0);
            return;
        }
        eecp = (uint8_t)((cap >> 8) & 0xFF);
    }
}

int ehci_init(const usb_pci_controller_t* controller) {
    if (!controller) return 0;
    ehci_ready = 0;
    ehci_fault = 0;
    ehci_ports = 0;

    uint32_t bar0 = controller->bar0;
    if (bar0 == 0 || bar0 == 0xFFFFFFFFu) {
        ehci_print("EHCI BAR=");
        ehci_print_hex32(bar0);
        ehci_print("EHCI: firmware did NOT program this BAR; "
                   "falling through to UHCI/OHCI.\n");
        return 0;
    }
    if (bar0 & 1) {
        ehci_print("EHCI BAR=");
        ehci_print_hex32(bar0);
        ehci_print("EHCI: BAR is IO-space (need MMIO); "
                   "falling through to UHCI/OHCI.\n");
        return 0;
    }
    if ((bar0 & 0xFFFFFFF0) == 0) {
        ehci_print("EHCI BAR=");
        ehci_print_hex32(bar0);
        ehci_print("EHCI: MMIO BAR address bits are zero "
                   "(firmware didn't program it); "
                   "falling through to UHCI/OHCI.\n");
        return 0;
    }

    uint16_t cmd = pci_read_config_word(controller->bus, controller->slot,
                                        controller->func, 0x04);
    pci_write_config_word(controller->bus, controller->slot,
                          controller->func, 0x04, cmd | 0x06);

    ehci_cap = (volatile uint8_t*)(uintptr_t)(bar0 & 0xFFFFFFF0);
    uint8_t cap_len = *ehci_cap;
    /*
     * If the cap registers read back as all-ones, the BAR was decoded but the
     * controller is not present (or in D3hot). Bail cleanly so the host
     * scanner moves to the next rung.
     */
    uint32_t hcsparams_probe = *(volatile uint32_t*)(ehci_cap + 0x04);
    if (cap_len == 0xFF || hcsparams_probe == 0xFFFFFFFFu) {
        ehci_print("EHCI: capability registers read 0xFF/0xFFFFFFFF -- "
                   "controller absent.\n");
        return 0;
    }
    if (cap_len < 0x10 || cap_len > 0x80) {
        ehci_print("EHCI bad capability length.\n");
        return 0;
    }
    ehci_op = ehci_cap + cap_len;

    /* CRITICAL: take ownership from BIOS before writing any operational regs. */
    ehci_bios_handoff(controller, ehci_cap);

    uint32_t hcsparams = *(volatile uint32_t*)(ehci_cap + 0x04);
    ehci_ports = hcsparams & 0x0F;
    if (ehci_ports == 0 || ehci_ports > 15) {
        ehci_print("EHCI invalid port count.\n");
        return 0;
    }

    ehci_write32(EHCI_USBCMD, ehci_read32(EHCI_USBCMD) & ~EHCI_CMD_RUN);
    ehci_wait_clear(EHCI_USBSTS, 0, 1);
    ehci_write32(EHCI_USBCMD, EHCI_CMD_RESET);
    if (!ehci_wait_clear(EHCI_USBCMD, EHCI_CMD_RESET, 100)) {
        ehci_print("EHCI reset timeout.\n");
        return 0;
    }

    ehci_write32(EHCI_USBINTR, 0);
    ehci_memzero(periodic_list, sizeof(periodic_list));
    for (int i = 0; i < 1024; i++) periodic_list[i] = EHCI_LINK_TERMINATE;
    qh_setup(&async_head, 0, 0, 64, EHCI_LINK_TERMINATE, 1);
    async_head.overlay.token = 0;
    ehci_write32(EHCI_PERIODICBASE, (uint32_t)(uintptr_t)periodic_list);
    ehci_write32(EHCI_ASYNCLIST, (uint32_t)(uintptr_t)&async_head);
    ehci_write32(EHCI_CONFIGFLAG, 1);
    ehci_write32(EHCI_USBCMD, EHCI_CMD_RUN | EHCI_CMD_ASE | EHCI_CMD_PSE);
    ehci_wait_set(EHCI_USBSTS, EHCI_STS_RECL, 100);
    if (ehci_read32(EHCI_USBSTS) & EHCI_STS_HALTED) {
        ehci_print("EHCI did not run.\n");
        return 0;
    }

    ehci_ready = 1;
    ehci_print("EHCI initialized.\n");
    return 1;
}

void ehci_poll(void) {
    if (!ehci_ready) return;
    uint32_t status = ehci_read32(EHCI_USBSTS);
    if (status & EHCI_STS_FATAL) {
        ehci_fault = 1;
        ehci_ready = 0;
        ehci_print("EHCI fatal error.\n");
    }
    if (status) ehci_write32(EHCI_USBSTS, status);
}

int ehci_controller_healthy(void) {
    return ehci_ready && !ehci_fault;
}

int ehci_port_count(void) {
    return ehci_ready ? (int)ehci_ports : 0;
}

int ehci_port_connected(int port) {
    if (!ehci_ready || port < 0 || (uint32_t)port >= ehci_ports) return 0;
    return (ehci_read32(EHCI_PORTSC_BASE + (uint32_t)port * 4) & EHCI_PORT_CCS) != 0;
}

int ehci_port_low_speed(int port) {
    if (!ehci_ready || port < 0 || (uint32_t)port >= ehci_ports) return 0;
    uint32_t v = ehci_read32(EHCI_PORTSC_BASE + (uint32_t)port * 4);
    if (v & EHCI_PORT_OWNER) return 1;
    if ((v & EHCI_PORT_LINE) == (1U << 10)) return 1;
    return 0;
}

int ehci_port_owned_by_companion(int port) {
    if (!ehci_ready || port < 0 || (uint32_t)port >= ehci_ports) return 0;
    return (ehci_read32(EHCI_PORTSC_BASE + (uint32_t)port * 4) & EHCI_PORT_OWNER) != 0;
}

/*
 * Hot-plug primitives. EHCI PORTSC has CSC at bit 1 (RW1C). The other RW
 * bits we must preserve across the ack are PE (bit 2), POWER (bit 12),
 * OWNER (bit 13), and the line-status read-only bits.
 */
#define EHCI_PORT_CSC_BIT  (1U << 1)
#define EHCI_PORT_PEDC_BIT (1U << 3)

int ehci_port_change_pending(int port) {
    if (!ehci_ready || ehci_fault) return 0;
    if (port < 0 || (uint32_t)port >= ehci_ports) return 0;
    uint32_t v = ehci_read32(EHCI_PORTSC_BASE + (uint32_t)port * 4);
    return (v & EHCI_PORT_CSC_BIT) != 0;
}

void ehci_port_change_ack(int port) {
    if (!ehci_ready || ehci_fault) return;
    if (port < 0 || (uint32_t)port >= ehci_ports) return;
    uint32_t reg = EHCI_PORTSC_BASE + (uint32_t)port * 4;
    uint32_t v = ehci_read32(reg);
    /* Mask out RW1C bits we don't want to clear (PEDC and OCC), then OR in
     * CSC so writing 1 there clears just the connect-status-change latch. */
    uint32_t preserve_mask = ~(EHCI_PORT_CSC_BIT | EHCI_PORT_PEDC_BIT |
                               (1U << 5) /* OCC */);
    ehci_write32(reg, (v & preserve_mask) | EHCI_PORT_CSC_BIT);
}

/*
 * EHCI port reset: high-speed handshake.
 * - If line status reports a low-/full-speed device, hand off to companion
 *   controller immediately (set PORT_OWNER) so UHCI/OHCI can drive it.
 * - Otherwise, hold reset for at least 50 ms (USB 2.0 §7.1.7.5), then clear
 *   and wait for the controller to enable the port. If it never enables,
 *   hand the port to the companion as well.
 */
void ehci_port_reset(int port) {
    if (!ehci_ready || port < 0 || (uint32_t)port >= ehci_ports) return;
    uint32_t reg = EHCI_PORTSC_BASE + (uint32_t)port * 4;
    uint32_t v = ehci_read32(reg);
    if (!(v & EHCI_PORT_CCS)) return;

    /* Make sure the port is powered before we read line status. */
    if (!(v & EHCI_PORT_POWER)) {
        ehci_write32(reg, v | EHCI_PORT_POWER);
        timer_busy_wait_ms(20);
        v = ehci_read32(reg);
    }

    /* Line status 01 = K-state = low-speed device. Hand it to companion. */
    if ((v & EHCI_PORT_LINE) == (1U << 10)) {
        ehci_write32(reg, v | EHCI_PORT_OWNER);
        ehci_print("EHCI: low-speed device, handed to companion.\n");
        return;
    }

    /* Drive the reset, hold for at least 50 ms. */
    ehci_write32(reg, (v | EHCI_PORT_RESET) & ~EHCI_PORT_PE & ~EHCI_PORT_OWNER);
    timer_busy_wait_ms(60);
    ehci_write32(reg, ehci_read32(reg) & ~EHCI_PORT_RESET);

    /* Wait up to 500 ms for hardware to clear the reset bit. */
    uint64_t deadline = timer_deadline_ms(500);
    uint32_t guard = EHCI_SPIN_CEILING;
    while (!timer_deadline_expired(deadline)) {
        if (!(ehci_read32(reg) & EHCI_PORT_RESET)) break;
        if (--guard == 0) break;
    }

    /* Allow another 20 ms for the port-enable handshake to settle. */
    timer_busy_wait_ms(20);

    v = ehci_read32(reg);
    if (!(v & EHCI_PORT_PE)) {
        /* Reset didn't enable the port: full-speed device, hand to companion. */
        ehci_write32(reg, v | EHCI_PORT_OWNER);
        ehci_print("EHCI: port did not enable, handed to companion.\n");
    }
}

int ehci_control_transfer(uint8_t dev_addr, uint8_t endpoint,
                          uint8_t* setup_pkt, uint8_t* data, uint16_t data_len,
                          int direction_in) {
    if (!ehci_ready || !setup_pkt) return -1;
    ehci_qtd_t* setup = &qtd_pool[0];
    ehci_qtd_t* data_td = data_len ? &qtd_pool[1] : 0;
    ehci_qtd_t* status = data_len ? &qtd_pool[2] : &qtd_pool[1];

    qtd_setup(setup, EHCI_QTD_PID_SETUP, setup_pkt, 8, 0, 0);
    setup->next = (uint32_t)(uintptr_t)(data_td ? data_td : status);

    if (data_td) {
        qtd_setup(data_td, direction_in ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT,
                  data, data_len, EHCI_QTD_TOGGLE, 0);
        data_td->next = (uint32_t)(uintptr_t)status;
    }

    qtd_setup(status, direction_in ? EHCI_QTD_PID_OUT : EHCI_QTD_PID_IN,
              0, 0, EHCI_QTD_TOGGLE, 1);

    uint16_t max_packet = (dev_addr == 0) ? 8 : 64;
    qh_setup(&control_qh, dev_addr, endpoint, max_packet,
             (uint32_t)(uintptr_t)setup, 0);
    control_qh.horiz_link = async_head.horiz_link;
    async_head.horiz_link = (uint32_t)(uintptr_t)&control_qh | EHCI_LINK_QH;

    /*
     * 250 ticks at 100 Hz is 2.5 seconds per control transfer. That's far too
     * generous on real hardware and on VMs with many empty ports -- each
     * descriptor probe can stack up to over a minute total. 25 ticks (~250 ms)
     * is plenty for a healthy device and lets enumeration fail fast on a dead
     * port so we can move on to the next candidate.
     */
    int ret = qtd_wait(status, 25);
    async_head.horiz_link = (uint32_t)(uintptr_t)&async_head | EHCI_LINK_QH;
    return ret;
}

int ehci_schedule_interrupt(uint8_t dev_addr, uint8_t endpoint,
                            uint16_t max_packet, uint8_t interval_frames) {
    if (!ehci_ready || intr_active) return -1;
    if (max_packet == 0 || max_packet > sizeof(report_buf)) max_packet = 8;
    intr_packet = max_packet;
    ehci_memzero(report_buf, sizeof(report_buf));
    ehci_qtd_t* qtd = &qtd_pool[4];
    qtd_setup(qtd, EHCI_QTD_PID_IN, report_buf, max_packet, 0, 1);
    qh_setup(&intr_qh, dev_addr, endpoint, max_packet, (uint32_t)(uintptr_t)qtd, 0);

    int step = interval_frames;
    if (step < 1) step = 1;
    if (step > 128) step = 128;
    for (int i = 0; i < 1024; i += step)
        periodic_list[i] = (uint32_t)(uintptr_t)&intr_qh | EHCI_LINK_QH;
    intr_active = 1;
    return 0;
}

void ehci_remove_interrupt(void) {
    for (int i = 0; i < 1024; i++) periodic_list[i] = EHCI_LINK_TERMINATE;
    intr_active = 0;
}

int ehci_interrupt_active(void) { return intr_active; }

uint8_t* ehci_get_report(int* ready) {
    if (!intr_active) {
        if (ready) *ready = 0;
        return report_buf;
    }
    ehci_qtd_t* qtd = &qtd_pool[4];
    if (qtd->token & EHCI_QTD_ACTIVE) {
        if (ready) *ready = 0;
    } else if (qtd->token & (EHCI_QTD_HALTED | EHCI_QTD_BUFERR | EHCI_QTD_BABBLE |
                             EHCI_QTD_XACTERR | EHCI_QTD_MMF | EHCI_QTD_STSERR)) {
        if (ready) *ready = -1;
    } else {
        if (ready) *ready = 1;
    }
    return report_buf;
}

void ehci_ack_report(void) {
    if (!intr_active) return;
    /* Clear stale bytes so a short report never leaves old wheel/data behind. */
    ehci_memzero(report_buf, intr_packet);
    ehci_qtd_t* qtd = &qtd_pool[4];
    qtd_setup(qtd, EHCI_QTD_PID_IN, report_buf, intr_packet,
              (qtd->token & EHCI_QTD_TOGGLE) ? 0 : EHCI_QTD_TOGGLE, 1);
    intr_qh.overlay.next = (uint32_t)(uintptr_t)qtd;
    intr_qh.overlay.alt_next = EHCI_LINK_TERMINATE;
}
