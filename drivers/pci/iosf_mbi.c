#include "iosf_mbi.h"
#include "pci.h"

#define MBI_MCR_OFFSET  0xD0U
#define MBI_MDR_OFFSET  0xD4U
#define MBI_MCRX_OFFSET 0xD8U
#define MBI_ENABLE      0xF0U

static uint32_t iosf_form_mcr(uint8_t op, uint8_t port, uint8_t offset_lo) {
    return ((uint32_t)op << 24) | ((uint32_t)port << 16) |
           ((uint32_t)offset_lo << 8) | MBI_ENABLE;
}

int iosf_mbi_available(void) {
    uint32_t id = pci_read_config_dword(0, 0, 0, 0x00);
    uint16_t ven = (uint16_t)(id & 0xFFFFU);
    uint16_t dev = (uint16_t)((id >> 16) & 0xFFFFU);
    /* Bay Trail 0F00 and Braswell/Cherry Trail 2280 share the MCR/MDR mailbox. */
    return ven == 0x8086U && (dev == 0x0F00U || dev == 0x2280U);
}

int iosf_mbi_is_braswell(void) {
    uint32_t id = pci_read_config_dword(0, 0, 0, 0x00);
    uint16_t ven = (uint16_t)(id & 0xFFFFU);
    uint16_t dev = (uint16_t)((id >> 16) & 0xFFFFU);
    return ven == 0x8086U && dev == 0x2280U;
}

int iosf_mbi_read(uint8_t port, uint8_t opcode, uint32_t offset, uint32_t* out) {
    uint32_t mcr;
    uint32_t mcrx;

    if (!out || !iosf_mbi_available()) return 0;

    mcrx = offset & 0xFFFFFF00U;
    mcr = iosf_form_mcr(opcode, port, (uint8_t)(offset & 0xFFU));

    if (mcrx)
        pci_write_config_dword(0, 0, 0, (uint8_t)MBI_MCRX_OFFSET, mcrx);
    pci_write_config_dword(0, 0, 0, (uint8_t)MBI_MCR_OFFSET, mcr);
    *out = pci_read_config_dword(0, 0, 0, (uint8_t)MBI_MDR_OFFSET);
    return 1;
}

int iosf_mbi_write(uint8_t port, uint8_t opcode, uint32_t offset, uint32_t value) {
    uint32_t mcr;
    uint32_t mcrx;

    if (!iosf_mbi_available()) return 0;

    mcrx = offset & 0xFFFFFF00U;
    mcr = iosf_form_mcr(opcode, port, (uint8_t)(offset & 0xFFU));

    pci_write_config_dword(0, 0, 0, (uint8_t)MBI_MDR_OFFSET, value);
    if (mcrx)
        pci_write_config_dword(0, 0, 0, (uint8_t)MBI_MCRX_OFFSET, mcrx);
    pci_write_config_dword(0, 0, 0, (uint8_t)MBI_MCR_OFFSET, mcr);
    return 1;
}

static int iosf_scc_enable_pci_emmc_at(uint32_t mmc_ctl_off,
                                       uint32_t* ctl_before_out,
                                       uint32_t* ctl_after_out) {
    uint32_t ctl = 0;
    uint32_t after = 0;

    if (!iosf_mbi_available()) return 0;
    if (!iosf_mbi_read(IOSF_PORT_SCC, IOSF_OP_READ_SCC, mmc_ctl_off, &ctl))
        return 0;
    if (ctl_before_out) *ctl_before_out = ctl;

    /* Clear ACPI-mode bits so the eMMC function reappears on PCI. */
    after = ctl & ~(SCC_CTL_PCI_CFG_DIS | SCC_CTL_ACPI_INT_EN);
    if (after != ctl) {
        if (!iosf_mbi_write(IOSF_PORT_SCC, IOSF_OP_WRITE_SCC, mmc_ctl_off, after))
            return 0;
        if (!iosf_mbi_read(IOSF_PORT_SCC, IOSF_OP_READ_SCC, mmc_ctl_off, &after))
            return 0;
    }
    if (ctl_after_out) *ctl_after_out = after;
    return 1;
}

int iosf_baytrail_scc_enable_pci_emmc(uint32_t* ctl_before_out, uint32_t* ctl_after_out) {
    if (iosf_mbi_is_braswell()) return 0;
    return iosf_scc_enable_pci_emmc_at(SCC_MMC_CTL, ctl_before_out, ctl_after_out);
}

int iosf_braswell_scc_enable_pci_emmc(uint32_t* ctl_before_out, uint32_t* ctl_after_out) {
    if (!iosf_mbi_is_braswell()) return 0;
    /* Braswell SCC_MMC_CTL is 0x500 (Bay Trail uses 0x50C). */
    return iosf_scc_enable_pci_emmc_at(SCC_MMC_CTL_BSW, ctl_before_out, ctl_after_out);
}
