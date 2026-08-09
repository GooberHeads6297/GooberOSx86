#include "dosemu_priv.h"
#include "../drivers/keyboard/keyboard.h"

static uint8_t ascii_to_scan(uint8_t ch) {
    if (ch >= 'a' && ch <= 'z') ch = (uint8_t)(ch - 'a' + 'A');
    if (ch >= 'A' && ch <= 'Z') return (uint8_t)(0x1E + ((ch - 'A') % 26)); /* rough */
    switch (ch) {
    case 0x1B: return 0x01;
    case '1': return 0x02; case '2': return 0x03; case '3': return 0x04;
    case '4': return 0x05; case '5': return 0x06; case '6': return 0x07;
    case '7': return 0x08; case '8': return 0x09; case '9': return 0x0A;
    case '0': return 0x0B;
    case '\r': case '\n': return 0x1C;
    case '\b': return 0x0E;
    case '\t': return 0x0F;
    case ' ': return 0x39;
    default: return 0x00;
    }
}

static uint16_t host_key_to_word(char key) {
    unsigned char k = (unsigned char)key;
    switch (k) {
    case KEY_UP:    return 0x4800;
    case KEY_DOWN:  return 0x5000;
    case KEY_LEFT:  return 0x4B00;
    case KEY_RIGHT: return 0x4D00;
    case KEY_F1:    return 0x3B00;
    case KEY_F2:    return 0x3C00;
    case KEY_F3:    return 0x3D00;
    case KEY_F4:    return 0x3E00;
    case KEY_F5:    return 0x3F00;
    case KEY_F6:    return 0x4000;
    case KEY_F7:    return 0x4100;
    case KEY_F8:    return 0x4200;
    case KEY_F9:    return 0x4300;
    case KEY_F10:   return 0x4400;
    default:
        return (uint16_t)((ascii_to_scan(k) << 8) | k);
    }
}

void dos_key_push_word(dos_session_t* s, uint16_t word) {
    int next;
    if (!s || !word) return;
    next = (s->keyq_w + 1) % DOS_KEYQ_MAX;
    if (next == s->keyq_r) return;
    s->keyq[s->keyq_w] = word;
    s->keyq_w = next;
}

void dos_key_push(dos_session_t* s, char key) {
    if (!s || !key) return;
    dos_key_push_word(s, host_key_to_word(key));
}

int dos_key_pop_word(dos_session_t* s, uint16_t* out) {
    if (!s || s->keyq_r == s->keyq_w) return 0;
    if (out) *out = s->keyq[s->keyq_r];
    s->keyq_r = (s->keyq_r + 1) % DOS_KEYQ_MAX;
    return 1;
}

int dos_key_peek_word(dos_session_t* s, uint16_t* out) {
    if (!s || s->keyq_r == s->keyq_w) return 0;
    if (out) *out = s->keyq[s->keyq_r];
    return 1;
}

int dos_key_pop(dos_session_t* s, char* out) {
    uint16_t w;
    if (!dos_key_pop_word(s, &w)) return 0;
    if (out) *out = (char)(w & 0xFF);
    return 1;
}

int dos_key_peek(dos_session_t* s, char* out) {
    uint16_t w;
    if (!dos_key_peek_word(s, &w)) return 0;
    if (out) *out = (char)(w & 0xFF);
    return 1;
}

