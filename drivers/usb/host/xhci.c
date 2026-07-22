/*
 * xHCI host controller driver (narrow boot mouse path).
 *
 * Scope: enough of the xHCI 1.1 spec to enumerate a single HID boot-protocol
 * USB mouse and read interrupt-IN reports. Multi-device, hubs, streams,
 * isochronous transfers, MSI(-X), and power management are intentionally out
 * of scope until the rest of the USB stack catches up.
 */

#include "xhci.h"
#include "../../io/io.h"
#include "../../timer/timer.h"

extern void print(const char* str);

/* ---- Register layout ---- */
#define XHCI_USBCMD       0x00
#define XHCI_USBSTS       0x04
#define XHCI_PAGESIZE     0x08
#define XHCI_DNCTRL       0x14
#define XHCI_CRCR         0x18
#define XHCI_DCBAAP       0x30
#define XHCI_CONFIG       0x38
#define XHCI_PORTSC_BASE  0x400

#define XHCI_CMD_RUN      (1U << 0)
#define XHCI_CMD_RESET    (1U << 1)
#define XHCI_CMD_INTE     (1U << 2)
#define XHCI_STS_HCH      (1U << 0)
#define XHCI_STS_HSE      (1U << 2)
#define XHCI_STS_EINT     (1U << 3)
#define XHCI_STS_PCD      (1U << 4)
#define XHCI_STS_CNR      (1U << 11)

#define XHCI_PORT_CCS     (1U << 0)
#define XHCI_PORT_PED     (1U << 1)
#define XHCI_PORT_PR      (1U << 4)
#define XHCI_PORT_PP      (1U << 9)
#define XHCI_PORT_PRC     (1U << 21)
#define XHCI_PORT_CSC     (1U << 17)

/* Interrupter 0 register offsets, relative to runtime base + 0x20. */
#define IR_IMAN   0x00
#define IR_IMOD   0x04
#define IR_ERSTSZ 0x08
#define IR_ERSTBA 0x10
#define IR_ERDP   0x18

/* TRB types (control field bits 15:10). */
#define TRB_NORMAL          1U
#define TRB_SETUP_STAGE     2U
#define TRB_DATA_STAGE      3U
#define TRB_STATUS_STAGE    4U
#define TRB_LINK            6U
#define TRB_NOOP_TRANSFER   8U
#define TRB_ENABLE_SLOT     9U
#define TRB_ADDRESS_DEVICE  11U
#define TRB_CONFIGURE_EP    12U
#define TRB_EVALUATE_CTX    13U
#define TRB_NOOP_CMD        23U
#define TRB_EV_TRANSFER     32U
#define TRB_EV_CMD_COMPLETE 33U
#define TRB_EV_PORT_STATUS  34U

#define TRB_TYPE(x)         (((uint32_t)(x)) << 10)
#define TRB_GET_TYPE(c)     (((c) >> 10) & 0x3FU)
#define TRB_GET_CC(s)       (((s) >> 24) & 0xFFU)
#define TRB_SLOT(c)         (((c) >> 24) & 0xFFU)
#define TRB_EP(c)           (((c) >> 16) & 0x1FU)

#define TRB_CYCLE           (1U << 0)
#define TRB_ENT             (1U << 1)
#define TRB_ISP             (1U << 2)
#define TRB_NS              (1U << 3)
#define TRB_CHAIN           (1U << 4)
#define TRB_IOC             (1U << 5)
#define TRB_IDT             (1U << 6)
#define TRB_BSR             (1U << 9)

#define TRB_TRT_NO_DATA     (0U << 16)
#define TRB_TRT_OUT         (2U << 16)
#define TRB_TRT_IN          (3U << 16)
#define TRB_DIR_IN          (1U << 16)

/* Endpoint context types. */
#define EP_TYPE_INVALID     0
#define EP_TYPE_ISOCH_OUT   1
#define EP_TYPE_BULK_OUT    2
#define EP_TYPE_INTERRUPT_OUT 3
#define EP_TYPE_CONTROL     4
#define EP_TYPE_ISOCH_IN    5
#define EP_TYPE_BULK_IN     6
#define EP_TYPE_INTERRUPT_IN 7

/* Ring sizes — keep small so allocation stays within static memory. */
#define CMD_RING_SIZE  64
#define EVT_RING_SIZE  64
#define EP_RING_SIZE   64
#define MAX_SLOTS_USED 4
#define XHCI_MAX_CONTEXTS 32

typedef struct __attribute__((packed, aligned(16))) {
    uint32_t param_low;
    uint32_t param_high;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

typedef struct __attribute__((packed, aligned(64))) {
    uint64_t base;
    uint32_t size;
    uint32_t reserved;
} xhci_erst_entry_t;

/* Slot/endpoint contexts. We size at 64 bytes so we match controllers with
 * CSZ=1; for CSZ=0 controllers we'll still write at 0/+64 offsets — the
 * controller stride will be 32 bytes, but we'll route them through the
 * correct field offsets by storing the stride at init. */
typedef struct __attribute__((packed, aligned(64))) {
    uint32_t words[16];
} xhci_ctx_block_t;

/* ---- Static state ---- */
static volatile uint8_t* xhci_cap = 0;
static volatile uint8_t* xhci_op = 0;
static volatile uint8_t* xhci_doorbell = 0;
static volatile uint8_t* xhci_runtime = 0;
static uint32_t xhci_ports = 0;
static uint32_t xhci_max_slots = 0;
static uint32_t xhci_context_size = 32; /* 32 or 64 */
static int xhci_ready = 0;
static int xhci_fault = 0;

/*
 * Hard iteration ceiling for every hardware-polling busy-wait. Belt-and-
 * suspenders on top of the TSC deadline so no loop can spin forever even if
 * the clock stops. Won't truncate a legitimate sub-second poll.
 */
#define XHCI_SPIN_CEILING 4000000U

/* Pre-allocated rings & arrays. Aligned to 64 to satisfy xHCI spec. */
static uint64_t xhci_dcbaa[256] __attribute__((aligned(64)));
static xhci_trb_t cmd_ring[CMD_RING_SIZE]   __attribute__((aligned(64)));
static xhci_trb_t evt_ring[EVT_RING_SIZE]   __attribute__((aligned(64)));
static xhci_erst_entry_t erst[1]            __attribute__((aligned(64)));

/*
 * Per-device state (single boot mouse).
 *
 * xHCI contexts are indexed by Device Context Index (DCI): slot=0, EP0=1,
 * EP1 OUT=2, EP1 IN=3, ... EP15 IN=31. The input context also has an Input
 * Control Context before the device contexts, so it needs 33 entries total.
 *
 * Earlier builds only allocated 3 entries here. Configuring a normal boot
 * mouse interrupt endpoint (usually EP1 IN, DCI=3) then wrote past the end of
 * input_ctx/device_ctx and corrupted adjacent kernel memory, which is a very
 * plausible source of #GP / vector 13 on bare metal after USB enumeration.
 */
static xhci_ctx_block_t input_ctx[XHCI_MAX_CONTEXTS + 1] __attribute__((aligned(64)));
static xhci_ctx_block_t device_ctx[XHCI_MAX_CONTEXTS]    __attribute__((aligned(64)));
static xhci_trb_t ep0_ring[EP_RING_SIZE]  __attribute__((aligned(64)));
static xhci_trb_t epi_ring[EP_RING_SIZE]  __attribute__((aligned(64)));
static uint8_t   xfer_data[256]          __attribute__((aligned(64)));
static uint8_t   intr_buf[64]            __attribute__((aligned(64)));

static uint32_t cmd_idx = 0, cmd_cycle = 1;
static uint32_t evt_idx = 0, evt_cycle = 1;
static uint32_t ep0_idx = 0, ep0_cycle = 1;
static uint32_t epi_idx = 0, epi_cycle = 1;

static int      slot_id = 0;
static int      cur_port = -1;
static uint8_t  usb_addr = 0;
static int      intr_active = 0;
static int      intr_pending = 0;
static int      intr_failed = 0;

/* ---- Helpers ---- */
static void xhci_serial(const char* s) { while (*s) outb(0xE9, *s++); }
static void xhci_print(const char* s) { print(s); xhci_serial(s); }

/* Print a 32-bit value in 0xHHHHHHHH form to both panel and serial. */
static void xhci_print_hex32(uint32_t v) {
    char buf[12];
    const char* hex = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) buf[2 + i] = hex[(v >> ((7 - i) * 4)) & 0xF];
    buf[10] = '\n'; buf[11] = '\0';
    xhci_print(buf);
}

