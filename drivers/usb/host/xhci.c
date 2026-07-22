/*
 * xHCI host controller engine (rings / contexts / transfers).
 *
 * PORTSC writes go through xhci_port.c (neutralizer — never echo PED).
 * Slot Context speed packing uses xhci_ctx.c. The redesigned USB core binds
 * this engine via usb_xhci_hcd_ops (drivers/usb/host/xhci_hcd.c).
 *
 * Scope today: one active slot + EP0 + one interrupt-IN (HID) or bulk pair
 * (MSC). Multi-slot rings remain the next incremental step; architecture for
 * concurrent devices lives in drivers/usb/core/.
 */

#include "xhci.h"
#include "xhci_port.h"
#include "xhci_ctx.h"
#include "baytrail_usb.h"
#include "../../io/io.h"
#include "../../timer/timer.h"
#include "../../diagnostics/driver_log.h"

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
#define TRB_DISABLE_SLOT    10U
#define TRB_ADDRESS_DEVICE  11U
#define TRB_CONFIGURE_EP    12U
#define TRB_EVALUATE_CTX    13U
#define TRB_RESET_EP        14U
#define TRB_STOP_EP         15U
#define TRB_SET_TR_DEQUEUE  16U
#define TRB_NOOP_CMD        23U
#define TRB_EV_TRANSFER     32U
#define TRB_EV_CMD_COMPLETE 33U
#define TRB_EV_PORT_STATUS  34U

/* USB2 PORTPMSC.HLE — hardware LPM enable (clear for boot enum). */
#define XHCI_PORTPMSC_HLE   (1U << 16)

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
/* Bay Trail root hubs often show CCS on many ports; abandon_slot() does not
 * Disable Slot (that wedges PCI after EP0 cc=4), so orphaned Enabled slots
 * still consume MaxSlotsEn. Cap high enough to cover a full port scan. */
#define MAX_SLOTS_USED 64
#define XHCI_MAX_CONTEXTS 32
#define XHCI_CC_NO_SLOTS  9U