int dos_bios_int10(dos_session_t* s) {
    uint8_t ah = (uint8_t)((s->cpu.ax >> 8) & 0xFF);
    uint8_t al = (uint8_t)(s->cpu.ax & 0xFF);
    uint8_t bl;
    int r, c, i;

    switch (ah) {
    case 0x00:
        dos_video_set_mode(s, al);
        return 0;
    case 0x01: return 0;
    case 0x02:
        s->cursor_r = (int)((s->cpu.dx >> 8) & 0xFF);
        s->cursor_c = (int)(s->cpu.dx & 0xFF);
        if (s->cursor_r >= DOS_TEXT_ROWS) s->cursor_r = DOS_TEXT_ROWS - 1;
        if (s->cursor_c >= DOS_TEXT_COLS) s->cursor_c = DOS_TEXT_COLS - 1;
        return 0;
    case 0x03:
        s->cpu.dx = (uint16_t)((s->cursor_r << 8) | (s->cursor_c & 0xFF));
        s->cpu.cx = 0x0607;
        return 0;
    case 0x05: s->active_page = al; return 0;
    case 0x06: {
        uint8_t n = al;
        int r1 = (s->cpu.cx >> 8) & 0xFF, c1 = s->cpu.cx & 0xFF;
        int r2 = (s->cpu.dx >> 8) & 0xFF, c2 = s->cpu.dx & 0xFF;
        uint8_t attr = (uint8_t)((s->cpu.bx >> 8) & 0xFF);
        if (r2 >= DOS_TEXT_ROWS) r2 = DOS_TEXT_ROWS - 1;
        if (c2 >= DOS_TEXT_COLS) c2 = DOS_TEXT_COLS - 1;
        if (n == 0) n = (uint8_t)(r2 - r1 + 1);
        for (i = 0; i < n; i++) {
            for (r = r1; r < r2; r++)
                for (c = c1; c <= c2; c++) {
                    s->text[r][c] = s->text[r + 1][c];
                    s->attr[r][c] = s->attr[r + 1][c];
                }
            for (c = c1; c <= c2; c++) {
                s->text[r2][c] = ' ';
                s->attr[r2][c] = attr ? attr : 0x07;
            }
        }
        dos_video_sync_to_b800(s);
        return 0;
    }
    case 0x07: {
        uint8_t attr = (uint8_t)((s->cpu.bx >> 8) & 0xFF);
        int r1 = (s->cpu.cx >> 8) & 0xFF, c1 = s->cpu.cx & 0xFF;
        int r2 = (s->cpu.dx >> 8) & 0xFF, c2 = s->cpu.dx & 0xFF;
        for (r = r1; r <= r2 && r < DOS_TEXT_ROWS; r++)
            for (c = c1; c <= c2 && c < DOS_TEXT_COLS; c++) {
                s->text[r][c] = ' ';
                s->attr[r][c] = attr ? attr : 0x07;
            }
        dos_video_sync_to_b800(s);
        return 0;
    }
    case 0x08:
        dos_video_sync_from_b800(s);
        s->cpu.ax = (uint16_t)((s->attr[s->cursor_r][s->cursor_c] << 8) |
                               (uint8_t)s->text[s->cursor_r][s->cursor_c]);
        return 0;
    case 0x09: case 0x0A: {
        uint16_t count = s->cpu.cx ? s->cpu.cx : 1;
        bl = (uint8_t)(s->cpu.bx & 0xFF);
        for (i = 0; i < (int)count; i++) {
            int cc = s->cursor_c + i;
            if (cc >= DOS_TEXT_COLS) break;
            s->text[s->cursor_r][cc] = (char)al;
            if (ah == 0x09) s->attr[s->cursor_r][cc] = bl;
        }
        dos_video_sync_to_b800(s);
        return 0;
    }
    case 0x0C: { /* write pixel Mode 13 */
        if (s->video_mode == 0x13) {
            int x = (int)s->cpu.cx, y = (int)s->cpu.dx;
            if (x >= 0 && x < 320 && y >= 0 && y < 200)
                dos_write8(s, DOS_A000_LINEAR + (uint32_t)y * 320u + (uint32_t)x, al);
        }
        return 0;
    }
    case 0x0D: {
        if (s->video_mode == 0x13) {
            int x = (int)s->cpu.cx, y = (int)s->cpu.dx;
            uint8_t pix = 0;
            if (x >= 0 && x < 320 && y >= 0 && y < 200)
                pix = dos_read8(s, DOS_A000_LINEAR + (uint32_t)y * 320u + (uint32_t)x);
            s->cpu.ax = (uint16_t)((s->cpu.ax & 0xFF00) | pix);
        }
        return 0;
    }
    case 0x0E:
        dos_video_putc(s, (char)al);
        return 0;
    case 0x0F:
        s->cpu.ax = (uint16_t)((DOS_TEXT_COLS << 8) | (s->video_mode & 0xFF));
        s->cpu.bx = (uint16_t)((s->cpu.bx & 0xFF00) | s->active_page);
        return 0;
    case 0x10: /* set palette registers (DAC subset) */
        if (al == 0x00) { /* set one register BH=color, BL=index — map to gray-ish */
            uint8_t idx = (uint8_t)(s->cpu.bx & 0xFF);
            uint8_t col = (uint8_t)((s->cpu.bx >> 8) & 0xFF);
            if (idx < 16) {
                s->vga_pal[idx][0] = (uint8_t)((col & 4) ? 42 : 0);
                s->vga_pal[idx][1] = (uint8_t)((col & 2) ? 42 : 0);
                s->vga_pal[idx][2] = (uint8_t)((col & 1) ? 42 : 0);
            }
        } else if (al == 0x10) { /* set DAC BX=index, CH/CL/DH = RGB 0..63 */
            uint16_t idx = s->cpu.bx & 0xFF;
            s->vga_pal[idx][0] = (uint8_t)((s->cpu.cx >> 8) & 0x3F);
            s->vga_pal[idx][1] = (uint8_t)(s->cpu.cx & 0x3F);
            s->vga_pal[idx][2] = (uint8_t)((s->cpu.dx >> 8) & 0x3F);
        }
        return 0;
    case 0x13: {
        uint16_t len = s->cpu.cx;
        uint32_t addr = dos_seg_off(s->cpu.es, s->cpu.bp);
        s->cursor_r = (int)((s->cpu.dx >> 8) & 0xFF);
        s->cursor_c = (int)(s->cpu.dx & 0xFF);
        for (i = 0; i < (int)len; i++)
            dos_video_putc(s, (char)dos_read8(s, addr + (uint32_t)i));
        return 0;
    }
    default:
        return 0;
    }
}

