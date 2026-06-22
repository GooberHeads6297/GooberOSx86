#ifndef TEXTCON_H
#define TEXTCON_H

/*
 * textcon -- 80x25 text-console abstraction with two interchangeable
 * backends:
 *
 *   - CON_BACKEND_VGA: mirrors the cell grid to the legacy 0xB8000 text
 *                      plane. Visible only on legacy BIOS (UEFI does not
 *                      expose the legacy text plane).
 *
 *   - CON_BACKEND_FB:  renders the cell grid into the top-left 640x400
 *                      region of a linear framebuffer (typically the
 *                      GRUB / firmware GOP framebuffer inherited via
 *                      multiboot2) using the shared 8x16 font.
 *
 * The 80x25 cell+attr grid is the single source of truth, so callers (the
 * shell, the boot log) write cells exactly the same way they always have;
 * the active backend mirrors each write to the panel.
 *
 * Designed so the x64 VGA-compatibility GRUB entries can produce a visible,
 * fully interactive text shell on UEFI hardware where 0xB8000 is dead.
 * On x86 BIOS the VGA backend is byte-equivalent to direct 0xB8000 writes.
 */

#include <stdint.h>

#define CON_COLS 80
#define CON_ROWS 25

typedef enum {
    CON_BACKEND_NONE = 0,
    CON_BACKEND_VGA  = 1,
    CON_BACKEND_FB   = 2
} con_backend_t;

/* Bind the VGA backend (0xB8000). Always succeeds (returns 1); the caller
 * should only pick this on a legacy-BIOS boot where 0xB8000 is mapped.
 * Idempotent. */
int  con_init_vga(void);

/* Bind the framebuffer backend. The console will render its 80x25 cell
 * grid into a 640x400 region anchored at the top-left of the framebuffer
 * using the shared 8x16 font. Returns 1 on success, 0 if the framebuffer
 * is too small or the bpp is unsupported (only 15/16/24/32 bpp direct-RGB
 * surfaces are accepted). */
int  con_init_fb(uintptr_t fb_addr, uint32_t fb_width, uint32_t fb_height,
                 uint32_t fb_pitch, uint8_t fb_bpp);

/* Current backend; CON_BACKEND_NONE before init or if init failed. */
con_backend_t con_backend(void);
int  con_ready(void);

/* Bounded read/write of a single cell. `attr` is a standard VGA attribute
 * byte (bg<<4 | fg). con_get_cell returns 0 for out-of-bounds. */
void     con_put_cell(int row, int col, char ch, uint8_t attr);
uint16_t con_get_cell(int row, int col);

/* Clear the entire console using `attr` as the background. */
void con_clear(uint8_t attr);

/* Scroll the cell grid up by `n` rows; the bottom `n` rows are filled with
 * spaces using `attr`. Re-renders only what changed on the active backend. */
void con_scroll_up(int n, uint8_t attr);

#endif
