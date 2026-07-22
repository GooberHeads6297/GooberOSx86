#include "ohci.h"
#include <stddef.h>
#include "../../io/io.h"
#include "../../timer/timer.h"

extern void print(const char* str);

static void ohci_serial(const char* s) {
    while (*s) { outb(0xE9, *s++); }
}
static void ohci_print(const char* s) {
    print(s);
    ohci_serial(s);
}

/* ---- MMIO register access ---- */
static volatile uint32_t* ohci_regs = NULL;
static int ohci_ready = 0;
static int ohci_fault_latched = 0;
static int ohci_ndp = 2; /* NumberDownstreamPorts from HcRhDescriptorA */

/*
 * Hard iteration ceiling for every hardware-polling busy-wait. Belt-and-
 * suspenders on top of the TSC deadline so no loop can spin forever even if
 * the clock stops. Won't truncate a legitimate sub-second poll.
 */
#define OHCI_SPIN_CEILING 4000000U

/*
 * Per-transfer timeout: 500 ms (was 30 s = 3000 ticks). timer_deadline_ms()
 * is the wall-clock bound; a single wedged transfer can no longer stall the
 * whole boot for the platform-watchdog window on real hardware.
 */
#define OHCI_TD_TIMEOUT_MS 500U

/* ---- Data structures (aligned) ---- */
static ohci_hcca_t ohci_hcca __attribute__((aligned(256)));
#define OHCI_ED_POOL_SIZE 8
#define OHCI_TD_POOL_SIZE 32
static ohci_ed_t ed_pool[OHCI_ED_POOL_SIZE] __attribute__((aligned(16)));
static ohci_td_t td_pool[OHCI_TD_POOL_SIZE] __attribute__((aligned(16)));
static int ed_used[OHCI_ED_POOL_SIZE];
static int td_used[OHCI_TD_POOL_SIZE];

/* ---- Interrupt transfer state ---- */
static ohci_ed_t*  intr_ed = NULL;
static ohci_td_t*  intr_td = NULL;
static uint8_t    intr_buf[64] __attribute__((aligned(8)));
static int         intr_active = 0;
static int         intr_toggle = 0;
static uint8_t     intr_sched_addr = 0;
static uint8_t     intr_sched_ep = 0;

/* ---- Helper: read/write MMIO ---- */
static inline uint32_t ohci_read(int reg) {
    return ohci_regs[reg / 4];
}
static inline void ohci_write(int reg, uint32_t val) {
    ohci_regs[reg / 4] = val;
}

/* ---- TD pool ---- */
static ohci_td_t* alloc_td(void) {
    for (int i = 0; i < OHCI_TD_POOL_SIZE; i++) {
        if (!td_used[i]) { td_used[i] = 1; return &td_pool[i]; }
    }
    return NULL;
}
static void free_td(ohci_td_t* td) {
    for (int i = 0; i < OHCI_TD_POOL_SIZE; i++) {
        if (&td_pool[i] == td) { td_used[i] = 0; return; }
    }
}

/* ---- ED pool ---- */
static ohci_ed_t* alloc_ed(void) {
    for (int i = 0; i < OHCI_ED_POOL_SIZE; i++) {
        if (!ed_used[i]) { ed_used[i] = 1; return &ed_pool[i]; }
    }
    return NULL;
}
static void free_ed(ohci_ed_t* ed) {
    for (int i = 0; i < OHCI_ED_POOL_SIZE; i++) {
        if (&ed_pool[i] == ed) { ed_used[i] = 0; return; }
    }
}

/* ---- Wait for TD completion ---- */
static int td_wait(ohci_td_t* td, uint32_t timeout_ms) {
    uint64_t deadline = timer_deadline_ms(timeout_ms);
    uint32_t guard = OHCI_SPIN_CEILING;
    while (!timer_deadline_expired(deadline)) {
        uint32_t info = td->hwINFO;
        uint8_t cc = (info >> OHCI_TD_CC_SHIFT) & 0x0F;
        if (cc != 0x0F) {
            if (cc == OHCI_TD_CC_NOERROR) return 0;
            return -1;
        }
        if (--guard == 0) break;
    }
    return -2;
}

