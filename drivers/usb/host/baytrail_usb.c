#include "baytrail_usb.h"
#include "../../pci/pci.h"
#include "../../pci/iosf_mbi.h"
#include "../../io/io.h"
#include "../../diagnostics/driver_log.h"
#include "../../timer/timer.h"

extern void print(const char* str);

/* Fixed Bay Trail PMC / ACPI / EHCI locations (coreboot iomap + pci_devs). */
#define BYT_PMC_BASE           0xFED03000U
#define BYT_PMC_FUNC_DIS       0x34U
#define BYT_EHCI_DIS           (1U << 18)
#define BYT_ACPI_BASE          0x0400U
#define BYT_UPRWC              0x3CU
#define BYT_UPRWC_WR_EN        (1U << 1)
#define BYT_EHCI_SLOT          0x1DU
#define BYT_EHCI_FUNC          0U
#define BYT_EHCI_DID           0x0F34U
/* coreboot TEMP_BASE — unused high MMIO for a freshly-unhidden EHCI BAR. */
#define BYT_EHCI_BAR_FALLBACK  0xFD000000U

#define BYT_XHCI_USB2PR        0xD0U
#define BYT_XHCI_USB2PRM       0xD4U
#define BYT_XHCI_USB3PR        0xD8U
#define BYT_XHCI_USB3PRM       0xDCU
#define BYT_XHCI_USB2PDO       0xE4U
#define BYT_XHCI_USB3PDO       0xE8U

/*
 * Lenovo 80M4 readbacks show XUSB2PRM=0x3F (six USB2 roots). Writing only
 * 0xF left upper ports unrouted on some boots; use the full BYT mask.
 */
#define BYT_USB2_PORT_MAP      0x3FU
#define BYT_USB3_PORT_MAP      0x1U

#define PCI_CMD_INTX_DISABLE   (1U << 10)

static int g_is_byt = 0;
static int g_ehci_seen = 0;
static int g_ehci_unhid = 0;
static int g_has_ls_companion = 0;
static uint32_t g_func_dis_before = 0;
static uint32_t g_func_dis_after = 0;
static uint32_t g_ehci_id = 0;
static uint32_t g_ehci_bar0 = 0;
static uint32_t g_xusb2pr = 0;
static uint32_t g_xusb2prm = 0;
static uint32_t g_usb2pdo = 0;
static uint32_t g_usb3pdo = 0;
static int g_route_noted = 0;
static int g_route_locked = 0;
static int g_prefer_xhci = 0;
static uint8_t g_xhci_bus = 0;
static uint8_t g_xhci_slot = 0;
static uint8_t g_xhci_func = 0;
static uint16_t g_xhci_did = 0;
static int g_xhci_found = 0;
static int g_is_braswell = 0;

static void byt_log(const char* s) {
    driver_log(s);
    print(s);
}

static void byt_log_hex32(uint32_t v) {
    char buf[11];
    const char* hex = "0123456789ABCDEF";
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 8; i++)
        buf[2 + i] = hex[(v >> (28 - i * 4)) & 0xF];
    buf[10] = '\0';
    byt_log(buf);
}

static void byt_uprwc_open(void) {
    uint16_t uprwc = inw(BYT_ACPI_BASE + BYT_UPRWC);
    outw(BYT_ACPI_BASE + BYT_UPRWC, (uint16_t)(uprwc | BYT_UPRWC_WR_EN));
}

static void byt_uprwc_close(void) {
    uint16_t uprwc = inw(BYT_ACPI_BASE + BYT_UPRWC);
    outw(BYT_ACPI_BASE + BYT_UPRWC, (uint16_t)(uprwc & (uint16_t)~BYT_UPRWC_WR_EN));
}

static int byt_is_valleyview_xhci(uint16_t ven, uint16_t did) {
    return ven == 0x8086U &&
           (did == 0x0F31U || did == 0x0F35U || did == 0x0F36U || did == 0x0F37U);
}

static int byt_is_braswell_xhci(uint16_t ven, uint16_t did) {
    return ven == 0x8086U && did == 0x22B5U;
}

