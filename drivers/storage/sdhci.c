#include "sdhci.h"
#include "../pci/pci.h"
#include "../../lib/string.h"

extern void print(const char*);

#define SDHCI_MAX_CONTROLLERS 4
/* Match reference GooberOS spin budgets (Bay Trail eMMC init can be slow). */
#define SDHCI_SPIN_SHORT  50000U
#define SDHCI_SPIN_MED   200000U
#define SDHCI_SPIN_LONG  400000U

#define PCI_COMMAND_OFFSET 0x04
#define PCI_COMMAND_MEMORY 0x0002
#define PCI_COMMAND_BUSMASTER 0x0004

#define SDHCI_REG_BLOCK_SIZE       0x04
#define SDHCI_REG_BLOCK_COUNT      0x06
#define SDHCI_REG_ARGUMENT         0x08
#define SDHCI_REG_TRANSFER_MODE    0x0C
#define SDHCI_REG_COMMAND          0x0E
#define SDHCI_REG_RESPONSE0        0x10
#define SDHCI_REG_RESPONSE1        0x14
#define SDHCI_REG_RESPONSE2        0x18
#define SDHCI_REG_RESPONSE3        0x1C
#define SDHCI_REG_BUFFER_DATA      0x20
#define SDHCI_REG_PRESENT_STATE    0x24
#define SDHCI_REG_HOST_CONTROL1    0x28
#define SDHCI_REG_POWER_CONTROL    0x29
#define SDHCI_REG_CLOCK_CONTROL    0x2C
#define SDHCI_REG_TIMEOUT_CONTROL  0x2E
#define SDHCI_REG_SOFTWARE_RESET   0x2F
#define SDHCI_REG_INT_STATUS       0x30
#define SDHCI_REG_INT_ENABLE       0x34
#define SDHCI_REG_ERR_INT_ENABLE   0x36
#define SDHCI_REG_SIGNAL_ENABLE    0x38
#define SDHCI_REG_ERR_SIGNAL_ENABLE 0x3A
#define SDHCI_REG_CAPABILITIES0    0x40
#define SDHCI_REG_CAPABILITIES1    0x44
#define SDHCI_REG_HOST_VERSION     0xFE
#define SDHCI_REG_HOST_CONTROL2    0x3E

#define SDHCI_CTRL_4BITBUS         0x02
#define SDHCI_CTRL_8BITBUS         0x20
#define SDHCI_CTRL_HISPD           0x04

#define SDHCI_HC2_PRESET_VAL_ENABLE 0x8000U

#define SDHCI_RESET_ALL            0x01
#define SDHCI_RESET_CMD            0x02
#define SDHCI_RESET_DATA           0x04

#define SDHCI_PRESENT_CMD_INHIBIT  0x00000001U
#define SDHCI_PRESENT_DATA_INHIBIT 0x00000002U
#define SDHCI_PRESENT_CARD_INSERTED   0x00010000U
#define SDHCI_PRESENT_WRITE_PROTECT   0x00080000U

#define SDHCI_CLOCK_INT_EN         0x0001
#define SDHCI_CLOCK_INT_STABLE     0x0002
#define SDHCI_CLOCK_CARD_EN        0x0004

#define SDHCI_POWER_ON             0x01
#define SDHCI_POWER_180            0x0A
#define SDHCI_POWER_300            0x0C
#define SDHCI_POWER_330            0x0E

#define SDHCI_INT_CMD_COMPLETE     0x00000001U
#define SDHCI_INT_XFER_COMPLETE    0x00000002U
#define SDHCI_INT_BUF_WRITE_READY  0x00000010U
#define SDHCI_INT_BUF_READ_READY   0x00000020U
#define SDHCI_INT_ERROR_MASK       0xFFFF0000U

#define SDHCI_NORMAL_INT_MASK      0x0133U
#define SDHCI_ERROR_INT_MASK       0xFFFFU

#define SDHCI_TRNS_BLK_CNT_EN      0x0002
#define SDHCI_TRNS_READ            0x0010

#define SDHCI_CMD_RESP_NONE        0x0000
#define SDHCI_CMD_RESP_LONG        0x0001
#define SDHCI_CMD_RESP_SHORT       0x0002
#define SDHCI_CMD_RESP_SHORT_BUSY  0x0003
#define SDHCI_CMD_CRC              0x0008
#define SDHCI_CMD_INDEX            0x0010
#define SDHCI_CMD_DATA             0x0020

#define MMC_CMD_GO_IDLE            0
#define MMC_CMD_SEND_OP_COND       1
#define MMC_CMD_ALL_SEND_CID       2
#define MMC_CMD_SET_RELATIVE_ADDR  3
#define MMC_CMD_SWITCH             6
#define MMC_CMD_SELECT_CARD        7
#define MMC_CMD_SEND_EXT_CSD       8
#define MMC_CMD_SEND_CSD           9
#define MMC_CMD_SEND_STATUS        13
#define MMC_CMD_SET_BLOCKLEN       16
#define MMC_CMD_READ_SINGLE_BLOCK  17
#define MMC_CMD_WRITE_BLOCK        24