typedef struct __attribute__((packed, aligned(16))) {
    uint32_t param_low;
    uint32_t param_high;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

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

/*
 * Rings/DCBAA live in .data (explicit section), not .bss.
 * Zero-init alone still lands in .bss after the x64 8 MiB heap (~10 MiB).
 * More importantly the ERST must be a flat 16-byte record: a
 * `struct { ... } __attribute__((aligned(64)))` typedef inflates sizeof to 64
 * and QEMU's xhci_er_reset() then rejects the segment (USBSTS.HCE), so Enable
 * Slot never completes. Keep a raw 4x u32 ERST entry below.
 */
static uint64_t xhci_dcbaa[256]
    __attribute__((aligned(64), section(".data.xhci"))) = {1};
static xhci_trb_t cmd_ring[CMD_RING_SIZE]
    __attribute__((aligned(64), section(".data.xhci"))) = {{1}};
static xhci_trb_t evt_ring[EVT_RING_SIZE]
    __attribute__((aligned(64), section(".data.xhci"))) = {{1}};
/* Flat 16-byte Event Ring Segment Table entry (base lo/hi, size, rsvd). */
static uint32_t xhci_erst_raw[4]
    __attribute__((aligned(64), section(".data.xhci"))) = {1, 0, 0, 0};

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
static xhci_ctx_block_t input_ctx[XHCI_MAX_CONTEXTS + 1]
    __attribute__((aligned(64), section(".data.xhci"))) = {{{1}}};
static xhci_ctx_block_t device_ctx[XHCI_MAX_CONTEXTS]
    __attribute__((aligned(64), section(".data.xhci"))) = {{{1}}};
static xhci_trb_t ep0_ring[EP_RING_SIZE]
    __attribute__((aligned(64), section(".data.xhci"))) = {{1}};
static xhci_trb_t epi_ring[EP_RING_SIZE]
    __attribute__((aligned(64), section(".data.xhci"))) = {{1}};
static xhci_trb_t bulk_out_ring[EP_RING_SIZE]
    __attribute__((aligned(64), section(".data.xhci"))) = {{1}};
static xhci_trb_t bulk_in_ring[EP_RING_SIZE]
    __attribute__((aligned(64), section(".data.xhci"))) = {{1}};
static uint8_t   xfer_data[256]
    __attribute__((aligned(64), section(".data.xhci"))) = {1};
static uint8_t   bulk_xfer_data[512]
    __attribute__((aligned(64), section(".data.xhci"))) = {1};
static uint8_t   intr_buf[64]
    __attribute__((aligned(64), section(".data.xhci"))) = {1};

static uint32_t cmd_idx = 0, cmd_cycle = 1;
static uint32_t evt_idx = 0, evt_cycle = 1;
static uint32_t ep0_idx = 0, ep0_cycle = 1;
static uint32_t epi_idx = 0, epi_cycle = 1;
static uint32_t bulk_out_idx = 0, bulk_out_cycle = 1;
static uint32_t bulk_in_idx = 0, bulk_in_cycle = 1;

static int      slot_id = 0;
static int      cur_port = -1;
static uint32_t slot_spd = 0;   /* Slot Context Speed (SPD) for rebuilds */
static uint8_t  usb_addr = 0;
static int      intr_active = 0;
static int      intr_pending = 0;
static int      intr_failed = 0;
static uint32_t intr_dci = 0;       /* Interrupt-IN DCI once configured */
static int      ep0_xfer_waiting = 0; /* drain_events must not steal EP0 TEs */
static uint16_t ep0_mps = 8;
static int      soft_fail_streak = 0;
static int      ep0_soft_fail_pending = 0;
static uint8_t  bulk_out_ep = 0;
static uint8_t  bulk_in_ep = 0;
static uint32_t bulk_out_dci = 0;
static uint32_t bulk_in_dci = 0;

/*
 * Scratchpad buffers (xHCI 4.20). Static .data pages so Bay Trail always has
 * DCBAA[0] wired (EP0 often completes cc=4 without them). Kept in .data.xhci
 * with the rings so they are not shoved past the 8 MiB BSS heap.
 */
#define XHCI_MAX_SCRATCHPADS 8
static uint32_t xhci_scratch_count = 0;
static uint64_t xhci_scratch_array[XHCI_MAX_SCRATCHPADS]
    __attribute__((aligned(64), section(".data.xhci"))) = {1};
static uint8_t xhci_scratch_bufs[XHCI_MAX_SCRATCHPADS][4096]
    __attribute__((aligned(4096), section(".data.xhci"))) = {{1}};

/* ---- Helpers ---- */
static void xhci_serial(const char* s) { while (*s) outb(0xE9, *s++); }
static void xhci_print(const char* s) {
    driver_log(s);
    print(s);
    xhci_serial(s);
}

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
    /* Ensure TRB stores are visible before the MMIO doorbell write. */
    __asm__ __volatile__("" ::: "memory");
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

/* Clear USBSTS / IMAN (used by poll path companions; wait_event only checks HSE). */
static void service_usbsts(void) {
    if (!xhci_op) return;
    uint32_t status = xhci_read32(XHCI_USBSTS);
    if (status & XHCI_STS_HSE) {
        xhci_fault = 1;
        xhci_ready = 0;
        xhci_print("xHCI host system error.\n");
        return;
    }
    if (status & (XHCI_STS_EINT | XHCI_STS_PCD)) {
        xhci_write32(XHCI_USBSTS, status & (XHCI_STS_EINT | XHCI_STS_PCD));
    }
    uint32_t iman = rt_read32(0x20 + IR_IMAN);
    if (iman & 1U) rt_write32(0x20 + IR_IMAN, iman);
}

static void evt_advance(void) {
    evt_idx++;
    if (evt_idx >= EVT_RING_SIZE) {
        evt_idx = 0;
        evt_cycle ^= 1;
    }
    rt_write64(0x20 + IR_ERDP,
               ((uint64_t)(uintptr_t)&evt_ring[evt_idx]) | (1ULL << 3));
}

static int wait_event(uint8_t want_type, uint32_t timeout_ticks,
                      xhci_trb_t* out) {
    uint64_t deadline = timer_deadline_ms(timeout_ticks * 10);
    /* Always poll a minimum number of times so a mis-calibrated TSC deadline
     * cannot skip the ring entirely (seen as instant Enable Slot timeouts
     * under a heavily loaded QEMU host). */
    uint32_t guard = XHCI_SPIN_CEILING;
    uint32_t min_spins = 50000U;
    while (min_spins > 0 || !timer_deadline_expired(deadline)) {
        if (min_spins > 0) min_spins--;
        if (--guard == 0) break;
        /* Do not clear EINT/IMAN every spin — that can race QEMU's nec-usb-xhci
         * event delivery. Only watch for fatal HSE. */
        {
            uint32_t status = xhci_read32(XHCI_USBSTS);
            if (status & XHCI_STS_HSE) {
                xhci_fault = 1;
                xhci_ready = 0;
                xhci_print("xHCI host system error.\n");
                return -1;
            }
        }
        if (xhci_fault) return -1;
        xhci_trb_t* ev = &evt_ring[evt_idx];
        uint32_t c = ev->control;
        if ((c & TRB_CYCLE) != (uint32_t)evt_cycle) continue;
        uint8_t type = TRB_GET_TYPE(c);
        if (want_type == 0 || type == want_type) {
            if (out) *out = *ev;
            evt_advance();
            soft_fail_streak = 0;
            return type;
        }
        /* Wrong type, still consume so we don't get stuck. */
        evt_advance();
    }
    return -1;
}

/*
 * Wait for EP0 TD completion. Match Transfer Events by slot + EP0 (DCI 1).
 * Control DATA stages intentionally omit TRB_ISP so only the Status (IOC)
 * TRB should raise a TE; accept any successful EP0 TE for this slot (Bay Trail
 * sometimes reports a TRB pointer that is not bit-identical to Status).
 */
static int wait_ep0_transfer(uint32_t timeout_ticks, uintptr_t status_trb,
                             xhci_trb_t* out) {
    uint64_t deadline = timer_deadline_ms(timeout_ticks * 10);
    uint32_t guard = XHCI_SPIN_CEILING;
    uint32_t min_spins = 50000U;
    (void)status_trb;
    ep0_xfer_waiting = 1;
    while (min_spins > 0 || !timer_deadline_expired(deadline)) {
        if (min_spins > 0) min_spins--;
        if (--guard == 0) break;
        {
            uint32_t status = xhci_read32(XHCI_USBSTS);
            if (status & XHCI_STS_HSE) {
                xhci_fault = 1;
                xhci_ready = 0;
                ep0_xfer_waiting = 0;
                xhci_print("xHCI host system error.\n");
                return -1;
            }
        }
        if (xhci_fault) {
            ep0_xfer_waiting = 0;
            return -1;
        }
        xhci_trb_t* ev = &evt_ring[evt_idx];
        uint32_t c = ev->control;
        if ((c & TRB_CYCLE) != (uint32_t)evt_cycle) continue;
        uint8_t type = TRB_GET_TYPE(c);
        if (type != TRB_EV_TRANSFER) {
            evt_advance();
            continue;
        }
        if (TRB_SLOT(c) != (uint32_t)slot_id || TRB_EP(c) != 1U) {
            if (intr_active && TRB_SLOT(c) == (uint32_t)slot_id &&
                (intr_dci == 0U || TRB_EP(c) == intr_dci)) {
                uint8_t cc = (uint8_t)TRB_GET_CC(ev->status);
                if (cc == 1 || cc == 13)
                    intr_pending = 1;
                else
                    intr_failed = 1;
            }
            evt_advance();
            continue;
        }
        /* EP0 Transfer Event for our slot — TD complete (or hard fail). */
        if (out) *out = *ev;
        evt_advance();
        soft_fail_streak = 0;
        ep0_xfer_waiting = 0;
        return TRB_EV_TRANSFER;
    }
    ep0_xfer_waiting = 0;
    return -1;
}

/* Drain pending events (called from poll). Returns the last transfer event
 * type if one occurred. Never steals EP0 Transfer Events while a control TD
 * is in flight, and never treats EP0 as interrupt-IN completion. */
static int drain_events(void) {
    int saw_transfer = 0;
    for (int safety = 0; safety < EVT_RING_SIZE; safety++) {
        xhci_trb_t* ev = &evt_ring[evt_idx];
        if ((ev->control & TRB_CYCLE) != (uint32_t)evt_cycle) break;
        uint8_t type = TRB_GET_TYPE(ev->control);
        if (type == TRB_EV_TRANSFER) {
            uint32_t ep = TRB_EP(ev->control);
            uint32_t sl = TRB_SLOT(ev->control);
            if (ep == 1U) {
                /* EP0: leave on ring for wait_ep0_transfer. */
                if (ep0_xfer_waiting) break;
                /* Stale EP0 after timeout — discard. */
                evt_advance();
                continue;
            }
            if (intr_active && sl == (uint32_t)slot_id &&
                (intr_dci == 0U || ep == intr_dci)) {
                uint8_t cc = TRB_GET_CC(ev->status);
                if (cc == 1 || cc == 13 /* Short */) {
                    intr_pending = 1;
                } else {
                    intr_failed = 1;
                }
                saw_transfer = 1;
            }
        }
        evt_advance();
    }
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
    intr_dci = 0;
    ep0_xfer_waiting = 0;
    ep0_mps = 8;
    soft_fail_streak = 0;
    cmd_idx = 0; cmd_cycle = 1;
    evt_idx = 0; evt_cycle = 1;
    ep0_idx = 0; ep0_cycle = 1;
    epi_idx = 0; epi_cycle = 1;

    uint32_t bar0 = controller->bar0;
    uint32_t bar1 = controller->bar1;
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
     *   For MMIO, bits [2:1] == 00 -> 32-bit, == 10 -> 64-bit, == 01 -> below-1MB.
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

    {
        uint64_t bar_addr = (uint64_t)(bar0 & 0xFFFFFFF0U);
        if ((bar0 & 0x6U) == 0x4U) {
            /* 64-bit MMIO BAR: high dword is BAR1. */
            bar_addr |= ((uint64_t)bar1) << 32;
        }
        xhci_print("xHCI: device=");
        xhci_print_hex32(((uint32_t)controller->vendor_id << 16) |
                         (uint32_t)controller->device_id);
        xhci_print("xHCI: mmio=");
        xhci_print_hex32((uint32_t)bar_addr);
        if (bar_addr >> 32) {
            xhci_print("xHCI: mmio_hi=");
            xhci_print_hex32((uint32_t)(bar_addr >> 32));
        }
        xhci_cap = (volatile uint8_t*)(uintptr_t)bar_addr;
    }

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
    uint16_t hci_ver = *(volatile uint16_t*)(xhci_cap + 0x02);
    xhci_print("xHCI: HCI version=");
    xhci_print_hex32(hci_ver);

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

    /* Max Scratchpad Bufs = (HCSPARAMS2[25:21] << 5) | HCSPARAMS2[31:27]. */
    {
        uint32_t hcsparams2 = *(volatile uint32_t*)(xhci_cap + 0x08);
        uint32_t sp_lo = (hcsparams2 >> 27) & 0x1FU;
        uint32_t sp_hi = (hcsparams2 >> 21) & 0x1FU;
        xhci_scratch_count = (sp_hi << 5) | sp_lo;
        if (xhci_scratch_count > XHCI_MAX_SCRATCHPADS)
            xhci_scratch_count = XHCI_MAX_SCRATCHPADS;
        xhci_print("xHCI: HCSPARAMS2 scratchpads=");
        {
            char b[8];
            int n = (int)xhci_scratch_count;
            int j = 0;
            if (n >= 10) { b[j++] = '0' + n / 10; b[j++] = '0' + n % 10; }
            else b[j++] = '0' + n;
            b[j++] = '\n';
            b[j] = 0;
            xhci_print(b);
        }
    }

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

    /*
     * Bay Trail / Cherry Trail switchable hosts: USB2 ports can sit on a
     * companion EHCI (8086:0F34) even when that EHCI is hidden from PCI.
     * coreboot routes with USB2PR=0xF / USB3PR=0x1. 0x0F34 is EHCI, not xHCI.
     * baytrail_usb_prepare_companion() already attempted UPRWC+PDO+PR; refresh
     * the route here in case prepare did not run (or PR needs a second shot
     * after MMIO mapping).
     */
    int is_baytrail_xhci = (controller->vendor_id == 0x8086) &&
                           (controller->device_id == 0x0F31 ||
                            controller->device_id == 0x0F35 ||
                            controller->device_id == 0x0F36 ||
                            controller->device_id == 0x0F37);
    /* Braswell 0x22B5: skip BYT UPRWC/PDO route (hangs Acer R3-131T). */
    int is_braswell_xhci = (controller->vendor_id == 0x8086) &&
                           (controller->device_id == 0x22B5);
    int do_intel_route = (controller->vendor_id == 0x8086) &&
                         !is_braswell_xhci &&
                         (have_ehci_companion || is_baytrail_xhci);

    if (do_intel_route) {
        uint32_t usb3prm = pci_read_config_dword(controller->bus, controller->slot,
                                                 controller->func, 0xDC);
        uint32_t usb3pre = pci_read_config_dword(controller->bus, controller->slot,
                                                 controller->func, 0xD8);
        uint32_t xusb2prm = pci_read_config_dword(controller->bus, controller->slot,
                                                  controller->func, 0xD4);
        uint32_t xusb2pr = pci_read_config_dword(controller->bus, controller->slot,
                                                 controller->func, 0xD0);
        int wrote_byt_route = 0;
        (void)wrote_byt_route;

        if (is_baytrail_xhci) {
            xhci_print("xHCI: Bay Trail switchable host -- UPRWC PDO+PRM+PR route.\n");
            wrote_byt_route = 1;
            (void)baytrail_usb_route_to_xhci(controller->bus, controller->slot,
                                             controller->func);
            usb3prm = pci_read_config_dword(controller->bus, controller->slot,
                                            controller->func, 0xDC);
            usb3pre = pci_read_config_dword(controller->bus, controller->slot,
                                            controller->func, 0xD8);
            xusb2prm = pci_read_config_dword(controller->bus, controller->slot,
                                             controller->func, 0xD4);
            xusb2pr = pci_read_config_dword(controller->bus, controller->slot,
                                            controller->func, 0xD0);
        } else {
            if (usb3prm)
                pci_write_config_dword(controller->bus, controller->slot,
                                       controller->func, 0xD8, usb3prm);
            if (xusb2prm)
                pci_write_config_dword(controller->bus, controller->slot,
                                       controller->func, 0xD0, xusb2prm);
            usb3pre = pci_read_config_dword(controller->bus, controller->slot,
                                            controller->func, 0xD8);
            xusb2pr = pci_read_config_dword(controller->bus, controller->slot,
                                            controller->func, 0xD0);
            baytrail_usb_note_route(xusb2pr, xusb2prm,
                                    have_ehci_companion ? 1 : 0);
        }

        {
            char buf[12];
            const char* hex = "0123456789ABCDEF";
            buf[0] = '0'; buf[1] = 'x';
            for (int i = 0; i < 8; i++) buf[2 + i] = hex[(usb3prm >> ((7 - i) * 4)) & 0xF];
            buf[10] = '\n'; buf[11] = '\0';
            xhci_print("xHCI Intel routing post USB3PRM=");  xhci_print(buf);
            for (int i = 0; i < 8; i++) buf[2 + i] = hex[(xusb2prm >> ((7 - i) * 4)) & 0xF];
            xhci_print("xHCI Intel routing post XUSB2PRM="); xhci_print(buf);
            for (int i = 0; i < 8; i++) buf[2 + i] = hex[(usb3pre >> ((7 - i) * 4)) & 0xF];
            xhci_print("xHCI Intel routing post USB3_PSSEN=");xhci_print(buf);
            for (int i = 0; i < 8; i++) buf[2 + i] = hex[(xusb2pr >> ((7 - i) * 4)) & 0xF];
            xhci_print("xHCI Intel routing post XUSB2PR=");  xhci_print(buf);
        }

        if (usb3pre == 0 && usb3prm != 0) {
            xhci_print("xHCI: USB3_PSSEN write was REJECTED by chipset.\n");
        }
        if (xusb2pr == 0) {
            xhci_print("xHCI: XUSB2PR stayed 0 -- USB2 ports remain on companion EHCI.\n");
        } else {
            xhci_print("xHCI: Intel port routing applied (USB2+USB3 -> xHCI).\n");
        }
        {
            uint32_t usb2pdo = pci_read_config_dword(controller->bus, controller->slot,
                                                     controller->func, 0xE4);
            uint32_t usb3pdo = pci_read_config_dword(controller->bus, controller->slot,
                                                     controller->func, 0xE8);
            xhci_print("xHCI Bay Trail USB2PDO=");
            xhci_print_hex32(usb2pdo);
            xhci_print("xHCI Bay Trail USB3PDO=");
            xhci_print_hex32(usb3pdo);
        }
    } else if (controller->vendor_id == 0x8086) {
        xhci_print("xHCI: Intel SoC (no companion EHCI) -- skipping USB2 port routing.\n");
        baytrail_usb_note_route(0, 0, 0);
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

    /*
     * Bay Trail: HCRESET does not restore vendor MMIO/PHY state that coreboot
     * / Windows firmware apply. Without CommonXhciHcInit + 0x80e0 pulse, USB2
     * ports can report CCS/speed and still fail every EP0 with cc=4.
     */
    if (is_baytrail_xhci) {
        baytrail_xhci_hc_bringup(xhci_cap, controller->bus, controller->slot,
                                 controller->func);
        /* 0x80e0 bit24 can pulse an internal reset — wait CNR again. */
        xhci_wait_clear(XHCI_USBSTS, XHCI_STS_CNR, 200);
        short_delay(20);

        /*
         * HCRESET (and the Bay Trail 0x80e0 pulse) can clear XUSB2PR back to
         * 0. Re-route USB2→xHCI HERE or EP0 against gray ports times out
         * forever while the mouse LED still lights from VBUS/PP.
         */
        xhci_print("xHCI: re-applying Bay Trail USB2 route after HCRESET.\n");
        if (!baytrail_usb_route_to_xhci(controller->bus, controller->slot,
                                        controller->func)) {
            xhci_print("xHCI: post-reset XUSB2PR=0 — USB2 mouse path dead.\n");
        } else {
            xhci_print("xHCI: post-reset XUSB2PR ok (USB2 on xHCI).\n");
        }
        {
            uint32_t usb2pdo = pci_read_config_dword(controller->bus,
                                                     controller->slot,
                                                     controller->func, 0xE4);
            uint32_t xusb2pr = pci_read_config_dword(controller->bus,
                                                     controller->slot,
                                                     controller->func, 0xD0);
            xhci_print("xHCI Bay Trail post-reset XUSB2PR=");
            xhci_print_hex32(xusb2pr);
            xhci_print("xHCI Bay Trail post-reset USB2PDO=");
            xhci_print_hex32(usb2pdo);
        }
    }

    /* Clear data structures. */
    xhci_memzero(xhci_dcbaa, sizeof(xhci_dcbaa));
    xhci_memzero(cmd_ring, sizeof(cmd_ring));
    xhci_memzero(evt_ring, sizeof(evt_ring));
    xhci_memzero(xhci_erst_raw, sizeof(xhci_erst_raw));
    xhci_memzero(input_ctx, sizeof(input_ctx));
    xhci_memzero(device_ctx, sizeof(device_ctx));
    xhci_memzero(ep0_ring, sizeof(ep0_ring));
    xhci_memzero(epi_ring, sizeof(epi_ring));

    /* Configure slot count — must exceed connected-port phantoms we abandon. */
    uint32_t slots = xhci_max_slots > MAX_SLOTS_USED ? MAX_SLOTS_USED : xhci_max_slots;
    if (slots < 8 && xhci_max_slots >= 8) slots = 8;
    xhci_write32(XHCI_CONFIG, (xhci_read32(XHCI_CONFIG) & ~0xFFU) | slots);
    xhci_print("xHCI: MaxSlotsEn=");
    xhci_print_hex32(slots);

    /* DCBAAP + optional scratchpad array at DCBAA[0]. */
    xhci_memzero(xhci_dcbaa, sizeof(xhci_dcbaa));
    if (xhci_scratch_count > 0) {
        uint32_t i;
        xhci_memzero(xhci_scratch_array, sizeof(xhci_scratch_array));
        for (i = 0; i < xhci_scratch_count; i++) {
            xhci_memzero(xhci_scratch_bufs[i], sizeof(xhci_scratch_bufs[i]));
            xhci_scratch_array[i] = (uint64_t)(uintptr_t)xhci_scratch_bufs[i];
        }
        xhci_dcbaa[0] = (uint64_t)(uintptr_t)xhci_scratch_array;
        xhci_print("xHCI: scratchpad DCBAA[0] armed.\n");
    } else {
        xhci_print("xHCI: no scratchpads required.\n");
    }
    xhci_write64(XHCI_DCBAAP, (uint64_t)(uintptr_t)xhci_dcbaa);

    /* Command ring: cycle bit + RCS=1.
     * Do NOT pre-install a Link TRB with ENT here — some HCs (QEMU nec-usb-xhci)
     * prefetch the ring at CRCR load and an ENT Link can trip HCE. */
    xhci_write64(XHCI_CRCR, ((uint64_t)(uintptr_t)cmd_ring) | 1ULL);

    /* Event ring: ERSTSZ, ERSTBA, ERDP for interrupter 0. */
    xhci_erst_raw[0] = (uint32_t)(uintptr_t)evt_ring; /* addr low */
    xhci_erst_raw[1] = (uint32_t)(((uint64_t)(uintptr_t)evt_ring) >> 32);
    xhci_erst_raw[2] = EVT_RING_SIZE; /* segment size in TRBs */
    xhci_erst_raw[3] = 0;
    rt_write32(0x20 + IR_ERSTSZ, 1);
    /* Low then high — QEMU calls xhci_er_reset on the high dword write. */
    rt_write32(0x20 + IR_ERSTBA, (uint32_t)(uintptr_t)xhci_erst_raw);
    rt_write32(0x20 + IR_ERSTBA + 4, 0);
    rt_write32(0x20 + IR_ERDP, (uint32_t)(uintptr_t)evt_ring);
    rt_write32(0x20 + IR_ERDP + 4, 0);
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
                /* Neutral write: never echo PED (RW1CS) or change bits. */
                xhci_write32(reg, xhci_port_write_value(v, XHCI_PORT_PP));
                powered++;
            }
        }
        if (powered > 0) {
            xhci_print("xHCI: asserted PORT_POWER on root-hub ports.\n");
            short_delay(20);   /* >= 20ms VBUS settle per USB spec */
        }
        /*
         * Kill USB2 HW LPM on every root port (PORTPMSC.HLE) and zero
         * PORTHLPMC. Bay Trail + LS/FS HID mice (AmazonBasics etc.) often
         * NACK the first SETUP when HLE left on after firmware/LPM quirks.
         */
        for (uint32_t i = 0; i < xhci_ports; i++) {
            uint32_t pmsc = XHCI_PORTSC_BASE + i * 0x10 + 0x4;
            uint32_t hlpmc = XHCI_PORTSC_BASE + i * 0x10 + 0xC;
            uint32_t v = xhci_read32(pmsc);
            if (v & XHCI_PORTPMSC_HLE)
                xhci_write32(pmsc, v & ~XHCI_PORTPMSC_HLE);
            xhci_write32(hlpmc, 0);
        }
        /* Clear stale Connect Status Change bits from boot-time noise. */
        for (uint32_t i = 0; i < xhci_ports; i++) {
            uint32_t reg = XHCI_PORTSC_BASE + i * 0x10;
            uint32_t v = xhci_read32(reg);
            xhci_write32(reg, xhci_port_write_value(v, XHCI_PORT_CSC));
        }
        /* Give the chipset another short window to report real connects. */
        short_delay(10);
        /* Drop any port-status events posted during power-on so the event
         * ring cannot fill before the first Enable Slot. */
        drain_events();
    }

    xhci_ready = 1;
    xhci_print("xHCI initialized (boot mouse path).\n");
    return 1;
}

