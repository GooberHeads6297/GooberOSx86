#include "vesa_window.h"
#include "../drivers/video/vesa.h"
#include "../drivers/keyboard/keyboard.h"
#include "../drivers/mouse/mouse.h"
#include "../drivers/input/input.h"
#include "../drivers/input/touchpad.h"
#include "../drivers/timer/timer.h"
#include "../drivers/timer/softclock.h"
#include "../drivers/usb/usb.h"
#include "../drivers/diagnostics/driver_log.h"
#include "../drivers/video/display.h"
#include "../fs/filesystem.h"
#include "../taskmgr/process.h"
#include "../lib/string.h"
#include "../lib/memory.h"
#include "../kernel.h"
#include "../drivers/io/io.h"

/* Drain any PS/2 bytes left in the 8042 output buffer (boot noise / ACKs). */
static void vdesk_drain_ps2_output(void) {
    uint32_t guard = 64;
    while (guard-- && (inb(0x64) & 0x01))
        (void)inb(0x60);
    while (keyboard_has_char())
        (void)keyboard_read_char();
}

/* Parse gooberos.theme=<original|dark|light>. Original is the shell-first default. */
static int vdesk_initial_theme_from_cmdline(void) {
    const char* cmdline = kernel_boot_cmdline();
    if (!cmdline) return VDESK_APPEARANCE_MODERN_DARK;
    const char* p = cmdline;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        const char* needle = "gooberos.theme=";
        int i = 0;
        while (needle[i] && p[i] && needle[i] == p[i]) i++;
        if (!needle[i]) {
            const char* v = p + i;
            if (v[0] == 'l' && v[1] == 'i') return VDESK_APPEARANCE_LIGHT;
            if (v[0] == 'o' && v[1] == 'r') return VDESK_APPEARANCE_ORIGINAL;
            if (v[0] == 'd' && v[1] == 'a') return VDESK_APPEARANCE_MODERN_DARK;
            return VDESK_APPEARANCE_MODERN_DARK;
        }
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return VDESK_APPEARANCE_MODERN_DARK;
}

static VDesktop desktop;
static int start_dos_flyout;
static int start_games_flyout;
static int start_tools_flyout;

typedef struct {
    int active;
    Directory* src_dir;
    char name[VICON_NAME_MAX];
    int is_dir;
} VFileDrag;

static VFileDrag g_file_drag;

#define VCTX_FS 3 /* filesystem item (desktop file/folder or explorer row) */

static int vdesk_copy_file_between(Directory* src, const char* name, Directory* dst);
int vdesk_file_drag_drop(void);
static int new_file_count = 1;
static int new_bitmap_count = 1;
static int new_folder_count = 1;
static int shell_output_color_index = 0;
static int shell_input_color_index = 0;
static uint32_t desktop_log_last_version = 0;
static uint32_t desktop_log_last_sync_tick = 0;

static void clear_icon_selection(void);

static void vdesk_sync_driver_log_file(void) {
    /*
     * Automatic Desktop/log.txt rewrites on FAT32/eMMC freeze the UI for
     * 1–4 seconds (full file rewrite + FAT scan). Keep the in-memory log
     * for `logs` / `driverlog`; only sync when explicitly requested or on
     * a cheap live memfs backend.
     */
    uint32_t now;
    uint32_t version;
    if (fs_is_persistent()) return;

    now = timer_ticks();
    version = driver_log_version();
    if (version == desktop_log_last_version) return;
    /* 60 s at 100 Hz on memfs (cheap); was 10 s and hit FAT too. */
    if (now - desktop_log_last_sync_tick < 6000U) return;
    if (driver_log_sync_desktop_file() == 0) {
        desktop_log_last_version = version;
        desktop_log_last_sync_tick = now;
    }
}

static const VTheme original_theme = {
    0x05090D, 0x061B3A, 0x12325F, 0x071018, 0x7FD88A, 0x254F8E,
    0x09131A, 0x000000, 0x153C7A, 0x102235, 0xD7E8FF, 0x7FD88A,
    0x5AA96A, 0x0D2C5C, 0x4D8F5D, 0x071A33, 0x000000, 0x315E9B,
    0x081520, 0x000000, 0x67B878, 0x5B7FDB, 0x5F9568
};

static const VTheme dark_theme = {
    0x202124, 0x16181C, 0x30343B, 0x252A31, 0xF1F3F4, 0x3A7BD5,
    0x2B3038, 0x1F2329, 0x0A64D8, 0x3A3F47, 0xFFFFFF, 0xE8EAED,
    0x9AA0A6, 0x050608, 0x4B5563, 0x111318, 0x050608, 0x4C8BF5,
    0x343A42, 0x000000, 0x00AA00, 0x0000AA, 0x9AA0A6
};

static const VTheme light_theme = {
    0xEAF2F8, 0xECEFF4, 0xFFFFFF, 0xFFFFFF, 0x111827, 0x2D6CDF,
    0xF3F4F6, 0xFFFFFF, 0x1B68D2, 0x9CA3AF, 0xFFFFFF, 0x111827,
    0x4B5563, 0x2F343D, 0xFFFFFF, 0x7B8190, 0x5A6070, 0x2563EB,
    0xE5E7EB, 0xFFFFFF, 0x008000, 0x0000AA, 0x4B5563
};

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ABS(a) ((a) < 0 ? -(a) : (a))
#define CLAMP(v, lo, hi) MAX((lo), MIN((hi), (v)))

#define VCTX_DESKTOP 0
#define VCTX_ICON    1
#define VCTX_TASKBAR 2

static const VTheme* theme(void) {
    if (desktop.appearance == VDESK_APPEARANCE_LIGHT) return &light_theme;
    if (desktop.appearance == VDESK_APPEARANCE_MODERN_DARK) return &dark_theme;
    return &original_theme;
}

void vdesk_mark_dirty(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int x2 = x + w;
    int y2 = y + h;
    x = CLAMP(x, 0, desktop.screen_w);
    y = CLAMP(y, 0, desktop.screen_h);
    x2 = CLAMP(x2, 0, desktop.screen_w);
    y2 = CLAMP(y2, 0, desktop.screen_h);
    if (x >= x2 || y >= y2) return;

    if (!desktop.dirty) {
        desktop.dirty_x1 = x;
        desktop.dirty_y1 = y;
        desktop.dirty_x2 = x2;
        desktop.dirty_y2 = y2;
        desktop.dirty = 1;
        return;
    }

    if (x < desktop.dirty_x1) desktop.dirty_x1 = x;
    if (y < desktop.dirty_y1) desktop.dirty_y1 = y;
    if (x2 > desktop.dirty_x2) desktop.dirty_x2 = x2;
    if (y2 > desktop.dirty_y2) desktop.dirty_y2 = y2;
}

void vdesk_mark_full_dirty(void) {
    vdesk_mark_dirty(0, 0, desktop.screen_w, desktop.screen_h);
}

static void mark_window_dirty(VWindow* win) {
    if (!win) return;
    vdesk_mark_dirty(win->x - 8, win->y - 8, win->width + 16, win->height + 16);
}

static void mark_taskbar_dirty(void) {
    int ty = (desktop.taskbar_position == VDESK_TASKBAR_TOP) ?
             0 : desktop.screen_h - TASKBAR_HEIGHT;
    vdesk_mark_dirty(0, ty, desktop.screen_w, TASKBAR_HEIGHT);
}

/* Primary shell typing only changes the prompt line — avoid marking the
 * maximized shell dirty (that promotes a multi-megabyte full-frame present).
 * We dirty the FULL width of the prompt but only a single row (char_h + 4 px)
 * tall, so every typed character is presented while the update stays a cheap
 * one-row blit. It can never promote to a full-screen present because
 * should_promote_full_present() requires dirty_h >= 48. A previous 128 px width
 * cap (a VirtualBox present-cost workaround) truncated the on-screen echo after
 * ~14 characters even though they were already in the back-buffer. */
static void mark_shell_prompt_dirty(VWindow* win) {
    int cx, cy, cw, ch;
    int char_h = 16;
    int text_y, text_h, rows, prompt_y;
    if (!win) return;
    cx = win->x + BORDER_SIZE;
    cy = win->y + TITLEBAR_HEIGHT + BORDER_SIZE;
    cw = win->width - BORDER_SIZE * 2;
    ch = win->height - TITLEBAR_HEIGHT - BORDER_SIZE * 2;
    if (cw < 8 || ch < char_h) {
        mark_window_dirty(win);
        return;
    }
    text_y = cy + 4; /* SHELL_PAD_Y */
    text_h = ch - 8;
    rows = text_h / char_h;
    if (rows < 2) rows = 2;
    prompt_y = text_y + (rows - 1) * char_h;
    vdesk_mark_dirty(cx, prompt_y, cw, char_h + 4);
}

static int shell_key_needs_full_dirty(char c) {
    /* Enter changes scrollback; history up/down only edits the prompt line. */
    return (c == '\r' || c == '\n');
}

/* Decide whether a dirty rect should promote to a whole-screen present.
 * Only true full-screen dirties (vdesk_mark_full_dirty) may promote — a
 * maximized shell dirty rect is nearly full-screen and must NOT promote. */
static int should_promote_full_present(int dirty_x, int dirty_y,
                                       int dirty_w, int dirty_h,
                                       int dirty_area, int screen_area) {
    (void)dirty_area;
    (void)screen_area;

    if (display_scanout_uncached())
        return 0;
    if (dirty_h < 48 || dirty_w * dirty_h < (64 * 64))
        return 0;
    /* Literal full-screen dirty only. */
    if (dirty_x == 0 && dirty_y == 0 &&
        dirty_w >= desktop.screen_w && dirty_h >= desktop.screen_h)
        return 1;
    return 0;
}

/* Classic arrow cursor (original silhouette; tip = hot-spot at 0,0).
 * Bit 11 = leftmost column. and = opaque pixels; xor = white fill (else black). */
#define CURSOR_W 12
#define CURSOR_H 19
static const uint16_t g_cursor_and[CURSOR_H] = {
    0x800, /* #           */
    0xC00, /* ##          */
    0xE00, /* ###         */
    0xF00, /* ####        */
    0xF80, /* #####       */
    0xFC0, /* ######      */
    0xFE0, /* #######     */
    0xFF0, /* ########    */
    0xFF8, /* #########   */
    0xF80, /* #####       */
    0xDC0, /* ## ###      */
    0xCE0, /* ##  ###     */
    0x870, /* #    ###    */
    0x030, /*       ##    */
    0x030, /*       ##    */
    0x018, /*        #    */
    0x018, /*        #    */
    0x00C, /*         #   */
    0x00C  /*         #   */
};
static const uint16_t g_cursor_xor[CURSOR_H] = {
    0x000,
    0x400, /* .#          */
    0x600, /* .##         */
    0x700, /* .###        */
    0x780, /* .####       */
    0x7C0, /* .#####      */
    0x7E0, /* .######     */
    0x7F0, /* .#######    */
    0x700, /* .###        */
    0x480, /* .#  #       */
    0x4C0, /* .#  ##      */
    0x060, /*      ##     */
    0x020, /*       #     */
    0x020, /*       #     */
    0x010, /*        #    */
    0x010, /*        #    */
    0x008, /*         #   */
    0x008, /*         #   */
    0x000
};

static void mark_mouse_dirty(int x, int y) {
    vdesk_mark_dirty(x - 1, y - 1, CURSOR_W + 2, CURSOR_H + 2);
}

static int vdesk_any_drag_active(void) {
    int i;
    for (i = 0; i < MAX_VWINDOWS; i++) {
        if (desktop.windows[i].visible &&
            (desktop.windows[i].drag_active || desktop.windows[i].resize_active))
            return 1;
    }
    for (i = 0; i < desktop.icon_count; i++) {
        if (desktop.icons[i].drag_active)
            return 1;
    }
    return 0;
}

/* Desktop freeze diagnostics: last breadcrumb trail stays visible on-screen
 * when the event pump stalls (VirtualBox has no easy serial). Enabled only by
 * gooberos.debug=1 on the cmdline -- F10 no longer toggles this strip. */
#define VDESK_BC_TRAIL 48
static char g_vdesk_bc_trail[VDESK_BC_TRAIL + 1];
static int g_vdesk_bc_len = 0;
static int g_vdesk_debug_hud = 0; /* cmdline gooberos.debug= only */
static int g_vdesk_alive_beacon = 0; /* top-right idle tick; F10 toggles; off by default */
static char g_vdesk_last_key = 0;
static int g_vdesk_last_dirty_w = 0;
static int g_vdesk_last_dirty_h = 0;
/* Freeze oracle bookkeeping: last observed keyboard IRQ count so the loop can
 * emit a serial stamp on every new IRQ (make AND break), independent of whether
 * a key produced a printable char. */
static uint32_t g_vdesk_last_irqn = 0;

static void vdesk_bc(char c) {
    outb(0xE9, (uint8_t)c);
    if (g_vdesk_bc_len < VDESK_BC_TRAIL) {
        g_vdesk_bc_trail[g_vdesk_bc_len++] = c;
        g_vdesk_bc_trail[g_vdesk_bc_len] = '\0';
    } else {
        int i;
        for (i = 1; i < VDESK_BC_TRAIL; i++)
            g_vdesk_bc_trail[i - 1] = g_vdesk_bc_trail[i];
        g_vdesk_bc_trail[VDESK_BC_TRAIL - 1] = c;
        g_vdesk_bc_trail[VDESK_BC_TRAIL] = '\0';
    }
}

/*
 * Keypress-freeze serial oracle (gated on the debug switch). Unlike the
 * on-screen HUD, kserial_note() writes only to COM1 + 0xE9, so it stays a valid
 * witness even if the compositor/panel is the thing that wedged. Capture on
 * VirtualBox with a COM1 -> host file port; on QEMU with -serial file:.
 *
 * g_vdesk_kbd_trace_pending is set when a key is consumed and cleared after the
 * next present, so the log proves the pump kept running THROUGH a keypress
 * (the exact VBox failure the EOI-first ISR change targets).
 */
static int g_vdesk_kbd_trace_pending = 0;

static void vdesk_serial_key(const char* tag, char c) {
    char buf[24];
    const char* hex = "0123456789ABCDEF";
    int i = 0;
    if (!g_vdesk_debug_hud) return;
    while (tag[i] && i < 12) { buf[i] = tag[i]; i++; }
    buf[i++] = '=';
    buf[i++] = 'x';
    buf[i++] = hex[((unsigned char)c >> 4) & 0xF];
    buf[i++] = hex[(unsigned char)c & 0xF];
    buf[i++] = '\n';
    buf[i] = '\0';
    kserial_note(buf);
}

/* Emit "<tag>=x<8 hex>\n" to the pure-serial oracle (gated on debug). Used for
 * the freeze diagnostics that must be independent of the on-screen compositor:
 * IRQ counts and loop heartbeats. */
static void vdesk_serial_hex(const char* tag, uint32_t v) {
    char buf[40];
    const char* hx = "0123456789ABCDEF";
    int i = 0;
    int s;
    if (!g_vdesk_debug_hud) return;
    while (tag[i] && i < 24) { buf[i] = tag[i]; i++; }
    buf[i++] = '=';
    buf[i++] = 'x';
    for (s = 28; s >= 0; s -= 4) buf[i++] = hx[(v >> s) & 0xF];
    buf[i++] = '\n';
    buf[i] = '\0';
    kserial_note(buf);
}

static void vdesk_render_debug_hud(void) {
    char line[96];
    char isr[28];
    uint8_t st = 0, sc = 0;
    uint32_t irqn = 0;
    int i = 0;
    int x, y;
    char hex[] = "0123456789ABCDEF";
    if (!g_vdesk_debug_hud) return;

    keyboard_debug_snapshot(isr, (int)sizeof(isr), &st, &sc, &irqn);

    /* dbg:K=.. irq=.. isr=.. bc=.. */
    line[i++] = 'd'; line[i++] = 'b'; line[i++] = 'g'; line[i++] = ':';
    line[i++] = 'K'; line[i++] = '=';
    if (g_vdesk_last_key >= 0x20 && g_vdesk_last_key < 0x7F)
        line[i++] = g_vdesk_last_key;
    else {
        line[i++] = 'x';
        line[i++] = hex[((unsigned char)g_vdesk_last_key >> 4) & 0xF];
        line[i++] = hex[(unsigned char)g_vdesk_last_key & 0xF];
    }
    line[i++] = ' ';
    line[i++] = 'i'; line[i++] = '=';
    {
        uint32_t n = irqn;
        char tmp[10];
        int t = 0;
        if (n == 0) tmp[t++] = '0';
        else {
            char rev[10];
            int r = 0;
            while (n > 0 && r < 9) { rev[r++] = (char)('0' + (n % 10)); n /= 10; }
            while (r > 0) tmp[t++] = rev[--r];
        }
        tmp[t] = 0;
        for (int k = 0; tmp[k] && i < 40; k++) line[i++] = tmp[k];
    }
    line[i++] = ' ';
    line[i++] = 's'; line[i++] = '=';
    line[i++] = hex[(st >> 4) & 0xF];
    line[i++] = hex[st & 0xF];
    line[i++] = '/';
    line[i++] = hex[(sc >> 4) & 0xF];
    line[i++] = hex[sc & 0xF];
    line[i++] = ' ';
    line[i++] = 'I'; line[i++] = '=';
    for (int k = 0; isr[k] && i < 70; k++) line[i++] = isr[k];
    line[i++] = ' ';
    line[i++] = 'b'; line[i++] = '=';
    for (int k = 0; k < g_vdesk_bc_len && i < 94; k++)
        line[i++] = g_vdesk_bc_trail[k];
    line[i] = '\0';

    y = desktop.screen_h - 18;
    if (y < 0) y = 0;
    x = 4;
    vesa_fill_rect(x, y, desktop.screen_w - 8, 16, 0x000000);
    vesa_draw_string(x, y + 1, line, 0x00FF00, 0x000000);
}

/*
 * Top-right idle tick / freeze oracle. F10 toggles this on and off.
 * When enabled it updates every frame WITHOUT needing keyboard, mouse, or
 * touchpad -- so you can tell "desktop loop alive, input dead" from a freeze.
 *
 *   [##] f=NNNN i=NNNN m=N
 *    ^^ blinks green/dark while the pump runs
 *    f= frame counter (must keep climbing)
 *    i= keyboard IRQ1 count
 *    m= scancode mode (1=AT, 2=VM set-2)
 */
static int g_beacon_last_x = 0, g_beacon_last_y = 0;
static int g_beacon_last_w = 0, g_beacon_last_h = 0;

