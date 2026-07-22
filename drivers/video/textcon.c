/*
 * textcon -- 80x25 text-console backend layer.
 *
 * The console owns the 80x25 cell+attr grid. con_put_cell mirrors writes to
 * either the legacy 0xB8000 plane (VGA backend) or a linear framebuffer
 * rendered via the shared 8x16 font (FB backend). Callers (the shell, the
 * boot-log mirror) write cells exactly the way they always have; the
 * backend choice is transparent.
 *
 * Why not reuse drivers/video/vesa.c? The VGA-compatibility boot path
 * deliberately does NOT bring up the VESA pixel surface (the user asked
 * for "text-only"), so vesa_init() is never called. textcon keeps its
 * own tiny pixel writer + font copy reference so it stays usable in
 * exactly that scenario, with zero coupling to the graphical display
 * stack.
 */

#include "textcon.h"
#include "font.h"
#include <stddef.h>

/* ---------------- Cell grid + cursor state ------------------------------- */

static uint16_t       g_cells[CON_ROWS * CON_COLS];
static con_backend_t  g_backend = CON_BACKEND_NONE;

/* VGA backend state -- the legacy 0xB8000 plane. */
static volatile uint16_t* const VGA_PLANE = (volatile uint16_t*)0xB8000;

/* FB backend state. */
static volatile uint8_t* g_fb_base   = NULL;
static uint32_t          g_fb_width  = 0;
static uint32_t          g_fb_height = 0;
static uint32_t          g_fb_pitch  = 0;
static uint8_t           g_fb_bpp    = 0;
static uint32_t          g_fb_bytes_per_px = 0;
/* Pace glyph writes on uncached GOP scanout (Braswell inherit path). */
static int               g_fb_paced  = 0;

/* ---------------- VGA 16-color palette ----------------------------------- */
/*
 * Standard IBM CGA/VGA palette, packed as XRGB8888. Index = low nibble of
 * the VGA attribute byte (fg) or high nibble (bg). These values match the
 * colors most users expect when they think "DOS console".
 */
static const uint32_t k_vga_palette[16] = {
    0x000000u, /* 0 BLACK         */
    0x0000AAu, /* 1 BLUE          */
    0x00AA00u, /* 2 GREEN         */
    0x00AAAAu, /* 3 CYAN          */
    0xAA0000u, /* 4 RED           */
    0xAA00AAu, /* 5 MAGENTA       */
    0xAA5500u, /* 6 BROWN         */
    0xAAAAAAu, /* 7 LIGHT_GREY    */
    0x555555u, /* 8 DARK_GREY     */
    0x5555FFu, /* 9 LIGHT_BLUE    */
    0x55FF55u, /* A LIGHT_GREEN   */
    0x55FFFFu, /* B LIGHT_CYAN    */
    0xFF5555u, /* C LIGHT_RED     */
    0xFF55FFu, /* D LIGHT_MAGENTA */
    0xFFFF55u, /* E LIGHT_BROWN   */
    0xFFFFFFu, /* F WHITE         */
};

/* ---------------- Helpers ----------------------------------------------- */

static inline uint16_t pack_cell(char ch, uint8_t attr) {
    return ((uint16_t)attr << 8) | (uint8_t)ch;
}

static inline int in_bounds(int row, int col) {
    return (row >= 0 && col >= 0 && row < CON_ROWS && col < CON_COLS);
}

/* Convert XRGB8888 to the active framebuffer's pixel layout. We make the
 * common assumption that GOP-style 32/24 bpp surfaces use byte order B,G,R
 * (most UEFI implementations) and 16 bpp surfaces are RGB565. The earlier
 * GRUB->kernel handoff prints the framebuffer color_info to serial so this
 * heuristic can be revisited per-platform if needed. */
static inline uint32_t fb_pack_xrgb(uint32_t xrgb) {
    uint32_t r = (xrgb >> 16) & 0xFFu;
    uint32_t g = (xrgb >>  8) & 0xFFu;
    uint32_t b =  xrgb        & 0xFFu;

    if (g_fb_bpp == 32 || g_fb_bpp == 24) {
        /* BGRX / BGR */
        return (r << 16) | (g << 8) | b;
    }
    if (g_fb_bpp == 16) {
        /* RGB565 */
        return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }
    /* 15: RGB555 */
    return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
}