void xhci_poll(void) {
    if (!xhci_ready) return;
    service_usbsts();
    if (xhci_fault) return;
    drain_events();
}

int xhci_controller_healthy(void) {
    return xhci_ready && !xhci_fault;
}

int xhci_port_count(void) {
    return xhci_ready ? (int)xhci_ports : 0;
}

int xhci_port_connected(int port) {
    uint32_t v;
    uint32_t speed;
    if (!xhci_ready || port < 0 || (uint32_t)port >= xhci_ports) return 0;
    v = xhci_read32(XHCI_PORTSC_BASE + (uint32_t)port * 0x10);
    if (!(v & XHCI_PORT_CCS)) return 0;
    /*
     * When Bay Trail XUSB2PR is 0, USB2 is owned by companion EHCI.
     * xHCI still latches CCS phantoms on FS/LS/HS roots; ignore them so we
     * don't burn slots on EP0 timeouts. SuperSpeed (4) still counts.
     * Use on_xhci (not only route_locked): HCRESET can clear PR after a
     * "successful" early route and leave locked=0 with PR=0.
     */
    if (baytrail_usb_is_soc() && !baytrail_usb_usb2_on_xhci()) {
        speed = (v >> 10) & 0xF;
        if (speed != 4) return 0;
    }
    return 1;
}