static void vdesk_erase_alive_beacon(void) {
    int x = g_beacon_last_x;
    int y = g_beacon_last_y;
    int w = g_beacon_last_w;
    int h = g_beacon_last_h;
    if (w <= 0 || h <= 0) {
        /* Generous fallback if we never drew yet. */
        w = 160;
        h = 16;
        y = 2;
        x = desktop.screen_w - w - 4;
        if (x < 0) x = 0;
    }
    /* Mark dirty so the next composite redraws taskbar/desktop under the strip. */
    vdesk_mark_dirty(x - 2, y, w + 4, h + 2);
    g_beacon_last_w = 0;
    g_beacon_last_h = 0;
}

static void vdesk_render_alive_beacon(void) {
    char line[40];
    uint8_t st = 0, sc = 0;
    uint32_t irqn = 0;
    uint32_t n;
    int i = 0;
    int x, y, w, h;
    int blink;
    uint32_t pulse_color;
    char rev[12];
    int r;

    if (!g_vdesk_alive_beacon) return;

    keyboard_debug_snapshot((char*)0, 0, &st, &sc, &irqn);

    /* Blink at ~2 Hz using PIT ticks (100 Hz). */
    blink = ((timer_ticks() / 25u) & 1u) != 0;
    pulse_color = blink ? 0x00FF00u : 0x003300u;

    line[i++] = 'f'; line[i++] = '=';
    n = desktop.metrics.frame_count;
    r = 0;
    if (n == 0) rev[r++] = '0';
    else while (n > 0 && r < 10) { rev[r++] = (char)('0' + (n % 10)); n /= 10; }
    while (r > 0 && i < 18) line[i++] = rev[--r];
    line[i++] = ' ';
    line[i++] = 'i'; line[i++] = '=';
    n = irqn;
    r = 0;
    if (n == 0) rev[r++] = '0';
    else while (n > 0 && r < 10) { rev[r++] = (char)('0' + (n % 10)); n /= 10; }
    while (r > 0 && i < 28) line[i++] = rev[--r];
    line[i++] = ' ';
    line[i++] = 'm'; line[i++] = '=';
    line[i++] = keyboard_scancode_mode();
    line[i] = '\0';

    w = 8 + (i * 8) + 16; /* pulse box + gap + text */
    h = 14;
    /* Sit just under the top taskbar so the clock tray stays readable. */
    y = (desktop.taskbar_position == VDESK_TASKBAR_TOP) ? (TASKBAR_HEIGHT + 2) : 2;
    x = desktop.screen_w - w - 4;
    if (x < 0) x = 0;

    g_beacon_last_x = x;
    g_beacon_last_y = y;
    g_beacon_last_w = w;
    g_beacon_last_h = h;

    vesa_fill_rect(x, y, w, h, 0x000000);
    vesa_fill_rect(x + 2, y + 2, 10, 10, pulse_color);
    vesa_draw_string(x + 16, y + 2, line, 0x00FF00, 0x000000);

    display_present_set_oneshot(1);
    display_present_rect(x, y, w, h);
    display_present_set_oneshot(0);
}

static void update_metrics_pointer(void) {
    input_device_t active = input_get_active_pointer();
    desktop.metrics.active_pointer = (int)active;
    desktop.metrics.usb_pointer_active =
        (active == INPUT_DEVICE_USB_MOUSE || active == INPUT_DEVICE_USB_TOUCHPAD);
    desktop.metrics.i2c_touchpad_active =
        (active == INPUT_DEVICE_I2C_TOUCHPAD);
}

/* ---- Drawing helpers ---- */

void vdesk_draw_rect(int x, int y, int w, int h, color_t color) {
    vesa_fill_rect(x, y, w, h, color);
}

void vdesk_draw_border(int x, int y, int w, int h, color_t light, color_t dark) {
    vesa_fill_rect(x, y, w, 1, light);
    vesa_fill_rect(x, y, 1, h, light);
    vesa_fill_rect(x + w - 1, y, 1, h, dark);
    vesa_fill_rect(x, y + h - 1, w, 1, dark);
}

void vdesk_draw_text(int x, int y, const char* str, color_t fg, color_t bg) {
    vesa_draw_string(x, y, str, fg, bg);
}

static int original_appearance(void) {
    return desktop.appearance == VDESK_APPEARANCE_ORIGINAL;
}

static void draw_soft_corners(int x, int y, int w, int h, color_t bg) {
    if (!original_appearance() || w < 8 || h < 8) return;
    vdesk_draw_rect(x, y, 2, 2, bg);
    vdesk_draw_rect(x + w - 2, y, 2, 2, bg);
    vdesk_draw_rect(x, y + h - 2, 2, 2, bg);
    vdesk_draw_rect(x + w - 2, y + h - 2, 2, 2, bg);
}

/* ---- Desktop state ---- */

void vdesk_set_screen_size(int screen_w, int screen_h) {
    int i;
    if (screen_w < 320) screen_w = 320;
    if (screen_h < 200) screen_h = 200;
    desktop.screen_w = screen_w;
    desktop.screen_h = screen_h;
    for (i = 0; i < desktop.window_count; i++) {
        VWindow* w = &desktop.windows[i];
        if (!w->visible) continue;
        if (w->x + w->width > screen_w) w->x = screen_w - w->width;
        if (w->y + w->height > screen_h - TASKBAR_HEIGHT)
            w->y = screen_h - TASKBAR_HEIGHT - w->height;
        if (w->x < 0) w->x = 0;
        if (w->y < vdesk_workspace_top()) w->y = vdesk_workspace_top();
    }
    if (desktop.mouse_x >= screen_w) desktop.mouse_x = screen_w - 1;
    if (desktop.mouse_y >= screen_h) desktop.mouse_y = screen_h - 1;
    vdesk_mark_full_dirty();
}

void vdesk_init(int screen_w, int screen_h) {
    desktop.screen_w = screen_w;
    desktop.screen_h = screen_h;
    desktop.window_count = 0;
    desktop.next_id = 1;
    desktop.z_count = 0;
    desktop.start_open = 0;
    start_dos_flyout = 0;
    start_games_flyout = 0;
    start_tools_flyout = 0;
    desktop.context_open = 0;
    desktop.context_x = 0;
    desktop.context_y = 0;
    desktop.context_kind = VCTX_DESKTOP;
    desktop.context_target_icon = -1;
    desktop.context_target_window_id = 0;
    desktop.rename_open = 0;
    desktop.rename_target_kind = VICON_FOLDER;
    desktop.rename_dir = NULL;
    desktop.rename_old_name[0] = '\0';
    desktop.rename_input[0] = '\0';
    desktop.rename_len = 0;
    desktop.rename_status = 0;
    desktop.clip_mode = 0;
    desktop.clip_dir = NULL;
    desktop.clip_name[0] = '\0';
    desktop.clip_is_dir = 0;
    desktop.context_fs_dir = NULL;
    desktop.context_fs_name[0] = '\0';
    desktop.context_fs_is_dir = 0;
    desktop.appearance = vdesk_initial_theme_from_cmdline();
    desktop.theme_mode = (desktop.appearance == VDESK_APPEARANCE_LIGHT);
    desktop.primary_shell_id = 0;
    desktop.shell_first_mode = 1;
    /* Shell-first boot: flag matches maximized GooberShell covering icons.
     * Start menu then shows "Show Desktop" (not a misleading "Hide Desktop"). */
    desktop.desktop_experience_visible = 0;
    desktop.tile_wm = 1;
    desktop.shift_click_rmb = 1;
    desktop.taskbar_position = VDESK_TASKBAR_TOP;
    /*
     * Phase 4 (display polish, item 2): target frame budget driven by the
     * gooberos.display.fps=N cmdline knob (default 60 Hz -> 16 ms/frame).
     * Stored as ms-per-frame so the existing timer_sleep()-based pacing
     * loop just consumes it.
     */
    {
        int fps = kernel_display_target_fps();
        if (fps < 10) fps = 10;
        if (fps > 120) fps = 120;
        desktop.target_frame_ms = 1000 / fps;
        if (desktop.target_frame_ms < 1) desktop.target_frame_ms = 1;
    }
    desktop.adaptive_pacing = 1;
    desktop.dirty = 0;
    desktop.icon_count = 0;
    desktop.app_icon_count = 0;
    desktop.fs_signature = 0;
    desktop.last_scan_tick = 0;
    desktop.launch_app = NULL;
    desktop.open_file = NULL;
    desktop.open_fs_item = NULL;
    desktop.mouse_x = screen_w / 2;
    desktop.mouse_y = screen_h / 2;
    desktop.last_mouse_x = desktop.mouse_x;
    desktop.last_mouse_y = desktop.mouse_y;
    desktop.icon_press_index = -1;
    desktop.icon_press_x = 0;
    desktop.icon_press_y = 0;
    desktop.icon_drag_moved = 0;
    desktop.mouse_buttons = 0;
    desktop.running = 1;
    desktop.status_msg[0] = '\0';
    desktop.status_until_tick = 0;
    desktop.clock_tray_stamp = -1;
    memset(desktop.toasts, 0, sizeof(desktop.toasts));
    memset(&desktop.metrics, 0, sizeof(desktop.metrics));
    desktop.metrics.theme_mode = desktop.theme_mode;
    desktop.metrics.appearance = desktop.appearance;
    softclock_init();
    vdesk_mark_full_dirty();

    for (int i = 0; i < MAX_VWINDOWS; i++) {
        desktop.windows[i].visible = 0;
    }
    for (int i = 0; i < MAX_VDESKTOP_ICONS; i++) {
        desktop.icons[i].id = 0;
        desktop.icons[i].drag_active = 0;
    }
}

static VWindow* get_window(int id) {
    for (int i = 0; i < MAX_VWINDOWS; i++) {
        if (desktop.windows[i].id == id && desktop.windows[i].visible)
            return &desktop.windows[i];
    }
    return NULL;
}

int vdesk_workspace_top(void) {
    return desktop.taskbar_position == VDESK_TASKBAR_TOP ? TASKBAR_HEIGHT : 0;
}

int vdesk_workspace_bottom(void) {
    return desktop.taskbar_position == VDESK_TASKBAR_BOTTOM ?
           desktop.screen_h - TASKBAR_HEIGHT : desktop.screen_h;
}

static void remove_z(int id) {
    int j = 0;
    for (int i = 0; i < desktop.z_count; i++) {
        if (desktop.z_order[i] != id)
            desktop.z_order[j++] = desktop.z_order[i];
    }
    desktop.z_count = j;
}

void vdesk_bring_to_front(VWindow* win) {
    if (!win) return;
    remove_z(win->id);
    if (desktop.z_count < MAX_VWINDOWS)
        desktop.z_order[desktop.z_count++] = win->id;
    for (int i = 0; i < MAX_VWINDOWS; i++) {
        if (desktop.windows[i].visible)
            desktop.windows[i].focused = 0;
    }
    win->focused = 1;
}

void vdesk_set_primary_shell(VWindow* win) {
    if (!win) return;
    mark_window_dirty(win);
    desktop.primary_shell_id = win->id;
    win->has_close = 0;
    win->has_minimize = 0;
    win->has_maximize = 0;
    win->saved_x = win->x;
    win->saved_y = win->y;
    win->saved_w = win->width;
    win->saved_h = win->height;
    win->x = 0;
    win->y = vdesk_workspace_top();
    win->width = desktop.screen_w;
    win->height = vdesk_workspace_bottom() - vdesk_workspace_top();
    win->maximized = 1;
    win->resize_active = 0;
    win->resize_edges = 0;
    vdesk_bring_to_front(win);
    vdesk_mark_full_dirty();
}

int vdesk_has_active_app_focus(void) {
    for (int i = 0; i < MAX_VWINDOWS; i++) {
        VWindow* w = &desktop.windows[i];
        if (!w->visible || w->minimized || !w->focused) continue;
        if (w->id != desktop.primary_shell_id) return 1;
    }
    return 0;
}

void vdesk_focus_primary_shell(void) {
    VWindow* shell = get_window(desktop.primary_shell_id);
    if (!desktop.shell_first_mode || !shell || !shell->visible) return;
    if (shell->minimized)
        shell->minimized = 0;
    if (!shell->focused) {
        vdesk_bring_to_front(shell);
        mark_window_dirty(shell);
    }
}

static int should_auto_focus_primary_shell(void) {
    return desktop.shell_first_mode && !desktop.desktop_experience_visible;
}

void vdesk_set_tile_wm(int enabled) {
    desktop.tile_wm = enabled ? 1 : 0;
}

int vdesk_tile_wm_enabled(void) {
    return desktop.tile_wm ? 1 : 0;
}

void vdesk_tile_window(VWindow* win) {
    (void)win;
    int ids[MAX_VWINDOWS];
    int count = 0;
    if (!desktop.tile_wm) return;
    for (int i = 0; i < MAX_VWINDOWS; i++) {
        VWindow* w = &desktop.windows[i];
        if (!w->visible || w->minimized) continue;
        if (w->id == desktop.primary_shell_id) continue;
        ids[count++] = w->id;
    }
    if (count <= 0) return;

    int gap = 8;
    int top = vdesk_workspace_top();
    int bottom = vdesk_workspace_bottom();
    int region_x = desktop.screen_w / 2;
    int region_w = desktop.screen_w - region_x - gap;
    int rows = count;
    if (rows > 3) rows = 3;
    int cols = (count + rows - 1) / rows;
    if (cols < 1) cols = 1;
    if (region_w < 240) {
        region_x = gap;
        region_w = desktop.screen_w - gap * 2;
    }

    int avail_w = region_w - gap * (cols - 1);
    int avail_h = bottom - top - gap * (rows + 1);
    if (avail_w < cols * 160 || avail_h < rows * 110) return;

    int tile_w = avail_w / cols;
    int tile_h = avail_h / rows;
    if (tile_w > 460) tile_w = 460;
    if (tile_h > 300) tile_h = 300;
    for (int i = 0; i < count; i++) {
        VWindow* w = get_window(ids[i]);
        if (!w) continue;
        int col = i % cols;
        int row = (i / cols) % rows;
        mark_window_dirty(w);
        w->x = region_x + col * (tile_w + gap);
        w->y = top + gap + row * (tile_h + gap);
        w->width = tile_w;
        w->height = tile_h;
        w->maximized = 0;
        w->saved_x = w->x;
        w->saved_y = w->y;
        w->saved_w = w->width;
        w->saved_h = w->height;
        mark_window_dirty(w);
    }
    vdesk_mark_full_dirty();
}

VWindow* vdesk_window_at(int x, int y) {
    for (int zi = desktop.z_count - 1; zi >= 0; zi--) {
        VWindow* win = get_window(desktop.z_order[zi]);
        if (!win || !win->visible || win->minimized) continue;
        if (x >= win->x && x < win->x + win->width &&
            y >= win->y && y < win->y + win->height)
            return win;
    }
    return NULL;
}

static int point_in_title(VWindow* win, int x, int y) {
    if (!win) return 0;
    return (y >= win->y && y < win->y + TITLEBAR_HEIGHT &&
            x >= win->x && x < win->x + win->width);
}

static int point_in_close(VWindow* win, int x, int y) {
    if (!win || !win->has_close) return 0;
    int bx = win->x + win->width - 18;
    int by = win->y + 2;
    return (x >= bx && x < bx + 14 && y >= by && y < by + 14);
}

static int point_in_maximize(VWindow* win, int x, int y) {
    if (!win || !win->has_maximize) return 0;
    int bx = win->x + win->width - (win->has_close ? 34 : 18);
    int by = win->y + 2;
    return (x >= bx && x < bx + 14 && y >= by && y < by + 14);
}

static int point_in_minimize(VWindow* win, int x, int y) {
    if (!win || !win->has_minimize) return 0;
    int bx = win->x + win->width - (win->has_close ? 50 : 34);
    if (!win->has_maximize) bx = win->x + win->width - (win->has_close ? 34 : 18);
    int by = win->y + 2;
    return (x >= bx && x < bx + 14 && y >= by && y < by + 14);
}

#define VWIN_RESIZE_N 1
#define VWIN_RESIZE_S 2
#define VWIN_RESIZE_E 4
#define VWIN_RESIZE_W 8
#define VWIN_MIN_W 200
#define VWIN_MIN_H 140
#define VWIN_RESIZE_GRIP (BORDER_SIZE + 2)

static int vdesk_window_resize_edges(VWindow* win, int x, int y) {
    int edges = 0;
    int grip = VWIN_RESIZE_GRIP;
    if (!win || !win->visible || win->minimized || win->maximized) return 0;
    if (win->id == desktop.primary_shell_id) return 0;
    if (x < win->x || x >= win->x + win->width ||
        y < win->y || y >= win->y + win->height)
        return 0;
    /* Title bar / chrome buttons are not resize grips. */
    if (y < win->y + TITLEBAR_HEIGHT) return 0;
    if (y < win->y + TITLEBAR_HEIGHT + grip) edges |= VWIN_RESIZE_N;
    if (y >= win->y + win->height - grip) edges |= VWIN_RESIZE_S;
    if (x < win->x + grip) edges |= VWIN_RESIZE_W;
    if (x >= win->x + win->width - grip) edges |= VWIN_RESIZE_E;
    return edges;
}

static void vdesk_toggle_maximize(VWindow* win) {
    if (!win) return;
    mark_window_dirty(win);
    if (win->maximized) {
        win->x = win->saved_x;
        win->y = win->saved_y;
        win->width = win->saved_w;
        win->height = win->saved_h;
        win->maximized = 0;
    } else {
        win->saved_x = win->x;
        win->saved_y = win->y;
        win->saved_w = win->width;
        win->saved_h = win->height;
        win->x = 0;
        win->y = vdesk_workspace_top();
        win->width = desktop.screen_w;
        win->height = vdesk_workspace_bottom() - vdesk_workspace_top();
        win->maximized = 1;
    }
    win->drag_active = 0;
    win->resize_active = 0;
    win->resize_edges = 0;
    vdesk_mark_full_dirty();
}

static void vdesk_minimize_window(VWindow* win) {
    if (!win) return;
    if (win->id == desktop.primary_shell_id) {
        vdesk_focus_primary_shell();
        return;
    }
    win->minimized = 1;
    win->drag_active = 0;
    win->focused = 0;
    if (should_auto_focus_primary_shell() && win->id != desktop.primary_shell_id)
        vdesk_focus_primary_shell();
    vdesk_mark_full_dirty();
}

static void vdesk_restore_window(VWindow* win) {
    if (!win) return;
    win->minimized = 0;
    vdesk_bring_to_front(win);
    vdesk_mark_full_dirty();
}

