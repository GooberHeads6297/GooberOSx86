#ifndef RM_THUNK_H
#define RM_THUNK_H

#include <stdint.h>

/*
 * Low-memory layout for BIOS real-mode calls (x86 BIOS only).
 * These addresses sit below the typical multiboot info block and are
 * documented so future INT 13h work can share the same gateway.
 */
#define BIOS_RM_STATE_PHYS    0x00007000U
#define BIOS_VBE_INFO_PHYS    0x00008000U
#define BIOS_VBE_PARAMS_PHYS  0x00008100U
#define BIOS_VBE_MODE_PHYS    0x00008200U
#define BIOS_RM_CODE_PHYS     0x00009000U

#define BIOS_RM_FN_VBE_SET_MODE     1U
#define BIOS_RM_FN_VBE_QUERY_CURRENT 2U

/* Saved PM state size (must match rm_thunk.s). */
#define BIOS_RM_STATE_SIZE    96U

/*
 * bios_rm_init() copies the 16-bit worker blob into BIOS_RM_CODE_PHYS.
 * Returns 1 on success, 0 if the blob is too large for the reserved region.
 */
int bios_rm_init(void);

/*
 * bios_rm_call(fn, params) drops to real mode, runs the selected worker,
 * and returns to protected mode. params is worker-specific; VBE uses the
 * fixed BIOS_VBE_PARAMS_PHYS block. Returns 0 on success, negative on
 * gateway failure, positive BIOS/VBE error code otherwise.
 */
int bios_rm_call(uint32_t fn, void* params);

#endif