#define MMC_OCR_BUSY               0x80000000U
#define MMC_OCR_SECTOR_MODE        0x40000000U
#define MMC_OCR_VOLTAGE_MASK       0x00FF8000U

#define EXT_CSD_SEC_COUNT          212
#define EXT_CSD_PART_CONFIG        179
#define EXT_CSD_PART_ACCESS_MASK   0x07U
#define EXT_CSD_BUS_WIDTH          183
#define EXT_CSD_BUS_WIDTH_1        0
#define EXT_CSD_BUS_WIDTH_4        1
#define EXT_CSD_BUS_WIDTH_8        2

#define SDHCI_PRESENT_DAT0         0x00100000U
#define SDHCI_SPIN_WRITE           2000000U

typedef struct {
    uint8_t active;
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint8_t write_protected;
    uint8_t high_capacity;
    uint16_t rca;
    uint32_t mmio_base;
    uint32_t sector_count;
} sdhci_state_t;

static sdhci_state_t controller_states[SDHCI_MAX_CONTROLLERS];

static uint8_t mmio_read8(uint32_t base, uint32_t offset) {
    volatile uint8_t* ptr = (volatile uint8_t*)(uintptr_t)(base + offset);
    return *ptr;
}

static uint16_t mmio_read16(uint32_t base, uint32_t offset) {
    volatile uint16_t* ptr = (volatile uint16_t*)(uintptr_t)(base + offset);
    return *ptr;
}

static uint32_t mmio_read32(uint32_t base, uint32_t offset) {
    volatile uint32_t* ptr = (volatile uint32_t*)(uintptr_t)(base + offset);
    return *ptr;
}

static void mmio_write8(uint32_t base, uint32_t offset, uint8_t value) {
    volatile uint8_t* ptr = (volatile uint8_t*)(uintptr_t)(base + offset);
    *ptr = value;
}

static void mmio_write16(uint32_t base, uint32_t offset, uint16_t value) {
    volatile uint16_t* ptr = (volatile uint16_t*)(uintptr_t)(base + offset);
    *ptr = value;
}

static void mmio_write32(uint32_t base, uint32_t offset, uint32_t value) {
    volatile uint32_t* ptr = (volatile uint32_t*)(uintptr_t)(base + offset);
    *ptr = value;
}

static int sdhci_wait_reset_clear(uint32_t base, uint8_t mask) {
    for (uint32_t spins = 0; spins < SDHCI_SPIN_MED; spins++) {
        if ((mmio_read8(base, SDHCI_REG_SOFTWARE_RESET) & mask) == 0) return 1;
    }
    return 0;
}

static int sdhci_wait_present_clear(uint32_t base, uint32_t mask) {
    for (uint32_t spins = 0; spins < SDHCI_SPIN_MED; spins++) {
        if ((mmio_read32(base, SDHCI_REG_PRESENT_STATE) & mask) == 0) return 1;
    }
    return 0;
}

static void sdhci_clear_interrupts(uint32_t base) {
    mmio_write32(base, SDHCI_REG_INT_STATUS, 0xFFFFFFFFU);
}

static void sdhci_recover_error(uint32_t base, uint8_t reset_mask) {
    mmio_write8(base, SDHCI_REG_SOFTWARE_RESET, reset_mask);
    sdhci_wait_reset_clear(base, reset_mask);
    sdhci_clear_interrupts(base);
}

static int sdhci_wait_interrupt_spins(uint32_t base, uint32_t mask, uint32_t* out_status,
                                      uint32_t spins_max) {
    for (uint32_t spins = 0; spins < spins_max; spins++) {
        uint32_t status = mmio_read32(base, SDHCI_REG_INT_STATUS);
        /*
         * Prefer success if the requested event is present. Bay Trail eMMC often
         * latches DATA_TIMEOUT (bit 20) together with Transfer Complete on writes;
         * treating error-first falsely fails a completed transfer (status 0x100002).
         */
        if (status & mask) {
            if (out_status) *out_status = status;
            mmio_write32(base, SDHCI_REG_INT_STATUS, mask | (status & SDHCI_INT_ERROR_MASK));
            return 1;
        }
        if (status & SDHCI_INT_ERROR_MASK) {
            if (out_status) *out_status = status;
            return 0;
        }
    }
    if (out_status) *out_status = mmio_read32(base, SDHCI_REG_INT_STATUS);
    return 0;
}

static int sdhci_wait_interrupt(uint32_t base, uint32_t mask, uint32_t* out_status) {
    return sdhci_wait_interrupt_spins(base, mask, out_status, SDHCI_SPIN_LONG);
}

/* After a write, wait until DAT0 is high (card not programming). */
static int sdhci_wait_dat0_ready(uint32_t base) {
    for (uint32_t spins = 0; spins < SDHCI_SPIN_WRITE; spins++) {
        uint32_t present = mmio_read32(base, SDHCI_REG_PRESENT_STATE);
        if (present & SDHCI_PRESENT_DAT0) return 1;
    }
    return 0;
}

