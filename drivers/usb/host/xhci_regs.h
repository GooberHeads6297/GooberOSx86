#ifndef XHCI_REGS_H
#define XHCI_REGS_H

#include <stdint.h>

/* xHCI 1.1 register and PORTSC field definitions (shared by port/ctx/HCD). */

#define XHCI_CAP_CAPLENGTH   0x00
#define XHCI_CAP_HCSPARAMS1  0x04
#define XHCI_CAP_HCSPARAMS2  0x08
#define XHCI_CAP_HCCPARAMS1  0x10
#define XHCI_CAP_DBOFF       0x14
#define XHCI_CAP_RTSOFF      0x18

#define XHCI_USBCMD          0x00
#define XHCI_USBSTS          0x04
#define XHCI_PAGESIZE        0x08
#define XHCI_DNCTRL          0x14
#define XHCI_CRCR            0x18
#define XHCI_DCBAAP          0x30
#define XHCI_CONFIG          0x38

#define XHCI_PORTSC_BASE     0x400

#define XHCI_CMD_RUN         (1U << 0)
#define XHCI_CMD_HCRST       (1U << 1)
#define XHCI_CMD_INTE        (1U << 2)
#define XHCI_CMD_HSEE        (1U << 3)

#define XHCI_STS_HCH         (1U << 0)
#define XHCI_STS_HSE         (1U << 2)
#define XHCI_STS_EINT        (1U << 3)
#define XHCI_STS_PCD         (1U << 4)
#define XHCI_STS_CNR         (1U << 11)

/* PORTSC bitfields (xHCI 1.1 §5.4.8) */
#define XHCI_PORT_CCS        (1U << 0)   /* RO */
#define XHCI_PORT_PED        (1U << 1)   /* RW1CS — write 1 disables! */
#define XHCI_PORT_OCA        (1U << 3)   /* RO */
#define XHCI_PORT_PR         (1U << 4)   /* RW1S */
#define XHCI_PORT_PLS_SHIFT  5
#define XHCI_PORT_PLS_MASK   (0xFU << 5)
#define XHCI_PORT_PP         (1U << 9)   /* RWS */
#define XHCI_PORT_SPEED_SHIFT 10
#define XHCI_PORT_SPEED_MASK (0xFU << 10)
#define XHCI_PORT_PIC_SHIFT  14
#define XHCI_PORT_PIC_MASK   (0x3U << 14)
#define XHCI_PORT_LWS        (1U << 16)
#define XHCI_PORT_CSC        (1U << 17)  /* RW1C */
#define XHCI_PORT_PEC        (1U << 18)  /* RW1C */
#define XHCI_PORT_WRC        (1U << 19)  /* RW1C */
#define XHCI_PORT_OCC        (1U << 20)  /* RW1C */
#define XHCI_PORT_PRC        (1U << 21)  /* RW1C */
#define XHCI_PORT_PLC        (1U << 22)  /* RW1C */
#define XHCI_PORT_CEC        (1U << 23)  /* RW1C */
#define XHCI_PORT_CAS        (1U << 24)  /* RO */
#define XHCI_PORT_WCE        (1U << 25)  /* RWS */
#define XHCI_PORT_WDE        (1U << 26)  /* RWS */
#define XHCI_PORT_WOE        (1U << 27)  /* RWS */
#define XHCI_PORT_DR         (1U << 30)  /* RO */
#define XHCI_PORT_WPR        (1U << 31)  /* RW1S */

/*
 * Read-write-settable bits that are safe to echo back after neutralizing
 * sticky / one-shot fields. PED/PR/PLS/change bits are intentionally absent.
 */
#define XHCI_PORT_RWS (XHCI_PORT_PP | XHCI_PORT_PIC_MASK | \
                       XHCI_PORT_WCE | XHCI_PORT_WDE | XHCI_PORT_WOE)

/* All change bits that clear when written as 1. */
#define XHCI_PORT_CHANGE_BITS (XHCI_PORT_CSC | XHCI_PORT_PEC | XHCI_PORT_WRC | \
                               XHCI_PORT_OCC | XHCI_PORT_PRC | XHCI_PORT_PLC | \
                               XHCI_PORT_CEC)

/* PORTSC Protocol Speed Identifier codes (port register encoding). */
#define XHCI_PORT_PSI_FS     1U
#define XHCI_PORT_PSI_LS     2U
#define XHCI_PORT_PSI_HS     3U
#define XHCI_PORT_PSI_SS     4U

/* Slot Context Speed values (xHCI 1.1 Table 57) — LS/FS swapped vs PORTSC. */
#define XHCI_SLOT_SPEED_FS   1U  /* note: Slot FS = 1, PORTSC FS = 1? Wait */
/*
 * PORTSC Port Speed field (default mapping without SPD proto cap override):
 *   1 = Full Speed, 2 = Low Speed, 3 = High Speed, 4 = SuperSpeed
 * Slot Context Speed (SPD):
 *   1 = Full Speed, 2 = Low Speed, 3 = High Speed, 4 = SuperSpeed Gen1 x1
 *
 * Wait — if they're the same, why did the audit say they swap?
 *
 * Re-check xHCI 1.1:
 * Table 157 / Port Speed field for USB2 ports WITHOUT protocol capability
 * parsing can differ. Looking at Linux xhci_port_speed and SeaBIOS:
 *
 * SeaBIOS usb-xhci.c:
 *   case 1: return USB_FULL_SPEED;
 *   case 2: return USB_LOW_SPEED;
 *
 * And slot context:
 * SeaBIOS uses usb_to_xhci_speed which maps:
 *   USB_FULL_SPEED -> 1
 *   USB_LOW_SPEED  -> 2
 *
 * So PORTSC PSI and Slot SPD use the SAME numbering for default USB2!
 *
 * But Linux comments and some sources say:
 * PORTSC: 1=FS, 2=LS
 * Slot: 1=FS, 2=LS  
 * They're the same in xHCI 1.0/1.1 default!
 *
 * Re-read the audit claim from conversation summary:
 * "FS/LS Slot Context speed swap: PORTSC speeds are 1=FS, 2=LS; Slot Context
 * needs 1=LS, 2=FS. Current activate_slot() copies PORTSC speed verbatim"
 *
 * Checking Intel xHCI 1.1 spec more carefully...
 * Section 7.2.2.1.1 Default USB Speed ID:
 * For USB2: Port Speed Value 1 = Full-speed, 2 = Low-speed, 3 = High-speed
 *
 * Section 4.5.2 Slot Context:
 * Speed (SPD) field: 1=Full, 2=Low, 3=High, 4=SuperSpeed
 *
 * So they MATCH. The audit claim about swap may be WRONG, OR refers to
 * EHCI / Hub descriptor encoding where Low=1 Full=2?
 *
 * USB 2.0 hub port status: bits encode differently.
 * Don't swap unless evidence. Keep 1:1 mapping for PORTSC default IDs.
 *
 * However some controllers with Supported Protocol Capability remap PSI.
 * We still provide a helper that maps PORTSC PSI -> Slot SPD using
 * parsed protocol caps when available; identity mapping otherwise.
 */

#define XHCI_PORTPMSC_HLE    (1U << 16) /* USB2 PORTPMSC.HLE */

#endif