static int byt_is_xhci_id(uint16_t ven, uint16_t did) {
    return byt_is_valleyview_xhci(ven, did) || byt_is_braswell_xhci(ven, did);
}

static int byt_find_xhci(uint8_t* bus, uint8_t* slot, uint8_t* func,
                         uint16_t* out_did) {
    usb_pci_controller_t ctrls[8];
    int nc = pci_find_usb_controllers(ctrls, 8);
    for (int i = 0; i < nc; i++) {
        if (ctrls[i].prog_if != 0x30) continue; /* xHCI */
        if (!byt_is_xhci_id(ctrls[i].vendor_id, ctrls[i].device_id)) continue;
        if (bus) *bus = ctrls[i].bus;
        if (slot) *slot = ctrls[i].slot;
        if (func) *func = ctrls[i].func;
        if (out_did) *out_did = ctrls[i].device_id;
        return 1;
    }
    return 0;
}

static void byt_scan_ls_companions(void) {
    usb_pci_controller_t ctrls[8];
    int nc = pci_find_usb_controllers(ctrls, 8);
    g_has_ls_companion = 0;
    for (int i = 0; i < nc; i++) {
        if (ctrls[i].prog_if == 0x00 || ctrls[i].prog_if == 0x10) {
            g_has_ls_companion = 1;
            break;
        }
    }
}

static void byt_clear_intx_disable(uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t cmd = pci_read_config_word(bus, slot, func, 0x04);
    if (cmd & PCI_CMD_INTX_DISABLE) {
        pci_write_config_word(bus, slot, func, 0x04,
                              (uint16_t)(cmd & (uint16_t)~PCI_CMD_INTX_DISABLE));
        byt_log("USB2ROUTE: cleared EHCI INTx Disable\n");
    }
}

static void byt_ensure_ehci_bar(void) {
    uint32_t bar0 = pci_read_config_dword(0, BYT_EHCI_SLOT, BYT_EHCI_FUNC, 0x10);
    uint32_t addr = bar0 & 0xFFFFFFF0U;

    if ((bar0 & 1U) != 0) {
        byt_log("USB2ROUTE: EHCI BAR is IO-space (unexpected)\n");
        g_ehci_bar0 = bar0;
        return;
    }
    if (addr != 0) {
        g_ehci_bar0 = bar0;
        pci_enable_device(0, BYT_EHCI_SLOT, BYT_EHCI_FUNC);
        byt_clear_intx_disable(0, BYT_EHCI_SLOT, BYT_EHCI_FUNC);
        return;
    }

    pci_write_config_dword(0, BYT_EHCI_SLOT, BYT_EHCI_FUNC, 0x10, 0xFFFFFFFFU);
    uint32_t size_mask = pci_read_config_dword(0, BYT_EHCI_SLOT, BYT_EHCI_FUNC, 0x10);
    uint32_t size = ~(size_mask & 0xFFFFFFF0U) + 1U;
    if (size < 0x100U || size > 0x100000U)
        size = 0x1000U;
    (void)size;

    pci_write_config_dword(0, BYT_EHCI_SLOT, BYT_EHCI_FUNC, 0x10, BYT_EHCI_BAR_FALLBACK);
    g_ehci_bar0 = pci_read_config_dword(0, BYT_EHCI_SLOT, BYT_EHCI_FUNC, 0x10);
    pci_enable_device(0, BYT_EHCI_SLOT, BYT_EHCI_FUNC);
    byt_clear_intx_disable(0, BYT_EHCI_SLOT, BYT_EHCI_FUNC);
    byt_log("USB2ROUTE: programmed EHCI BAR0=");
    byt_log_hex32(g_ehci_bar0);
    byt_log("\n");
}

