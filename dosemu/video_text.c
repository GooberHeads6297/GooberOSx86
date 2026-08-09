#include "dosemu_priv.h"
#include "../lib/string.h"
#include "../lib/memory.h"

void dos_video_sync_to_b800(dos_session_t* s) {
    int r, c;
    if (!s || !s->mem) return;
    for (r = 0; r < DOS_TEXT_ROWS; r++) {
        for (c = 0; c < DOS_TEXT_COLS; c++) {
            uint32_t a = DOS_B800_LINEAR + (uint32_t)(r * DOS_TEXT_COLS + c) * 2u;
            dos_write8(s, a, (uint8_t)(s->text[r][c] ? s->text[r][c] : ' '));
            dos_write8(s, a + 1, s->attr[r][c] ? s->attr[r][c] : 0x07);
        }
    }
}

void dos_video_sync_from_b800(dos_session_t* s) {
    int r, c;
    if (!s || !s->mem) return;
    for (r = 0; r < DOS_TEXT_ROWS; r++) {
        for (c = 0; c < DOS_TEXT_COLS; c++) {
            uint32_t a = DOS_B800_LINEAR + (uint32_t)(r * DOS_TEXT_COLS + c) * 2u;
            s->text[r][c] = (char)dos_read8(s, a);
            s->attr[r][c] = dos_read8(s, a + 1);
        }
    }
}

void dos_video_init(dos_session_t* s) {
    int r, c, i;
    if (!s) return;
    for (r = 0; r < DOS_TEXT_ROWS; r++) {
        for (c = 0; c < DOS_TEXT_COLS; c++) {
            s->text[r][c] = ' ';
            s->attr[r][c] = 0x07;
        }
    }
    s->cursor_r = 0;
    s->cursor_c = 0;
    s->video_mode = 3;
    s->active_page = 0;
    s->cur_attr = 0x07;
    if (s->vga13) {
        kfree(s->vga13);
        s->vga13 = NULL;
    }
    /* default VGA-ish palette */
    for (i = 0; i < 256; i++) {
        s->vga_pal[i][0] = (uint8_t)((i >> 2) & 0x3F);
        s->vga_pal[i][1] = (uint8_t)((i >> 2) & 0x3F);
        s->vga_pal[i][2] = (uint8_t)((i >> 2) & 0x3F);
    }
    dos_video_sync_to_b800(s);
}

void dos_video_set_mode(dos_session_t* s, uint8_t mode) {
    if (!s) return;
    s->video_mode = (int)mode;
    if (mode == 0x13) {
        uint32_t i;
        if (!s->vga13) {
            s->vga13 = (uint8_t*)kmalloc(320u * 200u);
        }
        if (s->vga13) memset(s->vga13, 0, 320u * 200u);
        for (i = 0; i < 320u * 200u; i++)
            dos_write8(s, DOS_A000_LINEAR + i, 0);
        return;
    }
    if (s->vga13) {
        kfree(s->vga13);
        s->vga13 = NULL;
    }
    dos_video_init(s);
    s->video_mode = (int)mode;
}

static void dos_scroll_up(dos_session_t* s) {
    int r, c;
    uint8_t a = s->cur_attr ? s->cur_attr : 0x07;
    for (r = 0; r < DOS_TEXT_ROWS - 1; r++) {
        for (c = 0; c < DOS_TEXT_COLS; c++) {
            s->text[r][c] = s->text[r + 1][c];
            s->attr[r][c] = s->attr[r + 1][c];
        }
    }
    for (c = 0; c < DOS_TEXT_COLS; c++) {
        s->text[DOS_TEXT_ROWS - 1][c] = ' ';
        s->attr[DOS_TEXT_ROWS - 1][c] = a;
    }
}