int dos_bios_int16(dos_session_t* s) {
    uint8_t ah = (uint8_t)((s->cpu.ax >> 8) & 0xFF);
    uint16_t w;
    if (ah == 0x00 || ah == 0x10) {
        if (!dos_key_pop_word(s, &w)) {
            s->cpu.ip = (uint16_t)(s->cpu.ip - 2);
            return 0;
        }
        s->cpu.ax = w;
        return 0;
    }
    if (ah == 0x01 || ah == 0x11) {
        if (!dos_key_peek_word(s, &w)) {
            s->cpu.flags |= 0x0040;
            return 0;
        }
        s->cpu.flags &= (uint16_t)~0x0040;
        s->cpu.ax = w;
        return 0;
    }
    if (ah == 0x02) {
        s->cpu.ax = (uint16_t)(s->cpu.ax & 0xFF00);
        return 0;
    }
    return 0;
}

int dos_bios_int1a(dos_session_t* s) {
    uint8_t ah = (uint8_t)((s->cpu.ax >> 8) & 0xFF);
    if (ah == 0x00) {
        s->cpu.dx = (uint16_t)(s->bios_ticks & 0xFFFF);
        s->cpu.cx = (uint16_t)((s->bios_ticks >> 16) & 0xFFFF);
        s->cpu.ax = (uint16_t)(s->cpu.ax & 0xFF00);
        return 0;
    }
    if (ah == 0x01) {
        s->bios_ticks = ((uint32_t)s->cpu.cx << 16) | s->cpu.dx;
        return 0;
    }
    if (ah == 0x02) { /* get RTC time stub from ticks */
        uint32_t t = s->bios_ticks;
        uint32_t sec = (t / 18) % 60;
        uint32_t min = (t / 18 / 60) % 60;
        uint32_t hr = (t / 18 / 60 / 60) % 24;
        s->cpu.cx = (uint16_t)(((hr / 10) << 12) | ((hr % 10) << 8) |
                               ((min / 10) << 4) | (min % 10));
        s->cpu.dx = (uint16_t)(((sec / 10) << 12) | ((sec % 10) << 8));
        s->cpu.flags &= (uint16_t)~0x0001;
        return 0;
    }
    if (ah == 0x04) { /* get RTC date stub */
        s->cpu.cx = 0x2026;
        s->cpu.dx = 0x0807;
        s->cpu.flags &= (uint16_t)~0x0001;
        return 0;
    }
    return 0;
}