static uint8_t sdhci_pick_power(uint32_t caps0) {
    if (caps0 & (1U << 24)) return SDHCI_POWER_330;
    if (caps0 & (1U << 25)) return SDHCI_POWER_300;
    if (caps0 & (1U << 26)) return SDHCI_POWER_180;
    return 0;
}

static int sdhci_enable_power(uint32_t base, uint32_t caps0) {
    uint8_t power = sdhci_pick_power(caps0);
    if (power == 0) return 0;
    mmio_write8(base, SDHCI_REG_POWER_CONTROL, power);
    for (uint32_t spins = 0; spins < 10000U; spins++) { }
    mmio_write8(base, SDHCI_REG_POWER_CONTROL, (uint8_t)(power | SDHCI_POWER_ON));
    for (uint32_t spins = 0; spins < 10000U; spins++) { }
    return 1;
}

static int sdhci_set_clock(uint32_t base, uint32_t caps0, uint32_t target_khz) {
    uint32_t base_mhz = (caps0 >> 8) & 0xFFU;
    uint32_t base_khz;
    uint32_t divisor = 1;
    uint32_t encoded;
    uint16_t clock_value;

    if (base_mhz == 0) base_mhz = 50;
    base_khz = base_mhz * 1000U;

    mmio_write16(base, SDHCI_REG_CLOCK_CONTROL, 0);

    while (divisor < 1024U && (base_khz / divisor) > target_khz) divisor <<= 1;
    encoded = (divisor <= 1U) ? 0U : (divisor >> 1);
    clock_value = (uint16_t)(SDHCI_CLOCK_INT_EN |
        ((encoded & 0xFFU) << 8) |
        ((encoded & 0x300U) >> 2));

    mmio_write16(base, SDHCI_REG_CLOCK_CONTROL, clock_value);
    for (uint32_t spins = 0; spins < SDHCI_SPIN_MED; spins++) {
        if (mmio_read16(base, SDHCI_REG_CLOCK_CONTROL) & SDHCI_CLOCK_INT_STABLE) break;
        if (spins == SDHCI_SPIN_MED - 1U) return 0;
    }

    mmio_write16(base, SDHCI_REG_CLOCK_CONTROL, (uint16_t)(clock_value | SDHCI_CLOCK_CARD_EN));
    return 1;
}

static int sdhci_send_command(uint32_t base, uint16_t command, uint32_t argument, uint32_t* response0, uint32_t* status_out) {
    uint16_t flags = (uint16_t)(command & 0x00FFU);
    uint32_t inhibit_mask = SDHCI_PRESENT_CMD_INHIBIT;
    uint32_t status = 0;

    if (flags & SDHCI_CMD_DATA) inhibit_mask |= SDHCI_PRESENT_DATA_INHIBIT;
    if (!sdhci_wait_present_clear(base, inhibit_mask)) return 0;

    sdhci_clear_interrupts(base);
    mmio_write32(base, SDHCI_REG_ARGUMENT, argument);
    mmio_write16(base, SDHCI_REG_COMMAND, command);

    if (!sdhci_wait_interrupt(base, SDHCI_INT_CMD_COMPLETE, &status)) {
        if (status_out) *status_out = status;
        sdhci_recover_error(base, SDHCI_RESET_CMD);
        return 0;
    }
    if (response0) *response0 = mmio_read32(base, SDHCI_REG_RESPONSE0);
    if (status_out) *status_out = status;

    if ((flags & 0x0003U) == SDHCI_CMD_RESP_SHORT_BUSY) {
        if (!sdhci_wait_present_clear(base, SDHCI_PRESENT_DATA_INHIBIT)) return 0;
    }
    return 1;
}

static int sdhci_read_data_block(uint32_t base, uint16_t cmd_index, uint32_t argument, void* buffer, uint32_t* status_out) {
    uint32_t* words = (uint32_t*)buffer;
    uint16_t command = (uint16_t)((cmd_index << 8) | SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC | SDHCI_CMD_INDEX | SDHCI_CMD_DATA);
    uint32_t response0 = 0;
    uint32_t status = 0;

    mmio_write16(base, SDHCI_REG_BLOCK_SIZE, 512);
    mmio_write16(base, SDHCI_REG_BLOCK_COUNT, 1);
    mmio_write16(base, SDHCI_REG_TRANSFER_MODE, (uint16_t)(SDHCI_TRNS_BLK_CNT_EN | SDHCI_TRNS_READ));

    if (!sdhci_send_command(base, command, argument, &response0, &status)) {
        if (status_out) *status_out = status;
        return 0;
    }
    if (!sdhci_wait_interrupt(base, SDHCI_INT_BUF_READ_READY, &status)) {
        if (status_out) *status_out = status;
        sdhci_recover_error(base, SDHCI_RESET_DATA);
        return 0;
    }
    for (int i = 0; i < 128; i++) words[i] = mmio_read32(base, SDHCI_REG_BUFFER_DATA);
    if (!sdhci_wait_interrupt(base, SDHCI_INT_XFER_COMPLETE, &status)) {
        if (status_out) *status_out = status;
        sdhci_recover_error(base, SDHCI_RESET_DATA);
        return 0;
    }
    if (status_out) *status_out = status;
    return 1;
}

