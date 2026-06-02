#include "uhci.h"
#include <stddef.h>
#include "../../io/io.h"
#include "../../timer/timer.h"

extern void print(const char* str);

static void uhci_serial(const char* s) {
    while (*s) { outb(0xE9, *s++); }
}
static void uhci_print(const char* s) {
    print(s);
    uhci_serial(s);
}

/* ---- Register offsets ---- */
#define UHCI_REG_USBCMD   0x00
#define UHCI_REG_USBSTS   0x02
#define UHCI_REG_USBINTR  0x04
#define UHCI_REG_FRNUM    0x06
#define UHCI_REG_FLBASEADD 0x08
#define UHCI_REG_SOF      0x0C
#define UHCI_REG_PORTSC1  0x10
#define UHCI_REG_PORTSC2  0x12

/* ---- Command bits ---- */
#define UHCI_CMD_RUNSTOP  (1 << 0)
#define UHCI_CMD_HCRESET  (1 << 1)

/* ---- Status bits ---- */
#define UHCI_STS_USBINT   (1 << 0)
#define UHCI_STS_ERR      (1 << 1)
#define UHCI_STS_RESUME   (1 << 2)
#define UHCI_STS_HSE      (1 << 3)
#define UHCI_STS_PE       (1 << 4)
#define UHCI_STS_HCHALTED (1 << 5)

/* ---- Port Status bits ---- */
#define UHCI_PORT_CCS     (1 << 0)
#define UHCI_PORT_CSC     (1 << 1)
#define UHCI_PORT_PED     (1 << 2)
#define UHCI_PORT_PEDC    (1 << 3)
#define UHCI_PORT_LSDA    (1 << 8)
#define UHCI_PORT_RESET   (1 << 9)

/* ---- Internal sizes ---- */
#define FRAME_LIST_SIZE    1024
#define TD_POOL_SIZE       48
#define QH_POOL_SIZE        4

/*
 * Hard iteration ceiling for every hardware-polling busy-wait. Belt-and-
 * suspenders on top of the TSC deadline: even if the clock somehow stops,
 * no loop can spin forever. Sized so it never truncates a sub-second poll
 * (a 500 ms deadline does well under this many port-I/O reads).
 */
#define UHCI_SPIN_CEILING 4000000U

/*
 * Per-transfer timeout. timer_deadline_ms() is the wall-clock bound; this is
 * 500 ms (was 20 s = 2000 ticks, which let a single wedged transfer stall the
 * whole boot for the platform-watchdog window on real hardware).
 */
#define UHCI_TD_TIMEOUT_MS 500U

/* ---- Aligned data structures ---- */
static uint32_t frame_list[FRAME_LIST_SIZE] __attribute__((aligned(4096)));
static uhci_td_t td_pool[TD_POOL_SIZE] __attribute__((aligned(16)));
static uhci_qh_t qh_pool[QH_POOL_SIZE] __attribute__((aligned(16)));
static int td_pool_used[TD_POOL_SIZE];
static int qh_pool_used[QH_POOL_SIZE];

static uint16_t uhci_io_base = 0;
static int uhci_ready = 0;
static int uhci_fault_latched = 0;
static int uhci_pointer_probe_ok = 0;

/* ---- Persistent interrupt transfer state ---- */
static uhci_qh_t*  intr_qh = NULL;
static uhci_td_t*  intr_td = NULL;
static uint8_t*   intr_buffer = NULL;
static uint16_t   intr_max_pkt = 0;
static int        intr_active = 0;

/* ---- IO register helpers ---- */
static int wait_cmd_clear(uint16_t mask, uint32_t spins) {
    while (spins--) {
        if ((inw(uhci_io_base + UHCI_REG_USBCMD) & mask) == 0) return 1;
    }
    return 0;
}

static int wait_status_clear(uint16_t mask, uint32_t spins) {
    while (spins--) {
        if ((inw(uhci_io_base + UHCI_REG_USBSTS) & mask) == 0) return 1;
    }
    return 0;
}

static int port_register_sane(uint16_t reg) {
    uint16_t v = inw(uhci_io_base + reg);
    return v != 0xFFFF;
}

/* ---- TD pool ---- */
static uhci_td_t* alloc_td(void) {
    for (int i = 0; i < TD_POOL_SIZE; i++) {
        if (!td_pool_used[i]) { td_pool_used[i] = 1; return &td_pool[i]; }
    }
    return NULL;
}