int xhci_port_low_speed(int port) {
    if (!xhci_ready || port < 0 || (uint32_t)port >= xhci_ports) return 0;
    uint32_t v = xhci_read32(XHCI_PORTSC_BASE + (uint32_t)port * 0x10);
    /* Port Speed in PORTSC[13:10]: 2 = Low, 1 = Full, 3 = High, 4 = Super. */
    uint32_t speed = (v >> 10) & 0xF;
    return speed == 2;
}

/* Raw PORTSC Port Speed field (0 if unknown / disconnected). */
int xhci_port_protocol_speed(int port) {
    if (!xhci_ready || port < 0 || (uint32_t)port >= xhci_ports) return 0;
    uint32_t v = xhci_read32(XHCI_PORTSC_BASE + (uint32_t)port * 0x10);
    if (!(v & XHCI_PORT_CCS)) return 0;
    return (int)((v >> 10) & 0xF);
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
    /* Clear CSC only via neutral write (never echo PED). */
    xhci_write32(reg, xhci_port_write_value(v, XHCI_PORT_CSC));
}

static uint32_t port_speed(int port) {
    uint32_t v = xhci_read32(XHCI_PORTSC_BASE + (uint32_t)port * 0x10);
    return (v >> 10) & 0xF;
}

static uint16_t default_max_packet_for_speed(uint32_t speed) {
    return xhci_default_ep0_mps(xhci_psi_to_slot_speed(speed));
}

static void reset_ep0_ring(void) {
    xhci_memzero(ep0_ring, sizeof(ep0_ring));
    ep0_idx = 0;
    ep0_cycle = 1;
}

static void log_portsc(const char* whentag, int port, uint32_t v);

/*
 * After EP0 timeout the endpoint is usually still Running (no completion), so
 * Reset Endpoint fails and Set TR Dequeue is illegal until Stopped. Sequence:
 * Stop Endpoint → Set TR Dequeue → fresh ring. Fall back to Reset EP if Stop
 * fails (already Halted/Error).
 */
static int recover_ep0_ring(void) {
    xhci_trb_t ev;
    uint64_t dq;
    int stopped = 0;

    if (!xhci_ready || xhci_fault || slot_id <= 0) return -1;

    enqueue_cmd(0, 0, 0,
                TRB_TYPE(TRB_STOP_EP) | (1U << 16) | ((uint32_t)slot_id << 24));
    if (wait_event(TRB_EV_CMD_COMPLETE, xhci_ms_to_ticks(100), &ev) >= 0 &&
        TRB_GET_CC(ev.status) == 1) {
        stopped = 1;
    } else {
        /* Already Halted/Error — try Reset Endpoint instead. */
        enqueue_cmd(0, 0, 0,
                    TRB_TYPE(TRB_RESET_EP) | (1U << 16) |
                    ((uint32_t)slot_id << 24));
        if (wait_event(TRB_EV_CMD_COMPLETE, xhci_ms_to_ticks(100), &ev) >= 0 &&
            TRB_GET_CC(ev.status) == 1) {
            stopped = 1;
        } else {
            xhci_print("xHCI: Stop/Reset Endpoint (EP0) failed.\n");
        }
    }

    reset_ep0_ring();
    dq = (uint64_t)(uintptr_t)ep0_ring | 1ULL; /* DCS = 1 */
    enqueue_cmd((uint32_t)dq, (uint32_t)(dq >> 32), 0,
                TRB_TYPE(TRB_SET_TR_DEQUEUE) | (1U << 16) |
                ((uint32_t)slot_id << 24));
    if (wait_event(TRB_EV_CMD_COMPLETE, xhci_ms_to_ticks(100), &ev) < 0) {
        xhci_print("xHCI: Set TR Dequeue (EP0) timeout.\n");
        return -1;
    }
    if (TRB_GET_CC(ev.status) != 1) {
        xhci_print("xHCI: Set TR Dequeue (EP0) failed cc=");
        xhci_print_hex32(TRB_GET_CC(ev.status));
        return -1;
    }
    timer_busy_wait_ms(stopped ? 20 : 40);
    return 0;
}

static void note_soft_fail(const char* why) {
    soft_fail_streak++;
    xhci_print(why);
    if (soft_fail_streak >= 24) {
        xhci_print("xHCI: repeated soft failures, poisoning controller.\n");
        xhci_fault = 1;
    }
}

/*
 * Local-only slot teardown. After EP0 Transaction Error (cc=4) on Bay Trail,
 * further xHCI commands (Disable Slot) can wedge the PCI fabric until the
 * platform WDT reboots (~1 min). Prefer this over disable_slot() on hard
 * transfer failures so enumeration can move to the next root port.
 */
/* Rebuild Input Slot Context so Address/Configure never see a wiped speed. */
static void fill_input_slot_ctx(uint32_t context_entries) {
    uint32_t* slot = (uint32_t*)ctx_at(input_ctx, 1);
    uint32_t entries = context_entries ? context_entries : 1U;
    slot[0] = xhci_slot_ctx_dword0(slot_spd, entries);
    if (cur_port >= 0)
        slot[1] = ((uint32_t)(cur_port + 1)) << 16;
    else
        slot[1] = 0;
    slot[2] = 0;
    slot[3] = 0;
}

static void abandon_slot(void) {
    int sid = slot_id;
    slot_id = 0;
    cur_port = -1;
    slot_spd = 0;
    usb_addr = 0;
    ep0_mps = 8;
    bulk_out_ep = 0;
    bulk_in_ep = 0;
    bulk_out_dci = 0;
    bulk_in_dci = 0;
    if (sid > 0 && sid < 256)
        xhci_dcbaa[sid] = 0;
    reset_ep0_ring();
    /* Also wipe EP-IN / bulk ring state so a later configure is clean. */
    xhci_memzero(epi_ring, sizeof(epi_ring));
    epi_idx = 0;
    epi_cycle = 1;
    xhci_memzero(bulk_out_ring, sizeof(bulk_out_ring));
    bulk_out_idx = 0;
    bulk_out_cycle = 1;
    xhci_memzero(bulk_in_ring, sizeof(bulk_in_ring));
    bulk_in_idx = 0;
    bulk_in_cycle = 1;
    intr_active = 0;
    intr_pending = 0;
    intr_failed = 0;
    intr_dci = 0;
    ep0_xfer_waiting = 0;
}

void xhci_abandon_slot(void) {
    abandon_slot();
}