static inline uint32_t xhci_read32(uint32_t reg) {
    return *(volatile uint32_t*)(xhci_op + reg);
}
static inline void xhci_write32(uint32_t reg, uint32_t value) {
    *(volatile uint32_t*)(xhci_op + reg) = value;
}
static inline void xhci_write64(uint32_t reg, uint64_t value) {
    *(volatile uint32_t*)(xhci_op + reg)       = (uint32_t)value;
    *(volatile uint32_t*)(xhci_op + reg + 4)   = (uint32_t)(value >> 32);
}

static inline uint32_t rt_read32(uint32_t reg) {
    return *(volatile uint32_t*)(xhci_runtime + reg);
}
static inline void rt_write32(uint32_t reg, uint32_t value) {
    *(volatile uint32_t*)(xhci_runtime + reg) = value;
}
static inline void rt_write64(uint32_t reg, uint64_t value) {
    *(volatile uint32_t*)(xhci_runtime + reg)     = (uint32_t)value;
    *(volatile uint32_t*)(xhci_runtime + reg + 4) = (uint32_t)(value >> 32);
}

/* timeout_ticks is in 100 Hz ticks (10 ms each); converted to the
 * IRQ-independent TSC deadline internally, with an absolute spin ceiling. */
static int xhci_wait_clear(uint32_t reg, uint32_t mask, uint32_t timeout_ticks) {
    uint64_t deadline = timer_deadline_ms(timeout_ticks * 10);
    uint32_t guard = XHCI_SPIN_CEILING;
    while (!timer_deadline_expired(deadline)) {
        if ((xhci_read32(reg) & mask) == 0) return 1;
        if (--guard == 0) break;
    }
    return 0;
}

static void xhci_memzero(void* p, uint32_t bytes) {
    uint8_t* b = (uint8_t*)p;
    for (uint32_t i = 0; i < bytes; i++) b[i] = 0;
}

/* `ticks` is in 100 Hz ticks (10 ms each). Bounded fixed delay off the TSC. */
static void short_delay(uint32_t ticks) {
    timer_busy_wait_ms(ticks * 10);
}

/*
 * Hardware context addressing.
 * We allocate space for 64-byte context blocks. When CSZ=0 the controller
 * uses 32-byte contexts, so when computing the address of the n-th context
 * the stride is xhci_context_size. We always start contexts at offsets
 * that match the runtime stride, so storage is just one xhci_ctx_block_t[].
 */
static uint8_t* ctx_at(xhci_ctx_block_t* base, int idx) {
    return ((uint8_t*)base) + idx * xhci_context_size;
}

/* ---- Ring management ---- */
static xhci_trb_t* enqueue_cmd(uint32_t plo, uint32_t phi, uint32_t status, uint32_t control) {
    xhci_trb_t* trb = &cmd_ring[cmd_idx];
    trb->param_low  = plo;
    trb->param_high = phi;
    trb->status     = status;
    trb->control    = (control & ~TRB_CYCLE) | (cmd_cycle ? TRB_CYCLE : 0);

    cmd_idx++;
    if (cmd_idx >= CMD_RING_SIZE - 1) {
        /* Insert link TRB pointing back to start, toggle cycle on wrap. */
        xhci_trb_t* link = &cmd_ring[CMD_RING_SIZE - 1];
        link->param_low  = (uint32_t)(uintptr_t)cmd_ring;
        link->param_high = 0;
        link->status     = 0;
        link->control    = TRB_TYPE(TRB_LINK) | TRB_ENT
                         | (cmd_cycle ? TRB_CYCLE : 0);
        cmd_idx = 0;
        cmd_cycle ^= 1;
    }
    /* Ring command doorbell (DB 0). */
    *(volatile uint32_t*)xhci_doorbell = 0;
    return trb;
}

/*
 * IMPORTANT: timer_ticks() is at 100 Hz (one tick = 10 ms). All "timeout"
 * arguments throughout this driver are in TICKS, not in milliseconds. The
 * parameter name is left as `timeout_ms` only for historical reasons; the
 * caller-side numeric constants below have all been chosen with 10 ms/tick
 * in mind. xhci_ms_to_ticks() exists so future code can convert milliseconds
 * to tick budgets without re-deriving the constant.
 */
static inline uint32_t xhci_ms_to_ticks(uint32_t ms) {
    /* 100 Hz timer → ceil(ms / 10). */
    uint32_t t = (ms + 9) / 10;
    return t ? t : 1;
}

