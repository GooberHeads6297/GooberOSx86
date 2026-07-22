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
/* HOST_CONTROL2 UHS Mode Select (bits [2:0]): 0=SDR12, 1=SDR25, 2=SDR50,
 * 3=SDR104/HS200, 4=DDR50. Any non-zero mode needs a tuned DAT sampling phase;
 * left set by firmware it corrupts the ID-phase data read (commands still work).
 * Force it to 0 (SDR12) so the untuned identification read samples correctly. */
#define SDHCI_HC2_UHS_MODE_MASK    0x0007U

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

#define SDHCI_TRNS_DMA             0x0001
#define SDHCI_TRNS_BLK_CNT_EN      0x0002
#define SDHCI_TRNS_READ            0x0010

#define SDHCI_REG_SDMA_ADDRESS     0x00
#define SDHCI_INT_DMA              0x00000008U
/* HOST_CONTROL1 DMA Select field (bits [4:3]); 00b = SDMA, 10b = 32-bit ADMA2. */
#define SDHCI_CTRL_DMA_SELECT_MASK 0x18
#define SDHCI_CTRL_ADMA2_32        0x10
/* SDMA buffer boundary = 512 KiB (largest) so a single 512-byte block never
 * crosses a boundary and no SDMA-interrupt reprogramming is needed. */
#define SDHCI_BLKSZ_BOUNDARY_512K  (7u << 12)

/*
 * ADMA2 (Advanced DMA v2) is the reference data path for Intel Bay Trail /
 * Braswell LPSS SDHCI: their PIO buffer port is broken and their SDMA engine
 * lands only partial/misplaced data, so Linux drives them via ADMA2. A 32-bit
 * ADMA2 descriptor is 8 bytes: 16-bit attribute, 16-bit length, 32-bit address.
 */
#define SDHCI_REG_ADMA_ERROR       0x54
#define SDHCI_REG_ADMA_ADDRESS     0x58
#define SDHCI_INT_ADMA_ERROR       0x02000000U
#define SDHCI_ADMA2_VALID          0x0001
#define SDHCI_ADMA2_END            0x0002
#define SDHCI_ADMA2_INT            0x0004
#define SDHCI_ADMA2_ACT_TRAN       0x0020

typedef struct {
    uint16_t attr;
    uint16_t len;
    uint32_t addr;
} __attribute__((packed)) sdhci_adma2_desc_t;

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

/*
 * SDMA bounce buffer. Intel Bay Trail/Braswell LPSS SDHCI does not deliver data
 * through the PIO buffer (BUFFER_DATA) reliably — reads return non-deterministic
 * garbage and latch DATA_TIMEOUT/END_BIT even though XFER_COMPLETE asserts. Like
 * Linux, we drive data transfers via SDMA instead. The x64 kernel identity-maps
 * the low 4 GiB (boot64.s), so this buffer's virtual address IS its physical
 * address, and x86 is cache-coherent with DMA, so no translation/flush needed.
 * 4 KiB alignment keeps the transfer well inside one SDMA boundary.
 */
static uint8_t sdhci_dma_buf[512] __attribute__((aligned(4096)));

/* Diagnostic-only second EXT_CSD landing zone, used to test whether the SDMA
 * data phase is deterministic (same bytes on a re-read) or racy/partial. */
static uint8_t sdhci_extcsd_cmp[512] __attribute__((aligned(4096)));

/* ADMA2 descriptor table (a single 512-byte block needs one TRAN descriptor;
 * spare entries keep the table valid if it is ever extended). Must be 32-bit
 * addressable and at least 4-byte aligned per the SDHCI spec. */
static sdhci_adma2_desc_t sdhci_adma_desc[4] __attribute__((aligned(8)));

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

/*
 * ADMA2 single-block data transfer (read or write). This is the reference data
 * path for Intel Bay Trail / Braswell LPSS SDHCI, whose PIO buffer port is
 * broken and whose SDMA engine lands only partial/misplaced data. On success
 * the caller's 512-byte buffer holds the read data (or has been written to the
 * card). Data errors are NOT masked: DATA_TIMEOUT/CRC/END_BIT/ADMA_ERROR cause a
 * real failure return with the raw INT_STATUS in *status_out.
 */