static int sdhci_write_data_block(uint32_t base, uint16_t cmd_index, uint32_t argument, const void* buffer, uint32_t* status_out) {
    const uint32_t* words = (const uint32_t*)buffer;
    uint16_t command = (uint16_t)((cmd_index << 8) | SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC | SDHCI_CMD_INDEX | SDHCI_CMD_DATA);
    uint32_t response0 = 0;
    uint32_t status = 0;
    uint32_t spins;

    if (!sdhci_wait_present_clear(base, SDHCI_PRESENT_CMD_INHIBIT | SDHCI_PRESENT_DATA_INHIBIT)) {
        if (status_out) *status_out = 0xE001U;
        return 0;
    }
    if (!sdhci_wait_dat0_ready(base)) {
        if (status_out) *status_out = 0xE002U;
        return 0;
    }

    /* Max data timeout; Bay Trail eMMC program latency is high. */
    mmio_write8(base, SDHCI_REG_TIMEOUT_CONTROL, 0x0E);
    mmio_write16(base, SDHCI_REG_BLOCK_SIZE, 512);
    mmio_write16(base, SDHCI_REG_BLOCK_COUNT, 1);
    /* Explicit write direction: clear READ bit (bit 4). */
    mmio_write16(base, SDHCI_REG_TRANSFER_MODE, SDHCI_TRNS_BLK_CNT_EN);

    if (!sdhci_send_command(base, command, argument, &response0, &status)) {
        if (status_out) *status_out = status ? status : 0xE003U;
        return 0;
    }
    if (!sdhci_wait_interrupt_spins(base, SDHCI_INT_BUF_WRITE_READY, &status, SDHCI_SPIN_WRITE)) {
        if (status_out) *status_out = status ? status : 0xE004U;
        sdhci_recover_error(base, SDHCI_RESET_DATA);
        return 0;
    }
    for (int i = 0; i < 128; i++) mmio_write32(base, SDHCI_REG_BUFFER_DATA, words[i]);

    /*
     * Wait for Transfer Complete and/or DAT0 idle. Ignore spurious data-timeout
     * if the transfer already completed (seen as status 0x100002 on 80M4).
     */
    for (spins = 0; spins < SDHCI_SPIN_WRITE; spins++) {
        uint32_t present;
        status = mmio_read32(base, SDHCI_REG_INT_STATUS);
        if (status & SDHCI_INT_XFER_COMPLETE) {
            mmio_write32(base, SDHCI_REG_INT_STATUS,
                         SDHCI_INT_XFER_COMPLETE | (status & SDHCI_INT_ERROR_MASK));
            break;
        }
        present = mmio_read32(base, SDHCI_REG_PRESENT_STATE);
        if ((present & SDHCI_PRESENT_DAT0) &&
            !(present & (SDHCI_PRESENT_CMD_INHIBIT | SDHCI_PRESENT_DATA_INHIBIT))) {
            sdhci_clear_interrupts(base);
            break;
        }
        /* Fatal data CRC / end-bit without completion. */
        if ((status & 0x00600000U) && !(status & SDHCI_INT_XFER_COMPLETE)) {
            if (status_out) *status_out = status;
            sdhci_recover_error(base, SDHCI_RESET_DATA);
            return 0;
        }
        if (spins == SDHCI_SPIN_WRITE - 1U) {
            if (status_out) *status_out = status ? status : 0xE005U;
            sdhci_recover_error(base, SDHCI_RESET_DATA);
            return 0;
        }
    }

    if (!sdhci_wait_dat0_ready(base)) {
        if (status_out) *status_out = 0xE006U;
        return 0;
    }
    if (!sdhci_wait_present_clear(base, SDHCI_PRESENT_CMD_INHIBIT | SDHCI_PRESENT_DATA_INHIBIT)) {
        if (status_out) *status_out = 0xE007U;
        return 0;
    }
    if (status_out) *status_out = status;
    return 1;
}

static sdhci_state_t* sdhci_find_state(uint8_t bus, uint8_t slot, uint8_t func) {
    for (int i = 0; i < SDHCI_MAX_CONTROLLERS; i++) {
        if (controller_states[i].active &&
            controller_states[i].bus == bus &&
            controller_states[i].slot == slot &&
            controller_states[i].func == func) {
            return &controller_states[i];
        }
    }
    return 0;
}

static sdhci_state_t* sdhci_alloc_state(uint8_t bus, uint8_t slot, uint8_t func) {
    sdhci_state_t* state = sdhci_find_state(bus, slot, func);
    if (state) return state;

    for (int i = 0; i < SDHCI_MAX_CONTROLLERS; i++) {
        if (!controller_states[i].active) {
            memset(&controller_states[i], 0, sizeof(controller_states[i]));
            controller_states[i].active = 1;
            controller_states[i].bus = bus;
            controller_states[i].slot = slot;
            controller_states[i].func = func;
            return &controller_states[i];
        }
    }
    return 0;
}