static uint32_t int15_desc_base(dos_session_t* s, uint32_t desc) {
    uint32_t base = dos_read16(s, desc + 2);
    base |= (uint32_t)dos_read8(s, desc + 4) << 16;
    base |= (uint32_t)dos_read8(s, desc + 7) << 24;
    return base;
}

int dos_bios_int15(dos_session_t* s) {
    uint8_t ah = (uint8_t)((s->cpu.ax >> 8) & 0xFF);
    uint8_t al = (uint8_t)(s->cpu.ax & 0xFF);
    if (ah == 0x86) { /* wait CX:DX microseconds — approximate by burning ticks */
        uint32_t us = ((uint32_t)s->cpu.cx << 16) | s->cpu.dx;
        uint32_t ticks = us / 54945u; /* ~18.2 Hz */
        if (ticks == 0 && us) ticks = 1;
        s->bios_ticks += ticks;
        s->cpu.flags &= (uint16_t)~0x0001;
        return 0;
    }
    if (ah == 0x87) { /* copy extended memory (words) via ES:SI GDT */
        uint32_t gdt = dos_seg_off(s->cpu.es, s->cpu.si);
        uint32_t src = int15_desc_base(s, gdt + 0x10); /* source descriptor */
        uint32_t dst = int15_desc_base(s, gdt + 0x18); /* dest descriptor */
        uint32_t words = s->cpu.cx;
        uint32_t i;
        if (src >= DOS_MEM_SIZE || dst >= DOS_MEM_SIZE ||
            src + words * 2u > DOS_MEM_SIZE || dst + words * 2u > DOS_MEM_SIZE) {
            s->cpu.ax = (uint16_t)((s->cpu.ax & 0xFF00) | 0x01);
            s->cpu.flags |= 0x0001;
            return 0;
        }
        for (i = 0; i < words; i++) {
            uint16_t w = dos_read16(s, src + i * 2u);
            dos_write16(s, dst + i * 2u, w);
        }
        s->cpu.ax = 0; /* AH=0 success */
        s->cpu.flags &= (uint16_t)~0x0001;
        return 0;
    }
    if (ah == 0x88) { /* extended memory size KB above 1 MiB */
        s->cpu.ax = (uint16_t)(DOS_EXT_KB > 0xFFFF ? 0xFFFF : DOS_EXT_KB);
        s->cpu.flags &= (uint16_t)~0x0001;
        return 0;
    }
    if (ah == 0xE8 && al == 0x01) { /* newer memory size */
        uint32_t ext_kb = DOS_EXT_KB;
        uint32_t below16 = ext_kb > 15u * 1024u ? 15u * 1024u : ext_kb;
        uint32_t above16 = ext_kb > 15u * 1024u ? ext_kb - 15u * 1024u : 0;
        s->cpu.cx = (uint16_t)below16;
        s->cpu.dx = (uint16_t)(above16 > 0xFFFFu ? 0xFFFFu : above16);
        s->cpu.ax = s->cpu.cx;
        s->cpu.bx = s->cpu.dx;
        s->cpu.flags &= (uint16_t)~0x0001;
        return 0;
    }
    if (ah == 0xC0) { /* ROM configuration table (AT/PS2) */
        s->cpu.es = DOS_IVT_IRET;
        s->cpu.bx = 0x0200;
        s->cpu.ax = (uint16_t)(s->cpu.ax & 0x00FFu); /* AH = 0 */
        s->cpu.flags &= (uint16_t)~0x0001;
        return 0;
    }
    if (ah == 0xC1) { /* return extended BIOS data area segment — none */
        s->cpu.flags |= 0x0001;
        return 0;
    }
    (void)al;
    s->cpu.flags |= 0x0001;
    return 0;
}