void baytrail_usb_note_route(uint32_t xusb2pr, uint32_t xusb2prm, int wrote) {
    g_xusb2pr = xusb2pr;
    g_xusb2prm = xusb2prm;
    g_route_noted = 1;
    g_route_locked = (wrote && xusb2pr == 0) ? 1 : 0;
    g_prefer_xhci = (!g_route_locked && xusb2pr != 0) ? 1 : 0;

    byt_log("USB2ROUTE: XUSB2PR=");
    byt_log_hex32(xusb2pr);
    byt_log(" XUSB2PRM=");
    byt_log_hex32(xusb2prm);
    byt_log(" USB2PDO=");
    byt_log_hex32(g_usb2pdo);
    if (g_route_locked)
        byt_log(" LOCKED=1 (USB2 on companion EHCI)\n");
    else if (xusb2pr != 0)
        byt_log(" LOCKED=0 (USB2 on xHCI)\n");
    else
        byt_log(" LOCKED=0\n");
}

void baytrail_usb_refresh_route_status(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t xusb2pr = pci_read_config_dword(bus, slot, func, BYT_XHCI_USB2PR);
    uint32_t xusb2prm = pci_read_config_dword(bus, slot, func, BYT_XHCI_USB2PRM);
    g_usb2pdo = pci_read_config_dword(bus, slot, func, BYT_XHCI_USB2PDO);
    g_usb3pdo = pci_read_config_dword(bus, slot, func, BYT_XHCI_USB3PDO);
    baytrail_usb_note_route(xusb2pr, xusb2prm, /*wrote=*/1);
}

int baytrail_usb_route_to_xhci(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t xusb2pr = 0;
    uint32_t xusb2prm = 0;
    uint32_t usb3pr = 0;
    uint32_t usb3prm = 0;
    int attempt;

    byt_log("USB2ROUTE: unlocking UPRWC, clearing PDO, writing PRM then PR\n");

    /*
     * HCRESET / firmware can clear XUSB2PR. Retry a few times with UPRWC held
     * open across PDO+PRM+PR (coreboot holds WR_EN for PDO; PR also needs it
     * on Lenovo when the first write is ignored).
     */
    for (attempt = 0; attempt < 3; attempt++) {
        uint32_t want2 = BYT_USB2_PORT_MAP;
        uint32_t want3 = BYT_USB3_PORT_MAP;

        byt_uprwc_open();

        /* Clear per-port disable masks (needs UPRWC.WR_EN). */
        pci_write_config_dword(bus, slot, func, BYT_XHCI_USB2PDO, 0);
        pci_write_config_dword(bus, slot, func, BYT_XHCI_USB3PDO, 0);

        /* Prefer firmware PRM width when it already exposes more ports. */
        xusb2prm = pci_read_config_dword(bus, slot, func, BYT_XHCI_USB2PRM);
        if ((xusb2prm & 0x3FU) != 0)
            want2 = xusb2prm | BYT_USB2_PORT_MAP;
        usb3prm = pci_read_config_dword(bus, slot, func, BYT_XHCI_USB3PRM);
        if ((usb3prm & 0xFU) != 0)
            want3 = usb3prm | BYT_USB3_PORT_MAP;

        pci_write_config_dword(bus, slot, func, BYT_XHCI_USB2PRM, want2);
        pci_write_config_dword(bus, slot, func, BYT_XHCI_USB3PRM, want3);
        pci_write_config_dword(bus, slot, func, BYT_XHCI_USB2PR, want2);
        pci_write_config_dword(bus, slot, func, BYT_XHCI_USB3PR, want3);

        timer_busy_wait_ms(20);

        g_usb2pdo = pci_read_config_dword(bus, slot, func, BYT_XHCI_USB2PDO);
        g_usb3pdo = pci_read_config_dword(bus, slot, func, BYT_XHCI_USB3PDO);
        xusb2prm = pci_read_config_dword(bus, slot, func, BYT_XHCI_USB2PRM);
        usb3prm = pci_read_config_dword(bus, slot, func, BYT_XHCI_USB3PRM);
        xusb2pr = pci_read_config_dword(bus, slot, func, BYT_XHCI_USB2PR);
        usb3pr = pci_read_config_dword(bus, slot, func, BYT_XHCI_USB3PR);

        byt_uprwc_close();

        if (xusb2pr != 0)
            break;

        byt_log("USB2ROUTE: XUSB2PR still 0 after attempt ");
        {
            char b[2];
            b[0] = (char)('1' + attempt);
            b[1] = 0;
            byt_log(b);
        }
        byt_log(", retrying\n");
        timer_busy_wait_ms(30);
    }

    byt_log("USB2ROUTE: post USB3PRM=");
    byt_log_hex32(usb3prm);
    byt_log(" USB3_PSSEN=");
    byt_log_hex32(usb3pr);
    byt_log("\n");

    baytrail_usb_note_route(xusb2pr, xusb2prm, /*wrote=*/1);

    if (xusb2pr == 0) {
        byt_log("USB2ROUTE: FAILED XUSB2PR=0 — USB2 mouse EP0 cannot work on "
                "xHCI (ports still on companion EHCI). "
                "Try again / check UPRWC; do not expect HID pointer.\n");
        return 0;
    }
    return 1;
}