static void sdhci_breadcrumb(uint8_t bus, uint8_t slot, uint8_t func,
                             uint8_t step) {
    char buf[8];
    print("sdhci: step ");
    itoa((int)step, buf, 10);
    print(buf);
    print(" ");
    itoa((int)bus, buf, 10);
    print(buf);
    print(":");
    itoa((int)slot, buf, 10);
    print(buf);
    print(".");
    itoa((int)func, buf, 10);
    print(buf);
    print("\n");
}

static int sdhci_is_intel_baytrail(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t id = pci_read_config_dword(bus, slot, func, 0x00);
    uint16_t ven = (uint16_t)(id & 0xFFFFU);
    uint16_t dev = (uint16_t)((id >> 16) & 0xFFFFU);
    if (ven != 0x8086U) return 0;
    /* 0F14 = ACPI-mode ID; 0F50 = PCI-mode ID after SCC restore (80M4). */
    return dev == 0x0F14U || dev == 0x0F15U || dev == 0x0F16U ||
           dev == 0x0F50U || dev == 0x0F51U || dev == 0x0F52U;
}

static void sdhci_set_host_bus_width(uint32_t base, int width_bits) {
    uint8_t hc1 = mmio_read8(base, SDHCI_REG_HOST_CONTROL1);
    hc1 = (uint8_t)(hc1 & ~(SDHCI_CTRL_4BITBUS | SDHCI_CTRL_8BITBUS));
    if (width_bits == 8) hc1 = (uint8_t)(hc1 | SDHCI_CTRL_8BITBUS);
    else if (width_bits == 4) hc1 = (uint8_t)(hc1 | SDHCI_CTRL_4BITBUS);
    mmio_write8(base, SDHCI_REG_HOST_CONTROL1, hc1);
}

/* MMC SWITCH write-byte to EXT_CSD index. Returns 1 on success. */
static int sdhci_mmc_switch(uint32_t base, uint8_t index, uint8_t value, uint32_t* status_out) {
    uint32_t sw = (3U << 24) | ((uint32_t)value << 16) | ((uint32_t)index << 8);
    return sdhci_send_command(base,
                              (uint16_t)((MMC_CMD_SWITCH << 8) |
                                         SDHCI_CMD_RESP_SHORT_BUSY |
                                         SDHCI_CMD_CRC | SDHCI_CMD_INDEX),
                              sw, 0, status_out);
}

static void sdhci_spin_delay(uint32_t iterations) {
    for (uint32_t i = 0; i < iterations; i++) { }
}

/*
 * Intel Bay Trail eMMC (8086:0F14): power-on before reset (SDHCI_INTEL quirk).
 * Many boards never come up with the generic reset-then-power sequence.
 */
static int sdhci_intel_byt_power_reset(uint32_t base, uint32_t caps0) {
    uint8_t power = sdhci_pick_power(caps0);

    mmio_write8(base, SDHCI_REG_POWER_CONTROL, 0);
    sdhci_spin_delay(50000U);
    if (power == 0) {
        /* Soldered eMMC on 80M4-class boards is typically 1.8V. */
        power = SDHCI_POWER_180;
    }
    mmio_write8(base, SDHCI_REG_POWER_CONTROL, power);
    sdhci_spin_delay(10000U);
    mmio_write8(base, SDHCI_REG_POWER_CONTROL, (uint8_t)(power | SDHCI_POWER_ON));
    sdhci_spin_delay(10000U);

    mmio_write8(base, SDHCI_REG_SOFTWARE_RESET, SDHCI_RESET_ALL);
    if (!sdhci_wait_reset_clear(base, SDHCI_RESET_ALL))
        return 0;

    mmio_write8(base, SDHCI_REG_TIMEOUT_CONTROL, 0x0E);
    sdhci_clear_interrupts(base);
    mmio_write16(base, SDHCI_REG_INT_ENABLE, SDHCI_NORMAL_INT_MASK);
    mmio_write16(base, SDHCI_REG_ERR_INT_ENABLE, SDHCI_ERROR_INT_MASK);
    mmio_write16(base, SDHCI_REG_SIGNAL_ENABLE, 0);
    mmio_write16(base, SDHCI_REG_ERR_SIGNAL_ENABLE, 0);

    /* Linux SDHCI_QUIRK2_PRESET_VALUE_BROKEN for Bay Trail eMMC. */
    {
        uint16_t hc2 = mmio_read16(base, SDHCI_REG_HOST_CONTROL2);
        hc2 = (uint16_t)(hc2 & ~SDHCI_HC2_PRESET_VAL_ENABLE);
        mmio_write16(base, SDHCI_REG_HOST_CONTROL2, hc2);
    }
    return 1;
}