/* ---- Initialize ---- */
int ohci_init(const usb_pci_controller_t* controller) {
    if (!controller) return 0;
    ohci_ready = 0;
    ohci_fault_latched = 0;
    ohci_ndp = 2;
    intr_active = 0;
    intr_ed = NULL;
    intr_td = NULL;

    /* Map BAR0 (memory-mapped) */
    uint32_t bar0 = controller->bar0;
    if ((bar0 & 0x1) != 0) {
        ohci_print("OHCI BAR0 is IO-mapped (expected memory); "
                   "falling through to other controllers.\n");
        return 0;
    }
    if ((bar0 & 0xFFFFFFF0) == 0 || bar0 == 0xFFFFFFFFu) {
        ohci_print("OHCI: firmware did NOT program this BAR; "
                   "falling through to other controllers.\n");
        return 0;
    }
    ohci_regs = (volatile uint32_t*)(uintptr_t)(bar0 & 0xFFFFFFF0);
    if (!ohci_regs) {
        ohci_print("OHCI invalid BAR0.\n");
        return 0;
    }
    /* Capability sanity: HcRevision should be 0x10 or similar (reads
     * 0xFFFFFFFF if the controller is not actually present). */
    {
        uint32_t rev_probe = ohci_regs[OHCI_HcRevision / 4];
        if (rev_probe == 0xFFFFFFFFu) {
            ohci_print("OHCI: HcRevision reads 0xFFFFFFFF -- "
                       "controller absent.\n");
            return 0;
        }
    }

    /* Enable PCI memory space and bus mastering */
    uint16_t cmd = pci_read_config_word(controller->bus, controller->slot,
                                        controller->func, 0x04);
    pci_write_config_word(controller->bus, controller->slot,
                          controller->func, 0x04, cmd | 0x06);

    /*
     * OHCI ownership / SMM handoff (OHCI spec 5.1.1.3.3).
     *
     * HcControl.InterruptRouting (IR) set => the SMM legacy driver currently
     * owns the controller. Yanking it out from under SMM (or leaving SMM
     * routing on) is the OHCI analogue of the UHCI SMI storm. Request an
     * ownership change via HcCommandStatus.OCR and wait (bounded) for IR to
     * clear; if SMM never releases, force IR off ourselves so port I/O no
     * longer routes to SMM. Done before reset and before going OPERATIONAL.
     * In VMs IR is clear, so this is a no-op.
     */
    {
        uint32_t hc = ohci_read(OHCI_HcControl);
        if (hc & OHCI_CTL_IR) {
            ohci_write(OHCI_HcCommandStatus, OHCI_CMD_OCR);
            uint64_t owner_dl = timer_deadline_ms(1000);
            uint32_t owner_guard = OHCI_SPIN_CEILING;
            while (ohci_read(OHCI_HcControl) & OHCI_CTL_IR) {
                if (timer_deadline_expired(owner_dl)) break;
                if (--owner_guard == 0) break;
            }
            if (ohci_read(OHCI_HcControl) & OHCI_CTL_IR) {
                /* SMM did not release: disable SMM routing ourselves. */
                ohci_write(OHCI_HcControl,
                           ohci_read(OHCI_HcControl) & ~OHCI_CTL_IR);
                ohci_print("OHCI: forced SMM ownership release.\n");
            } else {
                ohci_print("OHCI: SMM handoff complete.\n");
            }
        }
    }

    /* Reset HC */
    ohci_write(OHCI_HcCommandStatus, OHCI_CMD_HCR);
    uint64_t deadline = timer_deadline_ms(100);
    {
        uint32_t guard = OHCI_SPIN_CEILING;
        while ((ohci_read(OHCI_HcCommandStatus) & OHCI_CMD_HCR)) {
            if (timer_deadline_expired(deadline)) break;
            if (--guard == 0) break;
        }
    }
    if (ohci_read(OHCI_HcCommandStatus) & OHCI_CMD_HCR) {
        ohci_print("OHCI reset timeout.\n");
        return 0;
    }

    /* Set HCCA */
    ohci_write(OHCI_HcHCCA, (uint32_t)(uintptr_t)&ohci_hcca);

    /* Clear interrupt status */
    ohci_write(OHCI_HcInterruptStatus, ohci_read(OHCI_HcInterruptStatus));

    /* Set frame interval (default: 12000) */
    ohci_write(OHCI_HcFmInterval, 0x2EEF2EDF);  /* Typical for 12MHz USB */

    /* Set Control Head to 0 (empty) */
    ohci_write(OHCI_HcControlHeadED, 0);

    /* Clear pool */
    for (int i = 0; i < OHCI_ED_POOL_SIZE; i++) { ed_used[i] = 0; __builtin_memset(&ed_pool[i], 0, sizeof(ohci_ed_t)); }
    for (int i = 0; i < OHCI_TD_POOL_SIZE; i++) { td_used[i] = 0; __builtin_memset(&td_pool[i], 0, sizeof(ohci_td_t)); }

    /* Enable periodic list, control list, and set operational state */
    ohci_write(OHCI_HcControl,
               OHCI_CTL_HCFS_OPER | OHCI_CTL_PLE | OHCI_CTL_CLE | OHCI_CTL_BLE);

    /* Verify operational */
    uint32_t ctl = ohci_read(OHCI_HcControl);
    if ((ctl & OHCI_CTL_HCFS) != OHCI_CTL_HCFS_OPER) {
        ohci_print("OHCI failed to go operational.\n");
        return 0;
    }

    /* Set port power (global, then every root port). */
    ohci_write(OHCI_HcRhStatus, OHCI_RHS_SGP);
    timer_busy_wait_ms(20);
    {
        uint32_t rhda = ohci_read(OHCI_HcRhDescriptorA);
        int ndp = (int)(rhda & 0xFF);
        if (ndp < 1) ndp = 1;
        if (ndp > 15) ndp = 15;
        ohci_ndp = ndp;
        for (int p = 0; p < ohci_ndp; p++) {
            uint32_t reg = OHCI_HcRhPortStatus1 + p * 4;
            uint32_t ps = ohci_read(reg);
            if (ps == 0xFFFFFFFFu) continue;
            if (!(ps & OHCI_PORT_PPS))
                ohci_write(reg, OHCI_PORT_PPS);
        }
        timer_busy_wait_ms(50);
    }

    ohci_ready = 1;

    /* Debug: test register reads */
    {
        uint32_t rev = ohci_read(OHCI_HcRevision);
        uint32_t fm = ohci_read(OHCI_HcFmNumber);
        uint32_t rhda = ohci_read(OHCI_HcRhDescriptorA);
        uint32_t ps1 = ohci_read(OHCI_HcRhPortStatus1);
        ohci_serial("rev=");
        { uint32_t v=rev; for(int i=0;i<8;i++){int d=(v>>(28-i*4))&0xF;outb(0xE9,d<10?'0'+d:'A'+d-10);} }
        ohci_serial(" fm=");
        { uint32_t v=fm; for(int i=0;i<8;i++){int d=(v>>(28-i*4))&0xF;outb(0xE9,d<10?'0'+d:'A'+d-10);} }
        ohci_serial(" rhda=");
        { uint32_t v=rhda; for(int i=0;i<8;i++){int d=(v>>(28-i*4))&0xF;outb(0xE9,d<10?'0'+d:'A'+d-10);} }
        ohci_serial(" ps1=");
        { uint32_t v=ps1; for(int i=0;i<8;i++){int d=(v>>(28-i*4))&0xF;outb(0xE9,d<10?'0'+d:'A'+d-10);} }
        ohci_serial("\n");
    }
    ohci_print("OHCI initialized.\n");
    return 1;
}