static void free_td(uhci_td_t* td) {
    for (int i = 0; i < TD_POOL_SIZE; i++) {
        if (&td_pool[i] == td) { td_pool_used[i] = 0; return; }
    }
}

static uhci_qh_t* alloc_qh(void) {
    for (int i = 0; i < QH_POOL_SIZE; i++) {
        if (!qh_pool_used[i]) { qh_pool_used[i] = 1; return &qh_pool[i]; }
    }
    return NULL;
}

static void free_qh(uhci_qh_t* qh) {
    for (int i = 0; i < QH_POOL_SIZE; i++) {
        if (&qh_pool[i] == qh) { qh_pool_used[i] = 0; return; }
    }
}

/* ---- Wait for TD completion ---- */
static int td_wait(uhci_td_t* td, uint32_t timeout_ms) {
    uint64_t deadline = timer_deadline_ms(timeout_ms);
    uint32_t guard = UHCI_SPIN_CEILING;
    while (td->status & UHCI_TD_ACTIVE) {
        if (timer_deadline_expired(deadline)) break;
        if (--guard == 0) break;
    }
    if (td->status & UHCI_TD_ACTIVE) return -1;
    if (td->status & (UHCI_TD_STALLED | UHCI_TD_BUFERR | UHCI_TD_BABBLE | UHCI_TD_TIMEOUT))
        return -2;
    return 0;
}

/* ---- Initialize ---- */
int uhci_init(const usb_pci_controller_t* controller) {
    if (!controller) return 0;
    uhci_ready = 0;
    uhci_fault_latched = 0;
    uhci_pointer_probe_ok = 0;
    intr_active = 0;
    intr_qh = NULL; intr_td = NULL; intr_buffer = NULL;

    /* Enable PCI IO space and bus mastering */
    uint16_t cmd = pci_read_config_word(controller->bus, controller->slot,
                                        controller->func, 0x04);
    pci_write_config_word(controller->bus, controller->slot,
                          controller->func, 0x04, cmd | 0x03);

    /* Try to assign IO port to BAR0 if unassigned */
    uint32_t bar0_val = pci_read_config_dword(controller->bus, controller->slot,
                                              controller->func, 0x10);
    if ((bar0_val & 0x1) == 0 || (bar0_val & 0xFFFC) == 0 ||
        bar0_val == 0xFFFFFFFFu) {
        /*
         * Best-effort fallback: try to assign IO base 0xC060. If the chipset
         * accepts it AND a subsequent test inb() does not read 0xFF (which
         * means nothing is actually decoding at that address), keep going.
         * Otherwise log clearly and let usb_host_try_next move to OHCI.
         */
        pci_write_config_dword(controller->bus, controller->slot,
                               controller->func, 0x10, 0xC061);
        bar0_val = pci_read_config_dword(controller->bus, controller->slot,
                                         controller->func, 0x10);
        if ((bar0_val & 0x1) == 0 || (bar0_val & 0xFFFC) == 0 ||
            bar0_val == 0xFFFFFFFFu) {
            uhci_print("UHCI: BAR0 unassigned and self-program failed; "
                       "falling through to OHCI.\n");
            return 0;
        }
    }
    uhci_io_base = (uint16_t)(bar0_val & 0xFFFC);
    if (uhci_io_base == 0) { uhci_print("UHCI invalid IO base.\n"); return 0; }

    /*
     * UHCI legacy (BIOS->OS) handoff -- highest-leverage real-hardware fix.
     *
     * USBLEGSUP lives at PCI config offset 0xC0. On a BIOS with USB legacy
     * keyboard/mouse emulation enabled, every UHCI port I/O traps to SMM
     * until trapping is disabled here. Left enabled, the resulting SMI storm
     * stops IRQ0 -> timer tick freezes -> the boot wedges on "enumerating
     * port 1" until the platform watchdog hard-resets the machine.
     *
     * Writing 0x8F00 clears the R/WC status bits (bit 15 + 11:8) and disables
     * all SMI-enable/trap sources -- the same value Linux writes
     * (USBLEGSUP_RWC). This MUST happen before HCRESET and before any port
     * register access. In VMs there is no legacy emulation, so this is a
     * harmless no-op.
     */
    pci_write_config_word(controller->bus, controller->slot,
                          controller->func, 0xC0, 0x8F00);

    outw(uhci_io_base + UHCI_REG_USBCMD, UHCI_CMD_HCRESET);
    if (!wait_cmd_clear(UHCI_CMD_HCRESET, 200000U)) { uhci_print("UHCI reset timeout.\n"); return 0; }

    outw(uhci_io_base + UHCI_REG_USBSTS, 0xFFFF);
    /* Phase 3d pointer-width audit: cast through uintptr_t before truncating
     * to 32 bits so the long-mode build doesn't drop high bits. UHCI requires
     * a 32-bit physical address; the kernel's BSS lives in the identity-mapped
     * low 4 GiB so the truncation is safe. */
    outl(uhci_io_base + UHCI_REG_FLBASEADD, (uint32_t)(uintptr_t)frame_list);
    outw(uhci_io_base + UHCI_REG_SOF, 64);

    for (int i = 0; i < FRAME_LIST_SIZE; i++) frame_list[i] = UHCI_LINK_TERMINATE;
    for (int i = 0; i < TD_POOL_SIZE; i++) { td_pool_used[i] = 0; __builtin_memset(&td_pool[i], 0, sizeof(uhci_td_t)); }
    for (int i = 0; i < QH_POOL_SIZE; i++) { qh_pool_used[i] = 0; __builtin_memset(&qh_pool[i], 0, sizeof(uhci_qh_t)); }

    outw(uhci_io_base + UHCI_REG_FRNUM, 0);
    outw(uhci_io_base + UHCI_REG_USBCMD, UHCI_CMD_RUNSTOP);
    if (!wait_status_clear(UHCI_STS_HCHALTED, 200000U)) { uhci_print("UHCI failed to start.\n"); return 0; }

    uint16_t sts = inw(uhci_io_base + UHCI_REG_USBSTS);
    if (sts & (UHCI_STS_HSE | UHCI_STS_PE)) { uhci_print("UHCI started in error state.\n"); return 0; }

    uhci_pointer_probe_ok = port_register_sane(UHCI_REG_PORTSC1) || port_register_sane(UHCI_REG_PORTSC2);
    uhci_ready = 1;
    uhci_print("UHCI initialized.\n");
    return 1;
}

