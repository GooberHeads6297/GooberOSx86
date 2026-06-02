#ifndef NATIVE_FB_H
#define NATIVE_FB_H

#include <stdint.h>
#include "display.h"

/*
 * Generic linear-framebuffer drivers for the display framework.
 *
 * Two drivers live here:
 *
 *   "vesa"  - the inherited / simple-framebuffer driver. It adopts whatever
 *             linear framebuffer the bootloader (GRUB VBE) already handed us,
 *             formalizing the legacy boot path through the framework. This is
 *             registered first so a GRUB-provided LFB always wins.
 *
 *   "bochs" - the Bochs/QEMU "Bochs Dispi" (BGA) / stdvga VBE-extension
 *             driver. When GRUB did not hand us a usable framebuffer, this can
 *             program a chosen resolution directly via the dispi I/O ports
 *             (0x01CE/0x01CF) and adopt the PCI linear-framebuffer aperture.
 *             Widely supported on QEMU, VirtualBox and Bochs.
 */

/*
 * Provide the framework's simple-framebuffer driver with the framebuffer the
 * bootloader reported. type/bpp follow the Multiboot convention (type 1 =
 * direct RGB). Call before native_fb_register_drivers(); if the bootloader
 * gave nothing, pass addr = 0 and the "vesa" driver will simply decline.
 *
 * `addr` is `uintptr_t` (Phase 3b pointer-width audit) so the descriptor
 * survives a 64-bit physical address end-to-end. On the 32-bit kernel
 * `uintptr_t` is `uint32_t`, so the call sites stay byte-equivalent.
 */
void native_fb_set_inherited(uintptr_t addr, uint32_t width, uint32_t height,
                             uint32_t pitch, uint8_t bpp, uint8_t type);

/*
 * Read back the framebuffer the bootloader reported (the values last passed to
 * native_fb_set_inherited()). Any out pointer may be NULL. Returns non-zero
 * only when a usable direct-RGB linear framebuffer was inherited (addr != 0,
 * type == 1, non-zero dimensions); the Intel plane-repoint driver uses this to
 * discover the buffer it should scan out. Caller still validates depth/pitch.
 */
int native_fb_get_inherited(uintptr_t* addr, uint32_t* width, uint32_t* height,
                            uint32_t* pitch, uint8_t* bpp, uint8_t* type);

/* Register the "vesa" (inherited LFB) and "bochs" (dispi) drivers, in that
 * priority order, with the display framework. */
void native_fb_register_drivers(void);

#endif