static inline void fb_write_pixel(volatile uint8_t* p, uint32_t pix) {
    /* Unrolled by bpp; the bytes_per_px is stable for the FB session. */
    p[0] = (uint8_t)( pix        & 0xFFu);
    if (g_fb_bytes_per_px > 1) p[1] = (uint8_t)((pix >>  8) & 0xFFu);
    if (g_fb_bytes_per_px > 2) p[2] = (uint8_t)((pix >> 16) & 0xFFu);
    if (g_fb_bytes_per_px > 3) p[3] = (uint8_t)((pix >> 24) & 0xFFu);
}

/* Render a single 8x16 glyph at pixel position (px, py) into the FB. The
 * caller has already validated the FB is initialized and (px, py) is in
 * range. */
static void fb_draw_glyph(uint32_t px, uint32_t py, char ch, uint8_t attr) {
    uint32_t fg = fb_pack_xrgb(k_vga_palette[attr & 0x0Fu]);
    uint32_t bg = fb_pack_xrgb(k_vga_palette[(attr >> 4) & 0x0Fu]);

    unsigned char uc = (unsigned char)ch;
    int printable = (uc >= 0x20 && uc <= 0x7E);

    volatile uint8_t* row = g_fb_base
                          + (uintptr_t)py * (uintptr_t)g_fb_pitch
                          + (uintptr_t)px * (uintptr_t)g_fb_bytes_per_px;

    for (int gy = 0; gy < FONT_HEIGHT; gy++) {
        uint8_t bits = printable ? font8x16[uc][gy] : 0u;
        volatile uint8_t* p = row;
        for (int gx = 0; gx < FONT_WIDTH; gx++) {
            uint32_t pix = (bits & (uint8_t)(0x80u >> gx)) ? fg : bg;
            fb_write_pixel(p, pix);
            p += g_fb_bytes_per_px;
        }
        row += g_fb_pitch;
    }
}

/* Re-render a single cell on the active backend. */
static void render_cell(int row, int col) {
    uint16_t cell = g_cells[row * CON_COLS + col];
    if (g_backend == CON_BACKEND_VGA) {
        VGA_PLANE[row * CON_COLS + col] = cell;
        return;
    }
    if (g_backend == CON_BACKEND_SOFT) {
        /* Presented by the VESA VGA-compat console window. */
        (void)cell;
        return;
    }
    if (g_backend == CON_BACKEND_FB) {
        char    ch   = (char)(cell & 0xFFu);
        uint8_t attr = (uint8_t)(cell >> 8);
        fb_draw_glyph((uint32_t)col * (uint32_t)FONT_WIDTH,
                      (uint32_t)row * (uint32_t)FONT_HEIGHT,
                      ch, attr);
    }
}

static void render_all(void) {
    int i = 0;
    for (int r = 0; r < CON_ROWS; r++) {
        for (int c = 0; c < CON_COLS; c++) {
            render_cell(r, c);
            if (g_fb_paced && (++i & 0x0F) == 0)
                __asm__ volatile("pause");
        }
    }
}

/* ---------------- Public API -------------------------------------------- */

int con_init_vga(void) {
    /* Snapshot whatever the existing 0xB8000 plane already contains into
     * the cell grid so subsequent con_get_cell reads see the live screen.
     * Important: on x86 BIOS the boot log has already printed to 0xB8000
     * by the time the shell binds the console, and clearing it here would
     * be a visible behaviour change. (Callers that explicitly want a blank
     * console can call con_clear() after init.) */
    g_backend = CON_BACKEND_VGA;
    for (int i = 0; i < CON_ROWS * CON_COLS; i++) g_cells[i] = VGA_PLANE[i];
    return 1;
}

static int con_init_fb_common(uintptr_t fb_addr, uint32_t fb_width,
                              uint32_t fb_height, uint32_t fb_pitch,
                              uint8_t fb_bpp, int lite) {
    if (!fb_addr || fb_pitch == 0) return 0;
    if (fb_bpp != 32 && fb_bpp != 24 && fb_bpp != 16 && fb_bpp != 15) return 0;

    /* Need at least 640x400 to fit the 80x25 cell grid at 8x16. */
    if (fb_width < (uint32_t)(CON_COLS * FONT_WIDTH))  return 0;
    if (fb_height < (uint32_t)(CON_ROWS * FONT_HEIGHT)) return 0;

    g_fb_base   = (volatile uint8_t*)fb_addr;
    g_fb_width  = fb_width;
    g_fb_height = fb_height;
    g_fb_pitch  = fb_pitch;
    g_fb_bpp    = fb_bpp;
    g_fb_bytes_per_px = (fb_bpp + 7u) / 8u;
    g_fb_paced  = lite ? 1 : 0;

    /* Blank the grid (light-grey-on-black is the standard console look). */
    uint16_t blank = pack_cell(' ', 0x07u);
    for (int i = 0; i < CON_ROWS * CON_COLS; i++) g_cells[i] = blank;

    g_backend = CON_BACKEND_FB;
    if (lite)
        return 1;

    /* Paint the full console area: clear the 640x400 region to the cell
     * background (black) and render every cell once so the panel reflects
     * the cleared grid. */
    uint32_t bg = fb_pack_xrgb(k_vga_palette[0]);
    for (uint32_t y = 0; y < (uint32_t)(CON_ROWS * FONT_HEIGHT); y++) {
        volatile uint8_t* p = g_fb_base + (uintptr_t)y * (uintptr_t)g_fb_pitch;
        for (uint32_t x = 0; x < (uint32_t)(CON_COLS * FONT_WIDTH); x++) {
            fb_write_pixel(p, bg);
            p += g_fb_bytes_per_px;
        }
    }
    render_all();
    return 1;
}