static void byt_unhide_ehci(void) {
    volatile uint32_t* func_dis =
        (volatile uint32_t*)(uintptr_t)(BYT_PMC_BASE + BYT_PMC_FUNC_DIS);

    byt_uprwc_open();
    if ((*func_dis) & BYT_EHCI_DIS) {
        *func_dis = (*func_dis) & ~BYT_EHCI_DIS;
        timer_busy_wait_ms(10);
        g_ehci_unhid = 1;
    }
    g_func_dis_after = *func_dis;
    byt_uprwc_close();

    g_ehci_id = pci_read_config_dword(0, BYT_EHCI_SLOT, BYT_EHCI_FUNC, 0);
    if ((g_ehci_id & 0xFFFFU) == 0x8086U &&
        ((g_ehci_id >> 16) & 0xFFFFU) == BYT_EHCI_DID) {
        g_ehci_seen = 1;
        byt_log("USB2ROUTE: EHCI 8086:0F34 visible at 0:1d.0");
        if (g_ehci_unhid)
            byt_log(" (unhidden)");
        byt_log("\n");
        byt_ensure_ehci_bar();
    } else {
        byt_log("USB2ROUTE: EHCI 0:1d.0 still absent id=");
        byt_log_hex32(g_ehci_id);
        byt_log(" FUNC_DIS=");
        byt_log_hex32(g_func_dis_after);
        byt_log("\n");
    }
}

static void byt_rehide_ehci(void) {
    volatile uint32_t* func_dis =
        (volatile uint32_t*)(uintptr_t)(BYT_PMC_BASE + BYT_PMC_FUNC_DIS);
    uint32_t v = *func_dis;
    if ((v & BYT_EHCI_DIS) == 0) {
        *func_dis = v | BYT_EHCI_DIS;
        g_func_dis_after = *func_dis;
        g_ehci_seen = 0;
        byt_log("USB2ROUTE: re-hid EHCI (USB2 routed to xHCI)\n");
    }
}