int ohci_port_count(void) {
    return ohci_ready ? ohci_ndp : 0;
}

/* ---- Port operations ---- */
static int ohci_port_valid(int port) {
    if (!ohci_ready || port < 0 || port >= ohci_ndp) return 0;
    return 1;
}

int ohci_port_connected(int port) {
    uint32_t v;
    if (!ohci_port_valid(port)) return 0;
    v = ohci_read(OHCI_HcRhPortStatus1 + port * 4);
    if (v == 0xFFFFFFFFu) return 0;
    return (v & OHCI_PORT_CCS) != 0;
}

int ohci_port_low_speed(int port) {
    uint32_t v;
    if (!ohci_port_valid(port)) return 0;
    v = ohci_read(OHCI_HcRhPortStatus1 + port * 4);
    if (v == 0xFFFFFFFFu) return 0;
    return (v & OHCI_PORT_LSDA) != 0;
}

void ohci_port_reset(int port) {
    uint32_t reg;
    uint32_t v;
    uint64_t deadline;
    uint32_t guard;

    if (!ohci_port_valid(port)) return;
    reg = OHCI_HcRhPortStatus1 + port * 4;
    v = ohci_read(reg);
    if (v == 0xFFFFFFFFu) return;

    /* Power the port first if needed. */
    if (!(v & OHCI_PORT_PPS)) {
        ohci_write(reg, OHCI_PORT_PPS);
        timer_busy_wait_ms(20);
        v = ohci_read(reg);
    }
    if (!(v & OHCI_PORT_CCS)) return;

    /* Assert reset (write-1-to-set PRS). */
    ohci_write(reg, OHCI_PORT_PRS);
    deadline = timer_deadline_ms(100);
    guard = OHCI_SPIN_CEILING;
    while (!timer_deadline_expired(deadline)) {
        v = ohci_read(reg);
        if (v == 0xFFFFFFFFu) return;
        /* PRSC set means reset completed; PRS clears when done. */
        if ((v & OHCI_PORT_PRSC) || !(v & OHCI_PORT_PRS)) break;
        if (--guard == 0) break;
    }

    v = ohci_read(reg);
    if (v == 0xFFFFFFFFu) return;
    /* Clear PRSC (RW1C) and enable the port. */
    ohci_write(reg, OHCI_PORT_PRSC | OHCI_PORT_PES);
    timer_busy_wait_ms(10);
}