static int wait_event(uint8_t want_type, uint32_t timeout_ticks,
                      xhci_trb_t* out) {
    uint64_t deadline = timer_deadline_ms(timeout_ticks * 10);
    uint32_t guard = XHCI_SPIN_CEILING;
    while (!timer_deadline_expired(deadline)) {
        if (--guard == 0) break;
        xhci_trb_t* ev = &evt_ring[evt_idx];
        uint32_t c = ev->control;
        if ((c & TRB_CYCLE) != (uint32_t)evt_cycle) continue;
        uint8_t type = TRB_GET_TYPE(c);
        if (want_type == 0 || type == want_type) {
            if (out) *out = *ev;
            evt_idx++;
            if (evt_idx >= EVT_RING_SIZE) {
                evt_idx = 0;
                evt_cycle ^= 1;
            }
            /* Update ERDP so the controller knows we consumed events. */
            rt_write64(0x20 + IR_ERDP,
                       ((uint64_t)(uintptr_t)&evt_ring[evt_idx]) | (1ULL << 3));
            return type;
        }
        /* Wrong type, still consume so we don't get stuck. */
        evt_idx++;
        if (evt_idx >= EVT_RING_SIZE) {
            evt_idx = 0;
            evt_cycle ^= 1;
        }
        rt_write64(0x20 + IR_ERDP,
                   ((uint64_t)(uintptr_t)&evt_ring[evt_idx]) | (1ULL << 3));
    }
    return -1;
}

/* Drain pending events (called from poll). Returns the last transfer event
 * type if one occurred. */
static int drain_events(void) {
    int saw_transfer = 0;
    for (int safety = 0; safety < EVT_RING_SIZE; safety++) {
        xhci_trb_t* ev = &evt_ring[evt_idx];
        if ((ev->control & TRB_CYCLE) != (uint32_t)evt_cycle) break;
        uint8_t type = TRB_GET_TYPE(ev->control);
        if (type == TRB_EV_TRANSFER) {
            uint8_t cc = TRB_GET_CC(ev->status);
            if (cc == 1 || cc == 13 /* Short */) {
                intr_pending = 1;
            } else {
                intr_failed = 1;
            }
            saw_transfer = 1;
        }
        evt_idx++;
        if (evt_idx >= EVT_RING_SIZE) {
            evt_idx = 0;
            evt_cycle ^= 1;
        }
    }
    rt_write64(0x20 + IR_ERDP,
               ((uint64_t)(uintptr_t)&evt_ring[evt_idx]) | (1ULL << 3));
    return saw_transfer;
}