void baytrail_usb_prepare_companion(void) {
    volatile uint32_t* func_dis;
    int routed = 0;

    g_is_byt = 0;
    g_is_braswell = 0;
    g_ehci_seen = 0;
    g_ehci_unhid = 0;
    g_has_ls_companion = 0;
    g_func_dis_before = 0;
    g_func_dis_after = 0;
    g_ehci_id = 0;
    g_ehci_bar0 = 0;
    g_usb2pdo = 0;
    g_usb3pdo = 0;
    g_prefer_xhci = 0;
    g_xhci_found = 0;
    g_xhci_did = 0;
    g_route_locked = 0;

    g_xhci_found = byt_find_xhci(&g_xhci_bus, &g_xhci_slot, &g_xhci_func, &g_xhci_did);
    if (!g_xhci_found && !iosf_mbi_available())
        return;
    if (!g_xhci_found)
        return;

    /*
     * Braswell (Acer R3-131T, 8086:22B5): do NOT poke Bay Trail PMC at
     * 0xFED03000, UPRWC companion unhide, or PHY scripts. Those MMIO/SMM
     * paths hard-hang this SoC. Leave xHCI alone for the normal host stack.
     */
    if (byt_is_braswell_xhci(0x8086U, g_xhci_did)) {
        g_is_braswell = 1;
        g_is_byt = 0;
        g_prefer_xhci = 1;
        baytrail_usb_set_phy_quirks(0);
        /* driver_log only — print()/FB console mid-USB2ROUTE garbles the panel. */
        driver_log_line("USB2ROUTE: Braswell xHCI 8086:22B5 -- skipping BYT PMC/PHY "
                        "(xHCI-only, no companion route)");
        return;
    }

    g_is_byt = 1;
    byt_scan_ls_companions();

    func_dis = (volatile uint32_t*)(uintptr_t)(BYT_PMC_BASE + BYT_PMC_FUNC_DIS);
    g_func_dis_before = *func_dis;
    g_func_dis_after = g_func_dis_before;
    byt_log("USB2ROUTE: Bay Trail PMC FUNC_DIS=");
    byt_log_hex32(g_func_dis_before);
    if (g_func_dis_before & BYT_EHCI_DIS)
        byt_log(" (EHCI_DIS set)");
    byt_log("\n");
    byt_log("USB2ROUTE: LS companion UHCI/OHCI=");
    byt_log(g_has_ls_companion ? "yes\n" : "no\n");

    /*
     * Prefer USB2→xHCI BEFORE EHCI claims ports (CONFIGFLAG). Route under
     * UPRWC with PDO cleared so XUSB2PR can stick on Lenovo Bay Trail.
     */
    routed = baytrail_usb_route_to_xhci(g_xhci_bus, g_xhci_slot, g_xhci_func);

    if (routed) {
        /* Keep EHCI out of the host scan so xHCI owns gray USB2 ports. */
        byt_rehide_ehci();
        g_prefer_xhci = 1;
        return;
    }

    /* Route locked: USB2 stays on companion EHCI — unhide and BAR it. */
    byt_log("USB2ROUTE: route locked — bringing up companion EHCI\n");
    byt_unhide_ehci();
    byt_scan_ls_companions(); /* rescan after EHCI appears */
    g_prefer_xhci = 0;
}

int baytrail_usb_is_soc(void) {
    return g_is_byt;
}

int baytrail_usb_is_braswell(void) {
    return g_is_braswell;
}

uint32_t baytrail_usb_xusb2pr(void) {
    return g_xusb2pr;
}

uint32_t baytrail_usb_usb2pdo(void) {
    return g_usb2pdo;
}

int baytrail_usb_usb2_on_xhci(void) {
    return g_xusb2pr != 0;
}

int baytrail_usb_usb2_route_locked(void) {
    return g_route_locked;
}

int baytrail_usb_has_ls_companion(void) {
    return g_has_ls_companion;
}

int baytrail_usb_prefer_xhci(void) {
    return g_prefer_xhci;
}

/* ---- coreboot Bay Trail xHCI vendor bring-up (after HCRESET) ---- */

#define IOSF_PORT_USHPHY           0x61U
#define IOSF_OP_READ_USHPHY        0x06U
#define IOSF_OP_WRITE_USHPHY       0x07U
#define IOSF_PORT_USBPHY           0x43U
#define IOSF_OP_READ_USBPHY        0x06U
#define IOSF_OP_WRITE_USBPHY       0x07U
#define USBPHY_COMPBG              0x7F04U
#define USHPHY_CDN_PLL_CONTROL     0x03C0U
#define USHPHY_CDN_VCO_START_CAL   0x0054U
#define USHPHY_CCDRLF              0x8040U
#define USHPHY_PEAKING_AMP         0x80A8U
#define USHPHY_OFFSET_COR          0x80B0U
#define USHPHY_VGA_GAIN            0x8080U
#define USHPHY_REE_DAC             0x80B8U
#define USHPHY_CDN_U1_PWR          0x0000U

static uint32_t byt_mmio_read(volatile uint8_t* bar, uint32_t off) {
    return *(volatile uint32_t*)(bar + off);
}

static void byt_mmio_write(volatile uint8_t* bar, uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(bar + off) = v;
}

