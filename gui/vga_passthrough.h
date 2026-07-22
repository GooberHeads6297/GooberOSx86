#ifndef VGA_PASSTHROUGH_H
#define VGA_PASSTHROUGH_H

/*
 * Phase 4 (display polish, item 5): VGA-text application passthrough shim.
 *
 * GooberOS has a swarm of apps (editor, snake, pong, cubeDip, doom) that
 * originally wrote directly to the legacy 80x25 text plane at 0xB8000.
 * On x64 / UEFI that plane is dead -- the firmware does not expose the
 * legacy VGA text region -- so those apps are effectively invisible
 * without help. The passthrough shim is that help:
 *
 *   1. It owns a virtual 80x25 character-cell buffer (160 bytes/row * 25
 *      rows = 4000 bytes per "VGA window") plus an 8x16 glyph blit
 *      routine.
 *   2. vga_passthrough_arm() installs a redirect into drivers/video/vga.c
 *      so vga_text_putc() rerouts writes into this buffer.
 *   3. vga_passthrough_present_into(window, dest_w, dest_h) blits the
 *      80x25 cells through the 8x16 font onto a VESA window's client
 *      area (via the existing vesa_draw_char primitive).
 *   4. The desktop wraps the passthrough buffer in a normal VESA
 *      window class -- title bar, close button -- and dispatches
 *      keyboard events into the running app (editor / pong / etc.).
 *
 * The shim is single-window today (one passthrough buffer, one active
 * app at a time). That is sufficient for the desktop's launcher model
 * where the user opens one app, plays/edits, closes the window.
 *
 * NOT routed through the shim:
 *   - The shell window: 3e already renders shell output through the
 *     proper VESA shell_render() path. Routing it through here would
 *     be redundant and double-paint.
 *   - The kernel's print() boot log: it goes through kernel_set_
 *     print_sink (x64_print_sink under UEFI), NOT through the legacy
 *     0xB8000 plane.
 */

#include <stdint.h>

#define VGA_PT_COLS 80
#define VGA_PT_ROWS 25
#define VGA_PT_CELL_BYTES (VGA_PT_COLS * VGA_PT_ROWS)

/* Arm the shim. After this call, vga_text_putc(x,y,c,attr) writes go
 * into the shim's virtual cell grid instead of 0xB8000. Idempotent. */
void vga_passthrough_arm(void);

/* Disarm the shim and restore the direct 0xB8000 path. Useful when the
 * passthrough window is closed; on x64 nothing actually changes (0xB8000
 * is dead) but on x86 BIOS boots this lets a follow-up shell command
 * write to the visible text plane again. Idempotent. */
void vga_passthrough_disarm(void);

/* Returns 1 when the shim is armed (vga_text_putc is rerouting). */
int  vga_passthrough_active(void);

/* Clear the virtual cell grid to space + 0x07 (light grey on black). */
void vga_passthrough_clear(void);

/* Write one cell. Used both by the redirect hook (called from
 * drivers/video/vga.c::vga_text_putc) and directly by the host VESA
 * window when it wants to seed contents. */
void vga_passthrough_writechar(int x, int y, char c, unsigned char attr);

/* Read one cell back (used by the renderer; saves duplicating storage). */
void vga_passthrough_readcell(int x, int y, char* c, unsigned char* attr);

/* Composite the 80x25 cell grid into the active VESA-window's client
 * area through the embedded 8x16 font. The destination size determines
 * how many cells are blitted (truncated to fit). 32-bpp colours are
 * resolved from the 16-entry CGA-style palette below.
 *
 * (cx, cy)  -- VESA client-area top-left.
 * (cw, ch)  -- VESA client-area size in pixels.
 */
void vga_passthrough_present_into_window(int cx, int cy, int cw, int ch);

/* Copy textcon's 80x25 cell grid into the passthrough buffer (VGA-compat WM). */
void vga_passthrough_sync_from_textcon(void);

/* Pump the active passthrough app for one frame. The wrapper window's
 * render() callback installs a buffer-redirect, calls the per-app
 * render function (passed as `body`), then disarms the redirect so the
 * shell's writes are unaffected. The body sees the 80x25 grid as if it
 * were the real 0xB8000 plane. */
typedef void (*vga_passthrough_body_fn)(void* user);
void vga_passthrough_run_body(vga_passthrough_body_fn body, void* user);

#endif
