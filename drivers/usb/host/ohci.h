#ifndef USB_OHCI_H
#define USB_OHCI_H

#include <stdint.h>
#include <stddef.h>
#include "../../pci/pci.h"

/* ---- OHCI Endpoint Descriptor (16 bytes) ---- */
typedef struct __attribute__((packed, aligned(16))) ohci_ed {
    uint32_t hwINFO;
    uint32_t hwTailP;
    uint32_t hwHeadP;
    uint32_t hwNextED;
} ohci_ed_t;

/* ---- OHCI General Transfer Descriptor (16 bytes) ---- */
typedef struct __attribute__((packed, aligned(16))) ohci_td {
    uint32_t hwINFO;
    uint32_t hwCBP;
    uint32_t hwNextTD;
    uint32_t hwBE;
} ohci_td_t;

/* ---- OHCI HCCA (Host Controller Communication Area, 256 bytes) ---- */
typedef struct __attribute__((packed, aligned(256))) ohci_hcca {
    uint32_t hcca_interrupt_table[32];
    uint16_t hcca_frame_number;
    uint16_t hcca_pad1;
    uint32_t hcca_done_head;
    uint8_t  hcca_reserved[116];
    uint8_t  hcca_unknown[68];
} ohci_hcca_t;

/* ---- Register offsets ---- */
#define OHCI_HcRevision        0x00
#define OHCI_HcControl         0x04
#define OHCI_HcCommandStatus   0x08
#define OHCI_HcInterruptStatus 0x0C
#define OHCI_HcInterruptEnable 0x10
#define OHCI_HcInterruptDisable 0x14
#define OHCI_HcHCCA            0x18
#define OHCI_HcPeriodCurrentED 0x1C
#define OHCI_HcControlHeadED   0x20
#define OHCI_HcControlCurrentED 0x24
#define OHCI_HcBulkHeadED      0x28
#define OHCI_HcBulkCurrentED   0x2C
#define OHCI_HcDoneHead        0x30
#define OHCI_HcFmInterval      0x34
#define OHCI_HcFmRemaining     0x38
#define OHCI_HcFmNumber        0x3C
#define OHCI_HcPeriodicStart   0x40
#define OHCI_HcLSThreshold     0x44
#define OHCI_HcRhDescriptorA   0x48
#define OHCI_HcRhDescriptorB   0x4C
#define OHCI_HcRhStatus        0x50
#define OHCI_HcRhPortStatus1   0x54
#define OHCI_HcRhPortStatus2   0x58

/* HcControl bits */
#define OHCI_CTL_CBSR    (3 << 0)   /* Control/Bulk Service Ratio */
#define OHCI_CTL_PLE     (1 << 2)   /* Periodic List Enable */
#define OHCI_CTL_IE      (1 << 3)   /* Isochronous Enable */
#define OHCI_CTL_CLE     (1 << 4)   /* Control List Enable */
#define OHCI_CTL_BLE     (1 << 5)   /* Bulk List Enable */
#define OHCI_CTL_HCFS    (3 << 6)   /* Host Controller Functional State */
#define OHCI_CTL_HCFS_RESET   (0 << 6)
#define OHCI_CTL_HCFS_RESUME  (1 << 6)
#define OHCI_CTL_HCFS_OPER    (2 << 6)
#define OHCI_CTL_HCFS_SUSPEND (3 << 6)
#define OHCI_CTL_IR     (1 << 8)    /* Interrupt Routing */
#define OHCI_CTL_RWC    (1 << 9)    /* Remote Wakeup Connected */
#define OHCI_CTL_RWE    (1 << 10)   /* Remote Wakeup Enable */

/* HcCommandStatus bits */
#define OHCI_CMD_HCR    (1 << 0)    /* Host Controller Reset */
#define OHCI_CMD_CLF    (1 << 1)    /* Control List Filled */
#define OHCI_CMD_BLF    (1 << 2)    /* Bulk List Filled */
#define OHCI_CMD_OCR    (1 << 3)    /* Ownership Change Request */
#define OHCI_CMD_SOC    (3 << 16)   /* Scheduling Overrun Count */

/* HcInterruptStatus/HcInterruptEnable bits */
#define OHCI_INTR_SO    (1 << 0)    /* Scheduling Overrun */
#define OHCI_INTR_WDH   (1 << 1)    /* Writeback Done Head */
#define OHCI_INTR_SF    (1 << 2)    /* Start of Frame */
#define OHCI_INTR_RD    (1 << 3)    /* Resume Detect */
#define OHCI_INTR_UE    (1 << 4)    /* Unrecoverable Error */
#define OHCI_INTR_FNO   (1 << 5)    /* Frame Number Overflow */
#define OHCI_INTR_RHSC  (1 << 6)    /* Root Hub Status Change */
#define OHCI_INTR_OC    (1 << 30)   /* Ownership Change */
#define OHCI_INTR_MIE   (1 << 31)   /* Master Interrupt Enable */

/* HcRhStatus bits */
#define OHCI_RHS_SGP    (1 << 0)    /* Set Global Power */
#define OHCI_RHS_OCIC   (1 << 1)    /* Overcurrent Indicator Change */
#define OHCI_RHS_CGP    (1 << 16)   /* Clear Global Power */
#define OHCI_RHS_LPS    (1 << 17)   /* Local Power Status (read-only) */
#define OHCI_RHS_OCI    (1 << 18)   /* Overcurrent Indicator (read-only) */