VWindow* vdesk_create_window(const char* title, int x, int y, int w, int h) {
    int idx = -1;
    for (int i = 0; i < MAX_VWINDOWS; i++) {
        if (!desktop.windows[i].visible) { idx = i; break; }
    }
    if (idx < 0) return NULL;

    VWindow* win = &desktop.windows[idx];
    int top = vdesk_workspace_top();
    int bottom = vdesk_workspace_bottom();
    if (y < top) y = top + 2;
    if (h > bottom - top) h = bottom - top;
    if (y + h > bottom) y = bottom - h;
    if (x < 0) x = 0;
    if (w > desktop.screen_w) w = desktop.screen_w;
    if (x + w > desktop.screen_w) x = desktop.screen_w - w;
    win->id = desktop.next_id++;
    win->x = x;
    win->y = y;
    win->width = w;
    win->height = h;
    win->visible = 1;
    win->minimized = 0;
    win->focused = 0;
    win->has_close = 1;
    win->has_maximize = 1;
    win->has_minimize = 1;
    win->maximized = 0;
    win->saved_x = x;
    win->saved_y = y;
    win->saved_w = w;
    win->saved_h = h;
    win->drag_active = 0;
    win->resize_active = 0;
    win->resize_edges = 0;
    win->resize_start_mx = 0;
    win->resize_start_my = 0;
    win->resize_orig_x = x;
    win->resize_orig_y = y;
    win->resize_orig_w = w;
    win->resize_orig_h = h;
    win->title_bg = theme()->title_active_bg;
    win->title_fg = theme()->title_fg;
    win->render = NULL;
    win->key_handler = NULL;
    win->scroll_handler = NULL;
    win->tick_handler = NULL;
    win->click_handler = NULL;
    win->rclick_handler = NULL;
    win->user_data = NULL;
    win->process_pid = -1;

    strncpy(win->title, title, VWINDOW_TITLE_MAX - 1);
    win->title[VWINDOW_TITLE_MAX - 1] = '\0';

    desktop.window_count++;
    vdesk_bring_to_front(win);
    mark_window_dirty(win);
    mark_taskbar_dirty();
    return win;
}

void vdesk_close_window(VWindow* win) {
    if (!win || !win->visible) return;
    /* Primary shell may close via GooberShell `exit` (no title-bar X). */
    if (win->id == desktop.primary_shell_id)
        desktop.primary_shell_id = 0;
    {
        extern void dos_on_window_closed(VWindow* w);
        dos_on_window_closed(win);
    }
    if (win->process_pid > 0) {
        terminate_process(win->process_pid);
        win->process_pid = -1;
    }
    win->visible = 0;
    win->minimized = 0;
    mark_window_dirty(win);
    mark_taskbar_dirty();
    remove_z(win->id);
    desktop.window_count--;
    if (should_auto_focus_primary_shell() && !vdesk_has_active_app_focus())
        vdesk_focus_primary_shell();
}

void vdesk_close_windows_by_pid(int pid) {
    int i;
    if (pid <= 0) return;
    for (i = 0; i < MAX_VWINDOWS; i++) {
        VWindow* win = &desktop.windows[i];
        if (!win->visible) continue;
        if (win->process_pid == pid)
            vdesk_close_window(win);
    }
}

void vdesk_set_app_launcher(void (*launcher)(VDeskAppId app_id)) {
    desktop.launch_app = launcher;
}

void vdesk_add_icon(const char* label, VDeskAppId app_id, int x, int y) {
    if (desktop.icon_count >= MAX_VDESKTOP_ICONS) return;
    VDesktopIcon* icon = &desktop.icons[desktop.icon_count];
    icon->id = desktop.icon_count + 1;
    icon->x = x;
    icon->y = y;
    icon->app_id = app_id;
    icon->selected = 0;
    icon->drag_active = 0;
    icon->drag_off_x = 0;
    icon->drag_off_y = 0;
    icon->kind = VICON_APP;
    icon->filename[0] = '\0';
    strncpy(icon->label, label, sizeof(icon->label) - 1);
    icon->label[sizeof(icon->label) - 1] = '\0';
    desktop.icon_count++;
    /* App launcher icons are always added first, before filesystem items. */
    desktop.app_icon_count = desktop.icon_count;
    vdesk_mark_dirty(x - 2, y - 2, 76, 70);
}

void vdesk_set_file_opener(void (*opener)(const char* name, int kind)) {
    desktop.open_file = opener;
}

void vdesk_set_fs_item_opener(void (*opener)(Directory* dir, const char* name,
                                             int is_dir)) {
    desktop.open_fs_item = opener;
}

static int desktop_name_kind(const char* name, int is_dir) {
    if (is_dir) return VICON_FOLDER;
    if (str_has_suffix_ci(name, ".gbm") || str_has_suffix_ci(name, ".bmp"))
        return VICON_BITMAP;
    if (str_has_suffix_ci(name, ".txt")) return VICON_TEXT;
    if (str_has_suffix_ci(name, ".cfg")) return VICON_TEXT;
    if (str_has_suffix_ci(name, ".gob")) return VICON_GOB;
    if (str_has_suffix_ci(name, ".gc")) return VICON_CODE;
    if (str_has_suffix_ci(name, ".com") || str_has_suffix_ci(name, ".exe"))
        return VICON_DOS;
    return -1; /* not a desktop-visible file type */
}

static uint32_t desktop_name_hash(const char* name, int kind) {
    uint32_t h = 2166136261u ^ (uint32_t)kind;
    for (int i = 0; name[i]; i++) {
        h ^= (uint8_t)name[i];
        h *= 16777619u;
    }
    return h;
}

static void place_file_icon(VDesktopIcon* icon, int slot) {
    /* Grid to the right of both app-launcher columns (x=24 and x=112). */
    int col_w = 80;
    int row_h = 72;
    int start_x = 200;
    int start_y = vdesk_workspace_top() + 24;
    int usable_h = vdesk_workspace_bottom() - start_y - 24;
    int rows = usable_h / row_h;
    if (rows < 1) rows = 1;
    int col = slot / rows;
    int row = slot % rows;
    icon->x = start_x + col * col_w;
    icon->y = start_y + row * row_h;
}

/* True if a filesystem icon sits on top of a built-in app launcher icon. */
static int file_icon_overlaps_apps(int x, int y) {
    int i;
    for (i = 0; i < desktop.app_icon_count; i++) {
        int ax = desktop.icons[i].x;
        int ay = desktop.icons[i].y;
        if (ABS(x - ax) < 72 && ABS(y - ay) < 64)
            return 1;
    }
    return 0;
}

/*
 * Re-scan the current filesystem directory and rebuild the filesystem-backed
 * desktop icons (folders, .txt, .gbm). Existing icon positions/selection are
 * preserved across rescans when the file set is unchanged; the whole desktop is
 * marked dirty only when the set actually changes (so we never thrash the
 * render loop). Pass force=1 to rebuild unconditionally.
 */
void vdesk_set_status(const char* msg) {
    int i;
    if (!msg) msg = "";
    for (i = 0; i < (int)sizeof(desktop.status_msg) - 1 && msg[i]; i++)
        desktop.status_msg[i] = msg[i];
    desktop.status_msg[i] = '\0';
    desktop.status_until_tick = timer_ticks() + 300U; /* ~3 s at 100 Hz */
    vdesk_mark_full_dirty();
}

