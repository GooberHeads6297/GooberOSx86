#ifndef IOSF_MBI_H
#define IOSF_MBI_H

#include <stdint.h>

/* Bay Trail Storage Control Cluster (SCC) via IOSF sideband. */
#define IOSF_PORT_SCC           0x63U
#define IOSF_OP_READ_SCC        0x06U
#define IOSF_OP_WRITE_SCC       0x07U
#define SCC_MMC_CTL             0x50CU
#define SCC_SDIO_CTL            0x508U
#define SCC_SD_CTL              0x504U
#define SCC_CTL_PCI_CFG_DIS     (1U << 0)
#define SCC_CTL_ACPI_INT_EN     (1U << 1)

/* Returns 1 if host bridge 8086:0F00 is present (Bay Trail IOSF mailbox). */
int iosf_mbi_available(void);

/* Read/write IOSF sideband register. Returns 1 on success. */
int iosf_mbi_read(uint8_t port, uint8_t opcode, uint32_t offset, uint32_t* out);
int iosf_mbi_write(uint8_t port, uint8_t opcode, uint32_t offset, uint32_t value);

/*
 * Bay Trail firmware often hides eMMC (8086:0F14) from PCI by setting
 * SCC_MMC_CTL PCI_CFG_DIS. Clear that so the controller reappears for SDHCI.
 * Returns 1 if PCI mode was restored (or already PCI), 0 on failure.
 */
int iosf_baytrail_scc_enable_pci_emmc(uint32_t* ctl_before_out, uint32_t* ctl_after_out);

#endif