/* ---- Initialization ---- */
int xhci_init(const usb_pci_controller_t* controller) {
    if (!controller) return 0;
    xhci_ready = 0;
    xhci_fault = 0;
    xhci_ports = 0;
    xhci_max_slots = 0;
    slot_id = 0;
    cur_port = -1;
    usb_addr = 0;
    intr_active = 0;
    intr_pending = 0;
    intr_failed = 0;
    cmd_idx = 0; cmd_cycle = 1;
    evt_idx = 0; evt_cycle = 1;
    ep0_idx = 0; ep0_cycle = 1;
    epi_idx = 0; epi_cycle = 1;

    uint32_t bar0 = controller->bar0;
    /*
     * Concrete real-world bug: under QEMU+OVMF, the qemu-xhci PCI BAR is left
     * unprogrammed unless something LocateProtocol's the EFI USB stack. We
     * see bar0 == 0x00000000 (or, if the BIOS write probe leaked, 0xFFFFFFFF),
     * or values like 0x00000004 / 0x0000000C where the type bits are set
     * (64-bit MMIO + (un)prefetchable) but bits [31:4] are all zero -- the
     * address slot was never assigned. Either way the controller is not
     * addressable. Print the actual BAR value so the operator can tell at a
     * glance, and let usb_host_try_next fall through to the next rung in
     * the chain (EHCI -> UHCI -> OHCI).
     *
     * Decoding hint:
     *   bit 0 == 0 -> MMIO; bit 0 == 1 -> IO-space.
     *   For MMIO, bits [3:1] == 00 -> 32-bit, == 10 -> 64-bit, == 01 -> below-1MB.
     *   Bit 3 -> prefetchable (MMIO).
     *
     * BAR self-allocation is a documented stretch goal. We deliberately
     * skip it here: rewriting bridge windows from a freestanding kernel can
     * mask the real device's IO/MEM range and wedge unrelated cards.
     * Improved diagnostics is the safer, equally-effective fix.
     */
    if (bar0 == 0 || bar0 == 0xFFFFFFFFu) {
        xhci_print("xHCI BAR=");
        xhci_print_hex32(bar0);
        xhci_print("xHCI: firmware did NOT program this BAR (all zeros / "
                   "all ones); falling through to EHCI/UHCI/OHCI.\n");
        return 0;
    }
    if (bar0 & 1) {
        xhci_print("xHCI BAR=");
        xhci_print_hex32(bar0);
        xhci_print("xHCI: BAR is IO-space (need MMIO); "
                   "falling through to EHCI/UHCI/OHCI.\n");
        return 0;
    }
    if ((bar0 & 0xFFFFFFF0) == 0) {
        /* MMIO BAR with type bits set but address bits zero -- this is the
         * canonical OVMF-skipped-USB-stack signature. Log loudly. */
        xhci_print("xHCI BAR=");
        xhci_print_hex32(bar0);
        xhci_print("xHCI: MMIO BAR address bits are zero "
                   "(OVMF didn't program it); "
                   "falling through to EHCI/UHCI/OHCI.\n");
        return 0;
    }

    /* Enable bus mastering + memory accesses. */
    uint16_t cmd = pci_read_config_word(controller->bus, controller->slot,
                                        controller->func, 0x04);
    pci_write_config_word(controller->bus, controller->slot,
                          controller->func, 0x04, cmd | 0x06);

    xhci_cap = (volatile uint8_t*)(uintptr_t)(bar0 & 0xFFFFFFF0);

    /*
     * Capability-register sanity. A controller that is gone, powered down, or
     * whose BAR was decoded but pointed at unassigned address space will read
     * back as all-ones (0xFF / 0xFFFFFFFF) on every dword. CAPLENGTH=0xFF or
     * HCSPARAMS1=0xFFFFFFFF means "this device is not really there"; logging
     * the raw values + bailing cleanly lets usb_host_try_next move on to
     * EHCI/UHCI/OHCI instead of corrupting state by treating the noise as
     * real cap data.
     */
    uint8_t cap_len = *xhci_cap;
    uint32_t hcsparams1 = *(volatile uint32_t*)(xhci_cap + 0x04);
    uint32_t hccparams1 = *(volatile uint32_t*)(xhci_cap + 0x10);

    if (cap_len == 0xFF || hcsparams1 == 0xFFFFFFFFu) {
        xhci_print("xHCI: capability registers read 0xFF/0xFFFFFFFF -- "
                   "controller gone or BAR mapped to unassigned space.\n");
        xhci_print("xHCI HCSPARAMS1=");
        xhci_print_hex32(hcsparams1);
        return 0;
    }

    if (cap_len < 0x20 || cap_len > 0x80) {
        xhci_print("xHCI bad capability length.\n");
        return 0;
    }
    xhci_op = xhci_cap + cap_len;

    uint32_t dboff      = *(volatile uint32_t*)(xhci_cap + 0x14) & ~3U;
    uint32_t rtsoff     = *(volatile uint32_t*)(xhci_cap + 0x18) & ~31U;

    xhci_max_slots = hcsparams1 & 0xFF;
    xhci_ports = (hcsparams1 >> 24) & 0xFF;
    xhci_context_size = (hccparams1 & (1U << 2)) ? 64 : 32;

    if (xhci_max_slots == 0 || xhci_ports == 0) {
        xhci_print("xHCI invalid capability parameters.\n");
        return 0;
    }
    xhci_doorbell = xhci_cap + dboff;
    xhci_runtime  = xhci_cap + rtsoff;

    /*
     * Intel xHCI USB port re-routing.
     *
     * On Intel 7/8/9-series chipsets (PCH from Panther Point onward) the USB
     * 2.0 ports default to being owned by the EHCI controller. To make them
     * visible to xHCI we have to write the routing-mask registers in xHCI's
     * own PCI config space (per Intel "OS-OWNED handoff" guidance):
     *   0xD0 XUSB2PR     USB 2.0 Port Routing       (bit per port; 1 = xHCI)
     *   0xD4 XUSB2PRM    USB 2.0 Port Routing Mask  (which ports CAN be xHCI)
     *   0xD8 USB3_PSSEN  USB 3.0 SuperSpeed Enable  (bit per port; 1 = xHCI)
     *   0xDC USB3PRM     USB 3.0 Port Routing Mask
     *
     * Setting PSSEN/XUSB2PR = their respective masks moves every routable
     * port over to xHCI. This is why USB 2.0 mice on Lenovo laptops show up
     * with "no pointer device" under xHCI until we do this — the device is
     * actually attached to EHCI behind the scenes.
     */
    /*
     * The XUSB2PR/XUSB2PRM/USB3_PSSEN/USB3PRM routing registers at PCI config
     * 0xD0-0xDC exist on PCH 7/8/9-series desktops/laptops to move USB 2.0
     * ports away from a COMPANION EHCI controller and onto xHCI. SoC-class
     * parts (e.g. Bay Trail / Cherry Trail, as in the Lenovo S21e-20) have no
     * companion EHCI at all and a different config-space layout, so blindly
     * writing those offsets can poke unrelated registers and wedge the
     * controller. Only perform the routing when an EHCI controller actually
     * exists in the system.
     */
    int have_ehci_companion = 0;
    {
        usb_pci_controller_t ctrls[8];
        int nc = pci_find_usb_controllers(ctrls, 8);
        for (int i = 0; i < nc; i++) {
            if (ctrls[i].prog_if == 0x20) { have_ehci_companion = 1; break; }
        }
    }

    if (controller->vendor_id == 0x8086 && have_ehci_companion) {
        uint32_t usb3prm = pci_read_config_dword(controller->bus, controller->slot,
                                                 controller->func, 0xDC);
        uint32_t usb3pre = pci_read_config_dword(controller->bus, controller->slot,
                                                 controller->func, 0xD8);
        uint32_t xusb2prm = pci_read_config_dword(controller->bus, controller->slot,
                                                  controller->func, 0xD4);
        uint32_t xusb2pr = pci_read_config_dword(controller->bus, controller->slot,
                                                 controller->func, 0xD0);

        char buf[12];
        const char* hex = "0123456789ABCDEF";
        buf[0] = '0'; buf[1] = 'x';
        for (int i = 0; i < 8; i++) buf[2 + i] = hex[(usb3prm >> ((7 - i) * 4)) & 0xF];
        buf[10] = '\n'; buf[11] = '\0';
        xhci_print("xHCI Intel routing pre USB3PRM=");  xhci_print(buf);
        for (int i = 0; i < 8; i++) buf[2 + i] = hex[(xusb2prm >> ((7 - i) * 4)) & 0xF];
        xhci_print("xHCI Intel routing pre XUSB2PRM="); xhci_print(buf);
        for (int i = 0; i < 8; i++) buf[2 + i] = hex[(usb3pre >> ((7 - i) * 4)) & 0xF];
        xhci_print("xHCI Intel routing pre USB3_PSSEN=");xhci_print(buf);
        for (int i = 0; i < 8; i++) buf[2 + i] = hex[(xusb2pr >> ((7 - i) * 4)) & 0xF];
        xhci_print("xHCI Intel routing pre XUSB2PR=");  xhci_print(buf);

        if (usb3prm)
            pci_write_config_dword(controller->bus, controller->slot,
                                   controller->func, 0xD8, usb3prm);
        if (xusb2prm)
            pci_write_config_dword(controller->bus, controller->slot,
                                   controller->func, 0xD0, xusb2prm);

        /* Read back to verify the chipset accepted the writes. Some BIOSes
         * lock these registers; in that case we'll see the value unchanged
         * and need to call EHCI fallback. */
        usb3pre = pci_read_config_dword(controller->bus, controller->slot,
                                        controller->func, 0xD8);
        xusb2pr = pci_read_config_dword(controller->bus, controller->slot,
                                        controller->func, 0xD0);
        for (int i = 0; i < 8; i++) buf[2 + i] = hex[(usb3pre >> ((7 - i) * 4)) & 0xF];
        xhci_print("xHCI Intel routing post USB3_PSSEN=");xhci_print(buf);
        for (int i = 0; i < 8; i++) buf[2 + i] = hex[(xusb2pr >> ((7 - i) * 4)) & 0xF];
        xhci_print("xHCI Intel routing post XUSB2PR=");  xhci_print(buf);

        if (usb3pre == 0 && usb3prm != 0) {
            xhci_print("xHCI: USB3_PSSEN write was REJECTED by chipset.\n");
        }
        if (xusb2pr == 0 && xusb2prm != 0) {
            xhci_print("xHCI: XUSB2PR write was REJECTED by chipset.\n");
        }

        xhci_print("xHCI: Intel port routing applied (USB2+USB3 -> xHCI).\n");
    } else if (controller->vendor_id == 0x8086) {
        xhci_print("xHCI: Intel SoC (no companion EHCI) -- skipping USB2 port routing.\n");
    }

    /*
     * BIOS-to-OS handoff (xHCI spec 4.22.1).
     *
     * On real laptops the BIOS keeps ownership of the xHCI controller for
     * USB legacy emulation. Writing to USBCMD without first asking BIOS to
     * release ownership triggers an SMI storm and can freeze the chipset.
     *
     * Walk the xHCI Extended Capabilities list at xECP = HCCPARAMS1[31:16] << 2
     * (in dwords from xhci_cap), looking for capability ID 0x01 (USBLEGSUP).
     * Set HC OS Owned (bit 24), wait for HC BIOS Owned (bit 16) to clear, and
     * disable SMI sources by clearing USBLEGCTLSTS at +4.
     */
    {
        uint32_t xecp_dwords = (hccparams1 >> 16) & 0xFFFF;
        if (xecp_dwords) {
            volatile uint32_t* xecp = (volatile uint32_t*)(xhci_cap + xecp_dwords * 4);
            int guard = 0;
            while (xecp && guard++ < 32) {
                uint32_t cap = *xecp;
                uint8_t cap_id = (uint8_t)(cap & 0xFF);
                if (cap_id == 0x01) {
                    *xecp = cap | (1U << 24);
                    uint64_t deadline = timer_deadline_ms(1000);
                    uint32_t guard = XHCI_SPIN_CEILING;
                    while (!timer_deadline_expired(deadline)) {
                        if (!(*xecp & (1U << 16))) break;
                        if (--guard == 0) break;
                    }
                    if (*xecp & (1U << 16)) {
                        xhci_print("xHCI BIOS did not release legacy ownership (forcing).\n");
                        *xecp = (*xecp) & ~(1U << 16);
                    } else {
                        xhci_print("xHCI BIOS legacy handoff complete.\n");
                    }
                    /*
                     * Fully disable SMI in USBLEGCTLSTS (xECP+4): clear every
                     * SMI-enable bit (the 0x000E1FEE group -- Linux's
                     * XHCI_LEGACY_DISABLE_SMI) and write 1 to the RW1C SMI
                     * status bits (31:29) to clear them. The previous code
                     * masked the register *to* the enable bits (keeping them)
                     * before overwriting, which was misleading; a read-modify
                     * -write that clears the enables is the correct form.
                     */
                    {
                        uint32_t legctl = *(xecp + 1);
                        legctl &= ~0x000E1FEEU;  /* clear all SMI enables */
                        legctl |= 0xE0000000U;   /* RW1C: clear SMI status */
                        *(xecp + 1) = legctl;
                    }
                    break;
                }
                uint8_t next = (uint8_t)((cap >> 8) & 0xFF);
                if (!next) break;
                xecp = xecp + next;
            }
        }
    }

    /* Halt + reset. */
    xhci_write32(XHCI_USBCMD, xhci_read32(XHCI_USBCMD) & ~XHCI_CMD_RUN);
    if (!xhci_wait_clear(XHCI_USBSTS, XHCI_STS_HCH ^ 0, 100)) {
        /* Wait for HCH=1 (halted). Inverted check below. */
    }
    /* Ensure halted. */
    {
        uint64_t deadline = timer_deadline_ms(1000);
        uint32_t guard = XHCI_SPIN_CEILING;
        while (!timer_deadline_expired(deadline)) {
            if (xhci_read32(XHCI_USBSTS) & XHCI_STS_HCH) break;
            if (--guard == 0) break;
        }
    }
    xhci_write32(XHCI_USBCMD, XHCI_CMD_RESET);
    if (!xhci_wait_clear(XHCI_USBCMD, XHCI_CMD_RESET, 200)) {
        xhci_print("xHCI reset timeout.\n");
        return 0;
    }
    xhci_wait_clear(XHCI_USBSTS, XHCI_STS_CNR, 200);

    /* Clear data structures. */
    xhci_memzero(xhci_dcbaa, sizeof(xhci_dcbaa));
    xhci_memzero(cmd_ring, sizeof(cmd_ring));
    xhci_memzero(evt_ring, sizeof(evt_ring));
    xhci_memzero(erst, sizeof(erst));
    xhci_memzero(input_ctx, sizeof(input_ctx));
    xhci_memzero(device_ctx, sizeof(device_ctx));
    xhci_memzero(ep0_ring, sizeof(ep0_ring));
    xhci_memzero(epi_ring, sizeof(epi_ring));

    /* Configure slot count. */
    uint32_t slots = xhci_max_slots > MAX_SLOTS_USED ? MAX_SLOTS_USED : xhci_max_slots;
    xhci_write32(XHCI_CONFIG, (xhci_read32(XHCI_CONFIG) & ~0xFFU) | slots);

    /* DCBAAP. */
    xhci_write64(XHCI_DCBAAP, (uint64_t)(uintptr_t)xhci_dcbaa);

    /* Command ring: cycle bit + RCS=1. */
    xhci_write64(XHCI_CRCR, ((uint64_t)(uintptr_t)cmd_ring) | 1ULL);

    /* Event ring: ERSTSZ, ERSTBA, ERDP for interrupter 0. */
    erst[0].base = (uint64_t)(uintptr_t)evt_ring;
    erst[0].size = EVT_RING_SIZE;
    erst[0].reserved = 0;
    rt_write32(0x20 + IR_ERSTSZ, 1);
    rt_write64(0x20 + IR_ERDP, (uint64_t)(uintptr_t)evt_ring);
    rt_write64(0x20 + IR_ERSTBA, (uint64_t)(uintptr_t)erst);
    rt_write32(0x20 + IR_IMOD, 0);
    rt_write32(0x20 + IR_IMAN, 0);  /* leave interrupts masked; we poll */

    /* Start the controller. */
    xhci_write32(XHCI_USBCMD, XHCI_CMD_RUN);
    {
        uint64_t deadline = timer_deadline_ms(2000);
        uint32_t guard = XHCI_SPIN_CEILING;
        while (!timer_deadline_expired(deadline)) {
            if (!(xhci_read32(XHCI_USBSTS) & XHCI_STS_HCH)) break;
            if (--guard == 0) break;
        }
    }
    if (xhci_read32(XHCI_USBSTS) & XHCI_STS_HCH) {
        xhci_print("xHCI did not start.\n");
        return 0;
    }

    /*
     * Power on every root-hub port (PORTSC.PP).
     *
     * After an xHCI reset PORTSC.PP defaults to 0 on most controllers, which
     * means Connect Status Change / Current Connect Status will never fire
     * even with a device physically attached. This is the exact symptom
     * observed on a Lenovo PCH xHCI: "host reports 7 port(s) / no ports
     * report a connected device" right after a successful BIOS handoff and
     * Intel routing. We assert PP on every port, wait a short settle time,
     * then clear any stale CSC bits so subsequent enumeration sees a fresh
     * connect event.
     */
    {
        int powered = 0;
        for (uint32_t i = 0; i < xhci_ports; i++) {
            uint32_t reg = XHCI_PORTSC_BASE + i * 0x10;
            uint32_t v = xhci_read32(reg);
            if (!(v & XHCI_PORT_PP)) {
                /* Preserve RW1C status change bits when writing PP. */
                xhci_write32(reg, (v & ~0x00FE0000U) | XHCI_PORT_PP);
                powered++;
            }
        }
        if (powered > 0) {
            xhci_print("xHCI: asserted PORT_POWER on root-hub ports.\n");
            short_delay(20);   /* >= 20ms VBUS settle per USB spec */
        }
        /* Clear stale Connect Status Change bits from boot-time noise. */
        for (uint32_t i = 0; i < xhci_ports; i++) {
            uint32_t reg = XHCI_PORTSC_BASE + i * 0x10;
            uint32_t v = xhci_read32(reg);
            xhci_write32(reg, (v & ~0x00FE0000U) | XHCI_PORT_CSC);
        }
        /* Give the chipset another short window to report real connects. */
        short_delay(10);
    }

    xhci_ready = 1;
    xhci_print("xHCI initialized (boot mouse path).\n");
    return 1;
}