uint8_t xhci_assigned_address(void) {
    return usb_addr;
}

int xhci_has_active_slot(void) {
    return xhci_ready && !xhci_fault && slot_id > 0;
}

int xhci_ep0_soft_fail_pending(void) {
    return ep0_soft_fail_pending;
}

void xhci_clear_ep0_soft_fail(void) {
    ep0_soft_fail_pending = 0;
}

/* Best-effort Disable Slot with a short timeout; always abandon locally. */
static int disable_slot(void) {
    xhci_trb_t ev;
    int sid = slot_id;
    if (sid <= 0) {
        abandon_slot();
        return 0;
    }
    enqueue_cmd(0, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((uint32_t)sid << 24));
    if (wait_event(TRB_EV_CMD_COMPLETE, xhci_ms_to_ticks(50), &ev) < 0) {
        xhci_print("xHCI: Disable Slot timeout (abandoning locally).\n");
    } else if (TRB_GET_CC(ev.status) != 1) {
        xhci_print("xHCI: Disable Slot failed cc=");
        xhci_print_hex32(TRB_GET_CC(ev.status));
    }
    abandon_slot();
    return 0;
}

/* After short device descriptor, raise EP0 MPS via Evaluate Context. */
static int evaluate_ep0_mps(uint16_t mps) {
    xhci_trb_t ev;
    uint32_t* icc;
    uint32_t* ep0;
    uint64_t ep0_ring_ptr;

    if (!xhci_ready || xhci_fault || slot_id <= 0) return -1;
    if (mps != 8 && mps != 16 && mps != 32 && mps != 64) return -1;
    if (mps == ep0_mps) return 0;

    xhci_memzero(input_ctx, sizeof(input_ctx));
    icc = (uint32_t*)ctx_at(input_ctx, 0);
    icc[0] = 0;
    icc[1] = (1U << 1); /* Drop Context? No — Add EP0 only for Evaluate */

    ep0 = (uint32_t*)ctx_at(input_ctx, 2);
    ep0[0] = 0;
    ep0[1] = (EP_TYPE_CONTROL << 3)
           | (3U << 1)
           | ((uint32_t)mps << 16);
    ep0_ring_ptr = (uint64_t)(uintptr_t)ep0_ring;
    ep0[2] = (uint32_t)ep0_ring_ptr | (ep0_cycle ? 1U : 0U);
    ep0[3] = (uint32_t)(ep0_ring_ptr >> 32);
    ep0[4] = 8;

    enqueue_cmd((uint32_t)(uintptr_t)input_ctx, 0, 0,
                TRB_TYPE(TRB_EVALUATE_CTX) | ((uint32_t)slot_id << 24));
    if (wait_event(TRB_EV_CMD_COMPLETE, xhci_ms_to_ticks(250), &ev) < 0) {
        note_soft_fail("xHCI: Evaluate Context (EP0 MPS) timeout.\n");
        return -1;
    }
    if (TRB_GET_CC(ev.status) != 1) {
        xhci_print("xHCI: Evaluate Context (EP0 MPS) failed.\n");
        return -1;
    }
    ep0_mps = mps;
    xhci_print("xHCI: EP0 max packet updated to ");
    {
        char b[8];
        int n = (int)mps;
        int i = 0;
        if (n >= 100) { b[i++] = '0' + n / 100; n %= 100; b[i++] = '0' + n / 10; b[i++] = '0' + n % 10; }
        else if (n >= 10) { b[i++] = '0' + n / 10; b[i++] = '0' + n % 10; }
        else b[i++] = '0' + n;
        b[i++] = '\n';
        b[i] = 0;
        xhci_print(b);
    }
    return 0;
}

/* Issue Enable Slot and address the device into Default state. */
static int activate_slot(int port, uint32_t speed) {
    xhci_trb_t ev;
    uint16_t mps;

    /* Free previous slot without risking a wedging Disable Slot command. */
    abandon_slot();
    if (xhci_fault) return -1;

    /* Consume port-status noise before the first command completion. */
    drain_events();

    enqueue_cmd(0, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT));
    if (wait_event(TRB_EV_CMD_COMPLETE, xhci_ms_to_ticks(500), &ev) < 0) {
        note_soft_fail("xHCI: Enable Slot timeout.\n");
        return -1;
    }
    if (TRB_GET_CC(ev.status) != 1) {
        uint8_t cc = (uint8_t)TRB_GET_CC(ev.status);
        xhci_print("xHCI: Enable Slot failed cc=");
        xhci_print_hex32(cc);
        if (cc == XHCI_CC_NO_SLOTS)
            xhci_print("xHCI: No Slots Available (raise MaxSlotsEn / reclaim).\n");
        return -1;
    }
    slot_id = TRB_SLOT(ev.control);
    if (slot_id <= 0) {
        xhci_print("xHCI: Enable Slot returned invalid slot.\n");
        return -1;
    }

    xhci_print("xHCI: slot=");
    {
        char b[8]; int n = slot_id; int i = 0;
        if (n >= 10) { b[i++] = '0' + n / 10; b[i++] = '0' + n % 10; }
        else b[i++] = '0' + n;
        b[i++] = ' '; b[i] = 0; xhci_print(b);
    }
    xhci_print("port=");
    {
        char b[8]; int n = port; int i = 0;
        if (n >= 10) { b[i++] = '0' + n / 10; b[i++] = '0' + n % 10; }
        else b[i++] = '0' + n;
        b[i++] = ' '; b[i] = 0; xhci_print(b);
    }
    xhci_print("speed=");
    xhci_print_hex32(speed);

    /* Build input context (0=ICC, 1=Slot, 2=EP0). */
    xhci_memzero(input_ctx, sizeof(input_ctx));
    /* Input Control Context: enable A0 (slot) and A1 (EP0). */
    uint32_t* icc = (uint32_t*)ctx_at(input_ctx, 0);
    icc[1] = (1U << 0) | (1U << 1);  /* add slot + EP0 */

    /* Slot context — map PORTSC PSI to Slot SPD via helper (persist for rebuilds). */
    slot_spd = xhci_psi_to_slot_speed(speed);
    cur_port = port;
    fill_input_slot_ctx(1); /* context entries = 1 (EP0) */

    /* EP0 context — FS/LS start at 8 until short descriptor updates MPS. */
    uint32_t* ep0 = (uint32_t*)ctx_at(input_ctx, 2);
    mps = xhci_default_ep0_mps(slot_spd);
    ep0_mps = mps;
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
        note_soft_fail("xHCI: Address Device (default) timeout.\n");
        abandon_slot();
        return -1;
    }
    if (TRB_GET_CC(ev.status) != 1) {
        xhci_print("xHCI: Address Device (default) failed.\n");
        abandon_slot();
        return -1;
    }

    /*
     * USB 2.0 recovery after reset is >=10 ms. LS mice on Bay Trail need more
     * settle after Address Device (BSR=1) before the first SETUP, but keep it
     * short so PED does not race away (PLS=Polling).
     */
    timer_busy_wait_ms(slot_spd == 2U ? 50 : 10);

    usb_addr = 0;
    return 0;
}

static void log_portsc(const char* whentag, int port, uint32_t v) {
    uint32_t speed = (v >> 10) & 0xFU;
    uint32_t pls = (v >> 5) & 0xFU;
    xhci_print("xHCI: PORTSC ");
    xhci_print(whentag);
    xhci_print(" port=");
    {
        char b[4];
        if (port >= 10) { b[0] = '0' + port / 10; b[1] = '0' + port % 10; b[2] = 0; }
        else { b[0] = '0' + port; b[1] = 0; }
        xhci_print(b);
    }
    xhci_print(" raw=");
    xhci_print_hex32(v);
    xhci_print("xHCI: PORTSC flags CCS=");
    xhci_print((v & XHCI_PORT_CCS) ? "1" : "0");
    xhci_print(" PP=");
    xhci_print((v & XHCI_PORT_PP) ? "1" : "0");
    xhci_print(" PED=");
    xhci_print((v & XHCI_PORT_PED) ? "1" : "0");
    xhci_print(" PR=");
    xhci_print((v & XHCI_PORT_PR) ? "1" : "0");
    xhci_print(" speed=");
    xhci_print_hex32(speed);
    xhci_print("xHCI: PORTSC PLS=");
    xhci_print_hex32(pls);
}

/*
 * On-hardware EP0 diagnostic. QEMU always completes EP0, so this only matters
 * on real Bay Trail. Prints:
 *   MFINDEX advance  — is the controller running its USB frame schedule?
 *   USBCMD / USBSTS  — R/S, HCH (halted), HSE/HCE (fatal)
 *   EP0 out-ctx      — EP State (0=Disabled 1=Running 2=Halted 3=Stopped 4=Error)
 *                      and TR Dequeue Pointer (did the HC advance past SETUP?)
 * If MFINDEX does not advance, the PHY/clock is not driving the port (no SOF)
 * and no device can ever answer EP0 — a PHY problem, not a ring problem.
 */
