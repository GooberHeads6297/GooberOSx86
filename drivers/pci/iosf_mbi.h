#ifndef IOSF_MBI_H
#define IOSF_MBI_H

#include <stdint.h>

/* Bay Trail / Braswell Storage Control Cluster (SCC) via IOSF sideband. */
#define IOSF_PORT_SCC           0x63U
#define IOSF_OP_READ_SCC        0x06U
#define IOSF_OP_WRITE_SCC       0x07U
/* Bay Trail MMC control offset; Braswell uses SCC_MMC_CTL_BSW (0x500). */
#define SCC_MMC_CTL             0x50CU
#define SCC_MMC_CTL_BSW         0x500U
#define SCC_SDIO_CTL            0x508U
#define SCC_SD_CTL              0x504U
#define SCC_CTL_PCI_CFG_DIS     (1U << 0)
#define SCC_CTL_ACPI_INT_EN     (1U << 1)

/* Returns 1 if host bridge has IOSF mailbox (BYT 0F00 or Braswell 2280). */
int iosf_mbi_available(void);
int iosf_mbi_is_braswell(void);

/* Read/write IOSF sideband register. Returns 1 on success. */
int iosf_mbi_read(uint8_t port, uint8_t opcode, uint32_t offset, uint32_t* out);
int iosf_mbi_write(uint8_t port, uint8_t opcode, uint32_t offset, uint32_t value);

/*
 * Firmware often hides eMMC from PCI by setting SCC_MMC_CTL PCI_CFG_DIS.
 * Clear that so the controller reappears for SDHCI (BYT 0F14 / BSW 2294).
 * Returns 1 if PCI mode was restored (or already PCI), 0 on failure.
 */
int iosf_baytrail_scc_enable_pci_emmc(uint32_t* ctl_before_out, uint32_t* ctl_after_out);
int iosf_braswell_scc_enable_pci_emmc(uint32_t* ctl_before_out, uint32_t* ctl_after_out);

#endif