static int sdhci_bar_valid(uint32_t bar0, uint32_t* mmio_out) {
    uint32_t mmio_base;

    if ((bar0 & 0x1U) != 0) return 0;
    if (bar0 == 0 || bar0 == 0xFFFFFFFFU) return 0;
    mmio_base = bar0 & ~0xFU;
    if (mmio_base == 0) return 0;
    if (mmio_out) *mmio_out = mmio_base;
    return 1;
}

static int sdhci_init_card(uint8_t bus,
                           uint8_t slot,
                           uint8_t func,
                           uint32_t bar0,
                           int force_byt,
                           int skip_pci,
                           sdhci_probe_result_t* out) {
    uint16_t pci_command;
    uint32_t mmio_base;
    uint32_t caps0;
    uint32_t caps1;
    uint32_t present_state;
    uint32_t ocr = 0;
    uint16_t host_version;
    uint8_t ext_csd[512];
    sdhci_state_t* state;
    uint32_t status = 0;
    int intel_byt = force_byt || sdhci_is_intel_baytrail(bus, slot, func);

    if (!out) return 0;

    memset(out, 0, sizeof(*out));
    out->init_step = 1;
    sdhci_breadcrumb(bus, slot, func, 1);
    if (!sdhci_bar_valid(bar0, &mmio_base)) {
        if (!skip_pci) {
            uint32_t bar1 = pci_read_config_dword(bus, slot, func, 0x14);
            if (!sdhci_bar_valid(bar1, &mmio_base))
                return 0;
        } else {
            return 0;
        }
    }

    if (!skip_pci) {
        pci_command = pci_read_config_word(bus, slot, func, PCI_COMMAND_OFFSET);
        pci_command |= (uint16_t)(PCI_COMMAND_MEMORY | PCI_COMMAND_BUSMASTER);
        pci_write_config_word(bus, slot, func, PCI_COMMAND_OFFSET, pci_command);
    }

    caps0 = mmio_read32(mmio_base, SDHCI_REG_CAPABILITIES0);
    caps1 = mmio_read32(mmio_base, SDHCI_REG_CAPABILITIES1);
    host_version = mmio_read16(mmio_base, SDHCI_REG_HOST_VERSION);
    if (caps0 == 0xFFFFFFFFU || caps0 == 0 || host_version == 0xFFFFU) {
        if (intel_byt)
            print("sdhci: Intel Bay Trail MMIO unreadable (check BAR)\n");
        return 0;
    }

    out->mmio_accessible = 1;
    out->mmio_base = mmio_base;
    out->caps0 = caps0;
    out->caps1 = caps1;
    out->host_version = host_version;
    out->init_step = 2;
    sdhci_breadcrumb(bus, slot, func, 2);

    if (intel_byt) {
        print("sdhci: Intel Bay Trail eMMC/SDHCI init\n");
        if (!sdhci_intel_byt_power_reset(mmio_base, caps0))
            return 0;
    } else {
        mmio_write8(mmio_base, SDHCI_REG_SOFTWARE_RESET, SDHCI_RESET_ALL);
        if (!sdhci_wait_reset_clear(mmio_base, SDHCI_RESET_ALL)) return 0;

        mmio_write8(mmio_base, SDHCI_REG_TIMEOUT_CONTROL, 0x0E);
        sdhci_clear_interrupts(mmio_base);
        mmio_write16(mmio_base, SDHCI_REG_INT_ENABLE, SDHCI_NORMAL_INT_MASK);
        mmio_write16(mmio_base, SDHCI_REG_ERR_INT_ENABLE, SDHCI_ERROR_INT_MASK);
        mmio_write16(mmio_base, SDHCI_REG_SIGNAL_ENABLE, 0);
        mmio_write16(mmio_base, SDHCI_REG_ERR_SIGNAL_ENABLE, 0);

        if (!sdhci_enable_power(mmio_base, caps0)) return 0;
    }

    if (!sdhci_set_clock(mmio_base, caps0, 400U)) return 0;

    present_state = mmio_read32(mmio_base, SDHCI_REG_PRESENT_STATE);
    out->card_present = (present_state & SDHCI_PRESENT_CARD_INSERTED) ? 1 : 0;
    out->write_protected = (present_state & SDHCI_PRESENT_WRITE_PROTECT) ? 1 : 0;
    out->voltage_18 = (caps0 & (1U << 26)) ? 1 : 0;
    out->voltage_30 = (caps0 & (1U << 25)) ? 1 : 0;
    out->voltage_33 = (caps0 & (1U << 24)) ? 1 : 0;
    out->supports_8bit = (caps0 & (1U << 18)) ? 1 : 0;
    out->controller_ready = 1;
    out->init_step = 3;
    sdhci_breadcrumb(bus, slot, func, 3);

    /*
     * Soldered eMMC (PCI class 0x08/0x05) often never asserts the SD-slot
     * CARD_INSERTED bit. Treat a clear CD line as embedded media and continue
     * MMC init; failure still leaves the controller non-READY.
     */
    if (!out->card_present) {
        print("sdhci: CD clear; treating as embedded eMMC\n");
        out->card_present = 1;
    }

    if (!sdhci_send_command(mmio_base, (uint16_t)(MMC_CMD_GO_IDLE << 8), 0, 0, &status)) {
        out->last_status = status;
        return 1;
    }
    out->init_step = 4;
    sdhci_breadcrumb(bus, slot, func, 4);

    for (int attempt = 0; attempt < 200; attempt++) {
        if (!sdhci_send_command(mmio_base,
                                (uint16_t)((MMC_CMD_SEND_OP_COND << 8) | SDHCI_CMD_RESP_SHORT),
                                MMC_OCR_SECTOR_MODE | MMC_OCR_VOLTAGE_MASK,
                                &ocr,
                                &status)) {
            sdhci_spin_delay(20000U);
            continue;
        }
        if (ocr & MMC_OCR_BUSY) break;
        if (attempt == 199) {
            out->last_status = status;
            print("sdhci: eMMC SEND_OP_COND timed out\n");
            return 1;
        }
    }

    out->high_capacity = (ocr & MMC_OCR_SECTOR_MODE) ? 1 : 0;
    out->init_step = 5;
    sdhci_breadcrumb(bus, slot, func, 5);

    if (!sdhci_send_command(mmio_base,
                            (uint16_t)((MMC_CMD_ALL_SEND_CID << 8) | SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC),
                            0,
                            0,
                            &status)) {
        out->last_status = status;
        return 1;
    }

    out->rca = 1;
    if (!sdhci_send_command(mmio_base,
                            (uint16_t)((MMC_CMD_SET_RELATIVE_ADDR << 8) | SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC),
                            (uint32_t)out->rca << 16,
                            0,
                            &status)) {
        out->last_status = status;
        return 1;
    }
    out->init_step = 6;
    sdhci_breadcrumb(bus, slot, func, 6);

    if (!sdhci_send_command(mmio_base,
                            (uint16_t)((MMC_CMD_SELECT_CARD << 8) | SDHCI_CMD_RESP_SHORT_BUSY | SDHCI_CMD_CRC | SDHCI_CMD_INDEX),
                            (uint32_t)out->rca << 16,
                            0,
                            &status)) {
        out->last_status = status;
        return 1;
    }

    /* Stay on 1-bit until EXT_CSD succeeds (Linux / reference order). */
    sdhci_set_host_bus_width(mmio_base, 1);

    /* Always set block length; harmless on sector-addressed eMMC. */
    if (!sdhci_send_command(mmio_base,
                            (uint16_t)((MMC_CMD_SET_BLOCKLEN << 8) | SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC | SDHCI_CMD_INDEX),
                            512,
                            0,
                            &status)) {
        out->last_status = status;
        return 1;
    }
    out->init_step = 7;
    sdhci_breadcrumb(bus, slot, func, 7);

    if (!sdhci_read_data_block(mmio_base, MMC_CMD_SEND_EXT_CSD, 0, ext_csd, &status)) {
        /* Retry once after CMD/DATA reset + forced 1-bit (8-bit SWITCH may have run earlier). */
        print("sdhci: EXT_CSD failed; retry 1-bit\n");
        sdhci_recover_error(mmio_base, (uint8_t)(SDHCI_RESET_CMD | SDHCI_RESET_DATA));
        sdhci_set_host_bus_width(mmio_base, 1);
        sdhci_spin_delay(50000U);
        if (!sdhci_read_data_block(mmio_base, MMC_CMD_SEND_EXT_CSD, 0, ext_csd, &status)) {
            out->last_status = status;
            print("sdhci: EXT_CSD read failed status=");
            {
                char b[16];
                itoa((int)status, b, 16);
                print(b);
                print("\n");
            }
            return 1;
        }
    }

    out->sector_count =
        (uint32_t)ext_csd[EXT_CSD_SEC_COUNT] |
        ((uint32_t)ext_csd[EXT_CSD_SEC_COUNT + 1] << 8) |
        ((uint32_t)ext_csd[EXT_CSD_SEC_COUNT + 2] << 16) |
        ((uint32_t)ext_csd[EXT_CSD_SEC_COUNT + 3] << 24);

    /*
     * Firmware/OS may leave PARTITION_ACCESS pointing at a boot partition.
     * Always force user-area access before any install writes.
     */
    {
        uint8_t part_cfg = ext_csd[EXT_CSD_PART_CONFIG];
        uint8_t cleared = (uint8_t)(part_cfg & (uint8_t)~EXT_CSD_PART_ACCESS_MASK);
        if (cleared != part_cfg) {
            print("sdhci: clearing eMMC PARTITION_ACCESS\n");
        }
        if (!sdhci_mmc_switch(mmio_base, EXT_CSD_PART_CONFIG, cleared, &status)) {
            if (part_cfg & EXT_CSD_PART_ACCESS_MASK)
                print("sdhci: PARTITION_ACCESS switch failed\n");
        }
    }

    /*
     * Stay on 1-bit for Bay Trail install writes. 4/8-bit without tuning has
     * caused data-timeout on CMD24 (status 0x100002) on 80M4.
     */
    sdhci_set_host_bus_width(mmio_base, 1);
    (void)sdhci_mmc_switch(mmio_base, EXT_CSD_BUS_WIDTH, EXT_CSD_BUS_WIDTH_1, &status);

    /* eMMC write-protect pin is not a reliable install gate. */
    out->write_protected = 0;

    /* Moderate clock: 25 MHz is fine for reads; keep it for writes too. */
    if (!sdhci_set_clock(mmio_base, caps0, 25000U)) return 1;
    mmio_write8(mmio_base, SDHCI_REG_TIMEOUT_CONTROL, 0x0E);
    out->init_step = 8;
    sdhci_breadcrumb(bus, slot, func, 8);

    state = sdhci_alloc_state(bus, slot, func);
    if (!state) return 1;

    state->mmio_base = mmio_base;
    state->rca = out->rca;
    state->high_capacity = out->high_capacity;
    state->write_protected = out->write_protected;
    state->sector_count = out->sector_count;

    out->initialized = (out->sector_count != 0);
    if (out->initialized) {
        print("sdhci: eMMC sectors=");
        {
            char b[16];
            itoa((int)out->sector_count, b, 10);
            print(b);
            print("\n");
        }
    } else {
        print("sdhci: EXT_CSD SEC_COUNT is zero\n");
    }
    return 1;
}