/*
 * Hot-plug primitives. OHCI HcRhPortStatusN has CSC at bit 16 (RW1C).
 * Writing 1 clears the latch; the lower 8 bits (CCS/PES/...) are RO/RW
 * but write-1 to those does NOT change them, so the simple `write CSC`
 * pattern is safe.
 */
int ohci_port_change_pending(int port) {
    if (!ohci_ready || ohci_fault_latched) return 0;
    if (!ohci_port_valid(port)) return 0;
    uint32_t v = ohci_read(OHCI_HcRhPortStatus1 + port * 4);
    if (v == 0xFFFFFFFFu) return 0;
    return (v & OHCI_PORT_CSC) != 0;
}

void ohci_port_change_ack(int port) {
    if (!ohci_ready || ohci_fault_latched) return;
    if (!ohci_port_valid(port)) return;
    /* OHCI: writing 1 to CSC clears the latch. The R/WC encoding of the
     * other status-change bits means writing zero to them leaves them as
     * they are; we only want to clear CSC here. */
    ohci_write(OHCI_HcRhPortStatus1 + port * 4, OHCI_PORT_CSC);
}

/* ---- Control transfer (synchronous) ---- */
int ohci_control_transfer(uint8_t dev_addr, uint8_t endpoint,
                          uint8_t* setup_pkt, uint8_t* data, uint16_t data_len,
                          int direction_in) {
    if (!ohci_ready) return -1;

    ohci_ed_t* ed = alloc_ed();
    ohci_td_t* td_setup = alloc_td();
    ohci_td_t* td_status = alloc_td();
    ohci_td_t* td_data = NULL;
    if (!ed || !td_setup || !td_status) {
        if (ed) free_ed(ed);
        if (td_setup) free_td(td_setup);
        if (td_status) free_td(td_status);
        return -1;
    }

    /* ---- SETUP TD ---- */
    __builtin_memset(td_setup, 0, sizeof(ohci_td_t));
    td_setup->hwINFO = OHCI_TD_DP_SETUP | OHCI_TD_T_DATA0 | OHCI_TD_DI_NODELAY;
    td_setup->hwCBP = (uint32_t)(uintptr_t)setup_pkt;
    td_setup->hwBE = (uint32_t)(uintptr_t)(setup_pkt + 8 - 1);  /* inclusive end address */

    /* ---- DATA TD (if any) ---- */
    ohci_td_t* td_prev = td_setup;
    if (data_len > 0 && data) {
        td_data = alloc_td();
        if (!td_data) {
            free_ed(ed); free_td(td_setup); free_td(td_status);
            return -1;
        }
        __builtin_memset(td_data, 0, sizeof(ohci_td_t));
        td_data->hwINFO = (direction_in ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT)
                        | OHCI_TD_T_DATA1 | OHCI_TD_DI_NODELAY | OHCI_TD_R;
        td_data->hwCBP = (uint32_t)(uintptr_t)data;
        td_data->hwBE = (uint32_t)(uintptr_t)(data + data_len - 1);
        td_prev->hwNextTD = (uint32_t)(uintptr_t)td_data;
        td_prev = td_data;
    }

    /* ---- STATUS TD ---- */
    __builtin_memset(td_status, 0, sizeof(ohci_td_t));
    td_status->hwINFO = ((data_len > 0 && direction_in) ? OHCI_TD_DP_OUT : OHCI_TD_DP_IN)
                      | OHCI_TD_T_DATA1 | OHCI_TD_DI_NODELAY;
    uint32_t dummy_status_buf;
    td_status->hwCBP = (uint32_t)(uintptr_t)&dummy_status_buf;
    td_status->hwBE = (uint32_t)(uintptr_t)&dummy_status_buf;
    td_prev->hwNextTD = (uint32_t)(uintptr_t)td_status;

    /* ---- Set up ED ---- */
    __builtin_memset(ed, 0, sizeof(ohci_ed_t));
    ed->hwINFO = OHCI_ED_FA(dev_addr) | OHCI_ED_EN(endpoint) | OHCI_ED_D(0);
    ed->hwHeadP = (uint32_t)(uintptr_t)td_setup;
    ed->hwTailP = (uint32_t)(uintptr_t)td_status + 1;

    /* ---- Link ED to control list ---- */
    /* Read current ControlHeadED */
    uint32_t old_head = ohci_read(OHCI_HcControlHeadED);
    if (old_head == 0) {
        ohci_write(OHCI_HcControlHeadED, (uint32_t)(uintptr_t)ed);
    } else {
        /* Walk to end and append */
        ohci_ed_t* walk = (ohci_ed_t*)(uintptr_t)old_head;
        while (walk->hwNextED) {
            walk = (ohci_ed_t*)(uintptr_t)walk->hwNextED;
        }
        walk->hwNextED = (uint32_t)(uintptr_t)ed;
    }

    /* Enable control list and signal filled */
    uint32_t ctl = ohci_read(OHCI_HcControl);
    ohci_write(OHCI_HcControl, ctl | OHCI_CTL_CLE);
    ohci_write(OHCI_HcCommandStatus, OHCI_CMD_CLF);

    /* Wait for completion */
    int result = td_wait(td_status, 3000);

    /* Remove ED from list */
    uint32_t head = ohci_read(OHCI_HcControlHeadED);
    if (head == (uint32_t)(uintptr_t)ed) {
        ohci_write(OHCI_HcControlHeadED, ed->hwNextED);
    } else if (head != 0) {
        ohci_ed_t* prev = (ohci_ed_t*)(uintptr_t)head;
        ohci_ed_t* cur = (ohci_ed_t*)(uintptr_t)prev->hwNextED;
        while (cur && cur != ed) {
            prev = cur;
            cur = (ohci_ed_t*)(uintptr_t)cur->hwNextED;
        }
        if (cur == ed) {
            prev->hwNextED = ed->hwNextED;
        }
    }
    if (ohci_read(OHCI_HcControlHeadED) == 0) {
        ohci_write(OHCI_HcControl, ohci_read(OHCI_HcControl) & ~OHCI_CTL_CLE);
    }

    /* Cleanup */
    free_ed(ed);
    free_td(td_setup);
    if (td_data) free_td(td_data);
    free_td(td_status);

    return result;
}