void dos_video_putc(dos_session_t* s, char ch) {
    uint8_t a;
    if (!s) return;
    if (s->video_mode == 0x13) return; /* teletype ignored in graphics for now */
    a = s->cur_attr ? s->cur_attr : 0x07;
    if (ch == '\r') { s->cursor_c = 0; return; }
    if (ch == '\n') {
        s->cursor_r++;
        if (s->cursor_r >= DOS_TEXT_ROWS) {
            s->cursor_r = DOS_TEXT_ROWS - 1;
            dos_scroll_up(s);
        }
        dos_video_sync_to_b800(s);
        return;
    }
    if (ch == '\b') {
        if (s->cursor_c > 0) s->cursor_c--;
        else if (s->cursor_r > 0) {
            s->cursor_r--;
            s->cursor_c = DOS_TEXT_COLS - 1;
        }
        s->text[s->cursor_r][s->cursor_c] = ' ';
        s->attr[s->cursor_r][s->cursor_c] = a;
        dos_video_sync_to_b800(s);
        return;
    }
    if (ch == '\t') {
        int n = 8 - (s->cursor_c % 8);
        while (n-- > 0) dos_video_putc(s, ' ');
        return;
    }
    s->text[s->cursor_r][s->cursor_c] = ch ? ch : ' ';
    s->attr[s->cursor_r][s->cursor_c] = a;
    s->cursor_c++;
    if (s->cursor_c >= DOS_TEXT_COLS) {
        s->cursor_c = 0;
        s->cursor_r++;
        if (s->cursor_r >= DOS_TEXT_ROWS) {
            s->cursor_r = DOS_TEXT_ROWS - 1;
            dos_scroll_up(s);
        }
    }
    dos_video_sync_to_b800(s);
}

void dos_video_puts(dos_session_t* s, const char* str) {
    if (!s || !str) return;
    while (*str) dos_video_putc(s, *str++);
}

static uint32_t attr_fg(uint8_t attr) {
    static const uint32_t pal[16] = {
        0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
        0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
        0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
        0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
    };
    return pal[attr & 0x0F];
}

static uint32_t attr_bg(uint8_t attr) {
    static const uint32_t pal[8] = {
        0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
        0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA
    };
    return pal[(attr >> 4) & 0x07];
}

static uint32_t pal_rgb(dos_session_t* s, uint8_t idx) {
    uint32_t r = ((uint32_t)s->vga_pal[idx][0] * 255u) / 63u;
    uint32_t g = ((uint32_t)s->vga_pal[idx][1] * 255u) / 63u;
    uint32_t b = ((uint32_t)s->vga_pal[idx][2] * 255u) / 63u;
    return (r << 16) | (g << 8) | b;
}

void dos_video_render(dos_session_t* s, int cx, int cy, int cw, int ch) {
    int r, c;
    if (!s) return;

    if (s->video_mode == 0x13) {
        int bx, by, bw, bh;
        int cols = 160, rows = 100;
        if (cw < cols) cols = cw > 0 ? cw : 1;
        if (ch < rows) rows = ch > 0 ? ch : 1;
        bw = cw / cols;
        bh = ch / rows;
        if (bw < 1) bw = 1;
        if (bh < 1) bh = 1;
        vdesk_draw_rect(cx, cy, cw, ch, 0x000000);
        for (by = 0; by < rows; by++) {
            int sy = (by * 200) / rows;
            for (bx = 0; bx < cols; bx++) {
                int sx = (bx * 320) / cols;
                uint8_t pix = dos_read8(s, DOS_A000_LINEAR + (uint32_t)sy * 320u + (uint32_t)sx);
                vdesk_draw_rect(cx + bx * bw, cy + by * bh, bw, bh, pal_rgb(s, pix));
            }
        }
        return;
    }

    dos_video_sync_from_b800(s);
    {
        int rows = ch / 16;
        int cols = cw / 8;
        if (rows > DOS_TEXT_ROWS) rows = DOS_TEXT_ROWS;
        if (cols > DOS_TEXT_COLS) cols = DOS_TEXT_COLS;
        vdesk_draw_rect(cx, cy, cw, ch, 0x000000);
        for (r = 0; r < rows; r++) {
            for (c = 0; c < cols; c++) {
                char chs[2];
                uint8_t a = s->attr[r][c] ? s->attr[r][c] : 0x07;
                chs[0] = s->text[r][c] ? s->text[r][c] : ' ';
                chs[1] = '\0';
                vdesk_draw_text(cx + c * 8, cy + r * 16, chs, attr_fg(a), attr_bg(a));
            }
        }
        if (s->cursor_r < rows && s->cursor_c < cols) {
            vdesk_draw_rect(cx + s->cursor_c * 8, cy + s->cursor_r * 16 + 14, 8, 2, 0x00FF00);
        }
    }
}