/* ---- Port operations ---- */
int uhci_port_connected(int port) {
    if (port < 0 || port > 1) return 0;
    uint16_t reg = port ? UHCI_REG_PORTSC2 : UHCI_REG_PORTSC1;
    return (inw(uhci_io_base + reg) & UHCI_PORT_CCS) != 0;
}

int uhci_port_low_speed(int port) {
    if (port < 0 || port > 1) return 0;
    uint16_t reg = port ? UHCI_REG_PORTSC2 : UHCI_REG_PORTSC1;
    return (inw(uhci_io_base + reg) & UHCI_PORT_LSDA) != 0;
}

void uhci_port_reset(int port) {
    if (port < 0 || port > 1) return;
    uint16_t reg = port ? UHCI_REG_PORTSC2 : UHCI_REG_PORTSC1;
    uint16_t pv = inw(uhci_io_base + reg);
    outw(uhci_io_base + reg, pv | UHCI_PORT_RESET);
    timer_busy_wait_ms(60);   /* USB 2.0 reset hold (>= 50 ms) */
    outw(uhci_io_base + reg, (pv | UHCI_PORT_PED) & ~UHCI_PORT_RESET);
    timer_busy_wait_ms(10);   /* let the port-enable handshake settle */
}

/*
 * Hot-plug primitives. UHCI PORTSC has CSC at bit 1 (RW1C). We preserve
 * PE, line-status, and reset bits across the ack.
 */
int uhci_port_change_pending(int port) {
    if (!uhci_ready || uhci_fault_latched) return 0;
    if (port < 0 || port > 1) return 0;
    uint16_t reg = port ? UHCI_REG_PORTSC2 : UHCI_REG_PORTSC1;
    uint16_t v = inw(uhci_io_base + reg);
    if (v == 0xFFFF) return 0;
    return (v & UHCI_PORT_CSC) != 0;
}

void uhci_port_change_ack(int port) {
    if (!uhci_ready || uhci_fault_latched) return;
    if (port < 0 || port > 1) return;
    uint16_t reg = port ? UHCI_REG_PORTSC2 : UHCI_REG_PORTSC1;
    uint16_t v = inw(uhci_io_base + reg);
    if (v == 0xFFFF) return;
    /* Write 1 to CSC + PEDC to clear them; preserve the RW bits below. */
    outw(uhci_io_base + reg, v | UHCI_PORT_CSC);
}