static void byt_mmio_rmw(volatile uint8_t* bar, uint32_t off,
                         uint32_t and_mask, uint32_t or_mask) {
    uint32_t v = byt_mmio_read(bar, off);
    byt_mmio_write(bar, off, (v & and_mask) | or_mask);
}

static void byt_pci_rmw8(uint8_t bus, uint8_t slot, uint8_t func,
                         uint8_t offset, uint8_t and_mask, uint8_t or_mask) {
    uint8_t v = pci_read_config_byte(bus, slot, func, offset);
    uint8_t nv = (uint8_t)((v & and_mask) | or_mask);
    uint32_t dword = pci_read_config_dword(bus, slot, func, offset);
    uint8_t shift = (uint8_t)((offset & 3U) * 8U);
    dword = (dword & ~(0xFFU << shift)) | ((uint32_t)nv << shift);
    pci_write_config_dword(bus, slot, func, (uint8_t)(offset & ~3U), dword);
}

static void byt_pci_rmw16(uint8_t bus, uint8_t slot, uint8_t func,
                          uint8_t offset, uint16_t and_mask, uint16_t or_mask) {
    uint16_t v = pci_read_config_word(bus, slot, func, offset);
    pci_write_config_word(bus, slot, func, offset,
                          (uint16_t)((v & and_mask) | or_mask));
}

static void byt_pci_rmw32(uint8_t bus, uint8_t slot, uint8_t func,
                          uint8_t offset, uint32_t and_mask, uint32_t or_mask) {
    uint32_t v = pci_read_config_dword(bus, slot, func, offset);
    pci_write_config_dword(bus, slot, func, offset, (v & and_mask) | or_mask);
}

static void byt_ushphy_rmw(uint32_t reg, uint32_t and_mask, uint32_t or_mask) {
    uint32_t v = 0;
    if (!iosf_mbi_read(IOSF_PORT_USHPHY, IOSF_OP_READ_USHPHY, reg, &v))
        return;
    (void)iosf_mbi_write(IOSF_PORT_USHPHY, IOSF_OP_WRITE_USHPHY, reg,
                         (v & and_mask) | or_mask);
}

static void byt_usb3_phy_script(void) {
    /* coreboot usb3_phy_script / USB3PHYInit() */
    byt_ushphy_rmw(USHPHY_CDN_PLL_CONTROL, ~0x00700000U, 0x00500000U);
    byt_ushphy_rmw(USHPHY_CDN_VCO_START_CAL, ~0x001F0000U, 0x000A0000U);
    byt_ushphy_rmw(USHPHY_CCDRLF, ~0x0000000FU, 0x0000000BU);
    byt_ushphy_rmw(USHPHY_PEAKING_AMP, ~0x000000F0U, 0x000000F0U);
    byt_ushphy_rmw(USHPHY_OFFSET_COR, ~0x000001C0U, 0x00000000U);
    byt_ushphy_rmw(USHPHY_VGA_GAIN, ~0x00000070U, 0x00000020U);
    byt_ushphy_rmw(USHPHY_REE_DAC, ~0x00000002U, 0x00000002U);
    byt_ushphy_rmw(USHPHY_CDN_U1_PWR, ~0x00000000U, 0x00040000U);
}

static void byt_usb2_phy_script(void) {
    /*
     * coreboot usb2_phy_init COMPBG default. Per-port lane values are
     * board-specific — leave BIOS lanes alone; only restore the shared
     * bias generator so FS/LS EP0 is not starved electrically.
     */
    uint32_t v = 0;
    if (!iosf_mbi_read(IOSF_PORT_USBPHY, IOSF_OP_READ_USBPHY, USBPHY_COMPBG, &v))
        return;
    if ((v & 0xFFFFU) != 0x4700U) {
        (void)iosf_mbi_write(IOSF_PORT_USBPHY, IOSF_OP_WRITE_USBPHY, USBPHY_COMPBG,
                             (v & 0xFFFF0000U) | 0x4700U);
        byt_log("USB2ROUTE: USB2 PHY COMPBG -> 0x4700 (was ");
        byt_log_hex32(v);
        byt_log(")\n");
    }
}