/* ---- Schedule interrupt IN transfer ---- */
int ohci_schedule_interrupt(uint8_t dev_addr, uint8_t endpoint,
                            uint16_t max_packet, uint8_t interval_frames) {
    if (!ohci_ready) return -1;
    if (intr_active) return -1;

    /* Allocate ED and TD */
    intr_ed = alloc_ed();
    intr_td = alloc_td();
    if (!intr_ed || !intr_td) {
        if (intr_ed) free_ed(intr_ed);
        if (intr_td) free_td(intr_td);
        intr_ed = NULL; intr_td = NULL;
        return -1;
    }

    intr_sched_addr = dev_addr;
    intr_sched_ep = endpoint;
    intr_toggle = 0;

    /* Set up interrupt TD */
    __builtin_memset(intr_td, 0, sizeof(ohci_td_t));
    intr_td->hwINFO = OHCI_TD_DP_IN | OHCI_TD_T_TOGGLE | OHCI_TD_DI_NODELAY | OHCI_TD_R;
    intr_td->hwCBP = (uint32_t)(uintptr_t)intr_buf;
    intr_td->hwBE = (uint32_t)(uintptr_t)(intr_buf + max_packet - 1);

    /* Set up ED */
    __builtin_memset(intr_ed, 0, sizeof(ohci_ed_t));
    intr_ed->hwINFO = OHCI_ED_FA(dev_addr) | OHCI_ED_EN(endpoint) | OHCI_ED_D(2) | OHCI_ED_S(0);
    intr_ed->hwHeadP = (uint32_t)(uintptr_t)intr_td;
    intr_ed->hwTailP = (uint32_t)(uintptr_t)(intr_td) + 1;

    /* Add to periodic list (interrupt table entry 0, runs every 32 frames) */
    int slot = interval_frames < 32 ? interval_frames : 31;
    if (slot > 31) slot = 31;
    if (slot < 0) slot = 0;
    ohci_hcca.hcca_interrupt_table[slot] = (uint32_t)(uintptr_t)intr_ed;

    /* Make sure periodic list is enabled */
    ohci_write(OHCI_HcControl, ohci_read(OHCI_HcControl) | OHCI_CTL_PLE);

    intr_active = 1;
    return 0;
}