int con_init_fb(uintptr_t fb_addr, uint32_t fb_width, uint32_t fb_height,
                uint32_t fb_pitch, uint8_t fb_bpp) {
    return con_init_fb_common(fb_addr, fb_width, fb_height, fb_pitch, fb_bpp, 0);
}

int con_init_fb_lite(uintptr_t fb_addr, uint32_t fb_width, uint32_t fb_height,
                     uint32_t fb_pitch, uint8_t fb_bpp) {
    return con_init_fb_common(fb_addr, fb_width, fb_height, fb_pitch, fb_bpp, 1);
}

int con_init_soft(void) {
    uint16_t blank = pack_cell(' ', 0x07u);
    for (int i = 0; i < CON_ROWS * CON_COLS; i++) g_cells[i] = blank;
    g_fb_base = NULL;
    g_backend = CON_BACKEND_SOFT;
    return 1;
}

con_backend_t con_backend(void) { return g_backend; }
int           con_ready(void)   { return g_backend != CON_BACKEND_NONE; }

void con_put_cell(int row, int col, char ch, uint8_t attr) {
    if (!in_bounds(row, col)) return;
    g_cells[row * CON_COLS + col] = pack_cell(ch, attr);
    if (g_backend != CON_BACKEND_NONE) render_cell(row, col);
}

uint16_t con_get_cell(int row, int col) {
    if (!in_bounds(row, col)) return 0;
    return g_cells[row * CON_COLS + col];
}

void con_clear(uint8_t attr) {
    uint16_t blank = pack_cell(' ', attr);
    for (int i = 0; i < CON_ROWS * CON_COLS; i++) g_cells[i] = blank;
    if (g_backend == CON_BACKEND_VGA) {
        for (int i = 0; i < CON_ROWS * CON_COLS; i++) VGA_PLANE[i] = blank;
    } else if (g_backend == CON_BACKEND_FB) {
        render_all();
    }
}

void con_scroll_up(int n, uint8_t attr) {
    if (n <= 0) return;
    if (n >= CON_ROWS) { con_clear(attr); return; }

    /* Shift the grid up by `n` rows. */
    for (int r = 0; r < CON_ROWS - n; r++) {
        for (int c = 0; c < CON_COLS; c++) {
            g_cells[r * CON_COLS + c] = g_cells[(r + n) * CON_COLS + c];
        }
    }
    /* Fill the freshly-exposed bottom rows. */
    uint16_t blank = pack_cell(' ', attr);
    for (int r = CON_ROWS - n; r < CON_ROWS; r++) {
        for (int c = 0; c < CON_COLS; c++) {
            g_cells[r * CON_COLS + c] = blank;
        }
    }

    /* Mirror to the active backend. For VGA we can shift VRAM directly
     * (it is fast and avoids re-rendering the unchanged scrolled rows).
     * For the FB backend we re-render every row since the pixel area is
     * one contiguous 640x400 block and partial-blit gymnastics aren't
     * worth the complexity for a text console. */
    if (g_backend == CON_BACKEND_VGA) {
        for (int r = 0; r < CON_ROWS - n; r++) {
            for (int c = 0; c < CON_COLS; c++) {
                VGA_PLANE[r * CON_COLS + c] =
                    VGA_PLANE[(r + n) * CON_COLS + c];
            }
        }
        for (int r = CON_ROWS - n; r < CON_ROWS; r++) {
            for (int c = 0; c < CON_COLS; c++) {
                VGA_PLANE[r * CON_COLS + c] = blank;
            }
        }
    } else if (g_backend == CON_BACKEND_FB) {
        render_all();
    }
}
