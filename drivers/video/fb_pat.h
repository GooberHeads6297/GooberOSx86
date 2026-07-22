#ifndef FB_PAT_H
#define FB_PAT_H

#include <stdint.h>

/*
 * PAT-based Write-Combining for the firmware GOP framebuffer.
 *
 * Unlike fb_cache_enable_write_combining() (MTRR + cli/wbinvd), this path
 * only reprograms IA32_PAT and flips the PCD/PWT/PAT bits on the 2 MiB PDEs
 * that cover the LFB. Safe on Bay Trail / Braswell where MTRR WC hangs.
 *
 * Returns 1 on success (FB pages are WC), 0 on failure (leave uncached).
 */
int fb_pat_set_wc(uintptr_t fb_phys, uint32_t fb_bytes);

#endif