void ohci_remove_interrupt(void) {
    if (!intr_active) return;
    for (int i = 0; i < 32; i++) {
        if (ohci_hcca.hcca_interrupt_table[i] == (uint32_t)(uintptr_t)intr_ed)
            ohci_hcca.hcca_interrupt_table[i] = 0;
    }
    if (intr_td) free_td(intr_td);
    if (intr_ed) free_ed(intr_ed);
    intr_td = NULL; intr_ed = NULL;
    intr_active = 0;
}

int ohci_interrupt_active(void) {
    return intr_active;
}

uint8_t* ohci_get_report(int* ready) {
    if (!intr_active || !intr_td) {
        if (ready) *ready = 0;
        return NULL;
    }

    uint32_t info = intr_td->hwINFO;
    uint8_t cc = (info >> OHCI_TD_CC_SHIFT) & 0x0F;

    if (cc == 0x0F) {
        /* Still active/not yet completed */
        if (ready) *ready = 0;
        return intr_buf;
    }

    if (cc != OHCI_TD_CC_NOERROR) {
        if (ready) *ready = -1;
        return intr_buf;
    }

    /* Check if data was actually transferred */
    if (intr_td->hwCBP == (uint32_t)(uintptr_t)(intr_buf + 64 - 1)) {
        /* No data transferred (CBP not advanced) */
        if (ready) *ready = 0;
        return intr_buf;
    }

    if (ready) *ready = 1;
    return intr_buf;
}

void ohci_ack_report(void) {
    if (!intr_active || !intr_td) return;

    /* Clear stale bytes so a short report never leaves old wheel/data behind. */
    for (int i = 0; i < 8; i++) intr_buf[i] = 0;

    /* Re-arm: reset CBP to buffer start, clear CC */
    uint32_t info = OHCI_TD_DP_IN | OHCI_TD_T_TOGGLE | OHCI_TD_DI_NODELAY | OHCI_TD_R
                  | (0x0F << OHCI_TD_CC_SHIFT);  /* Active */
    intr_td->hwINFO = info;
    intr_td->hwCBP = (uint32_t)(uintptr_t)intr_buf;

    /* Re-link ED */
    intr_ed->hwHeadP = (uint32_t)(uintptr_t)intr_td;
    intr_ed->hwTailP = (uint32_t)(uintptr_t)intr_td + 1;
}

/* ---- Poll ---- */
void ohci_poll(void) {
    if (!ohci_ready) return;

    uint32_t intr_status = ohci_read(OHCI_HcInterruptStatus);

    if (intr_status & OHCI_INTR_UE) {
        ohci_write(OHCI_HcInterruptStatus, intr_status);
        ohci_ready = 0; ohci_fault_latched = 1;
        ohci_print("OHCI unrecoverable error.\n");
        return;
    }

    /* Clear handled interrupts */
    if (intr_status) {
        ohci_write(OHCI_HcInterruptStatus, intr_status);
    }
}

int ohci_controller_healthy(void) {
    return ohci_ready && !ohci_fault_latched;
}