int sdhci_probe_pci_controller(uint8_t bus,
                               uint8_t slot,
                               uint8_t func,
                               uint32_t bar0,
                               sdhci_probe_result_t* out) {
    return sdhci_init_card(bus, slot, func, bar0, 0, 0, out);
}

int sdhci_probe_mmio(uint32_t mmio_base,
                     int baytrail_quirks,
                     uint8_t bus,
                     uint8_t slot,
                     uint8_t func,
                     sdhci_probe_result_t* out) {
    return sdhci_init_card(bus, slot, func, mmio_base, baytrail_quirks ? 1 : 0, 1, out);
}

static int sdhci_rw_sector_common(sdhci_state_t* state, uint32_t lba, void* buffer, int write) {
    uint32_t argument;
    uint32_t base;
    uint32_t status = 0;

    if (!state || !buffer) return 0;
    if (state->sector_count != 0 && lba >= state->sector_count) return 0;

    base = state->mmio_base;
    argument = state->high_capacity ? lba : (lba << 9);

    if (write && state->write_protected) return 0;
    if (write) {
        if (!sdhci_write_data_block(base, MMC_CMD_WRITE_BLOCK, argument, buffer, &status)) {
            char b[16];
            print("sdhci: write LBA ");
            itoa((int)lba, b, 10); print(b);
            print(" failed status=");
            itoa((int)status, b, 16); print(b);
            print("\n");
            return 0;
        }
        return 1;
    }
    return sdhci_read_data_block(base, MMC_CMD_READ_SINGLE_BLOCK, argument, buffer, &status);
}