void xhci_poll(void) {
    if (!xhci_ready) return;
    uint32_t status = xhci_read32(XHCI_USBSTS);
    if (status & XHCI_STS_HSE) {
        xhci_fault = 1;
        xhci_ready = 0;
        xhci_print("xHCI host system error.\n");
    }
    if (status & (XHCI_STS_EINT | XHCI_STS_PCD)) {
        xhci_write32(XHCI_USBSTS, status);
        drain_events();
    } else if (status) {
        xhci_write32(XHCI_USBSTS, status);
    } else {
        drain_events();
    }
}

int xhci_controller_healthy(void) {
    return xhci_ready && !xhci_fault;
}

int xhci_port_count(void) {
    return xhci_ready ? (int)xhci_ports : 0;
}

int xhci_port_connected(int port) {
    if (!xhci_ready || port < 0 || (uint32_t)port >= xhci_ports) return 0;
    return (xhci_read32(XHCI_PORTSC_BASE + (uint32_t)port * 0x10) & XHCI_PORT_CCS) != 0;
}

int xhci_port_low_speed(int port) {
    if (!xhci_ready || port < 0 || (uint32_t)port >= xhci_ports) return 0;
    uint32_t v = xhci_read32(XHCI_PORTSC_BASE + (uint32_t)port * 0x10);
    /* Port Speed in PORTSC[13:10]: 2 = Low, 1 = Full, 3 = High, 4 = Super. */
    uint32_t speed = (v >> 10) & 0xF;
    return speed == 2;
}