static void byt_xhci_init_script(volatile uint8_t* bar,
                                 uint8_t bus, uint8_t slot, uint8_t func) {
    /* coreboot xhci_init_script / CommonXhciHcInit() — BAR offsets from CAP. */
    byt_mmio_rmw(bar, 0x000CU, 0x0000FFFFU, 0x02000000U);
    byt_mmio_rmw(bar, 0x000CU, 0xFFFFFF00U, 0x0000000AU);
    byt_mmio_rmw(bar, 0x8094U, ~0U, 0x00A04000U);
    byt_mmio_rmw(bar, 0x8110U, ~0x00000104U, 0x00100800U);
    byt_mmio_rmw(bar, 0x8144U, ~0U, 0x000001C0U);
    byt_mmio_rmw(bar, 0x8154U, ~0x00200008U, 0x80002000U);
    byt_mmio_rmw(bar, 0x816CU, 0xFFF08000U, 0x000E0030U);
    byt_mmio_rmw(bar, 0x8188U, ~0U, 0x05000000U);
    byt_mmio_rmw(bar, 0x8174U, 0xFE000000U, 0x01000C0AU);
    byt_mmio_rmw(bar, 0x854CU, ~0x20000000U, 0U);
    byt_mmio_rmw(bar, 0x8178U, ~0xFFFFE000U, 0U);
    byt_mmio_rmw(bar, 0x8164U, ~0U, 0x000000FFU);
    byt_mmio_rmw(bar, 0x0010U, ~0x00000020U, 0x00000600U);
    byt_mmio_rmw(bar, 0x8058U, ~0x00000100U, 0x00110000U);
    byt_mmio_rmw(bar, 0x8060U, ~0U, 0x02000000U);
    byt_mmio_rmw(bar, 0x80F0U, ~0x00100000U, 0U);
    /*
     * coreboot enables LPM here (bit 19). Leave it CLEAR for boot-mouse
     * enum: USB2 HW LPM has put Bay Trail EP0 into Transaction Error
     * (cc=4) against ordinary LS/FS HID mice (e.g. AmazonBasics).
     */
    byt_mmio_rmw(bar, 0x8008U, ~0x00080000U, 0U);
    byt_mmio_rmw(bar, 0x80FCU, ~0U, 0x02000000U);

    /* PCI 0x40/0x44 byte-wise (avoid touching bit 31 via dword). */
    byt_pci_rmw8(bus, slot, func, 0x41, (uint8_t)~0x06U, 0x01U);
    byt_pci_rmw8(bus, slot, func, 0x42, 0x3CU, 0x04U);
    byt_pci_rmw8(bus, slot, func, 0x44, 0x00U, 0x8FU);
    byt_pci_rmw8(bus, slot, func, 0x45, (uint8_t)~0xCFU, 0xC6U);
    byt_pci_rmw8(bus, slot, func, 0x46, (uint8_t)~0x0FU, 0x0FU);

    byt_mmio_rmw(bar, 0x8140U, 0U, 0xFF00F03CU);
}

static void byt_xhci_clock_gating(uint8_t bus, uint8_t slot, uint8_t func) {
    /* coreboot xhci_clock_gating_script + finalize XHCC1/XHCC2. */
    byt_pci_rmw16(bus, slot, func, 0x40, (uint16_t)~0x0600U, 0x0100U);
    byt_pci_rmw8(bus, slot, func, 0x42, (uint8_t)~0x38U, 0x04U);
    byt_pci_rmw16(bus, slot, func, 0x44, (uint16_t)~0x0030U, 0x0008U);
    byt_pci_rmw32(bus, slot, func, 0xA0, ~0x00080000U, 0x00040000U);
    pci_write_config_word(bus, slot, func, 0xA4, 0x0000U);
    byt_pci_rmw32(bus, slot, func, 0xB0, ~0x00376000U, 0U);
    pci_write_config_dword(bus, slot, func, 0x50, 0x0BCE6E5FU);

    byt_pci_rmw32(bus, slot, func, 0x44, ~0U, 0x83C00000U);
    byt_pci_rmw32(bus, slot, func, 0x40, ~0x00800000U, 0x80000000U);
}