void dos_mouse_update(dos_session_t* s) {
    int mx, my, buttons;
    int lx, ly, relx, rely;
    if (!s || !s->win) return;
    vdesk_get_pointer(&mx, &my, &buttons);
    /* map screen pointer into DOS window content → 640x200 mouse space */
    lx = mx - (s->win->x + 4);
    ly = my - (s->win->y + 24);
    if (s->win->width > 8) {
        s->mouse_x = (lx * 640) / (s->win->width - 8);
    }
    if (s->win->height > 28) {
        s->mouse_y = (ly * 200) / (s->win->height - 28);
    }
    if (s->mouse_x < (int)s->mouse_min_x) s->mouse_x = s->mouse_min_x;
    if (s->mouse_x > (int)s->mouse_max_x) s->mouse_x = s->mouse_max_x;
    if (s->mouse_y < (int)s->mouse_min_y) s->mouse_y = s->mouse_min_y;
    if (s->mouse_y > (int)s->mouse_max_y) s->mouse_y = s->mouse_max_y;
    relx = s->mouse_x; /* mickeys approximated as position delta stored */
    rely = s->mouse_y;
    (void)relx; (void)rely;
    s->mouse_buttons = 0;
    if (buttons & 1) s->mouse_buttons |= 1;
    if (buttons & 2) s->mouse_buttons |= 2;
    if (buttons & 4) s->mouse_buttons |= 4;
}

int dos_bios_int33(dos_session_t* s) {
    uint16_t ax = s->cpu.ax;
    switch (ax) {
    case 0x0000: /* reset/get status */
        s->mouse_shown = 0;
        s->mouse_min_x = 0; s->mouse_max_x = 639;
        s->mouse_min_y = 0; s->mouse_max_y = 199;
        s->mouse_x = 320; s->mouse_y = 100;
        s->cpu.ax = 0xFFFF; /* installed */
        s->cpu.bx = 3;      /* buttons */
        return 0;
    case 0x0001: s->mouse_shown = 1; return 0;
    case 0x0002: s->mouse_shown = 0; return 0;
    case 0x0003:
        dos_mouse_update(s);
        s->cpu.bx = (uint16_t)s->mouse_buttons;
        s->cpu.cx = (uint16_t)s->mouse_x;
        s->cpu.dx = (uint16_t)s->mouse_y;
        return 0;
    case 0x0004:
        s->mouse_x = (int)s->cpu.cx;
        s->mouse_y = (int)s->cpu.dx;
        return 0;
    case 0x0007:
        s->mouse_min_x = s->cpu.cx;
        s->mouse_max_x = s->cpu.dx;
        return 0;
    case 0x0008:
        s->mouse_min_y = s->cpu.cx;
        s->mouse_max_y = s->cpu.dx;
        return 0;
    case 0x000B:
        s->cpu.cx = (uint16_t)s->mouse_mickey_x;
        s->cpu.dx = (uint16_t)s->mouse_mickey_y;
        s->mouse_mickey_x = 0;
        s->mouse_mickey_y = 0;
        return 0;
    case 0x000C: /* set handler — store, call optional later */
        s->mouse_handler_mask = s->cpu.cx;
        s->mouse_handler_seg = s->cpu.es;
        s->mouse_handler_off = s->cpu.dx;
        return 0;
    default:
        return 0;
    }
}

void dos_timer_tick(dos_session_t* s) {
    if (!s || s->halted) return;
    s->bios_ticks++;
    dos_write16(s, 0x46C, (uint16_t)(s->bios_ticks & 0xFFFF));
    dos_write16(s, 0x46E, (uint16_t)((s->bios_ticks >> 16) & 0xFFFF));
    /* Advance PIT channel 0 toward zero for games that poll it */
    if (s->pit_count > 32) s->pit_count = (uint16_t)(s->pit_count - 32);
    else s->pit_count = s->pit_reload ? s->pit_reload : 0xFFFFu;
}