/*
 * Hot-plug primitives (one cheap MMIO read per port).
 *
 * xHCI PORTSC has CSC at bit 17 (RW1C). xhci_port_change_pending() returns
 * non-zero whenever CSC is latched -- that's a connect/disconnect transition
 * the controller observed since we last acked. xhci_port_change_ack() clears
 * the latch (writing 1 to CSC, preserving every RW1C-status-change bit and
 * leaving the other RW bits like PR/PED/PP untouched).
 */
int xhci_port_change_pending(int port) {
    if (!xhci_ready || xhci_fault) return 0;
    if (port < 0 || (uint32_t)port >= xhci_ports) return 0;
    uint32_t v = xhci_read32(XHCI_PORTSC_BASE + (uint32_t)port * 0x10);
    return (v & XHCI_PORT_CSC) != 0;
}

void xhci_port_change_ack(int port) {
    if (!xhci_ready || xhci_fault) return;
    if (port < 0 || (uint32_t)port >= xhci_ports) return;
    uint32_t reg = XHCI_PORTSC_BASE + (uint32_t)port * 0x10;
    uint32_t v = xhci_read32(reg);
    /* Clear CSC only: mask out the other RW1C status-change bits so writing
     * doesn't accidentally clear them, then OR in CSC to clear it. */
    xhci_write32(reg, (v & ~0x00FE0000U) | XHCI_PORT_CSC);
}

static uint32_t port_speed(int port) {
    uint32_t v = xhci_read32(XHCI_PORTSC_BASE + (uint32_t)port * 0x10);
    return (v >> 10) & 0xF;
}

static uint16_t default_max_packet_for_speed(uint32_t speed) {
    switch (speed) {
        case 4: return 512;     /* Super speed */
        case 3: return 64;      /* High speed */
        case 1: return 64;      /* Full speed */
        case 2: return 8;       /* Low speed */
        default: return 8;
    }
}

/* Issue Enable Slot and address the device into Default state. */
static int activate_slot(int port, uint32_t speed) {
    xhci_trb_t ev;

    enqueue_cmd(0, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT));
    if (wait_event(TRB_EV_CMD_COMPLETE, xhci_ms_to_ticks(250), &ev) < 0) {
        xhci_print("xHCI: Enable Slot timeout (poisoning controller).\n");
        xhci_fault = 1;
        return -1;
    }
    if (TRB_GET_CC(ev.status) != 1) {
        xhci_print("xHCI: Enable Slot failed.\n");
        return -1;
    }
    slot_id = TRB_SLOT(ev.control);
    if (slot_id <= 0) {
        xhci_print("xHCI: Enable Slot returned invalid slot.\n");
        return -1;
    }

    /* Build input context (0=ICC, 1=Slot, 2=EP0). */
    xhci_memzero(input_ctx, sizeof(input_ctx));
    /* Input Control Context: enable A0 (slot) and A1 (EP0). */
    uint32_t* icc = (uint32_t*)ctx_at(input_ctx, 0);
    icc[1] = (1U << 0) | (1U << 1);  /* add slot + EP0 */

    /* Slot context. */
    uint32_t* slot = (uint32_t*)ctx_at(input_ctx, 1);
    slot[0] = (1U << 27)             /* context entries = 1 (just EP0) */
            | ((speed & 0xF) << 20);
    slot[1] = ((uint32_t)(port + 1)) << 16;
    /* Interrupter target = 0, address = 0, slot state = enabled (HW writes). */

    /* EP0 context. */
    uint32_t* ep0 = (uint32_t*)ctx_at(input_ctx, 2);
    uint16_t mps = default_max_packet_for_speed(speed);
    ep0[0] = 0;
    ep0[1] = (EP_TYPE_CONTROL << 3)
           | (3U << 1)               /* CErr = 3 */
           | ((uint32_t)mps << 16);
    uint64_t ep0_ring_ptr = (uint64_t)(uintptr_t)ep0_ring;
    ep0[2] = (uint32_t)ep0_ring_ptr | 1U; /* DCS = 1 */
    ep0[3] = (uint32_t)(ep0_ring_ptr >> 32);
    ep0[4] = 8;                          /* avg TRB length = 8 */

    /* Device context placement. */
    xhci_dcbaa[slot_id] = (uint64_t)(uintptr_t)device_ctx;

    /* Address Device with BSR=1 first to land in Default state. */
    enqueue_cmd((uint32_t)(uintptr_t)input_ctx, 0, 0,
                TRB_TYPE(TRB_ADDRESS_DEVICE) | TRB_BSR | ((uint32_t)slot_id << 24));
    if (wait_event(TRB_EV_CMD_COMPLETE, xhci_ms_to_ticks(250), &ev) < 0) {
        xhci_print("xHCI: Address Device (default) timeout (poisoning controller).\n");
        xhci_fault = 1;
        return -1;
    }
    if (TRB_GET_CC(ev.status) != 1) {
        xhci_print("xHCI: Address Device (default) failed.\n");
        return -1;
    }

    cur_port = port;
    usb_addr = 0;
    return 0;
}