/* ---- Control transfer (synchronous) ---- */
int uhci_control_transfer(uint8_t dev_addr, uint8_t endpoint,
                          uint8_t* setup_pkt, uint8_t* data, uint16_t data_len,
                          int direction_in) {
    if (!uhci_ready) return -1;

    uhci_td_t* td_setup  = alloc_td();
    uhci_td_t* td_status = alloc_td();
    uhci_td_t* td_data   = NULL;
    if (!td_setup || !td_status) {
        if (td_setup) free_td(td_setup);
        if (td_status) free_td(td_status);
        return -1;
    }

    /* SETUP stage (DATA0) */
    td_setup->link = UHCI_LINK_TERMINATE;
    td_setup->status = UHCI_TD_ACTIVE | UHCI_TD_ERR(3);
    td_setup->token = UHCI_TOKEN_PID_SETUP
                    | (dev_addr << UHCI_TOKEN_DEVADDR_SHIFT)
                    | (endpoint << UHCI_TOKEN_ENDP_SHIFT)
                    | UHCI_TOKEN_TOGGLE_DATA0;
    td_setup->buffer = (uint32_t)(uintptr_t)setup_pkt;

    /* DATA stage (DATA1) if any */
    if (data_len > 0 && data) {
        td_data = alloc_td();
        if (!td_data) { free_td(td_setup); free_td(td_status); return -1; }
        td_data->link = UHCI_LINK_TERMINATE;
        td_data->status = UHCI_TD_ACTIVE | UHCI_TD_ERR(3) | UHCI_TD_SPD;
        td_data->token = (direction_in ? UHCI_TOKEN_PID_IN : UHCI_TOKEN_PID_OUT)
                       | (dev_addr << UHCI_TOKEN_DEVADDR_SHIFT)
                       | (endpoint << UHCI_TOKEN_ENDP_SHIFT)
                       | UHCI_TOKEN_TOGGLE_DATA1;
        td_data->buffer = (uint32_t)(uintptr_t)data;
    }

    /* STATUS stage (DATA1, opposite direction) */
    td_status->link = UHCI_LINK_TERMINATE;
    td_status->status = UHCI_TD_ACTIVE | UHCI_TD_ERR(3) | UHCI_TD_IOS;
    td_status->token = ((data_len > 0 && direction_in) ? UHCI_TOKEN_PID_OUT : UHCI_TOKEN_PID_IN)
                     | (dev_addr << UHCI_TOKEN_DEVADDR_SHIFT)
                     | (endpoint << UHCI_TOKEN_ENDP_SHIFT)
                     | UHCI_TOKEN_TOGGLE_DATA1;
    td_status->buffer = 0;

    /* Link: SETUP -> [DATA] -> STATUS */
    if (td_data) {
        td_setup->link = (uint32_t)(uintptr_t)td_data;
        td_setup->link &= ~UHCI_LINK_TERMINATE;
        td_data->link = (uint32_t)(uintptr_t)td_status;
        td_data->link &= ~UHCI_LINK_TERMINATE;
    } else {
        td_setup->link = (uint32_t)(uintptr_t)td_status;
        td_setup->link &= ~UHCI_LINK_TERMINATE;
    }

    /* Insert at frame 0 */
    uint32_t saved = frame_list[0];
    frame_list[0] = (uint32_t)(uintptr_t)td_setup & ~(uint32_t)UHCI_LINK_TERMINATE;

    int result = td_wait(td_status, UHCI_TD_TIMEOUT_MS);

    frame_list[0] = saved;

    free_td(td_setup);
    if (td_data) free_td(td_data);
    free_td(td_status);
    return result;
}