static void dump_ep0_diag(const char* tag) {
    uint32_t mf1 = rt_read32(0x00) & 0x3FFFU;
    uint32_t cmd, sts, mf2;
    timer_busy_wait_ms(2);
    mf2 = rt_read32(0x00) & 0x3FFFU;
    cmd = xhci_read32(XHCI_USBCMD);
    sts = xhci_read32(XHCI_USBSTS);
    xhci_print("xHCI: EP0DIAG ");
    xhci_print(tag);
    xhci_print(" MFINDEX ");
    xhci_print_hex32(mf1);
    xhci_print("xHCI: EP0DIAG MFINDEX2 ");
    xhci_print_hex32(mf2);
    xhci_print(mf2 != mf1 ? "xHCI: EP0DIAG schedule RUNNING\n"
                          : "xHCI: EP0DIAG schedule STALLED (PHY/clock?)\n");
    xhci_print("xHCI: EP0DIAG USBCMD ");
    xhci_print_hex32(cmd);
    xhci_print("xHCI: EP0DIAG USBSTS ");
    xhci_print_hex32(sts);
    if (slot_id > 0) {
        uint32_t* ep0 = (uint32_t*)ctx_at(device_ctx, 1); /* output EP0 ctx */
        xhci_print("xHCI: EP0DIAG epstate ");
        xhci_print_hex32(ep0[0] & 0x7U);
        xhci_print("xHCI: EP0DIAG epdeq ");
        xhci_print_hex32(ep0[2]);
    }
    /* Transfer-ring vs event-ring visibility: did the HC advance past SETUP,
     * and is a completion event sitting unconsumed (cycle desync)? */
    xhci_print("xHCI: EP0DIAG ep0ring ");
    xhci_print_hex32((uint32_t)(uintptr_t)ep0_ring);
    xhci_print("xHCI: EP0DIAG setupctl ");
    xhci_print_hex32(ep0_ring[0].control);
    xhci_print("xHCI: EP0DIAG evtidx ");
    xhci_print_hex32(evt_idx);
    xhci_print("xHCI: EP0DIAG evtcyc ");
    xhci_print_hex32((uint32_t)evt_cycle);
    {
        /* Dump control/status of the event TRB we are parked on + next one. */
        uint32_t i0 = evt_idx;
        uint32_t i1 = (evt_idx + 1) % EVT_RING_SIZE;
        xhci_print("xHCI: EP0DIAG ev0ctl ");
        xhci_print_hex32(evt_ring[i0].control);
        xhci_print("xHCI: EP0DIAG ev0sts ");
        xhci_print_hex32(evt_ring[i0].status);
        xhci_print("xHCI: EP0DIAG ev1ctl ");
        xhci_print_hex32(evt_ring[i1].control);
    }
}

/* Pulse PR and wait for PRC+PED. Returns PORTSC on success, 0 on failure. */
static uint32_t xhci_pulse_port_reset(int port) {
    uint32_t reg = XHCI_PORTSC_BASE + (uint32_t)port * 0x10;
    uint32_t v = xhci_read32(reg);
    uint64_t deadline;
    uint32_t guard;

    if (!(v & XHCI_PORT_CCS)) return 0;
    if (!(v & XHCI_PORT_PP)) {
        xhci_write32(reg, xhci_port_write_value(v, XHCI_PORT_PP));
        timer_busy_wait_ms(20);
        v = xhci_read32(reg);
    }
    /* Start reset without echoing PED (which would disable the port). */
    xhci_write32(reg, xhci_port_write_value(v, XHCI_PORT_PR));
    deadline = timer_deadline_ms(200);
    guard = XHCI_SPIN_CEILING;
    while (!timer_deadline_expired(deadline)) {
        if (--guard == 0) break;
        drain_events();
        v = xhci_read32(reg);
        if ((v & XHCI_PORT_PRC) && (v & XHCI_PORT_PED)) {
            /*
             * Ack PRC only. Legacy path wrote (v & ~change)|PRC which still
             * contained PED=1; PED is RW1CS so that write disabled the port
             * (PED=0, PLS=Polling) — the Lenovo failure mode.
             */
            xhci_write32(reg, xhci_port_write_value(v, XHCI_PORT_PRC));
            /* USB2 ports: clear HW LPM so first SETUP is not NACKed. */
            {
                uint32_t pmsc = reg + 0x4U;
                uint32_t p = xhci_read32(pmsc);
                if (p & XHCI_PORTPMSC_HLE)
                    xhci_write32(pmsc, p & ~XHCI_PORTPMSC_HLE);
            }
            return xhci_read32(reg);
        }
    }
    return 0;
}