static int sdhci_data_block_dma(uint32_t base, uint16_t cmd_index, uint32_t argument,
                                void* buffer, int is_write, uint32_t* status_out) {
    uint16_t command = (uint16_t)((cmd_index << 8) | SDHCI_CMD_RESP_SHORT |
                                  SDHCI_CMD_CRC | SDHCI_CMD_INDEX | SDHCI_CMD_DATA);
    uint32_t status = 0;
    uint32_t phys = (uint32_t)(uintptr_t)sdhci_dma_buf;
    uint32_t desc_phys = (uint32_t)(uintptr_t)sdhci_adma_desc;
    uint16_t mode;
    uint8_t hc1;
    uint32_t spins;

    if (is_write) memcpy(sdhci_dma_buf, buffer, 512);

    if (!sdhci_wait_present_clear(base, SDHCI_PRESENT_CMD_INHIBIT | SDHCI_PRESENT_DATA_INHIBIT)) {
        if (status_out) *status_out = 0xD001U;
        return 0;
    }
    if (is_write && !sdhci_wait_dat0_ready(base)) {
        if (status_out) *status_out = 0xD002U;
        return 0;
    }

    /* One ADMA2 TRAN descriptor covering the whole 512-byte block, marked END
     * so the engine stops after it. INT is not requested (we poll). */
    sdhci_adma_desc[0].attr = (uint16_t)(SDHCI_ADMA2_VALID | SDHCI_ADMA2_END |
                                         SDHCI_ADMA2_ACT_TRAN);
    sdhci_adma_desc[0].len = 512;
    sdhci_adma_desc[0].addr = phys;

    /* Select 32-bit ADMA2 (HOST_CONTROL1 DMA Select = 10b). */
    hc1 = mmio_read8(base, SDHCI_REG_HOST_CONTROL1);
    hc1 = (uint8_t)((hc1 & ~SDHCI_CTRL_DMA_SELECT_MASK) | SDHCI_CTRL_ADMA2_32);
    mmio_write8(base, SDHCI_REG_HOST_CONTROL1, hc1);

    mmio_write8(base, SDHCI_REG_TIMEOUT_CONTROL, 0x0E);
    mmio_write32(base, SDHCI_REG_ADMA_ADDRESS, desc_phys);
    mmio_write16(base, SDHCI_REG_BLOCK_SIZE, 512);
    mmio_write16(base, SDHCI_REG_BLOCK_COUNT, 1);
    mode = (uint16_t)(SDHCI_TRNS_DMA | SDHCI_TRNS_BLK_CNT_EN |
                      (is_write ? 0 : SDHCI_TRNS_READ));
    mmio_write16(base, SDHCI_REG_TRANSFER_MODE, mode);

    if (!sdhci_send_command(base, command, argument, 0, &status)) {
        if (status_out) *status_out = status ? status : 0xD003U;
        sdhci_recover_error(base, SDHCI_RESET_DATA);
        return 0;
    }

    for (spins = 0; spins < SDHCI_SPIN_WRITE; spins++) {
        status = mmio_read32(base, SDHCI_REG_INT_STATUS);
        if (status & SDHCI_INT_XFER_COMPLETE) {
            mmio_write32(base, SDHCI_REG_INT_STATUS, SDHCI_INT_XFER_COMPLETE);
            break;
        }
        if (status & SDHCI_INT_ERROR_MASK) {
            if (status_out) *status_out = status;
            sdhci_recover_error(base, SDHCI_RESET_DATA);
            return 0;
        }
        if (spins == SDHCI_SPIN_WRITE - 1U) {
            if (status_out) *status_out = status ? status : 0xD004U;
            sdhci_recover_error(base, SDHCI_RESET_DATA);
            return 0;
        }
    }

    if (is_write) {
        if (!sdhci_wait_dat0_ready(base)) {
            if (status_out) *status_out = 0xD005U;
            return 0;
        }
    } else {
        memcpy(buffer, sdhci_dma_buf, 512);
    }
    if (!sdhci_wait_present_clear(base, SDHCI_PRESENT_CMD_INHIBIT | SDHCI_PRESENT_DATA_INHIBIT)) {
        if (status_out) *status_out = 0xD006U;
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
    /* 0F14 = ACPI-mode ID; 0F50 = PCI-mode ID after SCC restore (80M4).
     * 2294/2295/2296 = Braswell eMMC/SDIO/SD (same init quirks as BYT). */
    return dev == 0x0F14U || dev == 0x0F15U || dev == 0x0F16U ||
           dev == 0x0F50U || dev == 0x0F51U || dev == 0x0F52U ||
           dev == 0x2294U || dev == 0x2295U || dev == 0x2296U;
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

/* itoa() is signed and mangles 32-bit values with bit31 set (e.g. OCR busy).
 * Print an unsigned 8-digit hex value directly. */
static void sdhci_print_hex32(uint32_t v) {
    const char* hx = "0123456789abcdef";
    char buf[9];
    for (int i = 0; i < 8; i++) buf[7 - i] = hx[(v >> (i * 4)) & 0xF];
    buf[8] = '\0';
    print(buf);
}

/*
 * Intel Bay Trail / Braswell eMMC: power-on before reset (SDHCI_INTEL quirk).
 * Braswell after IOSF unhide can bus-stall on SOFTWARE_RESET_ALL — skip the
 * full reset there and only clear CMD/DATA engines.
 */
static int sdhci_intel_byt_power_reset(uint32_t base, uint32_t caps0) {
    uint8_t power;
    uint32_t id;
    int braswell_host;

    /* Print BEFORE any PCI/MMIO so a hang is distinguishable from an old ISO. */
    print("sdhci: build=bsw8-20260719-sdr12 power_reset enter\n");

    power = sdhci_pick_power(caps0);
    id = pci_read_config_dword(0, 0, 0, 0x00);
    braswell_host = ((id & 0xFFFFU) == 0x8086U) &&
                    (((id >> 16) & 0xFFFFU) == 0x2280U);
    print(braswell_host ? "sdhci: power sequence (Braswell host 2280)\n"
                        : "sdhci: power sequence (Bay Trail / other)\n");

    mmio_write8(base, SDHCI_REG_POWER_CONTROL, 0);
    sdhci_spin_delay(braswell_host ? 20000U : 50000U);
    print("sdhci: power off done\n");

    if (power == 0) {
        power = SDHCI_POWER_180;
    }
    mmio_write8(base, SDHCI_REG_POWER_CONTROL, power);
    sdhci_spin_delay(10000U);
    mmio_write8(base, SDHCI_REG_POWER_CONTROL, (uint8_t)(power | SDHCI_POWER_ON));
    sdhci_spin_delay(braswell_host ? 50000U : 10000U);
    print("sdhci: power on done\n");

    if (braswell_host) {
        print("sdhci: Braswell skip RESET_ALL; CMD+DATA reset\n");
        mmio_write8(base, SDHCI_REG_SOFTWARE_RESET,
                    (uint8_t)(SDHCI_RESET_CMD | SDHCI_RESET_DATA));
        if (!sdhci_wait_reset_clear(base, (uint8_t)(SDHCI_RESET_CMD | SDHCI_RESET_DATA)))
            print("sdhci: CMD/DATA reset timeout (continuing)\n");
        else
            print("sdhci: CMD/DATA reset ok\n");
    } else {
        mmio_write8(base, SDHCI_REG_SOFTWARE_RESET, SDHCI_RESET_ALL);
        if (!sdhci_wait_reset_clear(base, SDHCI_RESET_ALL)) {
            print("sdhci: RESET_ALL timeout\n");
            return 0;
        }
        print("sdhci: RESET_ALL ok\n");
    }

    mmio_write8(base, SDHCI_REG_TIMEOUT_CONTROL, 0x0E);
    sdhci_clear_interrupts(base);
    mmio_write16(base, SDHCI_REG_INT_ENABLE, SDHCI_NORMAL_INT_MASK);
    mmio_write16(base, SDHCI_REG_ERR_INT_ENABLE, SDHCI_ERROR_INT_MASK);
    mmio_write16(base, SDHCI_REG_SIGNAL_ENABLE, 0);
    mmio_write16(base, SDHCI_REG_ERR_SIGNAL_ENABLE, 0);

    {
        /*
         * Braswell skips RESET_ALL, so HOST_CONTROL2 still holds whatever UHS/
         * HS200 mode the laptop firmware left behind. Those modes require a tuned
         * DAT sampling phase; without tuning the CMD line still works but the
         * data phase reads back deterministic garbage and the card stalls
         * mid-block (exactly the EXT_CSD failure we see). Force SDR12 + drop
         * preset-value enable so the identification/EXT_CSD read samples in the
         * legacy, tune-free timing. High-Speed Enable in HOST_CONTROL1 is
         * cleared for the same reason.
         */
        uint16_t hc2 = mmio_read16(base, SDHCI_REG_HOST_CONTROL2);
        uint8_t hc1 = mmio_read8(base, SDHCI_REG_HOST_CONTROL1);
        hc2 = (uint16_t)(hc2 & ~(SDHCI_HC2_PRESET_VAL_ENABLE | SDHCI_HC2_UHS_MODE_MASK));
        mmio_write16(base, SDHCI_REG_HOST_CONTROL2, hc2);
        hc1 = (uint8_t)(hc1 & ~SDHCI_CTRL_HISPD);
        mmio_write8(base, SDHCI_REG_HOST_CONTROL1, hc1);
        print("sdhci: force SDR12 hc2=");
        sdhci_print_hex32(hc2);
        print(" hc1=");
        sdhci_print_hex32(hc1);
        print("\n");
    }
    print("sdhci: build=bsw8-20260719-sdr12 power_reset exit ok\n");
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
        print("sdhci: Intel eMMC/SDHCI init (BYT/BSW)\n");
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
    /* Card needs a quiet interval after clock enable before the first CMD. */
    sdhci_spin_delay(intel_byt ? 200000U : 50000U);

    present_state = mmio_read32(mmio_base, SDHCI_REG_PRESENT_STATE);
    out->card_present = (present_state & SDHCI_PRESENT_CARD_INSERTED) ? 1 : 0;
    /*
     * PRESENT_STATE write-protect is meaningless for soldered Intel eMMC
     * (often stuck asserted). Only trust WP after a full successful init,
     * and even then we clear it for BYT/BSW below.
     */
    out->write_protected = 0;
    out->voltage_18 = (caps0 & (1U << 26)) ? 1 : 0;
    out->voltage_30 = (caps0 & (1U << 25)) ? 1 : 0;
    out->voltage_33 = (caps0 & (1U << 24)) ? 1 : 0;
    out->supports_8bit = (caps0 & (1U << 18)) ? 1 : 0;
    out->controller_ready = 1;
    out->init_step = 3;
    sdhci_breadcrumb(bus, slot, func, 3);

    {
        char b[16];
        print("sdhci: present=");
        itoa((int)present_state, b, 16); print(b);
        print(" clock=");
        itoa((int)mmio_read16(mmio_base, SDHCI_REG_CLOCK_CONTROL), b, 16); print(b);
        print(" power=");
        itoa((int)mmio_read8(mmio_base, SDHCI_REG_POWER_CONTROL), b, 16); print(b);
        print("\n");
    }

    /*
     * Soldered eMMC (PCI class 0x08/0x05) often never asserts the SD-slot
     * CARD_INSERTED bit. Treat a clear CD line as embedded media and continue
     * MMC init; failure still leaves the controller non-READY.
     */
    if (!out->card_present) {
        print("sdhci: CD clear; treating as embedded eMMC\n");
        out->card_present = 1;
    }

    {
        int cmd0_ok = 0;
        int attempt;
        for (attempt = 0; attempt < 8; attempt++) {
            status = 0;
            if (sdhci_send_command(mmio_base, (uint16_t)(MMC_CMD_GO_IDLE << 8),
                                   0, 0, &status)) {
                cmd0_ok = 1;
                break;
            }
            {
                char b[16];
                print("sdhci: CMD0 GO_IDLE fail try=");
                itoa(attempt, b, 10); print(b);
                print(" status=");
                itoa((int)status, b, 16); print(b);
                print(" present=");
                itoa((int)mmio_read32(mmio_base, SDHCI_REG_PRESENT_STATE), b, 16);
                print(b);
                print("\n");
            }
            sdhci_recover_error(mmio_base, (uint8_t)(SDHCI_RESET_CMD | SDHCI_RESET_DATA));
            sdhci_spin_delay(100000U);
            (void)sdhci_set_clock(mmio_base, caps0, 400U);
            sdhci_spin_delay(100000U);
        }
        if (!cmd0_ok) {
            out->last_status = status;
            print("sdhci: CMD0 GO_IDLE gave up (step 3)\n");
            return 1;
        }
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

    /* Diagnostic: real card OCR is a sane voltage window (bit31 busy + voltage
     * bits). ocr==0xFFFFFFFF means responses are floating (card not driving the
     * bus despite CMD_COMPLETE); ocr==0 means no response latched. */
    print("sdhci: OCR=");
    sdhci_print_hex32(ocr);
    print("\n");

    {
        uint32_t cid0 = 0;
        if (!sdhci_send_command(mmio_base,
                                (uint16_t)((MMC_CMD_ALL_SEND_CID << 8) | SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC),
                                0,
                                &cid0,
                                &status)) {
            out->last_status = status;
            return 1;
        }
        /* CID[0] of a real eMMC is nonzero and != 0xFFFFFFFF. */
        print("sdhci: CID0=");
        sdhci_print_hex32(cid0);
        print("\n");
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

    /* Confirm the sampling mode actually stuck by the time we read data: HC1
     * (bus width + HISPD), HC2 (UHS mode), and the live line state. */
    print("sdhci: pre-read hc1=");
    sdhci_print_hex32(mmio_read8(mmio_base, SDHCI_REG_HOST_CONTROL1));
    print(" hc2=");
    sdhci_print_hex32(mmio_read16(mmio_base, SDHCI_REG_HOST_CONTROL2));
    print(" present=");
    sdhci_print_hex32(mmio_read32(mmio_base, SDHCI_REG_PRESENT_STATE));
    print("\n");

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

    if (!sdhci_data_block_dma(mmio_base, MMC_CMD_SEND_EXT_CSD, 0, ext_csd, 0, &status)) {
        /* Retry once after CMD/DATA reset + forced 1-bit (8-bit SWITCH may have run earlier). */
        print("sdhci: EXT_CSD failed; retry 1-bit\n");
        sdhci_recover_error(mmio_base, (uint8_t)(SDHCI_RESET_CMD | SDHCI_RESET_DATA));
        sdhci_set_host_bus_width(mmio_base, 1);
        sdhci_spin_delay(50000U);
        if (!sdhci_data_block_dma(mmio_base, MMC_CMD_SEND_EXT_CSD, 0, ext_csd, 0, &status)) {
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

    /*
     * Data-phase sanity: a real EXT_CSD block is 512 bytes of mostly-populated
     * config. If nonzero==0 the PIO data read returned an empty buffer (data
     * path broken) even though CMD_COMPLETE/XFER_COMPLETE latched — that is the
     * step-8/SEC_COUNT=0/status=0 signature. Print rev + a nonzero-byte count so
     * we can tell "no data transferred" from "SEC_COUNT field genuinely 0".
     */
    {
        char b[16];
        const char* hx = "0123456789abcdef";
        int nonzero = 0;
        int fnz = -1;
        int lnz = -1;
        for (int i = 0; i < 512; i++) {
            if (ext_csd[i] != 0) {
                nonzero++;
                if (fnz < 0) fnz = i;
                lnz = i;
            }
        }
        print("sdhci: EXT_CSD nonzero=");
        itoa(nonzero, b, 10); print(b);
        print(" first=");
        itoa(fnz, b, 10); print(b);
        print(" last=");
        itoa(lnz, b, 10); print(b);
        print(" rev=");
        itoa((int)ext_csd[192], b, 10); print(b);
        print("\n");
        /* Raw INT_STATUS from the data read. Bit20 (0x100000)=DATA_TIMEOUT,
         * bit21 (0x200000)=DATA_CRC, bit22 (0x400000)=DATA_END_BIT. If any are
         * set alongside XFER_COMPLETE (bit1) we masked a corrupt transfer. */
        print("sdhci: EXT_CSD rdstatus=");
        sdhci_print_hex32(status);
        print("\n");
        /*
         * DMA capability advert: bit22 = SDMA, bit19 = ADMA2. Intel LPSS
         * (Bay Trail / Braswell) is known to need ADMA2 for a reliable data
         * phase; if SDMA lands only partial/misplaced bytes we switch.
         */
        print("sdhci: CAPS0=");
        sdhci_print_hex32(caps0);
        print(" sdma=");
        print((caps0 & (1u << 22)) ? "1" : "0");
        print(" adma2=");
        print((caps0 & (1u << 19)) ? "1" : "0");
        print("\n");

        /*
         * Determinism probe: re-read EXT_CSD into a separate buffer and compare.
         * STABLE across two reads => the transfer is deterministic and the data
         * is systematically wrong (shift / command issue). VARIES => the SDMA
         * data phase is racy/partial (points at ADMA2 as the fix). This one bit
         * decides the next move, so it is worth the extra command.
         */
        {
            uint32_t s2 = 0;
            int nz2 = 0;
            int same = 1;
            for (int i = 0; i < 512; i++) sdhci_extcsd_cmp[i] = 0;
            if (sdhci_data_block_dma(mmio_base, MMC_CMD_SEND_EXT_CSD, 0,
                                     sdhci_extcsd_cmp, 0, &s2)) {
                for (int i = 0; i < 512; i++) {
                    if (sdhci_extcsd_cmp[i] != 0) nz2++;
                    if (sdhci_extcsd_cmp[i] != ext_csd[i]) same = 0;
                }
                print("sdhci: REREAD nz2=");
                itoa(nz2, b, 10); print(b);
                print(same ? " STABLE" : " VARIES");
                print(" s2=");
                sdhci_print_hex32(s2);
                print("\n");
            } else {
                print("sdhci: REREAD failed s2=");
                sdhci_print_hex32(s2);
                print("\n");
            }
        }

        /*
         * Compact 16-char occupancy map: one char per 32-byte row, '#' if the
         * row has any nonzero byte else '.'. Shows the cluster layout in a
         * single photo-friendly line (row 6 = bytes 192-223 holds REV@192,
         * row 6 also covers SEC_COUNT@212).
         */
        {
            char map[24];
            int mp = 0;
            for (int row = 0; row < 512; row += 32) {
                int any = 0;
                for (int i = row; i < row + 32; i++)
                    if (ext_csd[i]) { any = 1; break; }
                map[mp++] = any ? '#' : '.';
            }
            map[mp] = '\0';
            print("sdhci: MAP ");
            print(map);
            print("\n");
        }

        /* Exact 16-byte windows over the REV (192) and SEC_COUNT (208-215)
         * fields, so we can see whether those specific fields are zero vs the
         * data being present but shifted. */
        {
            static const int wins[2] = {192, 208};
            for (int w = 0; w < 2; w++) {
                char line[48];
                int p = 0;
                char ob[8];
                itoa(wins[w], ob, 10);
                for (int k = 0; ob[k]; k++) line[p++] = ob[k];
                line[p++] = ':';
                for (int i = wins[w]; i < wins[w] + 16; i++) {
                    line[p++] = hx[(ext_csd[i] >> 4) & 0xF];
                    line[p++] = hx[ext_csd[i] & 0xF];
                }
                line[p] = '\0';
                print("sdhci: @");
                print(line);
                print("\n");
            }
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
        if (!sdhci_data_block_dma(base, MMC_CMD_WRITE_BLOCK, argument, buffer, 1, &status)) {
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
    return sdhci_data_block_dma(base, MMC_CMD_READ_SINGLE_BLOCK, argument, buffer, 0, &status);
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