int sdhci_read_sector(uint8_t bus,
                      uint8_t slot,
                      uint8_t func,
                      uint32_t lba,
                      void* out_sector) {
    sdhci_state_t* state = sdhci_find_state(bus, slot, func);
    if (!state) return -1;
    return sdhci_rw_sector_common(state, lba, out_sector, 0) ? 0 : -1;
}

int sdhci_write_sector(uint8_t bus,
                       uint8_t slot,
                       uint8_t func,
                       uint32_t lba,
                       const void* in_sector) {
    sdhci_state_t* state = sdhci_find_state(bus, slot, func);
    if (!state) {
        print("sdhci: write: no controller state\n");
        return -1;
    }
    return sdhci_rw_sector_common(state, lba, (void*)in_sector, 1) ? 0 : -1;
}

int sdhci_flush(uint8_t bus, uint8_t slot, uint8_t func) {
    sdhci_state_t* state = sdhci_find_state(bus, slot, func);
    uint32_t base;
    uint32_t status = 0;
    /* EXT_CSD CACHE_CTRL flush via SWITCH (index 32, value 1) when possible. */
    uint32_t sw = (3U << 24) | (1U << 16) | (32U << 8);

    (void)bus;
    (void)slot;
    (void)func;
    if (!state) return -1;
    base = state->mmio_base;
    if (!sdhci_wait_present_clear(base, SDHCI_PRESENT_CMD_INHIBIT | SDHCI_PRESENT_DATA_INHIBIT))
        return -1;
    /* Best-effort cache flush; ignore failure on devices without cache. */
    sdhci_send_command(base,
                       (uint16_t)((MMC_CMD_SWITCH << 8) |
                                  SDHCI_CMD_RESP_SHORT_BUSY |
                                  SDHCI_CMD_CRC | SDHCI_CMD_INDEX),
                       sw, 0, &status);
    if (!sdhci_wait_present_clear(base, SDHCI_PRESENT_CMD_INHIBIT | SDHCI_PRESENT_DATA_INHIBIT))
        return -1;
    return 0;
}