int xhci_port_reset(int port) {
    int attempt;

    if (!xhci_ready || xhci_fault || port < 0 || (uint32_t)port >= xhci_ports)
        return -1;

    log_portsc("reset-entry", port,
               xhci_read32(XHCI_PORTSC_BASE + (uint32_t)port * 0x10));
    if (!(xhci_read32(XHCI_PORTSC_BASE + (uint32_t)port * 0x10) & XHCI_PORT_CCS)) {
        xhci_print("xHCI: port reset aborted (no CCS).\n");
        abandon_slot();
        return -1;
    }

    /*
     * Bay Trail often enables PED after PR then drops it (PLS=Polling) within
     * ~20-50 ms. Long software delays made that inevitable. Keep the
     * reset→Address Device→EP0 window tight; retry one PR if PED races.
     */
    for (attempt = 0; attempt < 2; attempt++) {
        uint32_t after;
        uint32_t speed;
        uint32_t now;

        after = xhci_pulse_port_reset(port);
        if (!after) {
            log_portsc("reset-timeout", port,
                       xhci_read32(XHCI_PORTSC_BASE + (uint32_t)port * 0x10));
            xhci_print("xHCI: port reset did not complete.\n");
            abandon_slot();
            return -1;
        }
        log_portsc(attempt ? "reset-exit-retry" : "reset-exit", port, after);

        /* USB 2.0 §7.1.7.5: 10 ms recovery — do not linger. */
        timer_busy_wait_ms(10);
        now = xhci_read32(XHCI_PORTSC_BASE + (uint32_t)port * 0x10);
        if (!(now & XHCI_PORT_PED) || !(now & XHCI_PORT_CCS)) {
            xhci_print("xHCI: PED raced away before Address Device.\n");
            log_portsc("pre-addr-race", port, now);
            ep0_soft_fail_pending = 1;
            abandon_slot();
            if (attempt == 0 && (now & XHCI_PORT_CCS)) continue;
            return -1;
        }

        speed = port_speed(port);
        if (activate_slot(port, speed) != 0) {
            xhci_print("xHCI: activate_slot failed after reset.\n");
            abandon_slot();
            return -1;
        }

        now = xhci_read32(XHCI_PORTSC_BASE + (uint32_t)port * 0x10);
        if ((now & XHCI_PORT_PED) && (now & XHCI_PORT_CCS) && slot_id > 0)
            return 0;

        xhci_print("xHCI: PED/CCS lost after Address Device (default).\n");
        log_portsc("post-addr-lost", port, now);
        ep0_soft_fail_pending = 1;
        abandon_slot();
        if (attempt == 0 && (now & XHCI_PORT_CCS)) {
            xhci_print("xHCI: one fast PR retry while CCS still latched.\n");
            continue;
        }
        return -1;
    }
    return -1;
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

/* Require CCS+PED and clear HW LPM before ringing EP0. */
static int ep0_port_ready(void) {
    uint32_t reg, v, pmsc, p;
    if (cur_port < 0) return 0;
    reg = XHCI_PORTSC_BASE + (uint32_t)cur_port * 0x10;
    v = xhci_read32(reg);
    if (!(v & XHCI_PORT_CCS) || !(v & XHCI_PORT_PED)) {
        log_portsc("before-ep0-not-ready", cur_port, v);
        return 0;
    }
    pmsc = reg + 0x4U;
    p = xhci_read32(pmsc);
    if (p & XHCI_PORTPMSC_HLE)
        xhci_write32(pmsc, p & ~XHCI_PORTPMSC_HLE);
    return 1;
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
    if (!xhci_ready || xhci_fault || !setup_pkt) return -1;
    if (slot_id == 0) {
        xhci_print("xHCI: EP0 rejected (no active slot).\n");
        return -1;
    }

    uint8_t bRequest = setup_pkt[1];
    /* SET_ADDRESS: re-issue Address Device with BSR=0.
     * xHCI 1.1 §4.6.5: Input Slot Context Device Address shall be 0; the HC
     * assigns the address and writes it to the Output Context. Packing a
     * software-chosen address here made Bay Trail fail Address Device (set).
     */
    if (bRequest == 0x05 /* SET_ADDRESS */) {
        uint32_t* out_slot;
        /* Mark the input control context to update slot ctx + EP0 ctx. */
        uint32_t* icc = (uint32_t*)ctx_at(input_ctx, 0);
        icc[0] = 0;
        icc[1] = (1U << 0) | (1U << 1);

        /*
         * evaluate_ep0_mps() zeroes the whole input context. Rebuild a valid
         * Slot Context every Address Device (BSR=0) — SPD=0 wedges Bay Trail.
         * Leave Device Address = 0 (fill_input_slot_ctx only sets Root Hub Port).
         */
        fill_input_slot_ctx(1);

        /* Keep EP0 MPS from short descriptor in the Address Device context. */
        {
            uint32_t* ep0 = (uint32_t*)ctx_at(input_ctx, 2);
            uint64_t ep0_ring_ptr = (uint64_t)(uintptr_t)ep0_ring;
            ep0[0] = 0;
            ep0[1] = (EP_TYPE_CONTROL << 3)
                   | (3U << 1)
                   | ((uint32_t)ep0_mps << 16);
            ep0[2] = (uint32_t)ep0_ring_ptr | (ep0_cycle ? 1U : 0U);
            ep0[3] = (uint32_t)(ep0_ring_ptr >> 32);
            ep0[4] = 8;
        }

        /* Short-desc → Address Device: give LS mice a beat to settle. */
        timer_busy_wait_ms(slot_spd == 2U ? 20 : 10);

        if (cur_port >= 0 && !ep0_port_ready()) {
            note_soft_fail("xHCI: Address Device (set) aborted (port not Enabled).\n");
            abandon_slot();
            return -1;
        }

        enqueue_cmd((uint32_t)(uintptr_t)input_ctx, 0, 0,
                    TRB_TYPE(TRB_ADDRESS_DEVICE) | ((uint32_t)slot_id << 24));
        xhci_trb_t ev;
        if (wait_event(TRB_EV_CMD_COMPLETE, xhci_ms_to_ticks(250), &ev) < 0) {
            note_soft_fail("xHCI: Address Device (set) timeout.\n");
            abandon_slot();
            return -1;
        }
        if (TRB_GET_CC(ev.status) != 1) {
            xhci_print("xHCI: Address Device (set) failed cc=");
            xhci_print_hex32(TRB_GET_CC(ev.status));
            if (cur_port >= 0)
                log_portsc("after-addr-set-fail", cur_port,
                           xhci_read32(XHCI_PORTSC_BASE + (uint32_t)cur_port * 0x10));
            abandon_slot();
            return -1;
        }
        /* Prefer HC-assigned address from Output Slot Context (index 0),
         * USB Device Address = dword3 bits [7:0]. */
        out_slot = (uint32_t*)ctx_at(device_ctx, 0);
        usb_addr = (uint8_t)(out_slot[3] & 0xFFU);
        if (usb_addr == 0)
            usb_addr = setup_pkt[2]; /* fall back to software request */
        (void)dev_addr;
        return 0;
    }

    /* Build SETUP / DATA / STATUS TRBs as one TD (Chain on all but Status).
     *
     * Intel HCs are picky: do not publish the first TRB's Cycle bit until the
     * whole TD is written (u-boot/Linux giveback_first_trb), then barrier +
     * doorbell. Leaving Cycle wrong on TRB0 while writing TRB1/2 races the HC.
     */
    uint32_t setup_lo = ((uint32_t)setup_pkt[0])
                      | ((uint32_t)setup_pkt[1] << 8)
                      | ((uint32_t)setup_pkt[2] << 16)
                      | ((uint32_t)setup_pkt[3] << 24);
    uint32_t setup_hi = ((uint32_t)setup_pkt[4])
                      | ((uint32_t)setup_pkt[5] << 8)
                      | ((uint32_t)setup_pkt[6] << 16)
                      | ((uint32_t)setup_pkt[7] << 24);

    uint32_t trt;
    uintptr_t status_trb_ptr = 0;
    if (data_len == 0) trt = TRB_TRT_NO_DATA;
    else trt = direction_in ? TRB_TRT_IN : TRB_TRT_OUT;

    uint32_t start_idx = ep0_idx;
    uint32_t start_cycle = ep0_cycle;

    /* SETUP with inverted cycle so HC ignores it until giveback. */
    {
        xhci_trb_t* trb = &ep0_ring[ep0_idx];
        trb->param_low = setup_lo;
        trb->param_high = setup_hi;
        trb->status = 8;
        trb->control = (TRB_TYPE(TRB_SETUP_STAGE) | TRB_IDT | TRB_CHAIN | trt)
                     | (start_cycle ? 0u : TRB_CYCLE);
        ep0_idx++;
        if (ep0_idx >= EP_RING_SIZE - 1) {
            xhci_trb_t* link = &ep0_ring[EP_RING_SIZE - 1];
            link->param_low = (uint32_t)(uintptr_t)ep0_ring;
            link->param_high = 0;
            link->status = 0;
            link->control = TRB_TYPE(TRB_LINK) | TRB_ENT | (ep0_cycle ? TRB_CYCLE : 0);
            ep0_idx = 0;
            ep0_cycle ^= 1;
        }
    }

    if (data_len > 0) {
        if (data_len > sizeof(xfer_data)) data_len = sizeof(xfer_data);
        if (!direction_in && data) {
            for (uint16_t i = 0; i < data_len; i++) xfer_data[i] = data[i];
        }
        push_ring(ep0_ring, &ep0_idx, &ep0_cycle,
                  (uint32_t)(uintptr_t)xfer_data, 0, data_len,
                  TRB_TYPE(TRB_DATA_STAGE) | TRB_CHAIN
                  | (direction_in ? TRB_DIR_IN : 0));
    }
    {
        xhci_trb_t* st = push_ring(ep0_ring, &ep0_idx, &ep0_cycle, 0, 0, 0,
              TRB_TYPE(TRB_STATUS_STAGE) | TRB_IOC
              | (direction_in ? 0 : TRB_DIR_IN));
        status_trb_ptr = (uintptr_t)st;
    }

    /* Publish Setup Stage TRB (flip Cycle to start_cycle). */
    {
        xhci_trb_t* trb = &ep0_ring[start_idx];
        if (start_cycle)
            trb->control |= TRB_CYCLE;
        else
            trb->control &= ~TRB_CYCLE;
    }
    if (!ep0_port_ready()) {
        note_soft_fail("xHCI: EP0 aborted (port not Enabled).\n");
        abandon_slot();
        return -1;
    }
    __asm__ __volatile__("mfence" ::: "memory");
    ring_ep_doorbell(1); /* EP0 doorbell value = 1 */

    xhci_trb_t ev;
    int ep0_attempt;
    /* LS mice need longer EP0 windows; larger config descriptors more still. */
    uint32_t ep0_wait_ms = (slot_spd == 2U) ? 450U : 300U;
    if (data_len > 8U)
        ep0_wait_ms += ((uint32_t)data_len / 8U) * 40U;
    if (ep0_wait_ms > 900U) ep0_wait_ms = 900U;

    for (ep0_attempt = 0; ep0_attempt < 2; ep0_attempt++) {
        int do_retry = 0;
        if (wait_ep0_transfer(xhci_ms_to_ticks(ep0_wait_ms), status_trb_ptr,
                              &ev) < 0) {
            /*
             * Lenovo AmazonBasics path often times out once on the first
             * multi-packet CONFIG descriptor after Address Device. Recover
             * the halted EP0 ring and retry before abandoning the slot —
             * Disable Slot after a partial enum can hang Bay Trail PCI.
             */
            xhci_print("xHCI: EP0 transfer timeout");
            if (cur_port >= 0)
                log_portsc("after-ep0-timeout", cur_port,
                           xhci_read32(XHCI_PORTSC_BASE + (uint32_t)cur_port * 0x10));
            if (ep0_attempt == 0)
                dump_ep0_diag("timeout");
            if (ep0_attempt == 0 && recover_ep0_ring() == 0) {
                xhci_print(" — soft-recover, retrying control TD.\n");
                do_retry = 1;
            } else {
                note_soft_fail(" (abandoning slot).\n");
                ep0_soft_fail_pending = 1;
                abandon_slot();
                return -1;
            }
        } else {
            uint8_t cc = (uint8_t)TRB_GET_CC(ev.status);
            if (cc == 1 || cc == 13 /* Short */) break;
            /* cc=4 USB Transaction Error — device NACK/timeout on the wire. */
            xhci_print("xHCI: EP0 transfer failed cc=");
            xhci_print_hex32(cc);
            if (cur_port >= 0)
                log_portsc("after-ep0-fail", cur_port,
                           xhci_read32(XHCI_PORTSC_BASE + (uint32_t)cur_port * 0x10));
            if ((cc == 4 || cc == 6 /* Stall */) && ep0_attempt == 0 &&
                recover_ep0_ring() == 0) {
                /* Stall on phantoms: clear halt once; still useful for LS glitch. */
                xhci_print("xHCI: soft-recover EP0, retrying control TD.\n");
                do_retry = 1;
            } else {
                note_soft_fail("xHCI: abandoning slot after EP0 error.\n");
                if (cc == 4) ep0_soft_fail_pending = 1;
                abandon_slot();
                return -1;
            }
        }

        if (do_retry) {
            /* Rebuild the same SETUP/DATA/STATUS TD after ring reset. */
            start_idx = ep0_idx;
            start_cycle = ep0_cycle;
            {
                xhci_trb_t* trb = &ep0_ring[ep0_idx];
                trb->param_low = setup_lo;
                trb->param_high = setup_hi;
                trb->status = 8;
                trb->control = (TRB_TYPE(TRB_SETUP_STAGE) | TRB_IDT | TRB_CHAIN | trt)
                             | (start_cycle ? 0u : TRB_CYCLE);
                ep0_idx++;
                if (ep0_idx >= EP_RING_SIZE - 1) {
                    xhci_trb_t* link = &ep0_ring[EP_RING_SIZE - 1];
                    link->param_low = (uint32_t)(uintptr_t)ep0_ring;
                    link->param_high = 0;
                    link->status = 0;
                    link->control = TRB_TYPE(TRB_LINK) | TRB_ENT |
                                    (ep0_cycle ? TRB_CYCLE : 0);
                    ep0_idx = 0;
                    ep0_cycle ^= 1;
                }
            }
            if (data_len > 0) {
                push_ring(ep0_ring, &ep0_idx, &ep0_cycle,
                          (uint32_t)(uintptr_t)xfer_data, 0, data_len,
                          TRB_TYPE(TRB_DATA_STAGE) | TRB_CHAIN
                          | (direction_in ? TRB_DIR_IN : 0));
            }
            {
                xhci_trb_t* st = push_ring(ep0_ring, &ep0_idx, &ep0_cycle, 0, 0, 0,
                      TRB_TYPE(TRB_STATUS_STAGE) | TRB_IOC
                      | (direction_in ? 0 : TRB_DIR_IN));
                status_trb_ptr = (uintptr_t)st;
            }
            {
                xhci_trb_t* trb = &ep0_ring[start_idx];
                if (start_cycle) trb->control |= TRB_CYCLE;
                else trb->control &= ~TRB_CYCLE;
            }
            if (!ep0_port_ready()) {
                note_soft_fail("xHCI: EP0 retry aborted (port not Enabled).\n");
                abandon_slot();
                return -1;
            }
            __asm__ __volatile__("mfence" ::: "memory");
            ring_ep_doorbell(1);
            continue;
        }
    }
    if (direction_in && data && data_len > 0) {
        for (uint16_t i = 0; i < data_len; i++) data[i] = xfer_data[i];
    }

    /*
     * After the 8-byte device descriptor, raise EP0 MPS before full
     * descriptor / config reads (required for many FS devices).
     */
    if (bRequest == 0x06 /* GET_DESCRIPTOR */ &&
        setup_pkt[3] == 0x01 /* DEVICE */ &&
        direction_in && data && data_len >= 8) {
        uint8_t mps = data[7];
        if (mps == 8 || mps == 16 || mps == 32 || mps == 64)
            (void)evaluate_ep0_mps(mps);
    }
    return 0;
}

int xhci_configure_bulk_eps(uint8_t ep_out, uint8_t ep_in,
                            uint16_t mps_out, uint16_t mps_in) {
    xhci_trb_t ev;
    uint8_t out_num = ep_out & 0x0F;
    uint8_t in_num = ep_in & 0x0F;
    uint32_t dci_out = out_num * 2;
    uint32_t dci_in = in_num * 2 + 1;
    uint32_t* icc;
    uint32_t* slot;
    uint32_t* ep;
    uint32_t max_dci;

    if (!xhci_ready || xhci_fault || slot_id <= 0) return -1;
    if (out_num == 0 || in_num == 0) return -1;
    if (dci_out == 0 || dci_out >= XHCI_MAX_CONTEXTS) return -1;
    if (dci_in == 0 || dci_in >= XHCI_MAX_CONTEXTS) return -1;
    if (mps_out == 0) mps_out = 64;
    if (mps_in == 0) mps_in = 64;

    xhci_memzero(bulk_out_ring, sizeof(bulk_out_ring));
    bulk_out_idx = 0;
    bulk_out_cycle = 1;
    xhci_memzero(bulk_in_ring, sizeof(bulk_in_ring));
    bulk_in_idx = 0;
    bulk_in_cycle = 1;

    max_dci = dci_out > dci_in ? dci_out : dci_in;

    xhci_memzero(input_ctx, sizeof(input_ctx));
    icc = (uint32_t*)ctx_at(input_ctx, 0);
    icc[0] = 0;
    icc[1] = (1U << 0) | (1U << dci_out) | (1U << dci_in);

    slot = (uint32_t*)ctx_at(input_ctx, 1);
    slot[0] = (slot[0] & ~(0x1FU << 27)) | ((max_dci & 0x1FU) << 27);

    ep = (uint32_t*)ctx_at(input_ctx, 1 + dci_out);
    ep[0] = 0;
    ep[1] = (EP_TYPE_BULK_OUT << 3) | (3U << 1) | ((uint32_t)mps_out << 16);
    {
        uint64_t ring_ptr = (uint64_t)(uintptr_t)bulk_out_ring;
        ep[2] = (uint32_t)ring_ptr | 1U;
        ep[3] = (uint32_t)(ring_ptr >> 32);
    }
    ep[4] = mps_out;

    ep = (uint32_t*)ctx_at(input_ctx, 1 + dci_in);
    ep[0] = 0;
    ep[1] = (EP_TYPE_BULK_IN << 3) | (3U << 1) | ((uint32_t)mps_in << 16);
    {
        uint64_t ring_ptr = (uint64_t)(uintptr_t)bulk_in_ring;
        ep[2] = (uint32_t)ring_ptr | 1U;
        ep[3] = (uint32_t)(ring_ptr >> 32);
    }
    ep[4] = mps_in;

    enqueue_cmd((uint32_t)(uintptr_t)input_ctx, 0, 0,
                TRB_TYPE(TRB_CONFIGURE_EP) | ((uint32_t)slot_id << 24));
    if (wait_event(TRB_EV_CMD_COMPLETE, xhci_ms_to_ticks(250), &ev) < 0) {
        note_soft_fail("xHCI: Configure bulk EP timeout.\n");
        return -1;
    }
    if (TRB_GET_CC(ev.status) != 1) {
        xhci_print("xHCI: Configure bulk EP failed cc=");
        xhci_print_hex32(TRB_GET_CC(ev.status));
        return -1;
    }

    bulk_out_ep = ep_out;
    bulk_in_ep = ep_in;
    bulk_out_dci = dci_out;
    bulk_in_dci = dci_in;
    xhci_print("xHCI: bulk endpoints configured.\n");
    return 0;
}

int xhci_bulk_transfer(uint8_t endpoint, uint8_t* data, uint16_t data_len,
                       int direction_in) {
    xhci_trb_t ev;
    uint32_t dci;
    xhci_trb_t* ring;
    uint32_t* idx;
    uint32_t* cycle;
    uint16_t i;

    if (!xhci_ready || xhci_fault || slot_id <= 0 || !data || data_len == 0)
        return -1;
    if (data_len > sizeof(bulk_xfer_data)) data_len = sizeof(bulk_xfer_data);

    if (direction_in) {
        if ((endpoint & 0x0F) != (bulk_in_ep & 0x0F) || bulk_in_dci == 0)
            return -1;
        dci = bulk_in_dci;
        ring = bulk_in_ring;
        idx = &bulk_in_idx;
        cycle = &bulk_in_cycle;
    } else {
        if ((endpoint & 0x0F) != (bulk_out_ep & 0x0F) || bulk_out_dci == 0)
            return -1;
        dci = bulk_out_dci;
        ring = bulk_out_ring;
        idx = &bulk_out_idx;
        cycle = &bulk_out_cycle;
        for (i = 0; i < data_len; i++) bulk_xfer_data[i] = data[i];
    }

    push_ring(ring, idx, cycle,
              (uint32_t)(uintptr_t)bulk_xfer_data, 0, data_len,
              TRB_TYPE(TRB_NORMAL) | TRB_IOC | (direction_in ? TRB_ISP : 0));
    ring_ep_doorbell(dci);

    if (wait_event(TRB_EV_TRANSFER, xhci_ms_to_ticks(1000), &ev) < 0) {
        note_soft_fail("xHCI: bulk transfer timeout (abandoning slot).\n");
        abandon_slot();
        return -1;
    }
    {
        uint8_t cc = TRB_GET_CC(ev.status);
        if (cc != 1 && cc != 13) {
            xhci_print("xHCI: bulk transfer failed cc=");
            xhci_print_hex32(cc);
            note_soft_fail("xHCI: abandoning slot after bulk error.\n");
            abandon_slot();
            return -1;
        }
    }
    if (direction_in) {
        for (i = 0; i < data_len; i++) data[i] = bulk_xfer_data[i];
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

    /* Always rebuild slot (do not OR into a wiped/stale input_ctx). */
    fill_input_slot_ctx(dci);

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
        note_soft_fail("xHCI: Configure EP timeout.\n");
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

    intr_dci = dci;
    intr_active = 1;
    intr_pending = 0;
    intr_failed = 0;
    return 0;
}

void xhci_remove_interrupt(void) {
    intr_active = 0;
    intr_pending = 0;
    intr_failed = 0;
    intr_dci = 0;
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
