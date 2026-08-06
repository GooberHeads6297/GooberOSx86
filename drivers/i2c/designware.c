#include "designware.h"
#include "../pci/pci.h"
#include "../timer/timer.h"
#include "../diagnostics/driver_log.h"
#include "../../lib/string.h"

extern void print(const char* str);

#define DW_IC_CON              0x00
#define DW_IC_TAR              0x04
#define DW_IC_DATA_CMD         0x10
#define DW_IC_SS_SCL_HCNT      0x14
#define DW_IC_SS_SCL_LCNT      0x18
#define DW_IC_INTR_MASK        0x30
#define DW_IC_CLR_INTR         0x40
#define DW_IC_ENABLE           0x6C
#define DW_IC_STATUS           0x70
#define DW_IC_TXFLR            0x74
#define DW_IC_RXFLR            0x78
#define DW_IC_TX_ABRT_SOURCE   0x80
#define DW_IC_ENABLE_STATUS    0x9C

#define DW_CON_MASTER          0x0001
#define DW_CON_SPEED_STANDARD  0x0002
#define DW_CON_RESTART_EN      0x0020
#define DW_CON_SLAVE_DISABLE   0x0040

#define DW_DATA_CMD_READ       0x0100
#define DW_DATA_CMD_STOP       0x0200
#define DW_DATA_CMD_RESTART    0x0400

#define DW_STATUS_ACTIVITY     0x0001
#define DW_STATUS_TFNF         0x0002
#define DW_STATUS_RFNE         0x0008

static i2c_bus_t g_bus;
static i2c_pci_controller_t g_ctrls[8];
static int g_ctrl_count = -1;

static inline uint32_t mmio_read(uintptr_t base, uint32_t off) {
    return *(volatile uint32_t*)(base + off);
}

static inline void mmio_write(uintptr_t base, uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(base + off) = v;
}

static int wait_enable(uint32_t enabled) {
    uint64_t deadline = timer_deadline_ms(25);
    while (!timer_deadline_expired(deadline)) {
        if ((mmio_read(g_bus.mmio, DW_IC_ENABLE_STATUS) & 1U) == enabled) return 0;
    }
    return -1;
}

static int set_enabled(uint32_t enabled) {
    mmio_write(g_bus.mmio, DW_IC_ENABLE, enabled ? 1U : 0U);
    return wait_enable(enabled ? 1U : 0U);
}

static int wait_bus_idle(void) {
    uint64_t deadline = timer_deadline_ms(50);
    while (!timer_deadline_expired(deadline)) {
        if ((mmio_read(g_bus.mmio, DW_IC_STATUS) & DW_STATUS_ACTIVITY) == 0) return 0;
    }
    return -1;
}

static int wait_tfnf(void) {
    uint64_t deadline = timer_deadline_ms(20);
    while (!timer_deadline_expired(deadline)) {
        if (mmio_read(g_bus.mmio, DW_IC_STATUS) & DW_STATUS_TFNF) return 0;
    }
    return -1;
}

static int wait_rfne(void) {
    uint64_t deadline = timer_deadline_ms(50);
    while (!timer_deadline_expired(deadline)) {
        if (mmio_read(g_bus.mmio, DW_IC_STATUS) & DW_STATUS_RFNE) return 0;
        if (mmio_read(g_bus.mmio, DW_IC_TX_ABRT_SOURCE) != 0) return -1;
    }
    return -1;
}

static int set_target(uint8_t addr) {
    if (set_enabled(0) != 0) return -1;
    mmio_write(g_bus.mmio, DW_IC_TAR, addr & 0x7FU);
    return set_enabled(1);
}

static void i2c_discover_controllers(void) {
    if (g_ctrl_count >= 0) return;
    g_ctrl_count = pci_find_i2c_controllers(g_ctrls, 8);
    if (g_ctrl_count < 0) g_ctrl_count = 0;
    if (g_ctrl_count > 8) g_ctrl_count = 8;
}

int i2c_controller_count(void) {
    i2c_discover_controllers();
    return g_ctrl_count;
}

uint16_t i2c_controller_device_id(int index) {
    i2c_discover_controllers();
    if (index < 0 || index >= g_ctrl_count) return 0;
    return g_ctrls[index].device_id;
}