/* Speculative PHY/MMIO scripts — default ON for Bay Trail once USB2 is
 * routed to xHCI (Lenovo 80M4: XUSB2PR!=0 still needs COMPBG/0x80e0 for EP0).
 * Disable with gooberos.usb.byt.phy=off if a board misbehaves. */
static int g_phy_quirks = 1;

int baytrail_usb_phy_quirks_enabled(void) {
    return g_phy_quirks;
}

void baytrail_usb_set_phy_quirks(int enabled) {
    g_phy_quirks = enabled ? 1 : 0;
}

void baytrail_xhci_hc_bringup(volatile uint8_t* mmio_bar,
                              uint8_t bus, uint8_t slot, uint8_t func) {
    if (!mmio_bar) return;

    /* Braswell: never run BYT PHY/0x80e0/clock-gating MMIO scripts. */
    if (g_is_braswell || byt_is_braswell_xhci(0x8086U, g_xhci_did)) {
        byt_log("USB2ROUTE: Braswell xHCI bring-up skipped (no BYT PHY/MMIO)\n");
        return;
    }

    g_is_byt = 1;

    if (!g_phy_quirks) {
        byt_log("USB2ROUTE: Bay Trail xHCI bring-up skipped "
                "(gooberos.usb.byt.phy=off; routing only)\n");
        return;
    }

    byt_log("USB2ROUTE: Bay Trail xHCI MMIO/PCI bring-up "
            "(quirk: coreboot phy+0x80e0+clkgt for 8086:0F35)\n");
    byt_usb2_phy_script();   /* reason: restore COMPBG bias if BIOS drifted */
    byt_usb3_phy_script();   /* reason: USB3 PHY lane tune (SS receptacles) */
    byt_xhci_init_script(mmio_bar, bus, slot, func); /* reason: HC vendor init */

    /* Boot path: 0x80e0[16,9,6]=001b, pulse bit 24 to finish PHY/HC reset. */
    byt_mmio_rmw(mmio_bar, 0x80E0U, ~0x00010200U, 0x01000040U);
    timer_busy_wait_ms(1);
    byt_mmio_rmw(mmio_bar, 0x80E0U, ~0x01000000U, 0U);
    timer_busy_wait_ms(10);

    byt_xhci_clock_gating(bus, slot, func); /* reason: PCI XHCC clock gates */
    byt_log("USB2ROUTE: xHCI bring-up done 80e0=");
    byt_log_hex32(byt_mmio_read(mmio_bar, 0x80E0U));
    byt_log("\n");
}

void baytrail_usb_print_status(void (*write)(const char*)) {
    if (!write) return;
    if (!g_is_byt && !g_route_noted) {
        write("USB2ROUTE: (not Bay Trail / no route note)\n");
        return;
    }
    write("USB2ROUTE: status XUSB2PR=");
    {
        char buf[11];
        const char* hex = "0123456789ABCDEF";
        buf[0] = '0'; buf[1] = 'x';
        for (int i = 0; i < 8; i++)
            buf[2 + i] = hex[(g_xusb2pr >> (28 - i * 4)) & 0xF];
        buf[10] = '\0';
        write(buf);
    }
    write(" PDO=");
    {
        char buf[11];
        const char* hex = "0123456789ABCDEF";
        buf[0] = '0'; buf[1] = 'x';
        for (int i = 0; i < 8; i++)
            buf[2 + i] = hex[(g_usb2pdo >> (28 - i * 4)) & 0xF];
        buf[10] = '\0';
        write(buf);
    }
    write(" locked=");
    write(g_route_locked ? "1" : "0");
    write(" ehci=");
    write(g_ehci_seen ? "yes" : "no");
    write(" ls_comp=");
    write(g_has_ls_companion ? "yes" : "no");
    write(" prefer=");
    write(g_prefer_xhci ? "xhci" : "ehci");
    write("\n");
}