void xhci_port_reset(int port) {
    if (!xhci_ready || xhci_fault || port < 0 || (uint32_t)port >= xhci_ports) return;
    uint32_t reg = XHCI_PORTSC_BASE + (uint32_t)port * 0x10;
    uint32_t v = xhci_read32(reg);
    if (!(v & XHCI_PORT_CCS)) return;
    if (!(v & XHCI_PORT_PP)) {
        xhci_write32(reg, v | XHCI_PORT_PP);
        short_delay(20);
        v = xhci_read32(reg);
    }
    /* PORTSC reset is a write-1-to-set, write-1-to-clear-status field.
     * Preserve port-write-mask bits and only set PR + clear status bits. */
    uint32_t portsc = v;
    portsc = (portsc & ~0x00FE0000U); /* clear change bits */
    portsc |= XHCI_PORT_PR;
    xhci_write32(reg, portsc);

    uint64_t deadline = timer_deadline_ms(500);
    uint32_t guard = XHCI_SPIN_CEILING;
    while (!timer_deadline_expired(deadline)) {
        if (--guard == 0) break;
        uint32_t p = xhci_read32(reg);
        if ((p & XHCI_PORT_PRC) && (p & XHCI_PORT_PED)) {
            /* Reset complete and port enabled. */
            xhci_write32(reg, (p & ~0x00FE0000U) | XHCI_PORT_PRC);
            uint32_t speed = port_speed(port);
            if (activate_slot(port, speed) != 0) {
                slot_id = 0;
                cur_port = -1;
            }
            return;
        }
    }
    xhci_print("xHCI: port reset did not complete.\n");
}

/* Ring TRBs on a transfer ring with cycle bit and wrap. */
static xhci_trb_t* push_ring(xhci_trb_t* ring, uint32_t* idx, uint32_t* cycle,
                             uint32_t plo, uint32_t phi, uint32_t status,
                             uint32_t control) {
    xhci_trb_t* trb = &ring[*idx];
    trb->param_low = plo;
    trb->param_high = phi;
    trb->status = status;
    trb->control = (control & ~TRB_CYCLE) | (*cycle ? TRB_CYCLE : 0);

    (*idx)++;
    if (*idx >= EP_RING_SIZE - 1) {
        xhci_trb_t* link = &ring[EP_RING_SIZE - 1];
        link->param_low = (uint32_t)(uintptr_t)ring;
        link->param_high = 0;
        link->status = 0;
        link->control = TRB_TYPE(TRB_LINK) | TRB_ENT | (*cycle ? TRB_CYCLE : 0);
        *idx = 0;
        *cycle ^= 1;
    }
    return trb;
}

static void ring_ep_doorbell(uint32_t ep_index) {
    *(volatile uint32_t*)(xhci_doorbell + slot_id * 4) = ep_index;
}

/*
 * Control transfer on EP0.
 *
 * The enumeration layer issues SET_ADDRESS as a regular control transfer.
 * For xHCI, addresses are assigned via the Address Device command, not a
 * USB SETUP transaction. We intercept SET_ADDRESS and translate it.
 */
int xhci_control_transfer(uint8_t dev_addr, uint8_t endpoint,
                          uint8_t* setup_pkt, uint8_t* data, uint16_t data_len,
                          int direction_in) {
    (void)endpoint;
    if (!xhci_ready || xhci_fault || slot_id == 0 || !setup_pkt) return -1;

    uint8_t bRequest = setup_pkt[1];
    /* SET_ADDRESS: re-issue Address Device with BSR=0 and the requested addr. */
    if (bRequest == 0x05 /* SET_ADDRESS */) {
        uint8_t new_addr = setup_pkt[2];
        /* Mark the input control context to update slot ctx + EP0 ctx. */
        uint32_t* icc = (uint32_t*)ctx_at(input_ctx, 0);
        icc[0] = 0;
        icc[1] = (1U << 0) | (1U << 1);

        enqueue_cmd((uint32_t)(uintptr_t)input_ctx, 0, 0,
                    TRB_TYPE(TRB_ADDRESS_DEVICE) | ((uint32_t)slot_id << 24));
        xhci_trb_t ev;
        if (wait_event(TRB_EV_CMD_COMPLETE, xhci_ms_to_ticks(250), &ev) < 0) {
            xhci_fault = 1;
            return -1;
        }
        if (TRB_GET_CC(ev.status) != 1) {
            xhci_print("xHCI: Address Device (set) failed.\n");
            return -1;
        }
        usb_addr = new_addr;
        (void)dev_addr;
        return 0;
    }

    /* Build SETUP / DATA / STATUS TRBs. */
    uint32_t setup_lo = ((uint32_t)setup_pkt[0])
                      | ((uint32_t)setup_pkt[1] << 8)
                      | ((uint32_t)setup_pkt[2] << 16)
                      | ((uint32_t)setup_pkt[3] << 24);
    uint32_t setup_hi = ((uint32_t)setup_pkt[4])
                      | ((uint32_t)setup_pkt[5] << 8)
                      | ((uint32_t)setup_pkt[6] << 16)
                      | ((uint32_t)setup_pkt[7] << 24);

    uint32_t trt;
    if (data_len == 0) trt = TRB_TRT_NO_DATA;
    else trt = direction_in ? TRB_TRT_IN : TRB_TRT_OUT;

    push_ring(ep0_ring, &ep0_idx, &ep0_cycle,
              setup_lo, setup_hi, 8,
              TRB_TYPE(TRB_SETUP_STAGE) | TRB_IDT | trt);

    /* Buffer the data in our scratch area, then copy back on IN. */
    if (data_len > 0) {
        if (data_len > sizeof(xfer_data)) data_len = sizeof(xfer_data);
        if (!direction_in && data) {
            for (uint16_t i = 0; i < data_len; i++) xfer_data[i] = data[i];
        }
        push_ring(ep0_ring, &ep0_idx, &ep0_cycle,
                  (uint32_t)(uintptr_t)xfer_data, 0, data_len,
                  TRB_TYPE(TRB_DATA_STAGE)
                  | (direction_in ? TRB_DIR_IN : 0));
    }
    push_ring(ep0_ring, &ep0_idx, &ep0_cycle, 0, 0, 0,
              TRB_TYPE(TRB_STATUS_STAGE) | TRB_IOC
              | (direction_in ? 0 : TRB_DIR_IN));

    ring_ep_doorbell(1); /* EP0 doorbell value = 1 */

    xhci_trb_t ev;
    if (wait_event(TRB_EV_TRANSFER, xhci_ms_to_ticks(250), &ev) < 0) {
        xhci_print("xHCI: EP0 transfer timeout (poisoning controller).\n");
        /*
         * The transfer ring is now in an inconsistent state. Mark the
         * controller as faulted so subsequent control transfers fail fast
         * instead of stacking more dangling TRBs on top.
         */
        xhci_fault = 1;
        return -1;
    }
    uint8_t cc = TRB_GET_CC(ev.status);
    if (cc != 1 && cc != 13 /* Short */) {
        xhci_print("xHCI: EP0 transfer failed.\n");
        return -1;
    }
    if (direction_in && data && data_len > 0) {
        for (uint16_t i = 0; i < data_len; i++) data[i] = xfer_data[i];
    }
    return 0;
}