/* HcRhPortStatus bits */
#define OHCI_PORT_CCS           (1 << 0)    /* Current Connect Status */
#define OHCI_PORT_PES           (1 << 1)    /* Port Enable Status */
#define OHCI_PORT_PSS           (1 << 2)    /* Port Suspend Status */
#define OHCI_PORT_POCI          (1 << 3)    /* Port Overcurrent Indicator */
#define OHCI_PORT_PRS           (1 << 4)    /* Port Reset Status */
#define OHCI_PORT_PPS           (1 << 8)    /* Port Power Status */
#define OHCI_PORT_LSDA          (1 << 9)    /* Low Speed Device Attached */
#define OHCI_PORT_CSC           (1 << 16)   /* Connect Status Change */
#define OHCI_PORT_PESC          (1 << 17)   /* Port Enable Status Change */
#define OHCI_PORT_PSSC          (1 << 18)   /* Port Suspend Status Change */
#define OHCI_PORT_OCIC          (1 << 19)   /* Overcurrent Indicator Change */
#define OHCI_PORT_PRSC          (1 << 20)   /* Port Reset Status Change */

/* ED hwINFO bits */
#define OHCI_ED_FA(addr)        ((addr) << 0)       /* Function Address */
#define OHCI_ED_EN(ep)          ((ep) << 7)         /* Endpoint Number */
#define OHCI_ED_D(d)            ((d) << 13)         /* Direction: 0=fromTD,1=OUT,2=IN,3=fromTD */
#define OHCI_ED_S(speed)        ((speed) << 15)     /* Speed: 0=full, 1=low */
#define OHCI_ED_K               (1 << 14)           /* Skip bit */
#define OHCI_ED_F               (1 << 17)           /* Format: 0=general, 1=isochronous */
#define OHCI_ED_H               (1 << 0)            /* Halt (in hwHeadP bit 0) */
#define OHCI_ED_C               (1 << 1)            /* Toggle carry (in hwHeadP bit 1) */

/* TD hwINFO bits */
#define OHCI_TD_CC_NOERROR      0x00
#define OHCI_TD_CC_CRC          0x01
#define OHCI_TD_CC_BITSTUFFING  0x02
#define OHCI_TD_CC_DATATOGGLEM  0x03
#define OHCI_TD_CC_STALL        0x04
#define OHCI_TD_CC_DEVICENOTRESP 0x05
#define OHCI_TD_CC_PIDCHECKFAIL 0x06
#define OHCI_TD_CC_UNEXPECTEDPID 0x07
#define OHCI_TD_CC_DATAOVERRUN  0x08
#define OHCI_TD_CC_DATAUNDERRUN 0x09
#define OHCI_TD_CC_MASK         0x0F
#define OHCI_TD_CC_SHIFT        28

#define OHCI_TD_EC_SHIFT        26
#define OHCI_TD_EC_MASK         (3 << OHCI_TD_EC_SHIFT)

#define OHCI_TD_T_SHIFT         24
#define OHCI_TD_T_MASK          (3 << OHCI_TD_T_SHIFT)
#define OHCI_TD_T_DATA0         (0 << OHCI_TD_T_SHIFT)
#define OHCI_TD_T_DATA1         (2 << OHCI_TD_T_SHIFT)
#define OHCI_TD_T_TOGGLE        (3 << OHCI_TD_T_SHIFT) /* Use current toggle */

#define OHCI_TD_DI_SHIFT        16
#define OHCI_TD_DI_MASK         (7 << OHCI_TD_DI_SHIFT)
#define OHCI_TD_DI_NODELAY      7           /* No interrupt */
#define OHCI_TD_DI_IMMEDIATE    0           /* Immediate interrupt */

#define OHCI_TD_DP_SHIFT        19
#define OHCI_TD_DP_MASK         (3 << OHCI_TD_DP_SHIFT)
#define OHCI_TD_DP_SETUP        (0 << OHCI_TD_DP_SHIFT)
#define OHCI_TD_DP_OUT          (1 << OHCI_TD_DP_SHIFT)
#define OHCI_TD_DP_IN           (2 << OHCI_TD_DP_SHIFT)
#define OHCI_TD_R               (1 << 18)   /* Buffer Rounding */

/* OHCI functions (mirror UHCI API) */
int  ohci_init(const usb_pci_controller_t* controller);
int  ohci_port_connected(int port);
int  ohci_port_low_speed(int port);
void ohci_port_reset(int port);
int  ohci_port_change_pending(int port);
void ohci_port_change_ack(int port);
int  ohci_control_transfer(uint8_t dev_addr, uint8_t endpoint,
                           uint8_t* setup_pkt, uint8_t* data, uint16_t data_len,
                           int direction_in);
int  ohci_schedule_interrupt(uint8_t dev_addr, uint8_t endpoint,
                             uint16_t max_packet, uint8_t interval_frames);
void ohci_remove_interrupt(void);
int  ohci_interrupt_active(void);
uint8_t* ohci_get_report(int* ready);
void ohci_ack_report(void);
void ohci_poll(void);
int  ohci_controller_healthy(void);

#endif