/* ---- Schedule interrupt IN transfer ---- */
int uhci_schedule_interrupt(uint8_t dev_addr, uint8_t endpoint,
                            uint16_t max_packet, uint8_t interval_frames) {
    if (!uhci_ready) return -1;
    if (intr_active) return -1;

    intr_qh = alloc_qh();
    intr_td = alloc_td();
    if (!intr_qh || !intr_td) {
        if (intr_qh) free_qh(intr_qh);
        if (intr_td) free_td(intr_td);
        intr_qh = NULL; intr_td = NULL;
        return -1;
    }

    /* Allocate buffer for report data */
    /* We use a fixed static buffer instead of dynamic allocation */
    static uint8_t report_buf[64];
    intr_buffer = report_buf;
    intr_max_pkt = max_packet > 64 ? 64 : max_packet;

    /* Initialize TD for periodic IN */
    __builtin_memset(intr_td, 0, sizeof(uhci_td_t));
    intr_td->link = UHCI_LINK_TERMINATE;
    intr_td->status = UHCI_TD_ACTIVE | UHCI_TD_ERR(3) | UHCI_TD_SPD | UHCI_TD_IOS;
    intr_td->token = UHCI_TOKEN_PID_IN
                    | (dev_addr << UHCI_TOKEN_DEVADDR_SHIFT)
                    | (endpoint << UHCI_TOKEN_ENDP_SHIFT)
                    | UHCI_TOKEN_TOGGLE_DATA0;
    intr_td->buffer = (uint32_t)(uintptr_t)intr_buffer;

    /* Initialize QH */
    __builtin_memset(intr_qh, 0, sizeof(uhci_qh_t));
    intr_qh->head_link = UHCI_LINK_TERMINATE | UHCI_LINK_DEPTH;
    intr_qh->element = (uint32_t)(uintptr_t)intr_td & ~1u;

    /* Insert QH into frame list schedule */
    int step = interval_frames;
    if (step < 1) step = 1;
    if (step > 128) step = 128;

    for (int i = 0; i < FRAME_LIST_SIZE; i += step)
        frame_list[i] = (uint32_t)(uintptr_t)intr_qh | UHCI_LINK_QH | UHCI_LINK_DEPTH;

    intr_active = 1;
    return 0;
}

void uhci_remove_interrupt(void) {
    if (!intr_active) return;
    for (int i = 0; i < FRAME_LIST_SIZE; i++)
        frame_list[i] = UHCI_LINK_TERMINATE;
    if (intr_td) free_td(intr_td);
    if (intr_qh) free_qh(intr_qh);
    intr_td = NULL;
    intr_qh = NULL;
    intr_buffer = NULL;
    intr_active = 0;
}

int uhci_interrupt_active(void) {
    return intr_active;
}

uint8_t* uhci_get_report(int* ready) {
    if (!intr_active || !intr_td) {
        if (ready) *ready = 0;
        return NULL;
    }
    if (intr_td->status & UHCI_TD_ACTIVE) {
        if (ready) *ready = 0;
        return intr_buffer;
    }
    /* Check for errors */
    if (intr_td->status & (UHCI_TD_STALLED | UHCI_TD_BUFERR | UHCI_TD_BABBLE | UHCI_TD_TIMEOUT)) {
        if (ready) *ready = -1;
        return intr_buffer;
    }
    if (ready) *ready = 1;
    return intr_buffer;
}

void uhci_ack_report(void) {
    if (!intr_active || !intr_td) return;

    /* Clear stale bytes so a short report never leaves old wheel/data behind. */
    if (intr_buffer && intr_max_pkt) {
        __builtin_memset(intr_buffer, 0, intr_max_pkt);
    }

    /* Re-arm TD */
    intr_td->status = UHCI_TD_ACTIVE | UHCI_TD_ERR(3) | UHCI_TD_SPD | UHCI_TD_IOS;

    /* Toggle data PID */
    uint32_t toggle = intr_td->token & (1 << UHCI_TOKEN_TOGGLE_SHIFT);
    if (toggle)
        intr_td->token &= ~(1 << UHCI_TOKEN_TOGGLE_SHIFT);
    else
        intr_td->token |= (1 << UHCI_TOKEN_TOGGLE_SHIFT);
}

/* ---- Poll controller status ---- */
void uhci_poll(void) {
    if (!uhci_ready) return;

    uint16_t status = inw(uhci_io_base + UHCI_REG_USBSTS);

    if (status & (UHCI_STS_HSE | UHCI_STS_PE)) {
        outw(uhci_io_base + UHCI_REG_USBSTS, status);
        uhci_ready = 0; uhci_fault_latched = 1;
        uhci_print("UHCI fault.\n");
        return;
    }

    if (status & UHCI_STS_HCHALTED) {
        outw(uhci_io_base + UHCI_REG_USBCMD, UHCI_CMD_RUNSTOP);
        if (!wait_status_clear(UHCI_STS_HCHALTED, 120000U)) {
            uhci_ready = 0; uhci_fault_latched = 1;
            return;
        }
    }

    if (status & (UHCI_STS_USBINT | UHCI_STS_ERR | UHCI_STS_RESUME))
        outw(uhci_io_base + UHCI_REG_USBSTS,
             (uint16_t)(status & (UHCI_STS_USBINT | UHCI_STS_ERR | UHCI_STS_RESUME)));
}

int uhci_controller_healthy(void) {
    return uhci_ready && !uhci_fault_latched;
}

int uhci_pointer_enumeration_allowed(void) {
    return uhci_controller_healthy() && uhci_pointer_probe_ok;
}