int xhci_schedule_interrupt(uint8_t dev_addr, uint8_t endpoint,
                            uint16_t max_packet, uint8_t interval_frames) {
    (void)dev_addr;
    if (!xhci_ready || slot_id == 0) return -1;
    if (max_packet == 0) max_packet = 8;
    if (max_packet > sizeof(intr_buf)) max_packet = sizeof(intr_buf);

    /*
     * EP index in the device context for endpoint N IN is (N * 2 + 1).
     * For boot mouse we typically see EP1 IN → ctx index 3 (== 2*1+1).
     */
    uint8_t ep_num = endpoint & 0x0F;
    uint32_t dci = ep_num * 2 + 1; /* always IN here */
    if (dci == 0 || dci >= XHCI_MAX_CONTEXTS) {
        xhci_print("xHCI: interrupt endpoint DCI out of range.\n");
        return -1;
    }

    /* Set up input context: Add A0 (slot) + A_dci (target EP). */
    uint32_t* icc = (uint32_t*)ctx_at(input_ctx, 0);
    icc[0] = 0;
    icc[1] = (1U << 0) | (1U << dci);

    uint32_t* slot = (uint32_t*)ctx_at(input_ctx, 1);
    /* bump Context Entries to dci. */
    slot[0] = (slot[0] & ~(0x1FU << 27)) | ((dci & 0x1FU) << 27);

    /* Interrupt IN endpoint context lives at input_ctx index (1 + dci). */
    uint32_t* ep = (uint32_t*)ctx_at(input_ctx, 1 + dci);
    /* For low/full/high speed, Interval field is log2(bInterval); for SS it's
     * passed through. Pick a conservative interval that polls fast enough. */
    uint32_t interval;
    if (interval_frames < 1) interval_frames = 1;
    interval = 3; /* 2^3 = 8 microframes ≈ 1 ms */
    (void)interval_frames;

    ep[0] = (interval << 16);
    ep[1] = (EP_TYPE_INTERRUPT_IN << 3)
          | (3U << 1)
          | ((uint32_t)max_packet << 16);
    uint64_t ring_ptr = (uint64_t)(uintptr_t)epi_ring;
    ep[2] = (uint32_t)ring_ptr | 1U;  /* DCS = 1 */
    ep[3] = (uint32_t)(ring_ptr >> 32);
    ep[4] = max_packet;

    /* Configure Endpoint command. */
    enqueue_cmd((uint32_t)(uintptr_t)input_ctx, 0, 0,
                TRB_TYPE(TRB_CONFIGURE_EP) | ((uint32_t)slot_id << 24));
    xhci_trb_t ev;
    if (wait_event(TRB_EV_CMD_COMPLETE, xhci_ms_to_ticks(250), &ev) < 0) {
        xhci_print("xHCI: Configure EP timeout (poisoning controller).\n");
        xhci_fault = 1;
        return -1;
    }
    if (TRB_GET_CC(ev.status) != 1) {
        xhci_print("xHCI: Configure EP failed.\n");
        return -1;
    }

    /* Queue first interrupt-IN normal TRB. */
    push_ring(epi_ring, &epi_idx, &epi_cycle,
              (uint32_t)(uintptr_t)intr_buf, 0, max_packet,
              TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    ring_ep_doorbell(dci);

    intr_active = 1;
    intr_pending = 0;
    intr_failed = 0;
    return 0;
}

void xhci_remove_interrupt(void) {
    intr_active = 0;
    intr_pending = 0;
    intr_failed = 0;
}

int xhci_interrupt_active(void) { return intr_active; }

uint8_t* xhci_get_report(int* ready) {
    if (!intr_active) {
        if (ready) *ready = 0;
        return intr_buf;
    }
    if (intr_pending) {
        if (ready) *ready = 1;
    } else if (intr_failed) {
        if (ready) *ready = -1;
    } else {
        if (ready) *ready = 0;
    }
    return intr_buf;
}

void xhci_ack_report(void) {
    if (!intr_active) return;
    intr_pending = 0;
    intr_failed = 0;
    /* Clear stale bytes so a short report never leaves old wheel/data behind. */
    for (int i = 0; i < 8; i++) intr_buf[i] = 0;
    /* Re-enqueue the interrupt-IN normal TRB. */
    push_ring(epi_ring, &epi_idx, &epi_cycle,
              (uint32_t)(uintptr_t)intr_buf, 0, sizeof(intr_buf),
              TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    /* DCI 3 is EP1 IN; if a different EP number was scheduled, the same DCI
     * still maps to the slot's interrupt endpoint we configured. */
    ring_ep_doorbell(3);
}