int i2c_init_controller(int index) {
    i2c_discover_controllers();
    memset(&g_bus, 0, sizeof(g_bus));

    if (g_ctrl_count <= 0) {
        print("[i2c] no Intel DesignWare/LPSS I2C controller found.\n");
        driver_log_line("[i2c] no Intel DesignWare/LPSS I2C controller found.");
        return -1;
    }
    if (index < 0 || index >= g_ctrl_count) return -1;

    {
        int i = index;
        uint32_t bar = g_ctrls[i].bar0;
        uintptr_t mmio = (uintptr_t)(bar & ~0x0FU);
        if (!mmio) {
            print("[i2c] controller has no usable MMIO BAR.\n");
            driver_log_line("[i2c] controller has no usable MMIO BAR.");
            return -1;
        }

        g_bus.mmio = mmio;
        g_bus.bus = g_ctrls[i].bus;
        g_bus.slot = g_ctrls[i].slot;
        g_bus.func = g_ctrls[i].func;
        g_bus.vendor_id = g_ctrls[i].vendor_id;
        g_bus.device_id = g_ctrls[i].device_id;

        uint16_t cmd = pci_read_config_word(g_bus.bus, g_bus.slot, g_bus.func, 0x04);
        pci_write_config_word(g_bus.bus, g_bus.slot, g_bus.func, 0x04, (uint16_t)(cmd | 0x0006));

        if (set_enabled(0) != 0) return -1;
        mmio_write(g_bus.mmio, DW_IC_INTR_MASK, 0);
        (void)mmio_read(g_bus.mmio, DW_IC_CLR_INTR);
        mmio_write(g_bus.mmio, DW_IC_CON,
                   DW_CON_MASTER | DW_CON_SPEED_STANDARD |
                   DW_CON_RESTART_EN | DW_CON_SLAVE_DISABLE);
        /* Conservative 100 kHz-ish timings; firmware usually leaves sane values. */
        if (mmio_read(g_bus.mmio, DW_IC_SS_SCL_HCNT) == 0)
            mmio_write(g_bus.mmio, DW_IC_SS_SCL_HCNT, 0x190);
        if (mmio_read(g_bus.mmio, DW_IC_SS_SCL_LCNT) == 0)
            mmio_write(g_bus.mmio, DW_IC_SS_SCL_LCNT, 0x1D6);
        if (set_enabled(1) != 0) return -1;

        g_bus.ready = 1;
        print("[i2c] Intel DesignWare I2C ready (");
        {
            char id[12];
            const char* h = "0123456789ABCDEF";
            id[0] = '0'; id[1] = 'x';
            id[2] = h[(g_bus.device_id >> 12) & 0xF];
            id[3] = h[(g_bus.device_id >> 8) & 0xF];
            id[4] = h[(g_bus.device_id >> 4) & 0xF];
            id[5] = h[g_bus.device_id & 0xF];
            id[6] = '\0';
            print(id);
        }
        print(").\n");
        driver_log("[i2c] Intel DesignWare I2C ready at PCI ");
        driver_log_u32(g_bus.bus); driver_log(":");
        driver_log_u32(g_bus.slot); driver_log(":");
        driver_log_u32(g_bus.func);
        driver_log(" id=");
        driver_log_hex32(g_bus.device_id);
        driver_log("\n");
        return 0;
    }
}

int i2c_init(void) {
    int count = i2c_controller_count();
    for (int i = 0; i < count; i++) {
        if (i2c_init_controller(i) == 0) return 0;
    }
    print("[i2c] controller found but no usable MMIO BAR initialized.\n");
    driver_log_line("[i2c] controller found but no usable MMIO BAR initialized.");
    return -1;
}

const i2c_bus_t* i2c_get_bus(void) {
    return &g_bus;
}

int i2c_write_cmd(uint8_t addr, uint16_t reg, const uint8_t* data, uint16_t len) {
    if (!g_bus.ready) return -1;
    if (set_target(addr) != 0) return -1;

    if (wait_tfnf() != 0) return -1;
    mmio_write(g_bus.mmio, DW_IC_DATA_CMD, reg & 0xFF);
    if (wait_tfnf() != 0) return -1;
    mmio_write(g_bus.mmio, DW_IC_DATA_CMD, ((reg >> 8) & 0xFF) |
                                      (len == 0 ? DW_DATA_CMD_STOP : 0));

    for (uint16_t i = 0; i < len; i++) {
        uint32_t cmd = data ? data[i] : 0;
        if (i + 1 == len) cmd |= DW_DATA_CMD_STOP;
        if (wait_tfnf() != 0) return -1;
        mmio_write(g_bus.mmio, DW_IC_DATA_CMD, cmd);
    }
    return wait_bus_idle();
}

int i2c_read_reg16(uint8_t addr, uint16_t reg, uint8_t* data, uint16_t len) {
    if (!g_bus.ready || !data || len == 0) return -1;
    if (set_target(addr) != 0) return -1;

    if (wait_tfnf() != 0) return -1;
    mmio_write(g_bus.mmio, DW_IC_DATA_CMD, reg & 0xFF);
    if (wait_tfnf() != 0) return -1;
    mmio_write(g_bus.mmio, DW_IC_DATA_CMD, (reg >> 8) & 0xFF);

    for (uint16_t i = 0; i < len; i++) {
        uint32_t cmd = DW_DATA_CMD_READ;
        if (i == 0) cmd |= DW_DATA_CMD_RESTART;
        if (i + 1 == len) cmd |= DW_DATA_CMD_STOP;
        if (wait_tfnf() != 0) return -1;
        mmio_write(g_bus.mmio, DW_IC_DATA_CMD, cmd);
        if (wait_rfne() != 0) return -1;
        data[i] = (uint8_t)(mmio_read(g_bus.mmio, DW_IC_DATA_CMD) & 0xFF);
    }

    return wait_bus_idle();
}