void vdesk_notify(const char* title, const char* body) {
    int slot = -1;
    int i;
    uint32_t now = timer_ticks();
    if (!title) title = "GooberOS";
    if (!body) body = "";

    /* Prefer empty slot; else replace oldest (lowest expire). */
    for (i = 0; i < VDESK_TOAST_MAX; i++) {
        if (!desktop.toasts[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        uint32_t oldest = 0xFFFFFFFFu;
        slot = 0;
        for (i = 0; i < VDESK_TOAST_MAX; i++) {
            if (desktop.toasts[i].expire_tick < oldest) {
                oldest = desktop.toasts[i].expire_tick;
                slot = i;
            }
        }
    }

    desktop.toasts[slot].used = 1;
    strncpy(desktop.toasts[slot].title, title, VDESK_TOAST_TITLE_MAX - 1);
    desktop.toasts[slot].title[VDESK_TOAST_TITLE_MAX - 1] = '\0';
    strncpy(desktop.toasts[slot].body, body, VDESK_TOAST_BODY_MAX - 1);
    desktop.toasts[slot].body[VDESK_TOAST_BODY_MAX - 1] = '\0';
    desktop.toasts[slot].expire_tick = now + 700U; /* ~7 s at 100 Hz */
    vdesk_mark_full_dirty();
}

void vdesk_refresh_desktop_items(int force) {
    Directory* dir = fs_get_desktop_dir();
    if (!dir) return;

    /* Refresh only the Desktop directory (not the root twice). */
    if (dir->fat32)
        fs_dir_refresh(dir);

    uint32_t sig = (uint32_t)(dir->child_count * 131 + dir->file_count * 7);
    for (size_t i = 0; i < dir->child_count; i++)
        sig ^= desktop_name_hash(dir->children[i].name, VICON_FOLDER);
    for (size_t i = 0; i < dir->file_count; i++) {
        int k = desktop_name_kind(dir->files[i].name, 0);
        if (k >= 0) sig ^= desktop_name_hash(dir->files[i].name, k);
    }

    if (!force && sig == desktop.fs_signature) return;
    desktop.fs_signature = sig;

    /* Snapshot existing positions so survivors keep their spot. */
    VDesktopIcon old[MAX_VDESKTOP_ICONS];
    int old_count = 0;
    for (int i = desktop.app_icon_count; i < desktop.icon_count; i++)
        old[old_count++] = desktop.icons[i];

    desktop.icon_count = desktop.app_icon_count;
    int slot = 0;

    for (size_t i = 0; i < dir->child_count && desktop.icon_count < MAX_VDESKTOP_ICONS; i++) {
        VDesktopIcon* icon = &desktop.icons[desktop.icon_count];
        icon->id = desktop.icon_count + 1;
        icon->kind = VICON_FOLDER;
        icon->app_id = VDESK_APP_EXPLORER;
        icon->selected = 0;
        icon->drag_active = 0;
        strncpy(icon->filename, dir->children[i].name, VICON_NAME_MAX - 1);
        icon->filename[VICON_NAME_MAX - 1] = '\0';
        strncpy(icon->label, dir->children[i].name, sizeof(icon->label) - 1);
        icon->label[sizeof(icon->label) - 1] = '\0';
        int prev = -1;
        for (int o = 0; o < old_count; o++) {
            if (old[o].kind == VICON_FOLDER && strcmp(old[o].filename, icon->filename) == 0) {
                prev = o; break;
            }
        }
        if (prev >= 0 && !file_icon_overlaps_apps(old[prev].x, old[prev].y)) {
            icon->x = old[prev].x;
            icon->y = old[prev].y;
        } else {
            place_file_icon(icon, slot);
        }
        slot++;
        desktop.icon_count++;
    }

    for (size_t i = 0; i < dir->file_count && desktop.icon_count < MAX_VDESKTOP_ICONS; i++) {
        int kind = desktop_name_kind(dir->files[i].name, 0);
        if (kind < 0) continue;
        VDesktopIcon* icon = &desktop.icons[desktop.icon_count];
        icon->id = desktop.icon_count + 1;
        icon->kind = (VIconKind)kind;
        if (kind == VICON_TEXT) icon->app_id = VDESK_APP_EDITOR;
        else if (kind == VICON_CODE) icon->app_id = VDESK_APP_IDE;
        else if (kind == VICON_GOB) icon->app_id = VDESK_APP_WELCOME; /* runnable */
        else icon->app_id = VDESK_APP_PAINT;
        icon->selected = 0;
        icon->drag_active = 0;
        strncpy(icon->filename, dir->files[i].name, VICON_NAME_MAX - 1);
        icon->filename[VICON_NAME_MAX - 1] = '\0';
        strncpy(icon->label, dir->files[i].name, sizeof(icon->label) - 1);
        icon->label[sizeof(icon->label) - 1] = '\0';
        int prev = -1;
        for (int o = 0; o < old_count; o++) {
            if (old[o].kind == (VIconKind)kind && strcmp(old[o].filename, icon->filename) == 0) {
                prev = o; break;
            }
        }
        if (prev >= 0 && !file_icon_overlaps_apps(old[prev].x, old[prev].y)) {
            icon->x = old[prev].x;
            icon->y = old[prev].y;
        } else {
            place_file_icon(icon, slot);
        }
        slot++;
        desktop.icon_count++;
    }

    vdesk_mark_full_dirty();
}

void vdesk_toggle_theme(void) {
    desktop.appearance = (desktop.appearance + 1) % VDESK_APPEARANCE_COUNT;
    desktop.theme_mode = (desktop.appearance == VDESK_APPEARANCE_LIGHT);
    desktop.metrics.theme_mode = desktop.theme_mode;
    desktop.metrics.appearance = desktop.appearance;
    vdesk_mark_full_dirty();
    vdesk_prefs_persist();
}

void vdesk_set_appearance(int appearance) {
    if (appearance < 0 || appearance >= VDESK_APPEARANCE_COUNT)
        appearance = VDESK_APPEARANCE_MODERN_DARK;
    desktop.appearance = appearance;
    desktop.theme_mode = (appearance == VDESK_APPEARANCE_LIGHT);
    desktop.metrics.theme_mode = desktop.theme_mode;
    desktop.metrics.appearance = desktop.appearance;
    vdesk_mark_full_dirty();
}

int vdesk_get_appearance(void) {
    return desktop.appearance;
}

void vdesk_set_shift_click_rmb(int enabled) {
    desktop.shift_click_rmb = enabled ? 1 : 0;
}

int vdesk_shift_click_rmb_enabled(void) {
    return desktop.shift_click_rmb ? 1 : 0;
}

/* Weak default; desktop_vesa overrides to write Config/settings.cfg. */
void __attribute__((weak)) vdesk_prefs_persist(void) { }

const VTheme* vdesk_get_theme(void) {
    return theme();
}

const VDeskMetrics* vdesk_get_metrics(void) {
    return &desktop.metrics;
}

const char* vdesk_get_theme_name(void) {
    if (desktop.appearance == VDESK_APPEARANCE_LIGHT) return "Light";
    if (desktop.appearance == VDESK_APPEARANCE_MODERN_DARK) return "Modern Dark";
    return "Original";
}

static color_t shell_output_palette(int idx) {
    static const color_t colors[] = {
        0, 0x67B878, 0x8FD694, 0x88D8C0, 0xD6C76D, 0xC7D1D9
    };
    int count = (int)(sizeof(colors) / sizeof(colors[0]));
    if (idx <= 0 || idx >= count) return theme()->shell_output;
    return colors[idx];
}

static color_t shell_input_palette(int idx) {
    static const color_t colors[] = {
        0, 0x5B7FDB, 0x7FA6FF, 0x75D2FF, 0xB29DFF, 0xD7E8FF
    };
    int count = (int)(sizeof(colors) / sizeof(colors[0]));
    if (idx <= 0 || idx >= count) return theme()->shell_input;
    return colors[idx];
}

color_t vdesk_shell_bg_color(void) { return theme()->shell_bg; }
color_t vdesk_shell_output_color(void) { return shell_output_palette(shell_output_color_index); }
color_t vdesk_shell_input_color(void) { return shell_input_palette(shell_input_color_index); }
color_t vdesk_shell_muted_color(void) { return theme()->shell_muted; }

void vdesk_cycle_shell_output_color(void) {
    shell_output_color_index = (shell_output_color_index + 1) % 6;
    vdesk_mark_full_dirty();
}

void vdesk_cycle_shell_input_color(void) {
    shell_input_color_index = (shell_input_color_index + 1) % 6;
    vdesk_mark_full_dirty();
}

void vdesk_set_desktop_experience(int show_desktop) {
    VWindow* shell = get_window(desktop.primary_shell_id);
    desktop.desktop_experience_visible = show_desktop ? 1 : 0;
    clear_icon_selection();
    if (desktop.desktop_experience_visible) {
        if (shell) {
            shell->minimized = 1;
            shell->focused = 0;
        }
    } else if (shell) {
        shell->minimized = 0;
        vdesk_focus_primary_shell();
    }
    vdesk_mark_full_dirty();
}

void vdesk_toggle_desktop_experience(void) {
    vdesk_set_desktop_experience(desktop.desktop_experience_visible ? 0 : 1);
}

int vdesk_desktop_experience_visible(void) {
    return desktop.desktop_experience_visible;
}

/* ---- Render functions ---- */

static void render_titlebar(VWindow* win) {
    int bx = win->x, by = win->y, bw = win->width;
    const VTheme* t = theme();
    int button_space = 6;
    if (win->has_close) button_space += 16;
    if (win->has_maximize) button_space += 16;
    if (win->has_minimize) button_space += 16;

    vdesk_draw_rect(bx, by, bw, TITLEBAR_HEIGHT,
                    win->focused ? t->title_active_bg : t->title_inactive_bg);

    int tx = bx + 4;
    int ty = by + 3;
    int text_w = bw - button_space;
    if (text_w > 0) {
        char display[64];
        int len = 0;
        for (int i = 0; win->title[i] && i < text_w / 8 && i < 63; i++)
            display[len++] = win->title[i];
        display[len] = '\0';
        vdesk_draw_text(tx, ty, display,
                        win->focused ? t->title_fg : t->text_muted,
                        win->focused ? t->title_active_bg : t->title_inactive_bg);
    }

    if (win->has_minimize) {
        int mx = bx + bw - (win->has_close ? 50 : 34);
        if (!win->has_maximize) mx = bx + bw - (win->has_close ? 34 : 18);
        int my = by + 3;
        vdesk_draw_rect(mx, my, 14, 14, t->button_bg);
        vdesk_draw_border(mx, my, 14, 14, t->border_light, t->border_dark);
        vdesk_draw_text(mx + 4, my + 2, "_", t->text, t->button_bg);
    }

    if (win->has_maximize) {
        int mx = bx + bw - (win->has_close ? 34 : 18);
        int my = by + 3;
        vdesk_draw_rect(mx, my, 14, 14, t->button_bg);
        vdesk_draw_border(mx, my, 14, 14, t->border_light, t->border_dark);
        vdesk_draw_text(mx + 4, my + 2, win->maximized ? "R" : "O", t->text, t->button_bg);
    }

    if (win->has_close) {
        int clx = bx + bw - 18, cly = by + 3;
        vdesk_draw_rect(clx, cly, 14, 14, t->button_bg);
        vdesk_draw_border(clx, cly, 14, 14, t->border_light, t->border_dark);
        vdesk_draw_text(clx + 4, cly + 2, "X", t->text, t->button_bg);
    }
}

static void render_window_border(VWindow* win) {
    int x = win->x, y = win->y, w = win->width, h = win->height;
    const VTheme* t = theme();

    vdesk_draw_rect(x + 4, y + h, w, 3, t->shadow);
    vdesk_draw_rect(x + w, y + 4, 3, h, t->shadow);
    vdesk_draw_border(x, y, w, h, t->border_outer, t->border_outer);
    vdesk_draw_border(x + 1, y + 1, w - 2, h - 2,
                      win->focused ? t->accent : t->border_light,
                      t->border_dark);
    vdesk_draw_border(x + 2, y + 2, w - 4, h - 4,
                      t->border_light, t->border_dark);
    vdesk_draw_rect(x + BORDER_SIZE - 1, y + TITLEBAR_HEIGHT - 1,
                    w - (BORDER_SIZE - 1) * 2, 1, t->border_dark);
    draw_soft_corners(x, y, w, h, t->desktop_bg);
}

static void render_client_area(VWindow* win) {
    int cx = win->x + BORDER_SIZE;
    int cy = win->y + TITLEBAR_HEIGHT + BORDER_SIZE;
    int cw = win->width - BORDER_SIZE * 2;
    int ch = win->height - TITLEBAR_HEIGHT - BORDER_SIZE * 2;

    if (cw <= 0 || ch <= 0) return;

    vdesk_draw_rect(cx, cy, cw, ch, theme()->client_bg);

    if (win->render) {
        win->render(win, cx, cy, cw, ch);
    }
}

static void render_window(VWindow* win) {
    if (!win || !win->visible || win->minimized) return;
    render_titlebar(win);
    render_window_border(win);
    render_client_area(win);
}

static void render_desktop(void) {
    vdesk_draw_rect(0, 0, desktop.screen_w,
                    desktop.screen_h, theme()->desktop_bg);
}

/* ---- Taskbar window buttons (Windows-95 style) ---- */

#define TASK_BTN_START 66
#define TASK_BTN_W 132
#define TASK_BTN_GAP 4
#define TASK_TRAY_W 128
#define TOAST_W 220
#define TOAST_H 56
#define TOAST_GAP 6

static int taskbar_tray_left(void) {
    int left = desktop.screen_w - TASK_TRAY_W;
    if (left < TASK_BTN_START + 40) left = TASK_BTN_START + 40;
    return left;
}

static int taskbar_button_rect(int slot, int* bx, int* bw) {
    int x = TASK_BTN_START + slot * (TASK_BTN_W + TASK_BTN_GAP);
    int limit = taskbar_tray_left() - 8;
    if (x + TASK_BTN_W > limit) return 0;
    *bx = x;
    *bw = TASK_BTN_W;
    return 1;
}

static void toast_rect(int index, int* ox, int* oy, int* ow, int* oh) {
    int top = (desktop.taskbar_position == VDESK_TASKBAR_TOP) ?
              TASKBAR_HEIGHT + 6 : 6;
    *ow = TOAST_W;
    *oh = TOAST_H;
    *ox = desktop.screen_w - TOAST_W - 8;
    *oy = top + index * (TOAST_H + TOAST_GAP);
}

static void render_toasts(void) {
    const VTheme* t = theme();
    uint32_t now = timer_ticks();
    int visible = 0;
    int i;

    for (i = 0; i < VDESK_TOAST_MAX; i++) {
        int x, y, w, h;
        if (!desktop.toasts[i].used) continue;
        if ((int32_t)(now - desktop.toasts[i].expire_tick) >= 0) {
            desktop.toasts[i].used = 0;
            desktop.toasts[i].title[0] = '\0';
            continue;
        }
        toast_rect(visible, &x, &y, &w, &h);
        visible++;
        vdesk_draw_rect(x, y, w, h, t->menu_bg);
        vdesk_draw_border(x, y, w, h, t->border_light, t->border_dark);
        vdesk_draw_rect(x + 2, y + 2, 4, h - 4, t->accent);
        vdesk_draw_text(x + 12, y + 8, desktop.toasts[i].title, t->text, t->menu_bg);
        vdesk_draw_text(x + 12, y + 28, desktop.toasts[i].body, t->text_muted, t->menu_bg);
        /* Close affordance */
        vdesk_draw_rect(x + w - 22, y + 6, 16, 16, t->button_bg);
        vdesk_draw_text(x + w - 18, y + 8, "X", t->text, t->button_bg);
    }
}

/* Returns 1 if click was consumed by a toast (X dismiss). */
static int toast_click(int mx, int my) {
    uint32_t now = timer_ticks();
    int visible = 0;
    int i;

    for (i = 0; i < VDESK_TOAST_MAX; i++) {
        int x, y, w, h;
        if (!desktop.toasts[i].used) continue;
        if ((int32_t)(now - desktop.toasts[i].expire_tick) >= 0) {
            desktop.toasts[i].used = 0;
            continue;
        }
        toast_rect(visible, &x, &y, &w, &h);
        visible++;
        if (mx >= x && mx < x + w && my >= y && my < y + h) {
            if (mx >= x + w - 22 && mx < x + w - 6 && my >= y + 6 && my < y + 22) {
                desktop.toasts[i].used = 0;
                vdesk_mark_full_dirty();
                return 1;
            }
            return 1; /* consume click on toast body */
        }
    }
    return 0;
}

static void render_taskbar(void) {
    int ty = (desktop.taskbar_position == VDESK_TASKBAR_TOP) ?
             0 : desktop.screen_h - TASKBAR_HEIGHT;
    const VTheme* t = theme();
    int tray_x = taskbar_tray_left();
    char clock_buf[20];
    int stamp;

    softclock_tick();
    stamp = softclock_minute_stamp();
    if (stamp != desktop.clock_tray_stamp) {
        desktop.clock_tray_stamp = stamp;
        vdesk_mark_dirty(tray_x - 4, ty, desktop.screen_w - tray_x + 4, TASKBAR_HEIGHT);
    }

    vdesk_draw_rect(0, ty, desktop.screen_w, TASKBAR_HEIGHT, t->taskbar_bg);
    vdesk_draw_rect(0, ty, desktop.screen_w, 2, t->taskbar_top);

    int start_w = 60;
    vdesk_draw_rect(2, ty + 2, start_w, TASKBAR_HEIGHT - 4, t->button_bg);
    vdesk_draw_border(2, ty + 2, start_w, TASKBAR_HEIGHT - 4,
                      t->border_light, t->border_dark);
    vdesk_draw_rect(3, ty + 3, start_w - 2, TASKBAR_HEIGHT - 6, t->button_bg);
    draw_soft_corners(2, ty + 2, start_w, TASKBAR_HEIGHT - 4, t->taskbar_bg);

    vdesk_draw_text(8, ty + 6, "Start", t->text, t->button_bg);

    int slot = 0;
    for (int i = 0; i < MAX_VWINDOWS; i++) {
        VWindow* w = &desktop.windows[i];
        if (!w->visible) continue;
        int bx, bw;
        if (!taskbar_button_rect(slot, &bx, &bw)) break;
        int by = ty + 3;
        int bh = TASKBAR_HEIGHT - 6;
        int active = w->focused && !w->minimized;
        color_t bg = active ? t->menu_accent : t->button_bg;
        vdesk_draw_rect(bx, by, bw, bh, bg);
        vdesk_draw_border(bx, by, bw, bh,
                          active ? t->border_dark : t->border_light,
                          active ? t->border_light : t->border_dark);
        draw_soft_corners(bx, by, bw, bh, t->taskbar_bg);

        char label[18];
        int li = 0;
        for (int c = 0; w->title[c] && li < (int)sizeof(label) - 1 && li < (bw - 12) / 8; c++)
            label[li++] = w->title[c];
        label[li] = '\0';
        vdesk_draw_text(bx + 6, by + 5, label,
                        active ? VCOLOR_WHITE : t->text, bg);
        slot++;
    }

    /* Separator + clock tray (right). */
    vdesk_draw_rect(tray_x, ty + 4, 2, TASKBAR_HEIGHT - 8, t->border_dark);
    softclock_format_short(clock_buf, (int)sizeof(clock_buf));
    vdesk_draw_text(tray_x + 10, ty + 6, clock_buf, t->text, t->taskbar_bg);

    if (desktop.status_msg[0] &&
        (int32_t)(timer_ticks() - desktop.status_until_tick) < 0) {
        int sx = desktop.screen_w / 2 - 40;
        if (sx > tray_x - 8) sx = tray_x - 100;
        if (sx < TASK_BTN_START) sx = TASK_BTN_START;
        vdesk_draw_text(sx, ty + 6, desktop.status_msg, t->accent, t->taskbar_bg);
    } else if (desktop.status_msg[0]) {
        desktop.status_msg[0] = '\0';
    }
}

static int taskbar_button_click(int mx, int my) {
    int ty = (desktop.taskbar_position == VDESK_TASKBAR_TOP) ?
             0 : desktop.screen_h - TASKBAR_HEIGHT;
    if (my < ty + 3 || my >= ty + TASKBAR_HEIGHT - 3) return 0;
    if (mx < TASK_BTN_START) return 0;

    int slot = 0;
    for (int i = 0; i < MAX_VWINDOWS; i++) {
        VWindow* w = &desktop.windows[i];
        if (!w->visible) continue;
        int bx, bw;
        if (!taskbar_button_rect(slot, &bx, &bw)) break;
        if (mx >= bx && mx < bx + bw) {
            if (w->minimized) {
                vdesk_restore_window(w);
            } else if (w->focused) {
                vdesk_minimize_window(w);
                if (should_auto_focus_primary_shell() && !vdesk_has_active_app_focus())
                    vdesk_focus_primary_shell();
            } else {
                vdesk_bring_to_front(w);
                vdesk_mark_full_dirty();
            }
            return 1;
        }
        slot++;
    }
    return 0;
}

static VWindow* taskbar_window_at(int mx, int my) {
    int ty = (desktop.taskbar_position == VDESK_TASKBAR_TOP) ?
             0 : desktop.screen_h - TASKBAR_HEIGHT;
    if (my < ty + 3 || my >= ty + TASKBAR_HEIGHT - 3) return NULL;
    if (mx < TASK_BTN_START) return NULL;

    int slot = 0;
    for (int i = 0; i < MAX_VWINDOWS; i++) {
        VWindow* w = &desktop.windows[i];
        if (!w->visible) continue;
        int bx, bw;
        if (!taskbar_button_rect(slot, &bx, &bw)) break;
        if (mx >= bx && mx < bx + bw) return w;
        slot++;
    }
    return NULL;
}

/* Start menu: System Tools / Games / GooberDOS open right flyouts. */
static int start_menu_n_items(void) {
    return fs_is_persistent() ? 10 : 11;
}

static int start_menu_tools_item(void) {
    return 1; /* after Shell */
}

static int start_menu_games_item(void) {
    return fs_is_persistent() ? 7 : 8;
}

static int start_menu_dos_item(void) {
    return fs_is_persistent() ? 8 : 9;
}

static int start_menu_toggle_item(void) {
    return fs_is_persistent() ? 9 : 10;
}

static void start_menu_geometry(int* out_mx, int* out_my, int* out_w, int* out_h) {
    int n_items = start_menu_n_items();
    int menu_w = 168;
    int menu_h = 24 + n_items * 22 + 8;
    int mx = 2;
    int my = (desktop.taskbar_position == VDESK_TASKBAR_TOP) ?
             TASKBAR_HEIGHT : desktop.screen_h - TASKBAR_HEIGHT - menu_h;
    if (out_mx) *out_mx = mx;
    if (out_my) *out_my = my;
    if (out_w) *out_w = menu_w;
    if (out_h) *out_h = menu_h;
}

static void start_flyout_geometry(int row, int n_sub, int fw,
                                  int* out_fx, int* out_fy, int* out_fw, int* out_fh) {
    int mx, my, mw, mh;
    start_menu_geometry(&mx, &my, &mw, &mh);
    if (out_fx) *out_fx = mx + mw - 2;
    if (out_fy) *out_fy = my + 24 + row * 22;
    if (out_fw) *out_fw = fw;
    if (out_fh) *out_fh = 8 + n_sub * 22;
}

static void start_dos_flyout_geometry(int* out_fx, int* out_fy, int* out_fw, int* out_fh) {
    start_flyout_geometry(start_menu_dos_item(), 2, 120, out_fx, out_fy, out_fw, out_fh);
}

static void start_games_flyout_geometry(int* out_fx, int* out_fy, int* out_fw, int* out_fh) {
    start_flyout_geometry(start_menu_games_item(), 4, 140, out_fx, out_fy, out_fw, out_fh);
}

static void start_tools_flyout_geometry(int* out_fx, int* out_fy, int* out_fw, int* out_fh) {
    start_flyout_geometry(start_menu_tools_item(), 3, 140, out_fx, out_fy, out_fw, out_fh);
}

/* Main index; 100+ DOS; 110+ Games; 120+ System Tools. */
static int start_menu_pick_at(int x, int y) {
    int mx, my, mw, mh, i;
    if (!desktop.start_open) return -1;
    start_menu_geometry(&mx, &my, &mw, &mh);

    if (start_dos_flyout) {
        int fx, fy, fw, fh;
        start_dos_flyout_geometry(&fx, &fy, &fw, &fh);
        if (x >= fx && x < fx + fw && y >= fy && y < fy + fh) {
            for (i = 0; i < 2; i++) {
                int iy = fy + 4 + i * 22;
                if (y >= iy && y < iy + 20) return 100 + i;
            }
            return -2;
        }
    }
    if (start_games_flyout) {
        int fx, fy, fw, fh;
        start_games_flyout_geometry(&fx, &fy, &fw, &fh);
        if (x >= fx && x < fx + fw && y >= fy && y < fy + fh) {
            for (i = 0; i < 4; i++) {
                int iy = fy + 4 + i * 22;
                if (y >= iy && y < iy + 20) return 110 + i;
            }
            return -2;
        }
    }
    if (start_tools_flyout) {
        int fx, fy, fw, fh;
        start_tools_flyout_geometry(&fx, &fy, &fw, &fh);
        if (x >= fx && x < fx + fw && y >= fy && y < fy + fh) {
            for (i = 0; i < 3; i++) {
                int iy = fy + 4 + i * 22;
                if (y >= iy && y < iy + 20) return 120 + i;
            }
            return -2;
        }
    }

    if (x < mx || x >= mx + mw || y < my || y >= my + mh)
        return -1;
    for (i = 0; i < start_menu_n_items(); i++) {
        int iy = my + 24 + i * 22;
        if (y >= iy && y < iy + 20) return i;
    }
    return -2;
}

static void render_start_menu(void) {
    if (!desktop.start_open) return;
    const VTheme* t = theme();
    int live = !fs_is_persistent();
    int n_items = start_menu_n_items();
    int mx, my, menu_w, menu_h, i;
    int dos_item = start_menu_dos_item();
    int games_item = start_menu_games_item();
    int tools_item = start_menu_tools_item();
    start_menu_geometry(&mx, &my, &menu_w, &menu_h);

    vdesk_draw_rect(mx, my, menu_w, menu_h, t->menu_bg);
    vdesk_draw_border(mx, my, menu_w, menu_h, t->border_light, t->border_outer);
    vdesk_draw_rect(mx + 2, my + 2, menu_w - 4, 18, t->menu_accent);
    vdesk_draw_text(mx + 4, my + 4, "GooberOS", VCOLOR_WHITE, t->menu_accent);

    const char* items_persist[] = {
        "Shell", "System Tools  >", "Text Editor",
        "System Settings", "Display Settings", "Paint",
        "GooberC IDE", "Games  >", "GooberDOS  >",
        desktop.desktop_experience_visible ? "Hide Desktop" : "Show Desktop"
    };
    const char* items_live[] = {
        "Shell", "System Tools  >", "Text Editor",
        "System Settings", "Display Settings", "Paint",
        "GooberC IDE", "Install GooberOS", "Games  >", "GooberDOS  >",
        desktop.desktop_experience_visible ? "Hide Desktop" : "Show Desktop"
    };
    const char** items = live ? items_live : items_persist;
    for (i = 0; i < n_items; i++) {
        int iy = my + 24 + i * 22;
        int hi = (start_dos_flyout && i == dos_item) ||
                 (start_games_flyout && i == games_item) ||
                 (start_tools_flyout && i == tools_item);
        uint32_t bg = hi ? t->menu_accent : t->menu_bg;
        uint32_t fg = hi ? VCOLOR_WHITE : t->menu_fg;
        vdesk_draw_rect(mx + 2, iy, menu_w - 4, 20, bg);
        vdesk_draw_text(mx + 8, iy + 4, items[i], fg, bg);
    }

    if (start_tools_flyout) {
        int fx, fy, fw, fh;
        const char* sub[] = { "File Explorer", "System Info", "Task Manager" };
        start_tools_flyout_geometry(&fx, &fy, &fw, &fh);
        vdesk_draw_rect(fx, fy, fw, fh, t->menu_bg);
        vdesk_draw_border(fx, fy, fw, fh, t->border_light, t->border_outer);
        for (i = 0; i < 3; i++) {
            int iy = fy + 4 + i * 22;
            vdesk_draw_rect(fx + 2, iy, fw - 4, 20, t->menu_bg);
            vdesk_draw_text(fx + 8, iy + 4, sub[i], t->menu_fg, t->menu_bg);
        }
    }
    if (start_dos_flyout) {
        int fx, fy, fw, fh;
        const char* sub[] = { "Shell...", "Doom" };
        start_dos_flyout_geometry(&fx, &fy, &fw, &fh);
        vdesk_draw_rect(fx, fy, fw, fh, t->menu_bg);
        vdesk_draw_border(fx, fy, fw, fh, t->border_light, t->border_outer);
        for (i = 0; i < 2; i++) {
            int iy = fy + 4 + i * 22;
            vdesk_draw_rect(fx + 2, iy, fw - 4, 20, t->menu_bg);
            vdesk_draw_text(fx + 8, iy + 4, sub[i], t->menu_fg, t->menu_bg);
        }
    }
    if (start_games_flyout) {
        int fx, fy, fw, fh;
        const char* sub[] = { "Minesweeper", "CubeDip", "SnakeGame", "DoomRay" };
        start_games_flyout_geometry(&fx, &fy, &fw, &fh);
        vdesk_draw_rect(fx, fy, fw, fh, t->menu_bg);
        vdesk_draw_border(fx, fy, fw, fh, t->border_light, t->border_outer);
        for (i = 0; i < 4; i++) {
            int iy = fy + 4 + i * 22;
            vdesk_draw_rect(fx + 2, iy, fw - 4, 20, t->menu_bg);
            vdesk_draw_text(fx + 8, iy + 4, sub[i], t->menu_fg, t->menu_bg);
        }
    }
}

static int start_menu_hit(int x, int y) {
    int ty = (desktop.taskbar_position == VDESK_TASKBAR_TOP) ?
             0 : desktop.screen_h - TASKBAR_HEIGHT;
    return (y >= ty && y < ty + TASKBAR_HEIGHT && x >= 2 && x < 62);
}

static int start_menu_item_at(int x, int y) {
    int pick = start_menu_pick_at(x, y);
    if (pick < 0) return -1;
    return pick;
}

static VDeskAppId start_item_to_app(int item) {
    int live = !fs_is_persistent();
    if (item == 100) return VDESK_APP_DOS;
    if (item == 101) return VDESK_APP_DOOM;
    if (item == 110) return VDESK_APP_MINESWEEPER;
    if (item == 111) return VDESK_APP_CUBEDIP;
    if (item == 112) return VDESK_APP_SNAKEGAME;
    if (item == 113) return VDESK_APP_DOOMRAY;
    if (item == 120) return VDESK_APP_EXPLORER;
    if (item == 121) return VDESK_APP_SYSINFO;
    if (item == 122) return VDESK_APP_TASK_MANAGER;
    switch (item) {
        case 0: return VDESK_APP_SHELL;
        case 1: return VDESK_APP_SHELL; /* System Tools flyout */
        case 2: return VDESK_APP_EDITOR;
        case 3: return VDESK_APP_SYSTEM_SETTINGS;
        case 4: return VDESK_APP_DISPLAY_SETTINGS;
        case 5: return VDESK_APP_PAINT;
        case 6: return VDESK_APP_IDE;
        case 7: return live ? VDESK_APP_INSTALLER : VDESK_APP_SHELL; /* Games */
        case 8: return VDESK_APP_SHELL; /* Games/DOS */
        case 9: return VDESK_APP_SHELL; /* DOS/toggle */
        default: return VDESK_APP_SHELL;
    }
}

static void launch_app(VDeskAppId app_id) {
    if (desktop.launch_app) {
        desktop.launch_app(app_id);
        vdesk_mark_full_dirty();
    }
}

static void activate_icon(int idx) {
    if (idx < 0 || idx >= desktop.icon_count) return;
    VDesktopIcon* icon = &desktop.icons[idx];
    if (icon->kind == VICON_APP) {
        launch_app(icon->app_id);
        return;
    }
    if (desktop.open_file) {
        desktop.open_file(icon->filename, (int)icon->kind);
        vdesk_mark_full_dirty();
    }
}

static int icon_at(int x, int y) {
    if (!desktop.desktop_experience_visible) return -1;
    for (int i = desktop.icon_count - 1; i >= 0; i--) {
        VDesktopIcon* icon = &desktop.icons[i];
        if (x >= icon->x && x < icon->x + 64 &&
            y >= icon->y && y < icon->y + 58) {
            return i;
        }
    }
    return -1;
}

static void clear_icon_selection(void) {
    for (int i = 0; i < desktop.icon_count; i++)
        desktop.icons[i].selected = 0;
}

/*
 * Stylized desktop glyphs inspired by gui/exampleicons/ — drawn with rects
 * (AI PNGs are reference only; these are crisp Win9x-style recreations).
 */
static void glyph_px(int x, int y, color_t c) {
    vdesk_draw_rect(x, y, 1, 1, c);
}

/* Tiny 3x5 glyphs for ".GOB" badge (bits 2..0, MSB = left). */
static void glyph_draw_tiny_char(int x, int y, char ch, color_t fg) {
    static const uint8_t font_dot[5] = { 0, 0, 0, 0, 0x2 };
    static const uint8_t font_G[5]   = { 0x6, 0x4, 0x5, 0x5, 0x6 };
    static const uint8_t font_O[5]   = { 0x6, 0x5, 0x5, 0x5, 0x6 };
    static const uint8_t font_B[5]   = { 0x7, 0x5, 0x6, 0x5, 0x7 };
    const uint8_t* rows = 0;
    int r, c;
    if (ch == '.') rows = font_dot;
    else if (ch == 'G' || ch == 'g') rows = font_G;
    else if (ch == 'O' || ch == 'o') rows = font_O;
    else if (ch == 'B' || ch == 'b') rows = font_B;
    if (!rows) return;
    for (r = 0; r < 5; r++) {
        for (c = 0; c < 3; c++) {
            if (rows[r] & (0x4 >> c))
                glyph_px(x + c, y + r, fg);
        }
    }
}

/* Mini window + .GOB badge (exampleappicon.png). */
static void glyph_gob_app(int x, int y) {
    const color_t frame_hi = 0xE0E0E0;
    const color_t frame_mid = 0xC0C0C0;
    const color_t frame_lo = 0x808080;
    const color_t title = 0x0080FF;
    const color_t badge = 0x003090;
    const color_t badge_edge = 0x60A0FF;
    int wx = x + 2;
    int wy = y + 1;
    int ww = 22;
    int wh = 24;

    /* Raised Win9x frame */
    vdesk_draw_rect(wx, wy, ww, wh, frame_mid);
    vdesk_draw_border(wx, wy, ww, wh, frame_hi, frame_lo);
    vdesk_draw_rect(wx + 1, wy + 1, ww - 2, 1, frame_hi);
    vdesk_draw_rect(wx + 1, wy + 1, 1, wh - 2, frame_hi);
    /* Title bar + chrome buttons */
    vdesk_draw_rect(wx + 3, wy + 3, ww - 6, 5, title);
    vdesk_draw_rect(wx + ww - 12, wy + 4, 2, 3, frame_hi);
    vdesk_draw_rect(wx + ww - 9, wy + 4, 2, 3, frame_hi);
    vdesk_draw_rect(wx + ww - 6, wy + 4, 2, 3, frame_hi);
    /* Client */
    vdesk_draw_rect(wx + 3, wy + 9, ww - 6, wh - 12, VCOLOR_WHITE);
    vdesk_draw_rect(wx + 5, wy + 12, 12, 1, 0xD8D8D8);
    vdesk_draw_rect(wx + 5, wy + 15, 10, 1, 0xD8D8D8);
    vdesk_draw_rect(wx + 5, wy + 18, 8, 1, 0xD8D8D8);
    /* .GOB badge overlapping bottom-right */
    vdesk_draw_rect(wx + 8, wy + wh - 8, 16, 9, badge);
    vdesk_draw_border(wx + 8, wy + wh - 8, 16, 9, badge_edge, 0x001858);
    glyph_draw_tiny_char(wx + 10, wy + wh - 6, '.', VCOLOR_WHITE);
    glyph_draw_tiny_char(wx + 13, wy + wh - 6, 'G', VCOLOR_WHITE);
    glyph_draw_tiny_char(wx + 17, wy + wh - 6, 'O', VCOLOR_WHITE);
    glyph_draw_tiny_char(wx + 21, wy + wh - 6, 'B', VCOLOR_WHITE);
}

/* Download arrow into tray (installer icon.png). */
static void glyph_install_arrow(int x, int y) {
    const color_t hi = 0xB8E8FF;
    const color_t mid = 0x2080FF;
    const color_t deep = 0x003090;
    const color_t edge = 0x001860;
    int ax = x + 4;
    int ay = y + 1;

    /* Shaft */
    vdesk_draw_rect(ax + 8, ay + 1, 8, 12, mid);
    vdesk_draw_rect(ax + 8, ay + 1, 2, 12, hi);
    vdesk_draw_rect(ax + 14, ay + 1, 2, 12, deep);
    vdesk_draw_rect(ax + 8, ay + 1, 8, 1, hi);
    /* Arrow head (stepped triangle pointing down) */
    vdesk_draw_rect(ax + 2, ay + 12, 20, 3, mid);
    vdesk_draw_rect(ax + 2, ay + 12, 20, 1, hi);
    vdesk_draw_rect(ax + 5, ay + 15, 14, 3, mid);
    vdesk_draw_rect(ax + 5, ay + 15, 14, 1, hi);
    vdesk_draw_rect(ax + 8, ay + 18, 8, 3, mid);
    vdesk_draw_rect(ax + 8, ay + 18, 8, 1, hi);
    vdesk_draw_rect(ax + 11, ay + 21, 2, 2, mid);
    /* Dark outline accents */
    vdesk_draw_rect(ax + 2, ay + 14, 1, 1, edge);
    vdesk_draw_rect(ax + 21, ay + 14, 1, 1, edge);
    /* Tray / bracket */
    vdesk_draw_rect(ax + 1, ay + 24, 22, 3, mid);
    vdesk_draw_rect(ax + 1, ay + 24, 22, 1, hi);
    vdesk_draw_rect(ax + 1, ay + 26, 22, 1, deep);
    vdesk_draw_rect(ax + 1, ay + 21, 3, 6, mid);
    vdesk_draw_rect(ax + 1, ay + 21, 1, 6, hi);
    vdesk_draw_rect(ax + 20, ay + 21, 3, 6, mid);
    vdesk_draw_rect(ax + 22, ay + 21, 1, 6, deep);
}

/* Diagonal paintbrush (paintappicon.png). */
static void glyph_paintbrush(int x, int y) {
    const color_t outline = 0x101010;
    const color_t handle = 0x8B5A2B;
    const color_t handle_hi = 0xC48A55;
    const color_t handle_lo = 0x5A3A1E;
    const color_t ferrule = 0xC0C0C0;
    const color_t ferrule_hi = 0xF0F0F0;
    const color_t bristle = 0xE8D4B0;
    const color_t bristle_lo = 0xBFA888;
    const color_t paint = 0x2040FF;
    const color_t paint_hi = 0x80A8FF;
    const color_t paint_lo = 0x001080;
    int ox = x + 2;
    int oy = y + 2;
    int i;

    /* Stepped diagonal: tip (bottom-left) → handle (top-right). */
    for (i = 0; i < 7; i++) {
        int bx = ox + 14 - i * 2;
        int by = oy + 2 + i * 2;
        vdesk_draw_rect(bx, by, 8, 4, handle);
        vdesk_draw_rect(bx, by, 8, 1, handle_hi);
        vdesk_draw_rect(bx, by + 3, 8, 1, handle_lo);
        vdesk_draw_rect(bx - 1, by, 1, 4, outline);
        vdesk_draw_rect(bx + 8, by, 1, 4, outline);
    }
    /* Ferrule */
    vdesk_draw_rect(ox + 4, oy + 14, 8, 5, ferrule);
    vdesk_draw_rect(ox + 6, oy + 14, 2, 5, ferrule_hi);
    vdesk_draw_rect(ox + 3, oy + 14, 1, 5, outline);
    vdesk_draw_rect(ox + 12, oy + 14, 1, 5, outline);
    /* Bristles + blue paint tip */
    vdesk_draw_rect(ox + 1, oy + 18, 10, 5, bristle);
    vdesk_draw_rect(ox + 1, oy + 18, 10, 1, VCOLOR_WHITE);
    vdesk_draw_rect(ox + 1, oy + 22, 10, 1, bristle_lo);
    vdesk_draw_rect(ox + 0, oy + 22, 8, 5, paint);
    vdesk_draw_rect(ox + 2, oy + 23, 3, 2, paint_hi);
    vdesk_draw_rect(ox + 0, oy + 26, 8, 1, paint_lo);
    vdesk_draw_rect(ox + 0, oy + 22, 1, 5, outline);
    vdesk_draw_rect(ox + 7, oy + 22, 1, 5, outline);
    vdesk_draw_rect(ox + 1, oy + 27, 6, 1, outline);
}

static void render_icon_glyph(VDesktopIcon* icon) {
    const VTheme* t = theme();
    int x = icon->x + 20;
    int y = icon->y + 2;
    color_t body = desktop.theme_mode ? 0xF8FAFC : 0xDDE6F3;
    color_t fold = desktop.theme_mode ? 0xD1D5DB : 0x9DB6D8;

    /* Filesystem-backed desktop items get type-specific glyphs. */
    if (icon->kind == VICON_FOLDER) {
        vdesk_draw_rect(x, y + 8, 26, 20, 0xD9A441);
        vdesk_draw_rect(x + 3, y + 4, 12, 6, 0xEBC76A);
        vdesk_draw_border(x, y + 8, 26, 20, t->border_light, t->border_dark);
        return;
    }
    if (icon->kind == VICON_TEXT) {
        vdesk_draw_rect(x + 3, y, 22, 28, body);
        vdesk_draw_rect(x + 19, y, 6, 6, fold);
        vdesk_draw_border(x + 3, y, 22, 28, t->border_light, t->border_dark);
        vdesk_draw_rect(x + 7, y + 9, 14, 1, t->text_muted);
        vdesk_draw_rect(x + 7, y + 14, 14, 1, t->text_muted);
        vdesk_draw_rect(x + 7, y + 19, 10, 1, t->text_muted);
        return;
    }
    if (icon->kind == VICON_CODE) {
        vdesk_draw_rect(x + 3, y, 22, 28, 0x1E1E1E);
        vdesk_draw_border(x + 3, y, 22, 28, t->border_light, t->border_dark);
        vdesk_draw_rect(x + 7, y + 8, 14, 1, 0x569CD6);
        vdesk_draw_rect(x + 7, y + 13, 10, 1, 0xCE9178);
        vdesk_draw_rect(x + 7, y + 18, 12, 1, 0x6A9955);
        return;
    }
    if (icon->kind == VICON_GOB) {
        glyph_gob_app(x, y);
        return;
    }
    if (icon->kind == VICON_DOS) {
        /* DOS prompt glyph */
        vdesk_draw_rect(x + 2, y + 4, 28, 22, 0x000000);
        vdesk_draw_border(x + 2, y + 4, 28, 22, t->border_light, t->border_dark);
        vdesk_draw_rect(x + 6, y + 10, 8, 2, 0x00FF00);
        vdesk_draw_rect(x + 6, y + 16, 14, 2, 0x00FF00);
        return;
    }
    if (icon->kind == VICON_BITMAP) {
        glyph_paintbrush(x, y);
        return;
    }

    if (icon->app_id == VDESK_APP_EXPLORER) {
        vdesk_draw_rect(x, y + 8, 26, 20, 0xD9A441);
        vdesk_draw_rect(x + 3, y + 4, 12, 6, 0xEBC76A);
        vdesk_draw_border(x, y + 8, 26, 20, t->border_light, t->border_dark);
    } else if (icon->app_id == VDESK_APP_EDITOR) {
        vdesk_draw_rect(x + 3, y, 22, 28, body);
        vdesk_draw_rect(x + 19, y, 6, 6, fold);
        vdesk_draw_border(x + 3, y, 22, 28, t->border_light, t->border_dark);
        vdesk_draw_rect(x + 7, y + 9, 14, 1, t->text_muted);
        vdesk_draw_rect(x + 7, y + 14, 14, 1, t->text_muted);
        vdesk_draw_rect(x + 7, y + 19, 10, 1, t->text_muted);
    } else if (icon->app_id == VDESK_APP_TASK_MANAGER) {
        vdesk_draw_rect(x + 1, y + 2, 28, 24, body);
        vdesk_draw_border(x + 1, y + 2, 28, 24, t->border_light, t->border_dark);
        vdesk_draw_rect(x + 6, y + 19, 3, 4, t->accent);
        vdesk_draw_rect(x + 12, y + 14, 3, 9, t->accent);
        vdesk_draw_rect(x + 18, y + 9, 3, 14, t->accent);
    } else if (icon->app_id == VDESK_APP_SYSINFO) {
        vdesk_draw_rect(x + 2, y + 2, 24, 24, body);
        vdesk_draw_border(x + 2, y + 2, 24, 24, t->border_light, t->border_dark);
        vdesk_draw_text(x + 10, y + 6, "i", t->accent, body);
    } else if (icon->app_id == VDESK_APP_PAINT) {
        glyph_paintbrush(x, y);
    } else if (icon->app_id == VDESK_APP_INSTALLER) {
        glyph_install_arrow(x, y);
    } else if (icon->app_id == VDESK_APP_WELCOME || icon->app_id == VDESK_APP_IDE) {
        glyph_gob_app(x, y);
    } else {
        /* Shell / default: terminal slab */
        vdesk_draw_rect(x + 3, y + 3, 24, 20, 0x101820);
        vdesk_draw_border(x + 3, y + 3, 24, 20, t->border_light, t->border_dark);
        vdesk_draw_text(x + 7, y + 5, ">", VCOLOR_LIGHT_GREEN, 0x101820);
    }
}

static void render_desktop_icons(void) {
    if (!desktop.desktop_experience_visible) return;
    const VTheme* t = theme();
    for (int i = 0; i < desktop.icon_count; i++) {
        VDesktopIcon* icon = &desktop.icons[i];
        if (icon->selected) {
            vdesk_draw_rect(icon->x + 2, icon->y + 34, 60, 18, t->accent);
        }
        render_icon_glyph(icon);
        vdesk_draw_text(icon->x + 2, icon->y + 38, icon->label,
                        icon->selected ? VCOLOR_WHITE : t->text,
                        icon->selected ? t->accent : t->desktop_bg);
    }
}

static int context_menu_count(void) {
    if (desktop.context_kind == VCTX_FS) return 5;
    if (desktop.context_kind == VCTX_TASKBAR) return 3;
    if (desktop.context_kind == VCTX_DESKTOP)
        return desktop.clip_mode ? 11 : 10;
    return 10;
}

static int context_item_at(int x, int y) {
    int menu_w = 156;
    int item_h = 20;
    int count;
    if (!desktop.context_open) return -1;
    count = context_menu_count();
    if (x < desktop.context_x || x >= desktop.context_x + menu_w ||
        y < desktop.context_y || y >= desktop.context_y + count * item_h)
        return -1;
    return (y - desktop.context_y) / item_h;
}

static void render_context_menu(void) {
    if (!desktop.context_open) return;
    const VTheme* t = theme();
    int count = context_menu_count();
    int menu_w = 156;
    int item_h = 20;
    int x = desktop.context_x;
    int y = desktop.context_y;
    const char* desktop_items[] = {"New Folder", "New Text File", "New Bitmap", "Open Shell",
                                   "Open Editor", "File Explorer", "System Info", "Task Manager",
                                   "Display Settings", "System Settings", "Paste"};
    const char* fs_items[] = {"Open", "Rename", "Delete", "Cut", "Copy"};
    const char* task_items[3];
    const char** items = desktop_items;

    if (desktop.context_kind == VCTX_FS) {
        items = fs_items;
    } else if (desktop.context_kind == VCTX_TASKBAR) {
        VWindow* win = get_window(desktop.context_target_window_id);
        task_items[0] = (win && win->minimized) ? "Restore" : "Minimize";
        task_items[1] = (win && win->maximized) ? "Restore Size" : "Maximize";
        task_items[2] = "Close";
        items = task_items;
    }

    vdesk_draw_rect(x, y, menu_w, count * item_h, t->menu_bg);
    vdesk_draw_border(x, y, menu_w, count * item_h, t->border_light, t->border_outer);
    for (int i = 0; i < count; i++) {
        int iy = y + i * item_h;
        vdesk_draw_text(x + 8, iy + 4, items[i], t->menu_fg, t->menu_bg);
    }
}

static void rename_modal_rect(int* x, int* y, int* w, int* h) {
    *w = 300;
    *h = 136;
    *x = (desktop.screen_w - *w) / 2;
    *y = (desktop.screen_h - *h) / 2;
    if (*y < vdesk_workspace_top() + 8) *y = vdesk_workspace_top() + 8;
}

static void render_modal_button(int x, int y, int w, int h, const char* label) {
    const VTheme* t = theme();
    vdesk_draw_rect(x, y, w, h, t->button_bg);
    vdesk_draw_border(x, y, w, h, t->border_light, t->border_dark);
    vdesk_draw_text(x + 8, y + 5, label, t->text, t->button_bg);
}

static void render_rename_modal(void) {
    if (!desktop.rename_open) return;
    const VTheme* t = theme();
    int x, y, w, h;
    rename_modal_rect(&x, &y, &w, &h);

    vdesk_draw_rect(x, y, w, h, t->window_bg);
    vdesk_draw_border(x, y, w, h, t->border_light, t->border_outer);
    vdesk_draw_rect(x + 2, y + 2, w - 4, TITLEBAR_HEIGHT, t->title_active_bg);
    vdesk_draw_text(x + 8, y + 5, "Rename", t->title_fg, t->title_active_bg);

    vdesk_draw_text(x + 18, y + 34, "New name:", t->text, t->window_bg);
    vdesk_draw_rect(x + 18, y + 54, w - 36, 22, t->client_bg);
    vdesk_draw_border(x + 18, y + 54, w - 36, 22, t->border_dark, t->border_light);
    vdesk_draw_text(x + 24, y + 58, desktop.rename_input, t->text, t->client_bg);
    vdesk_draw_text(x + 24 + desktop.rename_len * 8, y + 58, "_", t->accent, t->client_bg);

    if (desktop.rename_status) {
        vdesk_draw_text(x + 18, y + 82, "Rename failed.", t->accent, t->window_bg);
    }

    render_modal_button(x + w - 170, y + h - 34, 70, 22, "Cancel");
    render_modal_button(x + w - 90, y + h - 34, 72, 22, "Rename");
}

static void submit_rename_modal(void) {
    Directory* dir = desktop.rename_dir ? desktop.rename_dir : fs_get_desktop_dir();
    if (!desktop.rename_open || desktop.rename_input[0] == '\0' || !dir) {
        desktop.rename_status = 1;
        return;
    }
    if (fs_dir_rename(dir, desktop.rename_old_name, desktop.rename_input) == 0) {
        desktop.rename_open = 0;
        desktop.rename_status = 0;
        vdesk_refresh_desktop_items(1);
        vdesk_set_status("Renamed");
    } else {
        desktop.rename_status = 1;
    }
    vdesk_mark_full_dirty();
}

static int rename_modal_click(int mx, int my) {
    if (!desktop.rename_open) return 0;
    int x, y, w, h;
    rename_modal_rect(&x, &y, &w, &h);
    if (mx < x || mx >= x + w || my < y || my >= y + h) return 1;
    if (mx >= x + w - 170 && mx < x + w - 100 &&
        my >= y + h - 34 && my < y + h - 12) {
        desktop.rename_open = 0;
        vdesk_mark_full_dirty();
        return 1;
    }
    if (mx >= x + w - 90 && mx < x + w - 18 &&
        my >= y + h - 34 && my < y + h - 12) {
        submit_rename_modal();
        return 1;
    }
    return 1;
}

static VDeskAppId context_item_to_app(int item) {
    switch (item) {
        case 3: return VDESK_APP_SHELL;
        case 4: return VDESK_APP_EDITOR;
        case 5: return VDESK_APP_EXPLORER;
        case 6: return VDESK_APP_SYSINFO;
        case 7: return VDESK_APP_TASK_MANAGER;
        case 8: return VDESK_APP_DISPLAY_SETTINGS;
        case 9: return VDESK_APP_SYSTEM_SETTINGS;
        default: return VDESK_APP_DISPLAY_SETTINGS;
    }
}

static void open_rename_modal_for_fs(Directory* dir, const char* name, int is_dir) {
    if (!dir || !name || !name[0]) return;
    desktop.rename_open = 1;
    desktop.rename_dir = dir;
    desktop.rename_target_kind = is_dir ? VICON_FOLDER : VICON_TEXT;
    strncpy(desktop.rename_old_name, name, VICON_NAME_MAX - 1);
    desktop.rename_old_name[VICON_NAME_MAX - 1] = '\0';
    strncpy(desktop.rename_input, name, VICON_NAME_MAX - 1);
    desktop.rename_input[VICON_NAME_MAX - 1] = '\0';
    desktop.rename_len = (int)strlen(desktop.rename_input);
    desktop.rename_status = 0;
    vdesk_mark_full_dirty();
}

static void open_rename_modal_for_icon(int icon_idx) {
    VDesktopIcon* icon;
    if (icon_idx < 0 || icon_idx >= desktop.icon_count) return;
    icon = &desktop.icons[icon_idx];
    if (icon->kind == VICON_APP) return;
    open_rename_modal_for_fs(fs_get_desktop_dir(), icon->filename,
                             icon->kind == VICON_FOLDER);
}

static void clipboard_set(Directory* dir, const char* name, int is_dir, int cut) {
    if (!dir || !name || !name[0]) return;
    desktop.clip_mode = cut ? 2 : 1;
    desktop.clip_dir = dir;
    desktop.clip_is_dir = is_dir ? 1 : 0;
    strncpy(desktop.clip_name, name, VICON_NAME_MAX - 1);
    desktop.clip_name[VICON_NAME_MAX - 1] = '\0';
    vdesk_set_status(cut ? "Cut" : "Copied");
}

void vdesk_clipboard_paste_into(Directory* dst) {
    int rc;
    if (!dst || !desktop.clip_mode || !desktop.clip_dir || !desktop.clip_name[0]) {
        vdesk_set_status("Clipboard empty");
        return;
    }
    if (desktop.clip_is_dir) {
        vdesk_notify("Paste", "Folder paste not supported yet");
        return;
    }
    rc = vdesk_copy_file_between(desktop.clip_dir, desktop.clip_name, dst);
    if (rc != 0) {
        vdesk_notify("Paste", "Paste failed");
        return;
    }
    if (desktop.clip_mode == 2) {
        if (fs_dir_delete(desktop.clip_dir, desktop.clip_name) != 0)
            vdesk_notify("Cut", "Copied, but delete failed");
        desktop.clip_mode = 0;
        desktop.clip_dir = NULL;
        desktop.clip_name[0] = '\0';
    }
    vdesk_refresh_desktop_items(1);
    vdesk_set_status("Pasted");
    vdesk_mark_full_dirty();
}

void vdesk_open_fs_context(Directory* dir, const char* name, int is_dir,
                           int mx, int my) {
    if (!dir || !name || !name[0]) return;
    desktop.start_open = 0;
    start_dos_flyout = 0;
    start_games_flyout = 0;
    start_tools_flyout = 0;
    desktop.context_open = 1;
    desktop.context_kind = VCTX_FS;
    desktop.context_fs_dir = dir;
    desktop.context_fs_is_dir = is_dir ? 1 : 0;
    strncpy(desktop.context_fs_name, name, VICON_NAME_MAX - 1);
    desktop.context_fs_name[VICON_NAME_MAX - 1] = '\0';
    desktop.context_target_icon = -1;
    desktop.context_target_window_id = 0;
    desktop.context_x = CLAMP(mx, 0, desktop.screen_w - 160);
    desktop.context_y = CLAMP(my, vdesk_workspace_top(),
                              vdesk_workspace_bottom() - 110);
    vdesk_mark_full_dirty();
}

static void context_run_fs_item(int item) {
    Directory* dir = desktop.context_fs_dir;
    const char* name = desktop.context_fs_name;
    int is_dir = desktop.context_fs_is_dir;
    int idx = desktop.context_target_icon;
    if (!dir || !name[0]) return;
    if (item == 0) {
        if (idx >= 0 && idx < desktop.icon_count)
            activate_icon(idx);
        else if (desktop.open_fs_item)
            desktop.open_fs_item(dir, name, is_dir);
        else if (desktop.open_file)
            desktop.open_file(name, is_dir ? VICON_FOLDER : VICON_TEXT);
    } else if (item == 1) {
        open_rename_modal_for_fs(dir, name, is_dir);
    } else if (item == 2) {
        if (fs_dir_delete(dir, name) == 0) {
            vdesk_set_status("Deleted");
            vdesk_refresh_desktop_items(1);
            vdesk_mark_full_dirty();
        } else {
            vdesk_notify("Delete", "Delete failed");
        }
    } else if (item == 3) {
        clipboard_set(dir, name, is_dir, 1);
    } else if (item == 4) {
        clipboard_set(dir, name, is_dir, 0);
    }
}

static void context_run_taskbar_item(int item) {
    VWindow* win = get_window(desktop.context_target_window_id);
    if (!win) return;
    if (item == 0) {
        if (win->minimized) vdesk_restore_window(win);
        else vdesk_minimize_window(win);
    } else if (item == 1) {
        if (!win->minimized) vdesk_toggle_maximize(win);
        else vdesk_restore_window(win);
    } else if (item == 2) {
        vdesk_close_window(win);
    }
}

static void context_run_item(int item) {
    char name[32];
    char num[12];
    int rc;

    if (desktop.context_kind == VCTX_FS) {
        context_run_fs_item(item);
        return;
    }
    if (desktop.context_kind == VCTX_ICON) {
        /* legacy: treat as FS item on desktop */
        if (desktop.context_target_icon >= 0 &&
            desktop.context_target_icon < desktop.icon_count) {
            VDesktopIcon* icon = &desktop.icons[desktop.context_target_icon];
            desktop.context_fs_dir = fs_get_desktop_dir();
            strncpy(desktop.context_fs_name, icon->filename, VICON_NAME_MAX - 1);
            desktop.context_fs_is_dir = (icon->kind == VICON_FOLDER);
            context_run_fs_item(item);
        }
        return;
    }
    if (desktop.context_kind == VCTX_TASKBAR) {
        context_run_taskbar_item(item);
        return;
    }

    if (item == 10 && desktop.clip_mode) {
        vdesk_clipboard_paste_into(fs_get_desktop_dir());
        return;
    }

    /* New items are placed in the fixed Desktop folder (not whatever directory
     * File Explorer happens to be browsing) so they appear on the desktop. */
    Directory* desk = fs_get_desktop_dir();
    if (!desk) {
        vdesk_set_status("Desktop folder unavailable");
        return;
    }

    if (item == 0) {
        strcpy(name, "Folder");
        itoa(new_folder_count++, num, 10);
        strcat(name, num);
        rc = fs_dir_create_dir(desk, name);
        if (rc != 0) {
            vdesk_set_status("Failed to create folder");
            return;
        }
        vdesk_set_status("Folder created");
        vdesk_refresh_desktop_items(1);
        vdesk_mark_full_dirty();
        return;
    }

    if (item == 1) {
        strcpy(name, "File");
        itoa(new_file_count++, num, 10);
        strcat(name, num);
        strcat(name, ".txt");
        rc = fs_dir_create(desk, name);
        if (rc != 0) {
            vdesk_set_status("Failed to create text file");
            return;
        }
        vdesk_set_status("Text file created");
        vdesk_refresh_desktop_items(1);
        vdesk_mark_full_dirty();
        return;
    }

    if (item == 2) {
        uint8_t pixels[32 * 32];
        for (int i = 0; i < 32 * 32; i++) pixels[i] = 0;
        strcpy(name, "Art");
        itoa(new_bitmap_count++, num, 10);
        strcat(name, num);
        strcat(name, ".gbm");
        rc = fs_dir_write(desk, name, pixels, sizeof(pixels));
        if (rc != 0) {
            vdesk_set_status("Failed to create bitmap");
            return;
        }
        vdesk_set_status("Bitmap created");
        vdesk_refresh_desktop_items(1);
        vdesk_mark_full_dirty();
        return;
    }

    launch_app(context_item_to_app(item));
}

static void render_drag_ghost(void) {
    const VTheme* t = theme();
    int gx, gy;
    char label[20];
    size_t n = 0;
    if (!g_file_drag.active || !g_file_drag.name[0]) return;
    gx = desktop.mouse_x + 14;
    gy = desktop.mouse_y + 14;
    if (gx + 96 > desktop.screen_w) gx = desktop.screen_w - 96;
    if (gy + 36 > desktop.screen_h) gy = desktop.screen_h - 36;
    if (gx < 0) gx = 0;
    if (gy < 0) gy = 0;
    vdesk_draw_rect(gx, gy, 92, 32, t->menu_bg);
    vdesk_draw_border(gx, gy, 92, 32, t->border_light, t->border_outer);
    if (g_file_drag.is_dir) {
        vdesk_draw_rect(gx + 6, gy + 10, 16, 12, 0xE8B84A);
        vdesk_draw_rect(gx + 6, gy + 8, 8, 4, 0xF0D078);
    } else {
        vdesk_draw_rect(gx + 8, gy + 6, 12, 18, VCOLOR_WHITE);
        vdesk_draw_border(gx + 8, gy + 6, 12, 18, t->border_dark, t->border_light);
        vdesk_draw_rect(gx + 10, gy + 10, 8, 1, t->border_dark);
        vdesk_draw_rect(gx + 10, gy + 14, 8, 1, t->border_dark);
    }
    while (g_file_drag.name[n] && n + 1 < sizeof(label)) {
        label[n] = g_file_drag.name[n];
        n++;
    }
    if (n > 10) {
        label[8] = '.';
        label[9] = '.';
        label[10] = '\0';
    } else {
        label[n] = '\0';
    }
    vdesk_draw_text(gx + 28, gy + 10, label, t->menu_fg, t->menu_bg);
}

static void render_mouse(void) {
    int mx = desktop.mouse_x;
    int my = desktop.mouse_y;
    int row, col;
    if (mx < 0 || mx >= desktop.screen_w || my < 0 || my >= desktop.screen_h)
        return;

    render_drag_ghost();

    for (row = 0; row < CURSOR_H; row++) {
        uint16_t andm = g_cursor_and[row];
        uint16_t xorm = g_cursor_xor[row];
        int py = my + row;
        if (py < 0 || py >= desktop.screen_h) continue;
        for (col = 0; col < CURSOR_W; col++) {
            uint16_t bit = (uint16_t)(1u << (11 - col));
            int px;
            if (!(andm & bit)) continue;
            px = mx + col;
            if (px < 0 || px >= desktop.screen_w) continue;
            vesa_put_pixel(px, py, (xorm & bit) ? VCOLOR_WHITE : VCOLOR_BLACK);
        }
    }
}

/* ---- Event handling ---- */

static int handle_rename_key(char c) {
    if (!desktop.rename_open) return 0;
    if (c == KEY_ESC) {
        desktop.rename_open = 0;
        vdesk_mark_full_dirty();
        return 1;
    }
    if (c == '\r' || c == '\n') {
        submit_rename_modal();
        return 1;
    }
    if ((unsigned char)c == KEY_BACKSPACE) {
        if (desktop.rename_len > 0) {
            desktop.rename_input[--desktop.rename_len] = '\0';
            desktop.rename_status = 0;
            vdesk_mark_full_dirty();
        }
        return 1;
    }
    if ((unsigned char)c >= 32 && (unsigned char)c <= 126 &&
        desktop.rename_len < VICON_NAME_MAX - 1) {
        desktop.rename_input[desktop.rename_len++] = c;
        desktop.rename_input[desktop.rename_len] = '\0';
        desktop.rename_status = 0;
        vdesk_mark_full_dirty();
        return 1;
    }
    return 1;
}

static void handle_keyboard(void) {
    int processed = 0;
    keyboard_poll();
    if (!keyboard_has_char()) return;

    vdesk_bc('H');
    /* Cap keys per frame so a stuck/autorepeat stream cannot monopolize
     * the compositor (or flood present work) after a single physical press. */
    while (keyboard_has_char() && processed < 8) {
        char c = keyboard_read_char();
        processed++;
        vdesk_serial_key("[kbd]in", c);
        g_vdesk_kbd_trace_pending = 1;

        if (handle_rename_key(c)) continue;

        if ((unsigned char)c == KEY_F1) {
            vdesk_focus_primary_shell();
            continue;
        }
        if ((unsigned char)c == KEY_F2 && !vdesk_has_active_app_focus()) {
            launch_app(VDESK_APP_EXPLORER);
            continue;
        }
        if ((unsigned char)c == KEY_F3 && !vdesk_has_active_app_focus()) {
            launch_app(VDESK_APP_EDITOR);
            continue;
        }
        if ((unsigned char)c == KEY_F4) {
            /* Alt+F4: close focused/top window (universal close). */
            if (keyboard_is_alt_active()) {
                VWindow* close_win = NULL;
                for (int zi = desktop.z_count - 1; zi >= 0; zi--) {
                    VWindow* win = get_window(desktop.z_order[zi]);
                    if (!win || !win->visible || win->minimized) continue;
                    if (win->focused) {
                        close_win = win;
                        break;
                    }
                    if (!close_win) close_win = win;
                }
                if (close_win) {
                    vdesk_close_window(close_win);
                    if (should_auto_focus_primary_shell() &&
                        !vdesk_has_active_app_focus())
                        vdesk_focus_primary_shell();
                }
                continue;
            }
            if (!vdesk_has_active_app_focus())
                launch_app(VDESK_APP_TASK_MANAGER);
            continue;
        }
        if ((unsigned char)c == KEY_F5) {
            vdesk_refresh_desktop_items(1);
            vdesk_mark_full_dirty();
            continue;
        }

        if (c == KEY_ESC) {
            if (desktop.context_open || desktop.start_open) {
                desktop.context_open = 0;
                desktop.start_open = 0;
                start_dos_flyout = 0;
                start_games_flyout = 0;
                    start_tools_flyout = 0;
                vdesk_mark_full_dirty();
                continue;
            }
            for (int zi = desktop.z_count - 1; zi >= 0; zi--) {
                VWindow* win = get_window(desktop.z_order[zi]);
                if (win && win->visible && !win->minimized) {
                    vdesk_close_window(win);
                    break;
                }
            }
            if (should_auto_focus_primary_shell() && !vdesk_has_active_app_focus())
                vdesk_focus_primary_shell();
            vdesk_bc('h');
            return;
        }

        if ((unsigned char)c == KEY_F9) {
            vdesk_toggle_theme();
            continue;
        }
        if ((unsigned char)c == KEY_F10) {
            g_vdesk_alive_beacon = !g_vdesk_alive_beacon;
            if (!g_vdesk_alive_beacon)
                vdesk_erase_alive_beacon();
            print(g_vdesk_alive_beacon ? "[desktop] idle tick on (F10)\n"
                                       : "[desktop] idle tick off (F10)\n");
            continue;
        }

        g_vdesk_last_key = c;
        for (int zi = desktop.z_count - 1; zi >= 0; zi--) {
            VWindow* win = get_window(desktop.z_order[zi]);
            if (win && win->visible && !win->minimized && win->focused && win->key_handler) {
                win->key_handler(win, c);
                if (win->id == desktop.primary_shell_id &&
                    !shell_key_needs_full_dirty(c)) {
                    mark_shell_prompt_dirty(win);
                } else {
                    mark_window_dirty(win);
                }
                break;
            }
        }
        if (should_auto_focus_primary_shell() && !vdesk_has_active_app_focus())
            vdesk_focus_primary_shell();
    }
    vdesk_bc('h');
}

static void open_context_menu_at(int mx, int my) {
    VWindow* task_win;
    VWindow* win;
    if (desktop.rename_open) return;
    task_win = taskbar_window_at(mx, my);
    if (task_win && task_win->id != desktop.primary_shell_id) {
        desktop.start_open = 0;
        start_dos_flyout = 0;
        start_games_flyout = 0;
        start_tools_flyout = 0;
        desktop.context_open = 1;
        desktop.context_kind = VCTX_TASKBAR;
        desktop.context_target_window_id = task_win->id;
        desktop.context_target_icon = -1;
        desktop.context_x = CLAMP(mx, 0, desktop.screen_w - 160);
        desktop.context_y = CLAMP(my, vdesk_workspace_top(),
                                  vdesk_workspace_bottom() - 62);
        vdesk_mark_full_dirty();
        return;
    }
    if (task_win) return;

    win = vdesk_window_at(mx, my);
    if (win && win->visible && !win->minimized && win->rclick_handler) {
        int lx = mx - win->x - BORDER_SIZE;
        int ly = my - win->y - TITLEBAR_HEIGHT;
        if (lx >= 0 && ly >= 0 &&
            lx < win->width - BORDER_SIZE * 2 &&
            ly < win->height - TITLEBAR_HEIGHT - BORDER_SIZE) {
            desktop.context_open = 0;
            win->rclick_handler(win, lx, ly);
            return;
        }
    }

    if (!win) {
        int idx = icon_at(mx, my);
        desktop.start_open = 0;
        start_dos_flyout = 0;
        start_games_flyout = 0;
        start_tools_flyout = 0;
        clear_icon_selection();
        if (idx >= desktop.app_icon_count && idx < desktop.icon_count) {
            VDesktopIcon* icon = &desktop.icons[idx];
            icon->selected = 1;
            desktop.context_target_icon = idx;
            vdesk_open_fs_context(fs_get_desktop_dir(), icon->filename,
                                  icon->kind == VICON_FOLDER, mx, my);
            desktop.context_target_icon = idx; /* restore after open_fs clears it */
            return;
        }
        desktop.context_open = 1;
        desktop.context_kind = VCTX_DESKTOP;
        desktop.context_target_icon = idx;
        desktop.context_target_window_id = 0;
        desktop.context_x = CLAMP(mx, 0, desktop.screen_w - 160);
        desktop.context_y = CLAMP(my, vdesk_workspace_top(),
                                  vdesk_workspace_bottom() - 222);
        if (idx >= 0 && idx < desktop.icon_count)
            desktop.icons[idx].selected = 1;
        vdesk_mark_full_dirty();
    }
}

static void handle_events(void) {
    input_event_t ev;
    while (input_poll_event(&ev)) {
        desktop.metrics.input_events++;
        if (ev.type == INPUT_EVENT_POINTER_MOVE) {
            mark_mouse_dirty(desktop.mouse_x, desktop.mouse_y);
            if (g_file_drag.active)
                vdesk_mark_dirty(desktop.mouse_x, desktop.mouse_y, 120, 48);
            desktop.mouse_x = ev.x;
            desktop.mouse_y = ev.y;
            desktop.mouse_x = CLAMP(desktop.mouse_x, 0, desktop.screen_w - 1);
            desktop.mouse_y = CLAMP(desktop.mouse_y, 0, desktop.screen_h - 1);
            mark_mouse_dirty(desktop.mouse_x, desktop.mouse_y);
            if (g_file_drag.active)
                vdesk_mark_dirty(desktop.mouse_x, desktop.mouse_y, 120, 48);
        }

        if (ev.type == INPUT_EVENT_BUTTON_DOWN && ev.button == INPUT_BUTTON_LEFT) {
            int mx = desktop.mouse_x;
            int my = desktop.mouse_y;

            /* Shift+left = right-click (context menu), when enabled. */
            if (desktop.shift_click_rmb && keyboard_is_shift_active()) {
                open_context_menu_at(mx, my);
                continue;
            }

            if (rename_modal_click(mx, my)) {
                desktop.context_open = 0;
                desktop.start_open = 0;
                start_dos_flyout = 0;
                start_games_flyout = 0;
                    start_tools_flyout = 0;
                continue;
            }

            if (toast_click(mx, my)) {
                desktop.context_open = 0;
                desktop.start_open = 0;
                start_dos_flyout = 0;
                start_games_flyout = 0;
                    start_tools_flyout = 0;
                continue;
            }

            if (start_menu_hit(mx, my)) {
                desktop.start_open = !desktop.start_open;
                start_dos_flyout = 0;
                start_games_flyout = 0;
                start_tools_flyout = 0;
                desktop.context_open = 0;
                vdesk_mark_dirty(0, 0, 320, 300 + TASKBAR_HEIGHT);
                continue;
            }

            if (desktop.context_open) {
                int item = context_item_at(mx, my);
                desktop.context_open = 0;
                vdesk_mark_full_dirty();
                if (item >= 0) {
                    context_run_item(item);
                }
                continue;
            }

            if (desktop.start_open) {
                int item = start_menu_pick_at(mx, my);
                int dos_item = start_menu_dos_item();
                int games_item = start_menu_games_item();
                int tools_item = start_menu_tools_item();
                int toggle_item = start_menu_toggle_item();
                if (item == -2) {
                    /* Click on menu/flyout chrome — keep open */
                    continue;
                }
                if (item == tools_item) {
                    start_tools_flyout = !start_tools_flyout;
                    start_dos_flyout = 0;
                    start_games_flyout = 0;
                    vdesk_mark_dirty(0, 0, 360, 340 + TASKBAR_HEIGHT);
                    continue;
                }
                if (item == dos_item) {
                    start_dos_flyout = !start_dos_flyout;
                    start_games_flyout = 0;
                    start_tools_flyout = 0;
                    vdesk_mark_dirty(0, 0, 360, 340 + TASKBAR_HEIGHT);
                    continue;
                }
                if (item == games_item) {
                    start_games_flyout = !start_games_flyout;
                    start_dos_flyout = 0;
                    start_tools_flyout = 0;
                    vdesk_mark_dirty(0, 0, 360, 340 + TASKBAR_HEIGHT);
                    continue;
                }
                if (item >= 0) {
                    desktop.start_open = 0;
                    start_dos_flyout = 0;
                    start_games_flyout = 0;
                    start_tools_flyout = 0;
                    vdesk_mark_full_dirty();
                    if (item == toggle_item) {
                        vdesk_toggle_desktop_experience();
                        continue;
                    }
                    launch_app(start_item_to_app(item));
                    continue;
                }
                desktop.start_open = 0;
                start_dos_flyout = 0;
                start_games_flyout = 0;
                start_tools_flyout = 0;
                vdesk_mark_full_dirty();
                continue;
            }

            if (taskbar_button_click(mx, my)) {
                continue;
            }

            VWindow* hit = vdesk_window_at(mx, my);
            if (hit) {
                mark_window_dirty(hit);
                vdesk_bring_to_front(hit);

                if (point_in_close(hit, mx, my)) {
                    vdesk_close_window(hit);
                    continue;
                }

                if (point_in_minimize(hit, mx, my)) {
                    vdesk_minimize_window(hit);
                    continue;
                }

                if (point_in_maximize(hit, mx, my)) {
                    vdesk_toggle_maximize(hit);
                    continue;
                }

                {
                    int redges = vdesk_window_resize_edges(hit, mx, my);
                    if (redges) {
                        hit->resize_active = 1;
                        hit->resize_edges = redges;
                        hit->resize_start_mx = mx;
                        hit->resize_start_my = my;
                        hit->resize_orig_x = hit->x;
                        hit->resize_orig_y = hit->y;
                        hit->resize_orig_w = hit->width;
                        hit->resize_orig_h = hit->height;
                        hit->drag_active = 0;
                        mark_window_dirty(hit);
                        continue;
                    }
                }

                if (point_in_title(hit, mx, my)) {
                    hit->drag_active = 1;
                    hit->drag_off_x = mx - hit->x;
                    hit->drag_off_y = my - hit->y;
                } else if (hit->click_handler) {
                    int client_x = hit->x + BORDER_SIZE;
                    int client_y = hit->y + TITLEBAR_HEIGHT + BORDER_SIZE;
                    hit->click_handler(hit, mx - client_x, my - client_y);
                    mark_window_dirty(hit);
                }
                mark_window_dirty(hit);
            } else {
                int idx = icon_at(mx, my);
                clear_icon_selection();
                if (idx >= 0) {
                    VDesktopIcon* icon = &desktop.icons[idx];
                    icon->selected = 1;
                    icon->drag_active = 1;
                    desktop.icon_press_index = idx;
                    desktop.icon_press_x = mx;
                    desktop.icon_press_y = my;
                    desktop.icon_drag_moved = 0;
                    icon->drag_off_x = mx - icon->x;
                    icon->drag_off_y = my - icon->y;
                    vdesk_mark_dirty(icon->x - 2, icon->y - 2, 76, 70);
                } else {
                    if (should_auto_focus_primary_shell())
                        vdesk_focus_primary_shell();
                    vdesk_mark_full_dirty();
                }
            }
        }

        if (ev.type == INPUT_EVENT_BUTTON_UP && ev.button == INPUT_BUTTON_LEFT) {
            int released_icon = desktop.icon_press_index;
            int file_dropped = 0;
            if (g_file_drag.active)
                file_dropped = vdesk_file_drag_drop();
            for (int i = 0; i < MAX_VWINDOWS; i++) {
                if (desktop.windows[i].visible) {
                    if (desktop.windows[i].resize_active)
                        vdesk_mark_full_dirty();
                    desktop.windows[i].drag_active = 0;
                    desktop.windows[i].resize_active = 0;
                    desktop.windows[i].resize_edges = 0;
                }
            }
            for (int i = 0; i < desktop.icon_count; i++)
                desktop.icons[i].drag_active = 0;
            if (!file_dropped && released_icon >= 0 &&
                released_icon < desktop.icon_count && !desktop.icon_drag_moved) {
                activate_icon(released_icon);
            }
            desktop.icon_press_index = -1;
            if (!file_dropped) vdesk_file_drag_cancel();
        }

        if (ev.type == INPUT_EVENT_BUTTON_DOWN && ev.button == INPUT_BUTTON_RIGHT) {
            open_context_menu_at(desktop.mouse_x, desktop.mouse_y);
            continue;
        }

        if (ev.type == INPUT_EVENT_SCROLL) {
            for (int zi = desktop.z_count - 1; zi >= 0; zi--) {
                VWindow* win = get_window(desktop.z_order[zi]);
                if (win && win->visible && !win->minimized && win->focused && win->scroll_handler) {
                    win->scroll_handler(win, ev.wheel);
                    mark_window_dirty(win);
                    break;
                }
            }
        }

        if (ev.type == INPUT_EVENT_POINTER_MOVE && (ev.buttons & 1)) {
            for (int i = 0; i < MAX_VWINDOWS; i++) {
                VWindow* win = &desktop.windows[i];
                if (win->visible && win->resize_active && win->resize_edges) {
                    int ddx = desktop.mouse_x - win->resize_start_mx;
                    int ddy = desktop.mouse_y - win->resize_start_my;
                    int nx = win->resize_orig_x;
                    int ny = win->resize_orig_y;
                    int nw = win->resize_orig_w;
                    int nh = win->resize_orig_h;
                    int top = vdesk_workspace_top();
                    int bottom = vdesk_workspace_bottom();
                    mark_window_dirty(win);
                    if (win->resize_edges & VWIN_RESIZE_E) nw = win->resize_orig_w + ddx;
                    if (win->resize_edges & VWIN_RESIZE_S) nh = win->resize_orig_h + ddy;
                    if (win->resize_edges & VWIN_RESIZE_W) {
                        nx = win->resize_orig_x + ddx;
                        nw = win->resize_orig_w - ddx;
                    }
                    if (win->resize_edges & VWIN_RESIZE_N) {
                        ny = win->resize_orig_y + ddy;
                        nh = win->resize_orig_h - ddy;
                    }
                    if (nw < VWIN_MIN_W) {
                        if (win->resize_edges & VWIN_RESIZE_W)
                            nx = win->resize_orig_x + win->resize_orig_w - VWIN_MIN_W;
                        nw = VWIN_MIN_W;
                    }
                    if (nh < VWIN_MIN_H) {
                        if (win->resize_edges & VWIN_RESIZE_N)
                            ny = win->resize_orig_y + win->resize_orig_h - VWIN_MIN_H;
                        nh = VWIN_MIN_H;
                    }
                    if (nx < 0) { nw += nx; nx = 0; }
                    if (ny < top) { nh -= (top - ny); ny = top; }
                    if (nx + nw > desktop.screen_w) nw = desktop.screen_w - nx;
                    if (ny + nh > bottom) nh = bottom - ny;
                    if (nw < VWIN_MIN_W) nw = VWIN_MIN_W;
                    if (nh < VWIN_MIN_H) nh = VWIN_MIN_H;
                    win->x = nx;
                    win->y = ny;
                    win->width = nw;
                    win->height = nh;
                    mark_window_dirty(win);
                    continue;
                }
                if (win->visible && win->drag_active) {
                    mark_window_dirty(win);
                    int nx = desktop.mouse_x - win->drag_off_x;
                    int ny = desktop.mouse_y - win->drag_off_y;
                    nx = CLAMP(nx, 0, desktop.screen_w - win->width - 2);
                    ny = CLAMP(ny, vdesk_workspace_top(),
                               vdesk_workspace_bottom() - win->height - 2);
                    win->x = nx;
                    win->y = ny;
                    mark_window_dirty(win);
                }
            }
            for (int i = 0; i < desktop.icon_count; i++) {
                VDesktopIcon* icon = &desktop.icons[i];
                if (icon->drag_active) {
                    vdesk_mark_dirty(icon->x - 2, icon->y - 2, 76, 70);
                    if (ABS(desktop.mouse_x - desktop.icon_press_x) > 3 ||
                        ABS(desktop.mouse_y - desktop.icon_press_y) > 3) {
                        desktop.icon_drag_moved = 1;
                    }
                    icon->x = CLAMP(desktop.mouse_x - icon->drag_off_x, 0, desktop.screen_w - 72);
                    icon->y = CLAMP(desktop.mouse_y - icon->drag_off_y,
                                    vdesk_workspace_top(),
                                    vdesk_workspace_bottom() - 64);
                    vdesk_mark_dirty(icon->x - 2, icon->y - 2, 76, 70);
                }
            }
        }

        desktop.mouse_buttons = ev.buttons;
    }
}

/* ---- Main loop ---- */

void vdesk_pump_one_frame(void) {
    int dirty_x, dirty_y, dirty_w, dirty_h;
    int dirty_area, screen_area, promote_full;
    VWindow* shell;

    if (!desktop.running) return;

    /*
     * Gob GUI wait / yield paths call this instead of vdesk_run(). Without
     * draining USB/PS2/HID the cursor and keyboard freeze for the whole
     * app lifetime even though IRQ handlers still queue events.
     */
    usb_poll();
    touchpad_poll();
    handle_keyboard();
    handle_events();

    /*
     * Prefer dirties already marked by the app (gob_win_dirty / doom / shell
     * output). Unconditionally dirtying the maximized primary shell here used
     * to inflate every Gob getkey/sleep pump to a workspace-sized present;
     * on budgeted UC scanout that paints as Win95-style horizontal tear bands
     * with black gaps (game window frame visibly sliced).
     */
    if (!desktop.dirty) {
        shell = get_window(desktop.primary_shell_id);
        if (shell)
            mark_window_dirty(shell);
        else
            vdesk_mark_full_dirty();
    }

    if (!desktop.dirty) return;

    dirty_x = desktop.dirty_x1;
    dirty_y = desktop.dirty_y1;
    dirty_w = desktop.dirty_x2 - desktop.dirty_x1;
    dirty_h = desktop.dirty_y2 - desktop.dirty_y1;
    dirty_area = dirty_w * dirty_h;
    screen_area = desktop.screen_w * desktop.screen_h;
    promote_full = should_promote_full_present(dirty_x, dirty_y, dirty_w, dirty_h,
                                               dirty_area, screen_area);
    desktop.dirty = 0;

    vesa_set_clip(dirty_x, dirty_y, dirty_w, dirty_h);
    render_desktop();
    render_desktop_icons();
    for (int zi = 0; zi < desktop.z_count; zi++) {
        VWindow* win = get_window(desktop.z_order[zi]);
        if (win) render_window(win);
    }
    render_taskbar();
    render_toasts();
    render_start_menu();
    render_context_menu();
    render_rename_modal();
    render_mouse();
    vesa_clear_clip();

    /*
     * Finish the dirty AABB in one blit (same as window drag). Budgeted
     * chunk drains are immediately visible on uncached GOP and look like
     * violent tearing even when the drain completes in this call.
     */
    display_present_set_oneshot(1);
    if (promote_full) {
        display_present_note_promotion();
        display_present_frame();
    } else {
        display_present_rect(dirty_x, dirty_y, dirty_w, dirty_h);
        while (display_present_has_pending()) {
            int px, py, pw, ph;
            display_present_consume_pending(&px, &py, &pw, &ph);
            display_present_rect(px, py, pw, ph);
        }
    }
    display_present_set_oneshot(0);
}

void vdesk_run(void) {
    /*
     * Phase 4 (display polish, item 2): tear-free repaint loop.
     *
     *   1. Drain HID input.
     *   2. Tick any per-window tick handlers + periodic refresh.
     *   3. If anything is dirty, repaint the WHOLE composite into the
     *      back-buffer (desktop -> icons -> windows -> taskbar -> menus
     *      -> mouse cursor) -- the cursor sprite is part of the back-
     *      buffer composite, not a separate overlay, so there is no
     *      cursor tearing either.
     *   4. Single whole-screen vesa_present() flips the back-buffer to
     *      the LFB in one memcpy. When no back-buffer is armed (kmalloc
     *      failed at desktop init) vesa_present() is a no-op and the
     *      direct-to-LFB writes are already visible (legacy path).
     *   5. Frame pacing: if the drawing+present took less than the
     *      target frame budget, sleep until the boundary; otherwise draw
     *      the next frame immediately (no skipping). Budget comes from
     *      gooberos.display.fps=N (default 60 Hz -> 16 ms/frame).
     */
    __asm__ volatile("sti" ::: "memory");
    kernel_set_fb_console_echo(0);
    /* Drop boot-time 8042/IRQ1 residue so the first desktop frame is not a
     * garbage key + near-fullscreen present. */
    vdesk_drain_ps2_output();
    {
        char buf[16];
        int fps = kernel_display_target_fps();
        uint32_t budget = display_present_budget_bytes();
        print("[desktop] fps=");
        itoa(fps, buf, 10); print(buf);
        print(" frame_ms=");
        itoa(desktop.target_frame_ms, buf, 10); print(buf);
        print(" present_budget=");
        itoa((int)budget, buf, 10); print(buf);
        print(" scanout_uncached=");
        itoa(display_scanout_uncached(), buf, 10); print(buf);
        print("\n");
        {
            const char* cmd = kernel_boot_cmdline();
            if (cmd) {
                int want = 0;
                for (const char* p = cmd; *p; p++) {
                    if (p[0] == 'g' && p[1] == 'o' && p[2] == 'o' &&
                        p[3] == 'b' && p[4] == 'e' && p[5] == 'r' &&
                        p[6] == 'o' && p[7] == 's' && p[8] == '.' &&
                        p[9] == 'd' && p[10] == 'e' && p[11] == 'b' &&
                        p[12] == 'u' && p[13] == 'g' && p[14] == '=') {
                        want = 1;
                        break;
                    }
                }
                if (want) {
                    g_vdesk_debug_hud = 1;
                    /* On x64 COM1 TX is disabled by default (real laptops lack a
                     * usable UART). Under a VM it's the only path that captures
                     * the kserial_note freeze oracle to a raw file, so enable it
                     * here when the operator explicitly asked for debug. */
                    kernel_serial_com1_enable(1);
                    print("[desktop] debug HUD on (cmdline). F10 toggles idle tick.\n");
                    kserial_note("[desktop] debug HUD armed; COM1 TX enabled\n");
                }
            }
        }
    }
    while (desktop.running) {
        uint32_t frame_start = timer_ticks();
        uint32_t render_start;
        uint32_t swap_start;
        int had_dirty;
        int drag_active;

        /* Drain ALL deferred present bands before input/render. One chunk per
         * frame left the shell half-updated when the alive-beacon oneshot used
         * to cancel the remainder; draining here keeps tall updates complete. */
        {
            int guard = 0;
            while (display_present_has_pending() && guard++ < 64) {
                int px, py, pw, ph;
                display_present_consume_pending(&px, &py, &pw, &ph);
                vdesk_bc('P');
                display_present_rect(px, py, pw, ph);
                vdesk_bc('p');
            }
        }

        usb_poll();
        touchpad_poll();
        if (!display_scanout_uncached())
            vdesk_sync_driver_log_file();
        update_metrics_pointer();
        handle_keyboard();
        handle_events();

        /* Soft clock tray + toast expiry need periodic dirty without input. */
        {
            int ty = (desktop.taskbar_position == VDESK_TASKBAR_TOP) ?
                     0 : desktop.screen_h - TASKBAR_HEIGHT;
            int stamp;
            int i;
            uint32_t now;
            softclock_tick();
            stamp = softclock_minute_stamp();
            if (stamp != desktop.clock_tray_stamp) {
                int tray_x = taskbar_tray_left();
                vdesk_mark_dirty(tray_x - 4, ty,
                                 desktop.screen_w - tray_x + 4, TASKBAR_HEIGHT);
            }
            now = timer_ticks();
            for (i = 0; i < VDESK_TOAST_MAX; i++) {
                if (desktop.toasts[i].used &&
                    (int32_t)(now - desktop.toasts[i].expire_tick) >= 0) {
                    desktop.toasts[i].used = 0;
                    vdesk_mark_full_dirty();
                }
            }
        }

        /*
         * Re-scan Desktop icons ~1 Hz. Faster polls + FAT refresh on eMMC
         * caused multi-hundred-ms hitches every few frames after install.
         */
        if ((uint32_t)(frame_start - desktop.last_scan_tick) >= 100) {
            desktop.last_scan_tick = frame_start;
            if (!display_scanout_uncached())
                vdesk_refresh_desktop_items(0);
        }

        /*
         * Periodically repaint live windows (taskmgr/metrics). Skip the
         * fullscreen primary shell — dirtying it promotes a full copy-frame
         * present (~fullscreen memcpy) and freezes Bay Trail for hundreds ms.
         * Shell input already marks dirty on keystrokes.
         */
        if ((desktop.metrics.frame_count % 25) == 0) {
            for (int i = 0; i < MAX_VWINDOWS; i++) {
                if (!desktop.windows[i].visible || desktop.windows[i].minimized)
                    continue;
                if (desktop.windows[i].id == desktop.primary_shell_id)
                    continue;
                mark_window_dirty(&desktop.windows[i]);
            }
        }

        for (int zi = 0; zi < desktop.z_count; zi++) {
            VWindow* win = get_window(desktop.z_order[zi]);
            if (win && win->visible && !win->minimized && win->tick_handler)
                win->tick_handler(win);
        }

        had_dirty = desktop.dirty;
        drag_active = vdesk_any_drag_active();
        if (had_dirty) {
            int dirty_x = desktop.dirty_x1;
            int dirty_y = desktop.dirty_y1;
            int dirty_w = desktop.dirty_x2 - desktop.dirty_x1;
            int dirty_h = desktop.dirty_y2 - desktop.dirty_y1;
            int dirty_area = dirty_w * dirty_h;
            int screen_area = desktop.screen_w * desktop.screen_h;
            int promote_full = should_promote_full_present(dirty_x, dirty_y,
                                                           dirty_w, dirty_h,
                                                           dirty_area,
                                                           screen_area);
            desktop.metrics.last_dirty_x = desktop.dirty_x1;
            desktop.metrics.last_dirty_y = desktop.dirty_y1;
            desktop.metrics.last_dirty_w = dirty_w;
            desktop.metrics.last_dirty_h = dirty_h;
            desktop.dirty = 0;
            desktop.metrics.dirty_frames++;

            render_start = timer_ticks();
            /*
             * Render the normal desktop stack through a VESA clip rectangle.
             * This keeps each app renderer simple while avoiding full-screen
             * memory writes and bus copies for ordinary input/cursor updates.
             */
            vesa_set_clip(dirty_x, dirty_y, dirty_w, dirty_h);
            render_desktop();
            render_desktop_icons();

            for (int zi = 0; zi < desktop.z_count; zi++) {
                VWindow* win = get_window(desktop.z_order[zi]);
                if (win) render_window(win);
            }

            render_taskbar();
            render_toasts();
            render_start_menu();
            render_context_menu();
            render_rename_modal();
            /* Cursor sprite is the LAST step of the composite so it
             * always lands on top of every other surface. */
            render_mouse();
            vesa_clear_clip();
            /* HUD is drawn outside the dirty clip so a freeze mid-frame still
             * leaves the previous trail readable; also redraw into dirty area. */
            vdesk_render_debug_hud();
            desktop.metrics.render_ticks = timer_ticks() - render_start;

            swap_start = timer_ticks();
            /*
             * Publish through the display framework so hardware-specific
             * drivers can wait for vblank or page-flip. Generic VM/firmware
             * framebuffers favor dirty rects for speed; full-frame publish is
             * reserved for near-fullscreen damage or reliable vblank paths.
             * During drag, oneshot present finishes the AABB in one blit to
             * avoid Win95-style horizontal tear bands.
             */
            g_vdesk_last_dirty_w = dirty_w;
            g_vdesk_last_dirty_h = dirty_h;
            /*
             * Oneshot for drag (existing) and focused app windows (Gob games /
             * Doom). Budgeted presents of tall app dirties show as horizontal
             * tear bands on UC scanout; shell-only typing stays budgeted.
             */
            display_present_set_oneshot(drag_active || vdesk_has_active_app_focus());
            vdesk_bc('P');
            if (promote_full) {
                display_present_note_promotion();
                display_present_frame();
            } else {
                display_present_rect(dirty_x, dirty_y, dirty_w, dirty_h);
                /* Finish budgeted shell/window presents this frame so help
                 * output and scrollback are not left half-drawn on screen. */
                {
                    int guard = 0;
                    while (display_present_has_pending() && guard++ < 64) {
                        int px, py, pw, ph;
                        display_present_consume_pending(&px, &py, &pw, &ph);
                        display_present_rect(px, py, pw, ph);
                    }
                }
            }
            /* HUD is drawn outside the dirty clip — publish its strip too. */
            if (g_vdesk_debug_hud) {
                int hy = desktop.screen_h - 18;
                if (hy < 0) hy = 0;
                display_present_set_oneshot(1);
                display_present_rect(0, hy, desktop.screen_w, 18);
            }
            vdesk_bc('p');
            display_present_set_oneshot(0);
            desktop.metrics.swap_ticks = timer_ticks() - swap_start;
        } else {
            desktop.metrics.skipped_frames++;
        }

        /*
         * Frame pacing. timer_ticks() runs at 100 Hz (1 tick = 10 ms).
         * Compute how much budget remains in the frame and sleep that
         * many ms. If we ran over budget, do NOT skip (the next frame
         * just starts immediately). When the mouse button is held we
         * shorten the budget to keep the drag responsive ("adaptive
         * pacing" the historical loop already did).
         */
        {
            uint32_t elapsed_ticks = timer_ticks() - frame_start;
            uint32_t elapsed_ms = elapsed_ticks * 10u;
            int budget = desktop.adaptive_pacing && desktop.mouse_buttons
                       ? 5 : desktop.target_frame_ms;
            if ((int)elapsed_ms < budget) {
                /*
                 * Busy-wait the remaining budget. Do NOT use timer_sleep()/HLT:
                 * after a keypress IRQ the PIC/IF state has historically left
                 * HLT parked forever on both VirtualBox and Braswell, which
                 * looked like a hard freeze (mouse IRQs queued but the pump
                 * never ran again). TSC/PIT busy-wait always returns.
                 */
                __asm__ volatile("sti" ::: "memory");
                timer_busy_wait_ms((uint32_t)(budget - (int)elapsed_ms));
            }
            desktop.metrics.sleep_ticks = (timer_ticks() - frame_start) - elapsed_ticks;
            desktop.metrics.frame_ticks = timer_ticks() - frame_start;
        }
        desktop.metrics.frame_count++;
        desktop.metrics.window_count = desktop.window_count;
        desktop.metrics.icon_count = desktop.icon_count;
        desktop.metrics.theme_mode = desktop.theme_mode;

        /* Top-right idle tick (F10 toggles). Runs even when dirty==0. */
        vdesk_render_alive_beacon();
        {
            const display_present_stats_t* ps = display_get_present_stats();
            desktop.metrics.present_count = ps->present_count;
            desktop.metrics.vblank_waits = ps->vblank_waits;
            desktop.metrics.vblank_misses = ps->vblank_misses;
            desktop.metrics.present_area = ps->last_dirty_area;
            desktop.metrics.present_mode = (int)ps->last_mode;
            desktop.metrics.promoted_frames = (int)ps->promoted_frames;
        }
        if (desktop.metrics.frame_ticks > 0) {
            desktop.metrics.fps = 100 / desktop.metrics.frame_ticks;
        }
        /* Prove the pump completed a full frame (incl. present) AFTER a key was
         * consumed. If serial shows [kbd]in=.. but never this line, the freeze
         * is in the post-keypress present path, not the ISR. */
        if (g_vdesk_kbd_trace_pending) {
            g_vdesk_kbd_trace_pending = 0;
            if (g_vdesk_debug_hud) kserial_note("[loop]alive-after-key\n");
        }
        /*
         * Freeze oracle, independent of the char/dirty path. The previous
         * oracle only proved liveness when a PRINTABLE key was consumed; a
         * non-char key (e.g. NumLock sc=0x45) left no serial trail, so a stale
         * screen was indistinguishable from a true hard freeze. Here we (a)
         * stamp every NEW keyboard IRQ so make+break both show (a live loop
         * sees the break IRQ arrive as i=2), and (b) emit a throttled heartbeat
         * so the log proves the pump keeps iterating even when nothing repaints.
         * If the capture stops right after the first [irq] with no heartbeat,
         * the freeze is real and in the IRQ-return/post-key path; if heartbeats
         * continue, the loop is alive and the bug is a missed repaint instead.
         */
        if (g_vdesk_debug_hud) {
            uint8_t hb_st = 0, hb_sc = 0;
            uint32_t hb_irqn = 0;
            keyboard_debug_snapshot((char*)0, 0, &hb_st, &hb_sc, &hb_irqn);
            if (hb_irqn != g_vdesk_last_irqn) {
                g_vdesk_last_irqn = hb_irqn;
                vdesk_serial_hex("[irq]n", hb_irqn);
                vdesk_serial_hex("[irq]sc", hb_sc);
            }
            if ((desktop.metrics.frame_count % 50u) == 0u) {
                uint32_t st64 = keyboard_live_status();
                uint32_t pmask = keyboard_pic_mask();
                vdesk_serial_hex("[loop]hb f", desktop.metrics.frame_count);
                /* st64 bit0=OBF (data waiting), bit5=AUX; pmask bit1=IRQ1 masked;
                 * isr bit1=IRQ1 stuck in-service; irr bit1=IRQ1 pending. */
                vdesk_serial_hex("[loop]st64", st64);
                vdesk_serial_hex("[loop]pmask", pmask);
                vdesk_serial_hex("[loop]isr", keyboard_pic_isr());
                vdesk_serial_hex("[loop]irr", keyboard_pic_irr());
            }
        }
        vdesk_bc('.');
    }

    vesa_boot_splash("Desktop stopped. Reboot or choose VGA safe mode.");
}

void vdesk_get_pointer(int* x, int* y, int* buttons) {
    if (x) *x = desktop.mouse_x;
    if (y) *y = desktop.mouse_y;
    if (buttons) *buttons = desktop.mouse_buttons;
}

void vdesk_file_drag_begin_ex(Directory* src_dir, const char* name, int is_dir) {
    if (!src_dir || !name || !name[0]) return;
    g_file_drag.active = 1;
    g_file_drag.src_dir = src_dir;
    g_file_drag.is_dir = is_dir ? 1 : 0;
    strncpy(g_file_drag.name, name, sizeof(g_file_drag.name) - 1);
    g_file_drag.name[sizeof(g_file_drag.name) - 1] = '\0';
    vdesk_set_status(is_dir ? "Dragging folder…" : "Dragging file…");
    vdesk_mark_dirty(desktop.mouse_x, desktop.mouse_y, 120, 48);
}

void vdesk_file_drag_begin(Directory* src_dir, const char* name) {
    vdesk_file_drag_begin_ex(src_dir, name, 0);
}

int vdesk_file_drag_active(void) {
    return g_file_drag.active;
}

void vdesk_file_drag_cancel(void) {
    g_file_drag.active = 0;
    g_file_drag.src_dir = NULL;
    g_file_drag.name[0] = '\0';
    g_file_drag.is_dir = 0;
}

static Directory* vdesk_find_dos_dir(void) {
    Directory* root = fs_get_cwd_dir();
    Directory* dos;
    while (root && root->parent) root = root->parent;
    if (!root) return NULL;
    dos = fs_dir_find_child(root, "Dos");
    if (!dos) {
        (void)fs_dir_create_dir(root, "Dos");
        dos = fs_dir_find_child(root, "Dos");
    }
    return dos;
}

static int vdesk_copy_file_between(Directory* src, const char* name, Directory* dst) {
    FileHandle* fh;
    uint8_t* buf;
    size_t cap = 65536;
    size_t total = 0, n;
    int rc;
    if (!src || !dst || !name) return -1;
    if (src == dst) return 0;
    fh = fs_dir_open(src, name);
    if (!fh) return -1;
    buf = (uint8_t*)kmalloc(cap);
    if (!buf) { fs_close(fh); return -1; }
    while ((n = fs_read(fh, buf + total, cap - total)) > 0) {
        total += n;
        if (total >= cap) break;
    }
    fs_close(fh);
    rc = fs_dir_write(dst, name, buf, total);
    kfree(buf);
    return rc;
}

int vdesk_file_drag_drop(void) {
    Directory* dst = NULL;
    int idx;
    if (!g_file_drag.active || !g_file_drag.src_dir || !g_file_drag.name[0])
        return 0;

    idx = icon_at(desktop.mouse_x, desktop.mouse_y);
    if (idx >= 0 && idx < desktop.icon_count) {
        VDesktopIcon* icon = &desktop.icons[idx];
        if (icon->kind == VICON_FOLDER) {
            Directory* desk = fs_get_desktop_dir();
            if (desk && strcmp(icon->filename, "Dos") == 0) {
                dst = fs_dir_find_child(desk, "Dos");
                if (!dst) dst = vdesk_find_dos_dir();
            } else if (desk) {
                dst = fs_dir_find_child(desk, icon->filename);
            }
        }
    }
    if (!dst) {
        /* Drop on empty desktop / any miss → /Dos */
        dst = vdesk_find_dos_dir();
    }
    if (!dst) {
        vdesk_notify("Drag & Drop", "No Dos folder");
        vdesk_file_drag_cancel();
        return 1;
    }
    if (vdesk_copy_file_between(g_file_drag.src_dir, g_file_drag.name, dst) == 0) {
        vdesk_notify("Drag & Drop", "Copied to /Dos");
        vdesk_refresh_desktop_items(1);
        vdesk_mark_full_dirty();
    } else {
        vdesk_notify("Drag & Drop", "Copy failed");
    }
    vdesk_file_drag_cancel();
    return 1;
}
