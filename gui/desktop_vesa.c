#include "vesa_window.h"
#include "../drivers/video/vesa.h"
#include "../drivers/video/vga.h"
#include "../drivers/keyboard/keyboard.h"
#include "../drivers/mouse/mouse.h"
#include "../drivers/input/input.h"
#include "../drivers/timer/timer.h"
#include "../drivers/timer/softclock.h"
#include "../lib/string.h"
#include "../lib/memory.h"
#include "../shell/shell.h"
#include "../fs/filesystem.h"
#include "../taskmgr/process.h"
#include "../userspace/userspace.h"
#include "../kernel.h"
#include "../drivers/usb/host/host.h"
#include "../drivers/diagnostics/driver_log.h"
#include "../drivers/video/display.h"

static int has_suffix(const char* name, const char* suffix) {
    int nl = (int)strlen(name);
    int sl = (int)strlen(suffix);
    if (sl > nl) return 0;
    return strcmp(name + nl - sl, suffix) == 0;
}

/* Map a clicked cell (row/col in the visible text area) to a buffer index.
 * Matches the wrap rules used by the text editor / IDE renderers. */
static int text_pos_at_cell(const char* text, int len, int cols,
                            int scroll_row, int cell_row, int cell_col) {
    int target = scroll_row + cell_row;
    int text_row = 0;
    int text_col = 0;
    int i;

    if (!text || len < 0) return 0;
    if (cols < 1) cols = 1;
    if (cell_col < 0) cell_col = 0;
    if (cell_row < 0) return 0;
    if (target < 0) return 0;

    for (i = 0; i < len; i++) {
        if (text_row == target && text_col >= cell_col)
            return i;
        if (text[i] == '\n') {
            if (text_row == target)
                return i;
            text_row++;
            text_col = 0;
            continue;
        }
        text_col++;
        if (text_col >= cols) {
            if (text_row == target)
                return i + 1;
            text_row++;
            text_col = 0;
        }
    }
    return len;
}

#define IN_RECT(px, py, rx, ry, rw, rh) \
    ((px) >= (rx) && (px) < (rx) + (rw) && (py) >= (ry) && (py) < (ry) + (rh))

/* Draw a simple raised push-button with a centered label. */
static void draw_button(int x, int y, int w, int h, const char* label, int active) {
    const VTheme* t = vdesk_get_theme();
    uint32_t bg = active ? t->accent : t->button_bg;
    vdesk_draw_rect(x, y, w, h, bg);
    vdesk_draw_border(x, y, w, h, t->border_light, t->border_dark);
    int ty = y + (h - 16) / 2;
    vdesk_draw_text(x + 6, ty, label, active ? VCOLOR_WHITE : t->text, bg);
}

static int client_width(VWindow* win) {
    return win->width - BORDER_SIZE * 2;
}

/* ---- Shell app ---- */
#define SHELL_MAX_LINES 60
#define SHELL_LINE_LEN  256
#define SHELL_HISTORY_SIZE 16
#define SHELL_PAD_X      6
#define SHELL_PAD_Y      4
#define SHELL_SCROLLBAR_W 10
#define SHELL_SCROLLBAR_GAP 4
#define SHELL_LINE_OUTPUT 0
#define SHELL_LINE_INPUT  1
#define SHELL_LINE_MUTED  2

typedef struct {
    char lines[SHELL_MAX_LINES][SHELL_LINE_LEN];
    uint8_t line_kind[SHELL_MAX_LINES];
    int line_count;
    int scroll;
    char history[SHELL_HISTORY_SIZE][SHELL_LINE_LEN];
    int history_next;
    int history_count;
    int history_nav_offset;
    char saved_input[SHELL_LINE_LEN];
    char pending_output[SHELL_LINE_LEN];
    int pending_len;
    char input[SHELL_LINE_LEN];
    int input_len;
    int input_pos;
} ShellApp;

static void open_explorer_window(void);
static void open_editor_file(const char* filename, Directory* dir);
static void open_editor_window(void);
static void open_paint_window(void);
static void open_paint_file(const char* filename, Directory* dir);
static void open_taskmgr_window(void);
static void open_display_settings_window(void);
static void open_ide_window(void);
static void open_ide_file(const char* filename, Directory* dir);

static int shell_max_int(int a, int b) { return a > b ? a : b; }
static int shell_min_int(int a, int b) { return a < b ? a : b; }
static int shell_clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void shell_set_input(ShellApp* sa, const char* text) {
    int i = 0;
    if (!sa) return;
    while (text && text[i] && i < SHELL_LINE_LEN - 1) {
        sa->input[i] = text[i];
        i++;
    }
    sa->input[i] = '\0';
    sa->input_len = i;
    sa->input_pos = i;
}

static int shell_history_index_from_offset(ShellApp* sa, int offset) {
    return (sa->history_next - 1 - offset + SHELL_HISTORY_SIZE) % SHELL_HISTORY_SIZE;
}

static void shell_history_store(ShellApp* sa, const char* cmd) {
    if (!sa || !cmd || !cmd[0]) return;
    strncpy(sa->history[sa->history_next], cmd, SHELL_LINE_LEN - 1);
    sa->history[sa->history_next][SHELL_LINE_LEN - 1] = '\0';
    sa->history_next = (sa->history_next + 1) % SHELL_HISTORY_SIZE;
    if (sa->history_count < SHELL_HISTORY_SIZE) sa->history_count++;
    sa->history_nav_offset = -1;
    sa->saved_input[0] = '\0';
}

static void shell_write_line_kind(ShellApp* sa, const char* text, uint8_t kind) {
    if (!sa || !text) return;
    if (sa->line_count < SHELL_MAX_LINES) {
        strncpy(sa->lines[sa->line_count], text, SHELL_LINE_LEN - 1);
        sa->lines[sa->line_count][SHELL_LINE_LEN - 1] = '\0';
        sa->line_kind[sa->line_count] = kind;
        sa->line_count++;
    } else {
        for (int i = 1; i < SHELL_MAX_LINES; i++) {
            strcpy(sa->lines[i - 1], sa->lines[i]);
            sa->line_kind[i - 1] = sa->line_kind[i];
        }
        strncpy(sa->lines[SHELL_MAX_LINES - 1], text, SHELL_LINE_LEN - 1);
        sa->lines[SHELL_MAX_LINES - 1][SHELL_LINE_LEN - 1] = '\0';
        sa->line_kind[SHELL_MAX_LINES - 1] = kind;
    }
    if (sa->scroll < sa->line_count - 1)
        sa->scroll = sa->line_count - 1;
}

static void shell_write_line(ShellApp* sa, const char* text) {
    shell_write_line_kind(sa, text, SHELL_LINE_OUTPUT);
}

static void shell_capture_write(const char* text, void* ctx) {
    ShellApp* sa = (ShellApp*)ctx;
    if (!sa || !text) return;
    for (int i = 0; text[i]; i++) {
        if (text[i] == '\n') {
            sa->pending_output[sa->pending_len] = '\0';
            shell_write_line(sa, sa->pending_output);
            sa->pending_len = 0;
            sa->pending_output[0] = '\0';
        } else if (text[i] != '\r') {
            if (sa->pending_len < SHELL_LINE_LEN - 1) {
                sa->pending_output[sa->pending_len++] = text[i];
                sa->pending_output[sa->pending_len] = '\0';
            }
        }
    }
}

/* Called from install progress lines so the shell window paints mid-command. */
void install_ui_yield(void) {
    vdesk_pump_one_frame();
}

/*
 * Build the GooberOS shell prompt prefix into out (NUL-terminated), returning
 * its length. At the filesystem root it is "GooberOS>"; inside subdirectories it
 * carries the path, e.g. "GooberOS[DESKTOP/TEST3/DOCUMENTS]>". The leading slash
 * from fs_get_cwd() ("/DESKTOP/TEST3/...") is dropped so the brackets hold a
 * clean relative path. A trailing space is NOT added (callers add it).
 */
static int shell_build_prompt_prefix(char* out, int out_max) {
    static const char base[] = "GooberOS";
    const char* cwd = fs_get_cwd();
    int n = 0;
    for (int i = 0; base[i] && n < out_max - 1; i++) out[n++] = base[i];
    if (cwd && cwd[0] && !(cwd[0] == '/' && cwd[1] == '\0')) {
        const char* p = (cwd[0] == '/') ? cwd + 1 : cwd;
        if (n < out_max - 1) out[n++] = '[';
        for (int i = 0; p[i] && n < out_max - 1; i++) out[n++] = p[i];
        if (n < out_max - 1) out[n++] = ']';
    }
    if (n < out_max - 1) out[n++] = '>';
    out[n] = '\0';
    return n;
}

static void shell_do_exec(ShellApp* sa) {
    char cmd[SHELL_LINE_LEN];
    if (sa->input_len <= 0) return;

    strncpy(cmd, sa->input, SHELL_LINE_LEN - 1);
    cmd[SHELL_LINE_LEN - 1] = '\0';
    shell_history_store(sa, cmd);

    {
        char prompt[SHELL_LINE_LEN];
        int pi = shell_build_prompt_prefix(prompt, SHELL_LINE_LEN);
        if (pi < SHELL_LINE_LEN - 1) prompt[pi++] = ' ';
        for (int i = 0; cmd[i] && pi < SHELL_LINE_LEN - 1; i++)
            prompt[pi++] = cmd[i];
        prompt[pi] = '\0';
        shell_write_line_kind(sa, prompt, SHELL_LINE_INPUT);
    }

    if (strcmp(cmd, "clear") == 0 || strcmp(cmd, "cls") == 0) {
        sa->line_count = 0;
        sa->scroll = 0;
    } else if (strcmp(cmd, "taskview") == 0) {
        open_taskmgr_window();
        vdesk_tile_window(NULL);
        shell_write_line_kind(sa, "Opened native VESA Task Manager.", SHELL_LINE_MUTED);
    } else if (strncmp(cmd, "edit ", 5) == 0) {
        open_editor_file(cmd + 5, fs_get_cwd_dir());
        vdesk_tile_window(NULL);
        shell_write_line_kind(sa, "Opened native VESA Text Editor.", SHELL_LINE_MUTED);
    } else if (strcmp(cmd, "edit") == 0) {
        open_editor_window();
        vdesk_tile_window(NULL);
        shell_write_line_kind(sa, "Opened native VESA Text Editor.", SHELL_LINE_MUTED);
    } else if (strcmp(cmd, "files") == 0 || strcmp(cmd, "explorer") == 0) {
        open_explorer_window();
        vdesk_tile_window(NULL);
        shell_write_line_kind(sa, "Opened native VESA File Explorer.", SHELL_LINE_MUTED);
    } else if (strcmp(cmd, "paint") == 0) {
        open_paint_window();
        vdesk_tile_window(NULL);
        shell_write_line_kind(sa, "Opened native VESA Paint.", SHELL_LINE_MUTED);
    } else if (strcmp(cmd, "settings") == 0 || strcmp(cmd, "gui") == 0) {
        open_display_settings_window();
        vdesk_tile_window(NULL);
        shell_write_line_kind(sa, "Opened VESA Display Settings.", SHELL_LINE_MUTED);
    } else if (strcmp(cmd, "snakeGame.exe") == 0 ||
               strcmp(cmd, "cubeDip.exe") == 0 ||
               strcmp(cmd, "pong.exe") == 0 ||
               strcmp(cmd, "doom.exe") == 0) {
        shell_write_line_kind(sa, "Legacy VGA App surface removed; use native VESA apps.", SHELL_LINE_MUTED);
    } else {
        shell_set_redirect(shell_capture_write, NULL, sa);
        execute_command(cmd);
        shell_clear_redirect();
        if (sa->pending_len > 0) {
            sa->pending_output[sa->pending_len] = '\0';
            shell_write_line(sa, sa->pending_output);
            sa->pending_len = 0;
            sa->pending_output[0] = '\0';
        }
    }

    sa->input_len = 0;
    sa->input_pos = 0;
    sa->input[0] = '\0';
    sa->history_nav_offset = -1;
}

static void shell_draw_text_clipped(int x, int y, const char* text,
                                    int max_cols, color_t fg, color_t bg) {
    char buf[SHELL_LINE_LEN];
    int i = 0;
    if (max_cols <= 0) return;
    if (max_cols >= SHELL_LINE_LEN) max_cols = SHELL_LINE_LEN - 1;
    while (text && text[i] && i < max_cols) {
        buf[i] = text[i];
        i++;
    }
    buf[i] = '\0';
    vdesk_draw_text(x, y, buf, fg, bg);
}

static void shell_render(VWindow* win, int cx, int cy, int cw, int ch) {
    ShellApp* sa = (ShellApp*)win->user_data;
    const VTheme* theme = vdesk_get_theme();
    if (!sa) return;

    int char_w = 8;
    int char_h = 16;
    int text_x = cx + SHELL_PAD_X;
    int text_y = cy + SHELL_PAD_Y;
    int text_w = cw - SHELL_PAD_X * 2 - SHELL_SCROLLBAR_W - SHELL_SCROLLBAR_GAP;
    int text_h = ch - SHELL_PAD_Y * 2;
    int cols = text_w / char_w;
    int rows = text_h / char_h;
    if (cols < 2 || rows < 2) return;

    color_t shell_bg = vdesk_shell_bg_color();
    color_t shell_output = vdesk_shell_output_color();
    color_t shell_input = vdesk_shell_input_color();
    color_t shell_muted = vdesk_shell_muted_color();

    vdesk_draw_rect(cx, cy, cw, ch, shell_bg);

    int draw_lines = rows - 1;
    int max_scroll = shell_max_int(0, sa->line_count - draw_lines);
    if (sa->scroll > sa->line_count - draw_lines)
        sa->scroll = sa->line_count - draw_lines;
    if (sa->scroll < 0) sa->scroll = 0;

    for (int r = 0; r < draw_lines; r++) {
        int src = sa->scroll + r;
        if (src >= 0 && src < sa->line_count) {
            color_t fg = shell_output;
            if (sa->line_kind[src] == SHELL_LINE_INPUT) fg = shell_input;
            else if (sa->line_kind[src] == SHELL_LINE_MUTED) fg = shell_muted;
            shell_draw_text_clipped(text_x, text_y + r * char_h,
                                    sa->lines[src], cols,
                                    fg, shell_bg);
        }
    }

    {
        int sb_x = cx + cw - SHELL_PAD_X - SHELL_SCROLLBAR_W;
        int sb_y = text_y;
        int sb_h = draw_lines * char_h;
        int thumb_h = sb_h;
        int thumb_y = sb_y;

        if (sb_h > 0) {
            vdesk_draw_rect(sb_x, sb_y, SHELL_SCROLLBAR_W, sb_h, theme->border_dark);
            vdesk_draw_border(sb_x, sb_y, SHELL_SCROLLBAR_W, sb_h,
                              theme->border_light, shell_bg);
            if (sa->line_count > draw_lines) {
                thumb_h = (sb_h * draw_lines) / shell_max_int(1, sa->line_count);
                if (thumb_h < 16) thumb_h = 16;
                if (thumb_h > sb_h) thumb_h = sb_h;
                thumb_y = sb_y + (sa->scroll * shell_max_int(1, sb_h - thumb_h)) /
                                  shell_max_int(1, max_scroll);
            }
            vdesk_draw_rect(sb_x + 2, thumb_y + 2,
                            SHELL_SCROLLBAR_W - 4, thumb_h - 4,
                            theme->accent);
        }
    }

    {
        char prompt[SHELL_LINE_LEN];
        int base_len = shell_build_prompt_prefix(prompt, SHELL_LINE_LEN);
        if (base_len < SHELL_LINE_LEN - 1) prompt[base_len++] = ' ';
        int pi = base_len;
        for (int i = 0; i < sa->input_len && pi < SHELL_LINE_LEN - 1; i++)
            prompt[pi++] = sa->input[i];
        prompt[pi] = '\0';

        int cursor_display = sa->input_pos + base_len;
        if (cursor_display < pi) {
            char old = prompt[cursor_display];
            prompt[cursor_display] = '_';
            shell_draw_text_clipped(text_x, text_y + (rows - 1) * char_h,
                                    prompt, cols, shell_input, shell_bg);
            prompt[cursor_display] = old;
        } else {
            shell_draw_text_clipped(text_x, text_y + (rows - 1) * char_h,
                                    prompt, cols, shell_input, shell_bg);
        }
    }
}

static void shell_key(VWindow* win, char key) {
    ShellApp* sa = (ShellApp*)win->user_data;
    if (!sa) return;

    if ((unsigned char)key == KEY_BACKSPACE) {
        if (sa->input_pos > 0 && sa->input_len > 0) {
            for (int i = sa->input_pos - 1; i < sa->input_len; i++)
                sa->input[i] = sa->input[i + 1];
            sa->input_len--;
            sa->input_pos--;
        }
        return;
    }
    if ((unsigned char)key == KEY_LEFT) {
        if (sa->input_pos > 0) sa->input_pos--;
        return;
    }
    if ((unsigned char)key == KEY_RIGHT) {
        if (sa->input_pos < sa->input_len) sa->input_pos++;
        return;
    }
    if ((unsigned char)key == KEY_UP) {
        if (sa->history_count > 0) {
            if (sa->history_nav_offset < 0) {
                strncpy(sa->saved_input, sa->input, SHELL_LINE_LEN - 1);
                sa->saved_input[SHELL_LINE_LEN - 1] = '\0';
                sa->history_nav_offset = 0;
            } else if (sa->history_nav_offset < sa->history_count - 1) {
                sa->history_nav_offset++;
            }
            shell_set_input(sa, sa->history[shell_history_index_from_offset(sa, sa->history_nav_offset)]);
        }
        return;
    }
    if ((unsigned char)key == KEY_DOWN) {
        if (sa->history_count > 0 && sa->history_nav_offset >= 0) {
            sa->history_nav_offset--;
            if (sa->history_nav_offset < 0) {
                shell_set_input(sa, sa->saved_input);
            } else {
                shell_set_input(sa, sa->history[shell_history_index_from_offset(sa, sa->history_nav_offset)]);
            }
        }
        return;
    }
    if (key == '\r' || key == '\n') {
        shell_do_exec(sa);
        return;
    }
    if ((unsigned char)key >= 32 && (unsigned char)key <= 126) {
        if (sa->input_len < SHELL_LINE_LEN - 1) {
            for (int i = sa->input_len; i >= sa->input_pos; i--)
                sa->input[i + 1] = sa->input[i];
            sa->input[sa->input_pos] = key;
            sa->input_len++;
            sa->input_pos++;
            sa->history_nav_offset = -1;
        }
    }
}

static void shell_scroll(VWindow* win, int amount) {
    ShellApp* sa = (ShellApp*)win->user_data;
    if (!sa) return;
    sa->scroll -= amount;
    if (sa->scroll < 0) sa->scroll = 0;
    if (sa->scroll > sa->line_count - 1) sa->scroll = sa->line_count - 1;
}

static void shell_click(VWindow* win, int client_x, int client_y) {
    ShellApp* sa = (ShellApp*)win->user_data;
    int cw = client_width(win);
    int ch = win->height - TITLEBAR_HEIGHT - BORDER_SIZE * 2;
    int char_h = 16;
    int rows = (ch - SHELL_PAD_Y * 2) / char_h;
    int draw_lines = rows - 1;
    int sb_x = cw - SHELL_PAD_X - SHELL_SCROLLBAR_W;
    int sb_y = SHELL_PAD_Y;
    int sb_h = draw_lines * char_h;
    int max_scroll;

    if (!sa || draw_lines <= 0 || sb_h <= 0) return;
    if (client_x < sb_x || client_x >= sb_x + SHELL_SCROLLBAR_W) return;
    if (client_y < sb_y || client_y >= sb_y + sb_h) return;

    max_scroll = shell_max_int(0, sa->line_count - draw_lines);
    if (max_scroll <= 0) {
        sa->scroll = 0;
        return;
    }
    sa->scroll = ((client_y - sb_y) * max_scroll) / shell_max_int(1, sb_h - 1);
    sa->scroll = shell_clamp_int(sa->scroll, 0, max_scroll);
}

static ShellApp* create_shell_app(void) {
    ShellApp* sa = (ShellApp*)kmalloc(sizeof(ShellApp));
    if (!sa) return NULL;
    memset(sa, 0, sizeof(ShellApp));
    sa->history_nav_offset = -1;
    shell_write_line_kind(sa, "GooberOS VESA Shell", SHELL_LINE_MUTED);
    shell_write_line_kind(sa, "F1 shell | F2 files | F3 edit | F4 tasks | F9 appearance", SHELL_LINE_MUTED);
    shell_write_line(sa, "Type 'help' for commands.");
    return sa;
}

/* ---- System Info app ---- */
static void sysinfo_render(VWindow* win, int cx, int cy, int cw, int ch) {
    (void)win;
    const VTheme* theme = vdesk_get_theme();
    const VDeskMetrics* metrics = vdesk_get_metrics();
    int row = cy + 4;
    vdesk_draw_rect(cx, cy, cw, ch, theme->client_bg);
    vdesk_draw_text(cx + 4, row, "GooberOS x86", theme->text, theme->client_bg);
    row += 16;
    vdesk_draw_text(cx + 4, row, "VESA Display Manager", theme->text, theme->client_bg);
    row += 20;
    {
        char buf[32];
        itoa(vesa_get_width(), buf, 10);
        vdesk_draw_text(cx + 4, row, "Width: ", theme->text, theme->client_bg);
        vdesk_draw_text(cx + 68, row, buf, theme->accent, theme->client_bg);
    }
    row += 16;
    {
        char buf[32];
        itoa(vesa_get_height(), buf, 10);
        vdesk_draw_text(cx + 4, row, "Height:", theme->text, theme->client_bg);
        vdesk_draw_text(cx + 68, row, buf, theme->accent, theme->client_bg);
    }
    row += 16;
    {
        char buf[32];
        itoa((int)vesa_get_pitch(), buf, 10);
        vdesk_draw_text(cx + 4, row, "Pitch: ", theme->text, theme->client_bg);
        vdesk_draw_text(cx + 68, row, buf, theme->accent, theme->client_bg);
    }
    row += 16;
    {
        char buf[32];
        itoa((int)vesa_get_bpp(), buf, 10);
        vdesk_draw_text(cx + 4, row, "BPP:   ", theme->text, theme->client_bg);
        vdesk_draw_text(cx + 68, row, buf, theme->accent, theme->client_bg);
    }
    row += 16;
    vdesk_draw_text(cx + 4, row, "Buffer:", theme->text, theme->client_bg);
    vdesk_draw_text(cx + 76, row, vesa_has_backbuffer() ? "Back" : "Direct",
                    theme->accent, theme->client_bg);
    row += 16;
    vdesk_draw_text(cx + 4, row, "USB HC:", theme->text, theme->client_bg);
    vdesk_draw_text(cx + 76, row, usb_host_controller_name(), theme->accent, theme->client_bg);
    row += 16;
    {
        char buf[32];
        itoa((int)metrics->fps, buf, 10);
        vdesk_draw_text(cx + 4, row, "FPS:   ", theme->text, theme->client_bg);
        vdesk_draw_text(cx + 68, row, buf, theme->accent, theme->client_bg);
    }
    row += 16;
    {
        char buf[32];
        itoa((int)metrics->frame_ticks, buf, 10);
        vdesk_draw_text(cx + 4, row, "Ticks: ", theme->text, theme->client_bg);
        vdesk_draw_text(cx + 68, row, buf, theme->accent, theme->client_bg);
    }
    row += 16;
    vdesk_draw_text(cx + 4, row, "Present:", theme->text, theme->client_bg);
    vdesk_draw_text(cx + 84, row,
                    display_present_mode_name((display_present_mode_t)metrics->present_mode),
                    theme->accent, theme->client_bg);
    row += 16;
    {
        char buf[32];
        itoa((int)metrics->vblank_waits, buf, 10);
        vdesk_draw_text(cx + 4, row, "VBlank:", theme->text, theme->client_bg);
        vdesk_draw_text(cx + 76, row, buf, theme->accent, theme->client_bg);
    }
    row += 16;
    {
        char buf[32];
        itoa((int)metrics->vblank_misses, buf, 10);
        vdesk_draw_text(cx + 4, row, "Misses:", theme->text, theme->client_bg);
        vdesk_draw_text(cx + 76, row, buf, theme->accent, theme->client_bg);
    }
    row += 16;
    vdesk_draw_text(cx + 4, row, "Pointer:", theme->text, theme->client_bg);
    if (metrics->usb_pointer_active)
        vdesk_draw_text(cx + 84, row, "USB", theme->accent, theme->client_bg);
    else if (metrics->i2c_touchpad_active)
        vdesk_draw_text(cx + 84, row, "I2C pad", theme->accent, theme->client_bg);
    else
        vdesk_draw_text(cx + 84, row, "PS/2", theme->accent, theme->client_bg);
    row += 16;
    vdesk_draw_text(cx + 4, row, "Look:  ", theme->text, theme->client_bg);
    vdesk_draw_text(cx + 68, row, vdesk_get_theme_name(), theme->accent, theme->client_bg);
    row += 16;
    {
        char buf[32];
        itoa((int)metrics->input_events, buf, 10);
        vdesk_draw_text(cx + 4, row, "Events:", theme->text, theme->client_bg);
        vdesk_draw_text(cx + 68, row, buf, theme->accent, theme->client_bg);
    }
    row += 16;
    vdesk_draw_text(cx + 4, row, "VESA:", theme->text, theme->client_bg);
    vdesk_draw_text(cx + 68, row, vesa_boot_status(), theme->text_muted, theme->client_bg);
    row += 16;
    vdesk_draw_text(cx + 4, row, "Mode:", theme->text, theme->client_bg);
    vdesk_draw_text(cx + 68, row, kernel_boot_request(), theme->accent, theme->client_bg);
    row += 16;
    {
        char buf[16];
        itoa((int)kernel_fb_type(), buf, 10);
        vdesk_draw_text(cx + 4, row, "FB Type:", theme->text, theme->client_bg);
        vdesk_draw_text(cx + 84, row, buf, theme->accent, theme->client_bg);
    }
}

/* ---- VESA Task Manager app ---- */
typedef struct {
    int selected_pid;
    int status; /* 0 none, 1 killed, 2 protected, 3 not found */
} TaskmgrApp;

#define TASKMGR_HEADER_H 36
#define TASKMGR_LIST_TOP (TASKMGR_HEADER_H + 24) /* after column labels */
#define TASKMGR_ROW_H 18
#define TASKMGR_FOOTER_H 64

static void taskmgr_button_rect(int cw, int* bx, int* by, int* bw, int* bh) {
    *bw = 96;
    *bh = 22;
    *bx = cw - *bw - 8;
    *by = 6;
}

static void taskmgr_render(VWindow* win, int cx, int cy, int cw, int ch) {
    TaskmgrApp* app = (TaskmgrApp*)win->user_data;
    const VTheme* theme = vdesk_get_theme();
    const VDeskMetrics* metrics = vdesk_get_metrics();
    process_entry_t* table = get_kernel_process_table();
    int total = get_kernel_process_count();
    int row = cy + 8;
    int max_rows = (ch - TASKMGR_LIST_TOP - TASKMGR_FOOTER_H) / TASKMGR_ROW_H;
    int drawn = 0;
    int alt = 0;

    vdesk_draw_rect(cx, cy, cw, ch, theme->client_bg);

    /* Header strip */
    vdesk_draw_rect(cx, cy, cw, TASKMGR_HEADER_H, theme->title_active_bg);
    vdesk_draw_text(cx + 8, cy + 10, "Task Manager", theme->text, theme->title_active_bg);

    {
        int bx, by, bw, bh;
        taskmgr_button_rect(cw, &bx, &by, &bw, &bh);
        draw_button(cx + bx, cy + by, bw, bh, "End Task", 1);
    }

    row = cy + TASKMGR_HEADER_H + 4;
    vdesk_draw_text(cx + 8, row, "PID", theme->text_muted, theme->client_bg);
    vdesk_draw_text(cx + 48, row, "Name", theme->text_muted, theme->client_bg);
    vdesk_draw_text(cx + 140, row, "Kind", theme->text_muted, theme->client_bg);
    vdesk_draw_text(cx + 220, row, "State", theme->text_muted, theme->client_bg);
    vdesk_draw_text(cx + 290, row, "Mem", theme->text_muted, theme->client_bg);
    row += 14;
    vdesk_draw_rect(cx + 6, row, cw - 12, 1, theme->border_dark);
    row += 6;

    for (int i = 0; i < total && drawn < max_rows; i++) {
        char pid[12];
        char mem[16];
        int sel;
        uint32_t bg;
        uint32_t fg;
        if (!table[i].active) continue;
        sel = (app && table[i].pid == app->selected_pid);
        bg = sel ? theme->accent
                 : (alt ? theme->button_bg : theme->client_bg);
        fg = sel ? VCOLOR_WHITE : theme->text;
        vdesk_draw_rect(cx + 6, row - 2, cw - 12, TASKMGR_ROW_H, bg);
        itoa(table[i].pid, pid, 10);
        itoa((int)table[i].memory_kb, mem, 10);
        vdesk_draw_text(cx + 8, row, pid, fg, bg);
        vdesk_draw_text(cx + 48, row, table[i].name, fg, bg);
        vdesk_draw_text(cx + 140, row, process_kind_name(table[i].kind), fg, bg);
        vdesk_draw_text(cx + 220, row, process_state_name(table[i].state), fg, bg);
        vdesk_draw_text(cx + 290, row, mem, fg, bg);
        row += TASKMGR_ROW_H;
        drawn++;
        alt = !alt;
    }

    if (app && app->status) {
        const char* msg = "";
        if (app->status == 1) msg = "Process terminated.";
        else if (app->status == 2) msg = "Cannot kill kernel process.";
        else if (app->status == 3) msg = "Process not found.";
        vdesk_draw_text(cx + 8, cy + ch - TASKMGR_FOOTER_H - 4, msg,
                        app->status == 2 ? theme->accent : theme->text_muted,
                        theme->client_bg);
    }

    /* Footer metrics */
    vdesk_draw_rect(cx, cy + ch - TASKMGR_FOOTER_H, cw, TASKMGR_FOOTER_H, theme->taskbar_bg);
    row = cy + ch - TASKMGR_FOOTER_H + 6;
    {
        char buf[16];
        itoa((int)metrics->window_count, buf, 10);
        vdesk_draw_text(cx + 8, row, "Windows", theme->text_muted, theme->taskbar_bg);
        vdesk_draw_text(cx + 80, row, buf, theme->accent, theme->taskbar_bg);
        itoa((int)metrics->render_ticks, buf, 10);
        vdesk_draw_text(cx + 120, row, "Render", theme->text_muted, theme->taskbar_bg);
        vdesk_draw_text(cx + 184, row, buf, theme->accent, theme->taskbar_bg);
    }
    row += 16;
    {
        char buf[16];
        itoa((int)metrics->swap_ticks, buf, 10);
        vdesk_draw_text(cx + 8, row, "Swap", theme->text_muted, theme->taskbar_bg);
        vdesk_draw_text(cx + 56, row, buf, theme->accent, theme->taskbar_bg);
        itoa((int)metrics->input_events, buf, 10);
        vdesk_draw_text(cx + 120, row, "Input", theme->text_muted, theme->taskbar_bg);
        vdesk_draw_text(cx + 176, row, buf, theme->accent, theme->taskbar_bg);
    }
    row += 16;
    vdesk_draw_text(cx + 8, row, "Click a row, then End Task",
                    theme->text_muted, theme->taskbar_bg);
}

static void taskmgr_click(VWindow* win, int lx, int ly) {
    TaskmgrApp* app = (TaskmgrApp*)win->user_data;
    if (!app) return;
    int cw = client_width(win);
    process_entry_t* table = get_kernel_process_table();
    int total = get_kernel_process_count();

    int bx, by, bw, bh;
    taskmgr_button_rect(cw, &bx, &by, &bw, &bh);
    if (IN_RECT(lx, ly, bx, by, bw, bh)) {
        if (app->selected_pid > 0) {
            int r = terminate_process(app->selected_pid);
            if (r == PROCESS_KILL_PROTECTED) {
                app->status = 2;
            } else if (r == 1) {
                vdesk_close_windows_by_pid(app->selected_pid);
                app->status = 1;
                app->selected_pid = -1;
            } else {
                app->status = 3;
            }
        }
        return;
    }

    if (ly >= TASKMGR_LIST_TOP) {
        int disp = (ly - TASKMGR_LIST_TOP) / TASKMGR_ROW_H;
        int count = 0;
        for (int i = 0; i < total; i++) {
            if (!table[i].active) continue;
            if (count == disp) {
                app->selected_pid = table[i].pid;
                app->status = 0;
                return;
            }
            count++;
        }
    }
}

/* ---- Display Settings app ---- */
/* Theme-toggle button rect in client coordinates. */
#define SETTINGS_BTN_X 4
#define SETTINGS_BTN_W 150
#define SETTINGS_BTN_H 22
#define SETTINGS_OUTPUT_BTN_Y 190
#define SETTINGS_INPUT_BTN_Y 216

static void display_settings_render(VWindow* win, int cx, int cy, int cw, int ch) {
    (void)win;
    const VTheme* theme = vdesk_get_theme();
    const VDeskMetrics* metrics = vdesk_get_metrics();
    vdesk_draw_rect(cx, cy, cw, ch, theme->client_bg);
    vdesk_draw_text(cx + 4, cy + 4, "Display Settings", theme->text, theme->client_bg);
    vdesk_draw_text(cx + 4, cy + 24, "Appearance:", theme->text, theme->client_bg);
    vdesk_draw_text(cx + 76, cy + 24, vdesk_get_theme_name(), theme->accent, theme->client_bg);
    {
        char buf[32];
        itoa((int)vesa_get_width(), buf, 10);
        vdesk_draw_text(cx + 4, cy + 48, "Width:", theme->text, theme->client_bg);
        vdesk_draw_text(cx + 76, cy + 48, buf, theme->accent, theme->client_bg);
        itoa((int)vesa_get_height(), buf, 10);
        vdesk_draw_text(cx + 4, cy + 64, "Height:", theme->text, theme->client_bg);
        vdesk_draw_text(cx + 76, cy + 64, buf, theme->accent, theme->client_bg);
        itoa((int)vesa_get_pitch(), buf, 10);
        vdesk_draw_text(cx + 4, cy + 80, "Pitch:", theme->text, theme->client_bg);
        vdesk_draw_text(cx + 76, cy + 80, buf, theme->accent, theme->client_bg);
        itoa((int)metrics->fps, buf, 10);
        vdesk_draw_text(cx + 4, cy + 96, "FPS:", theme->text, theme->client_bg);
        vdesk_draw_text(cx + 76, cy + 96, buf, theme->accent, theme->client_bg);
    }
    draw_button(cx + SETTINGS_BTN_X, cy + 118, SETTINGS_BTN_W, SETTINGS_BTN_H,
                "Cycle Look", 0);
    vdesk_draw_text(cx + 4, cy + 150, "Click button, F2, or F9 cycles appearance.",
                    theme->text_muted, theme->client_bg);
    vdesk_draw_text(cx + 4, cy + 166, "F1 snaps focus back to GooberShell.",
                    theme->text_muted, theme->client_bg);
    vdesk_draw_text(cx + 4, cy + 182, "Original keeps blue input + green output.",
                    theme->text_muted, theme->client_bg);
    vdesk_draw_text(cx + 4, cy + 206, "Shell output:", theme->text, theme->client_bg);
    vdesk_draw_text(cx + 112, cy + 206, "sample", vdesk_shell_output_color(), theme->client_bg);
    draw_button(cx + 200, cy + SETTINGS_OUTPUT_BTN_Y, SETTINGS_BTN_W, SETTINGS_BTN_H,
                "Output Color", 0);
    vdesk_draw_text(cx + 4, cy + 232, "Shell cursor/input:", theme->text, theme->client_bg);
    vdesk_draw_text(cx + 144, cy + 232, "> sample_", vdesk_shell_input_color(), theme->client_bg);
    draw_button(cx + 200, cy + SETTINGS_INPUT_BTN_Y, SETTINGS_BTN_W, SETTINGS_BTN_H,
                "Input Color", 0);
    vdesk_draw_text(cx + 4, cy + 258, "F6 output color | F7 input/cursor color",
                    theme->text_muted, theme->client_bg);
}

static void display_settings_key(VWindow* win, char key) {
    (void)win;
    if ((unsigned char)key == KEY_F2) vdesk_toggle_theme();
    else if ((unsigned char)key == KEY_F6) vdesk_cycle_shell_output_color();
    else if ((unsigned char)key == KEY_F7) vdesk_cycle_shell_input_color();
}

static void display_settings_click(VWindow* win, int lx, int ly) {
    (void)win;
    if (IN_RECT(lx, ly, SETTINGS_BTN_X, 118, SETTINGS_BTN_W, SETTINGS_BTN_H))
        vdesk_toggle_theme();
    else if (IN_RECT(lx, ly, 200, SETTINGS_OUTPUT_BTN_Y, SETTINGS_BTN_W, SETTINGS_BTN_H))
        vdesk_cycle_shell_output_color();
    else if (IN_RECT(lx, ly, 200, SETTINGS_INPUT_BTN_Y, SETTINGS_BTN_W, SETTINGS_BTN_H))
        vdesk_cycle_shell_input_color();
}

/* ---- System Settings (tabbed) ---- */
#define SYSSET_NAV_W 120
#define SYSSET_TAB_H 22
#define SYSSET_TAB_COUNT 5
/* 0 General, 1 Appearance, 2 Mouse, 3 Input, 4 About */

typedef struct {
    int tab;
    int pointer_speed; /* 1..10 preference (scaling stub) */
    int hide_shell;    /* auto-hide GooberShell on startup */
} SystemSettingsApp;

static int g_pointer_speed_pref = 5;
static int g_hide_shell_pref = 0;

static int cfg_parse_int(const char* p) {
    int v = 0;
    int any = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
        any = 1;
    }
    return any ? v : -1;
}

static void system_settings_try_save(SystemSettingsApp* app) {
    char body[128];
    softclock_t clk;
    int n = 0;
    Directory* prev;

    if (app) {
        g_pointer_speed_pref = app->pointer_speed;
        g_hide_shell_pref = app->hide_shell ? 1 : 0;
    }
    softclock_get(&clk);

    /* speed=N\nhide_shell=0|1\nclock=YYYY-MM-DD HH:MM:SS\n */
    body[n++] = 's'; body[n++] = 'p'; body[n++] = 'e'; body[n++] = 'e';
    body[n++] = 'd'; body[n++] = '=';
    if (g_pointer_speed_pref >= 10) {
        body[n++] = '1';
        body[n++] = '0';
    } else {
        body[n++] = (char)('0' + g_pointer_speed_pref);
    }
    body[n++] = '\n';
    body[n++] = 'h'; body[n++] = 'i'; body[n++] = 'd'; body[n++] = 'e';
    body[n++] = '_'; body[n++] = 's'; body[n++] = 'h'; body[n++] = 'e';
    body[n++] = 'l'; body[n++] = 'l'; body[n++] = '=';
    body[n++] = g_hide_shell_pref ? '1' : '0';
    body[n++] = '\n';
    body[n++] = 'c'; body[n++] = 'l'; body[n++] = 'o'; body[n++] = 'c';
    body[n++] = 'k'; body[n++] = '=';
    {
        char tmp[20];
        int i;
        softclock_format(tmp, (int)sizeof(tmp));
        /* extend with :SS */
        for (i = 0; tmp[i] && n + 1 < (int)sizeof(body); i++)
            body[n++] = tmp[i];
        body[n++] = ':';
        body[n++] = (char)('0' + (clk.second / 10));
        body[n++] = (char)('0' + (clk.second % 10));
    }
    body[n++] = '\n';
    body[n] = '\0';

    prev = fs_get_cwd_dir();
    (void)fs_create_dir("Config");
    if (fs_change_dir("/") == 0 && fs_change_dir("Config") == 0) {
        (void)fs_write("settings.cfg", (const uint8_t*)body, (size_t)n);
        while (fs_cd_up() == 0) { }
    }
    if (prev) fs_set_current_dir(prev);
}

static void system_settings_load(void) {
    FileHandle* fh;
    char buf[160];
    size_t n = 0, got;
    Directory* prev = fs_get_cwd_dir();

    if (fs_change_dir("/") != 0) return;
    if (fs_change_dir("Config") != 0) {
        if (prev) fs_set_current_dir(prev);
        else while (fs_cd_up() == 0) { }
        return;
    }
    fh = fs_open("settings.cfg");
    if (!fh) {
        while (fs_cd_up() == 0) { }
        if (prev) fs_set_current_dir(prev);
        return;
    }
    while (n + 1 < sizeof(buf) &&
           (got = fs_read(fh, (uint8_t*)buf + n, sizeof(buf) - 1 - n)) > 0)
        n += got;
    fs_close(fh);
    buf[n] = '\0';
    while (fs_cd_up() == 0) { }
    if (prev) fs_set_current_dir(prev);

    {
        char* p = buf;
        while (*p) {
            if (p[0] == 's' && p[1] == 'p' && p[2] == 'e' && p[3] == 'e' &&
                p[4] == 'd' && p[5] == '=') {
                int v = cfg_parse_int(p + 6);
                if (v >= 1 && v <= 10) g_pointer_speed_pref = v;
            } else if (strncmp(p, "hide_shell=", 11) == 0) {
                g_hide_shell_pref = (p[11] == '1') ? 1 : 0;
            } else if (strncmp(p, "clock=", 6) == 0) {
                /* YYYY-MM-DD HH:MM:SS */
                const char* c = p + 6;
                softclock_t clk;
                int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
                if (c[0] >= '0' && c[4] == '-' && c[7] == '-' && c[10] == ' ' &&
                    c[13] == ':' && c[16] == ':') {
                    y = (c[0] - '0') * 1000 + (c[1] - '0') * 100 +
                        (c[2] - '0') * 10 + (c[3] - '0');
                    mo = (c[5] - '0') * 10 + (c[6] - '0');
                    d = (c[8] - '0') * 10 + (c[9] - '0');
                    h = (c[11] - '0') * 10 + (c[12] - '0');
                    mi = (c[14] - '0') * 10 + (c[15] - '0');
                    s = (c[17] - '0') * 10 + (c[18] - '0');
                    clk.year = y; clk.month = mo; clk.day = d;
                    clk.hour = h; clk.minute = mi; clk.second = s;
                    softclock_set(&clk);
                }
            }
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
        }
    }
}

static void system_settings_render(VWindow* win, int cx, int cy, int cw, int ch) {
    SystemSettingsApp* app = (SystemSettingsApp*)win->user_data;
    const VTheme* theme = vdesk_get_theme();
    const VDeskMetrics* metrics = vdesk_get_metrics();
    int tab = app ? app->tab : 0;
    int content_x = cx + SYSSET_NAV_W + 8;
    int content_w = cw - SYSSET_NAV_W - 16;
    int row;
    const char* tabs[SYSSET_TAB_COUNT] = {
        "General", "Appearance", "Mouse", "Input", "About"
    };
    int i;

    vdesk_draw_rect(cx, cy, cw, ch, theme->client_bg);
    vdesk_draw_rect(cx, cy, SYSSET_NAV_W, ch, theme->taskbar_bg);
    vdesk_draw_text(cx + 8, cy + 8, "Settings", theme->text, theme->taskbar_bg);

    for (i = 0; i < SYSSET_TAB_COUNT; i++) {
        int ty = cy + 28 + i * (SYSSET_TAB_H + 4);
        int sel = (i == tab);
        uint32_t bg = sel ? theme->accent : theme->taskbar_bg;
        uint32_t fg = sel ? VCOLOR_WHITE : theme->text;
        if (sel) vdesk_draw_rect(cx + 4, ty, SYSSET_NAV_W - 8, SYSSET_TAB_H, bg);
        vdesk_draw_text(cx + 10, ty + 4, tabs[i], fg, bg);
    }

    row = cy + 12;
    if (tab == 0) {
        softclock_t clk;
        char cbuf[20];
        softclock_get(&clk);
        softclock_format(cbuf, (int)sizeof(cbuf));
        vdesk_draw_text(content_x, row, "General", theme->text, theme->client_bg);
        row += 22;
        vdesk_draw_text(content_x, row, "Auto-hide GooberShell on startup:",
                        theme->text, theme->client_bg);
        row += 22;
        draw_button(content_x, row, 120, SETTINGS_BTN_H,
                    (app && app->hide_shell) ? "Enabled" : "Disabled",
                    app && app->hide_shell);
        row += 34;
        vdesk_draw_text(content_x, row, "Date / time:", theme->text, theme->client_bg);
        vdesk_draw_text(content_x + 104, row, cbuf, theme->accent, theme->client_bg);
        row += 22;
        draw_button(content_x, row, 36, SETTINGS_BTN_H, "D-", 0);
        draw_button(content_x + 40, row, 36, SETTINGS_BTN_H, "D+", 0);
        draw_button(content_x + 84, row, 36, SETTINGS_BTN_H, "H-", 0);
        draw_button(content_x + 124, row, 36, SETTINGS_BTN_H, "H+", 0);
        draw_button(content_x + 164, row, 36, SETTINGS_BTN_H, "M-", 0);
        draw_button(content_x + 204, row, 36, SETTINGS_BTN_H, "M+", 0);
        row += 30;
        draw_button(content_x, row, 100, SETTINGS_BTN_H, "Apply Clock", 0);
        row += 28;
        vdesk_draw_text(content_x, row, "Software clock — saved to Config/settings.cfg",
                        theme->text_muted, theme->client_bg);
        (void)content_w;
        (void)clk;
    } else if (tab == 1) {
        vdesk_draw_text(content_x, row, "Appearance", theme->text, theme->client_bg);
        row += 22;
        vdesk_draw_text(content_x, row, "Look:", theme->text, theme->client_bg);
        vdesk_draw_text(content_x + 56, row, vdesk_get_theme_name(),
                        theme->accent, theme->client_bg);
        row += 24;
        draw_button(content_x, row, SETTINGS_BTN_W, SETTINGS_BTN_H, "Cycle Look", 0);
        row += 30;
        draw_button(content_x, row, SETTINGS_BTN_W, SETTINGS_BTN_H, "Shell Output", 0);
        row += 30;
        draw_button(content_x, row, SETTINGS_BTN_W, SETTINGS_BTN_H, "Shell Input", 0);
        row += 28;
        vdesk_draw_text(content_x, row, "F2 / F9 cycle look. F6/F7 shell colors.",
                        theme->text_muted, theme->client_bg);
    } else if (tab == 2) {
        const char* ptr = "PS/2 or none";
        char speed[8];
        if (metrics->usb_pointer_active) ptr = "USB HID";
        else if (metrics->i2c_touchpad_active) ptr = "I2C touchpad";
        vdesk_draw_text(content_x, row, "Mouse / Pointer", theme->text, theme->client_bg);
        row += 22;
        vdesk_draw_text(content_x, row, "Source:", theme->text, theme->client_bg);
        vdesk_draw_text(content_x + 72, row, ptr, theme->accent, theme->client_bg);
        row += 20;
        vdesk_draw_text(content_x, row, "Controller:", theme->text, theme->client_bg);
        vdesk_draw_text(content_x + 96, row, usb_host_controller_name(),
                        theme->accent, theme->client_bg);
        row += 24;
        vdesk_draw_text(content_x, row, "Pointer speed (stored preference):",
                        theme->text, theme->client_bg);
        row += 20;
        itoa(app ? app->pointer_speed : g_pointer_speed_pref, speed, 10);
        vdesk_draw_text(content_x, row, speed, theme->accent, theme->client_bg);
        draw_button(content_x + 40, row - 2, 36, SETTINGS_BTN_H, "-", 0);
        draw_button(content_x + 84, row - 2, 36, SETTINGS_BTN_H, "+", 0);
        row += 28;
        vdesk_draw_text(content_x, row, "Scaling stub — preference saved when writable.",
                        theme->text_muted, theme->client_bg);
    } else if (tab == 3) {
        vdesk_draw_text(content_x, row, "Input", theme->text, theme->client_bg);
        row += 22;
        vdesk_draw_text(content_x, row, "Keyboard layout: US QWERTY (default)",
                        theme->text, theme->client_bg);
        row += 20;
        vdesk_draw_text(content_x, row, "F1  Focus GooberShell",
                        theme->text_muted, theme->client_bg);
        row += 16;
        vdesk_draw_text(content_x, row, "F2  Cycle appearance",
                        theme->text_muted, theme->client_bg);
        row += 16;
        vdesk_draw_text(content_x, row, "F4  Keyboard enable / diagnostics",
                        theme->text_muted, theme->client_bg);
        row += 16;
        vdesk_draw_text(content_x, row, "F6/F7  Shell output / input color",
                        theme->text_muted, theme->client_bg);
        row += 16;
        vdesk_draw_text(content_x, row, "Esc  Fall back to shell if unfocused",
                        theme->text_muted, theme->client_bg);
        row += 24;
        vdesk_draw_text(content_x, row, "Future: layout picker and remaps.",
                        theme->text_muted, theme->client_bg);
    } else {
        vdesk_draw_text(content_x, row, "About GooberOS", theme->text, theme->client_bg);
        row += 22;
        vdesk_draw_text(content_x, row, "Desktop:", theme->text, theme->client_bg);
        vdesk_draw_text(content_x + 80, row, "VESA window manager",
                        theme->accent, theme->client_bg);
        row += 18;
        vdesk_draw_text(content_x, row, "USB host:", theme->text, theme->client_bg);
        vdesk_draw_text(content_x + 80, row, usb_host_controller_name(),
                        theme->accent, theme->client_bg);
        row += 18;
        vdesk_draw_text(content_x, row, "Boot mode:", theme->text, theme->client_bg);
        vdesk_draw_text(content_x + 88, row, kernel_boot_request(),
                        theme->accent, theme->client_bg);
        row += 18;
        vdesk_draw_text(content_x, row, "VESA:", theme->text, theme->client_bg);
        vdesk_draw_text(content_x + 56, row, vesa_boot_status(),
                        theme->text_muted, theme->client_bg);
        row += 24;
        {
            char buf[16];
            itoa((int)metrics->window_count, buf, 10);
            vdesk_draw_text(content_x, row, "Windows:", theme->text, theme->client_bg);
            vdesk_draw_text(content_x + 80, row, buf, theme->accent, theme->client_bg);
        }
    }
}

static void system_settings_key(VWindow* win, char key) {
    SystemSettingsApp* app = (SystemSettingsApp*)win->user_data;
    if ((unsigned char)key == KEY_F2) vdesk_toggle_theme();
    else if ((unsigned char)key == KEY_F6) vdesk_cycle_shell_output_color();
    else if ((unsigned char)key == KEY_F7) vdesk_cycle_shell_input_color();
    else if ((unsigned char)key == KEY_LEFT && app && app->tab > 0) app->tab--;
    else if ((unsigned char)key == KEY_RIGHT && app && app->tab < SYSSET_TAB_COUNT - 1)
        app->tab++;
    else if ((unsigned char)key == KEY_UP && app && app->tab > 0) app->tab--;
    else if ((unsigned char)key == KEY_DOWN && app && app->tab < SYSSET_TAB_COUNT - 1)
        app->tab++;
}

static void system_settings_click(VWindow* win, int lx, int ly) {
    SystemSettingsApp* app = (SystemSettingsApp*)win->user_data;
    int content_x = SYSSET_NAV_W + 8;
    int i;

    if (!app) return;

    for (i = 0; i < SYSSET_TAB_COUNT; i++) {
        int ty = 28 + i * (SYSSET_TAB_H + 4);
        if (IN_RECT(lx, ly, 4, ty, SYSSET_NAV_W - 8, SYSSET_TAB_H)) {
            app->tab = i;
            return;
        }
    }

    if (app->tab == 0) {
        int row = 12 + 22 + 22;
        softclock_t clk;
        if (IN_RECT(lx, ly, content_x, row, 120, SETTINGS_BTN_H)) {
            app->hide_shell = !app->hide_shell;
            system_settings_try_save(app);
            return;
        }
        row += 34 + 22;
        softclock_get(&clk);
        if (IN_RECT(lx, ly, content_x, row, 36, SETTINGS_BTN_H)) {
            clk.day--;
            if (clk.day < 1) clk.day = 1;
            softclock_set(&clk);
        } else if (IN_RECT(lx, ly, content_x + 40, row, 36, SETTINGS_BTN_H)) {
            clk.day++;
            softclock_set(&clk);
        } else if (IN_RECT(lx, ly, content_x + 84, row, 36, SETTINGS_BTN_H)) {
            clk.hour = (clk.hour + 23) % 24;
            softclock_set(&clk);
        } else if (IN_RECT(lx, ly, content_x + 124, row, 36, SETTINGS_BTN_H)) {
            clk.hour = (clk.hour + 1) % 24;
            softclock_set(&clk);
        } else if (IN_RECT(lx, ly, content_x + 164, row, 36, SETTINGS_BTN_H)) {
            clk.minute = (clk.minute + 59) % 60;
            softclock_set(&clk);
        } else if (IN_RECT(lx, ly, content_x + 204, row, 36, SETTINGS_BTN_H)) {
            clk.minute = (clk.minute + 1) % 60;
            softclock_set(&clk);
        }
        row += 30;
        if (IN_RECT(lx, ly, content_x, row, 100, SETTINGS_BTN_H)) {
            system_settings_try_save(app);
            vdesk_notify("Settings", "Clock saved");
        }
    } else if (app->tab == 1) {
        int row = 12 + 22 + 24;
        if (IN_RECT(lx, ly, content_x, row, SETTINGS_BTN_W, SETTINGS_BTN_H))
            vdesk_toggle_theme();
        else if (IN_RECT(lx, ly, content_x, row + 30, SETTINGS_BTN_W, SETTINGS_BTN_H))
            vdesk_cycle_shell_output_color();
        else if (IN_RECT(lx, ly, content_x, row + 60, SETTINGS_BTN_W, SETTINGS_BTN_H))
            vdesk_cycle_shell_input_color();
    } else if (app->tab == 2) {
        int row = 12 + 22 + 20 + 24 + 20;
        if (IN_RECT(lx, ly, content_x + 40, row - 2, 36, SETTINGS_BTN_H)) {
            if (app->pointer_speed > 1) {
                app->pointer_speed--;
                system_settings_try_save(app);
            }
        } else if (IN_RECT(lx, ly, content_x + 84, row - 2, 36, SETTINGS_BTN_H)) {
            if (app->pointer_speed < 10) {
                app->pointer_speed++;
                system_settings_try_save(app);
            }
        }
    }
}

/* ---- File Explorer app ---- */
#define EXPLORER_SIDEBAR_W 100
#define EXPLORER_TOOLBAR_H 26
#define EXPLORER_STATUS_H  20
#define EXPLORER_ROW_H     18
#define EXPLORER_HIST_MAX  8
#define EXPLORER_DBLCLICK_TICKS 40

typedef struct {
    int selected;
    int folder_seq;
    int file_seq;
    int bitmap_seq;
    int scroll;
    Directory* hist[EXPLORER_HIST_MAX];
    int hist_count;
    int last_click_index;
    uint32_t last_click_tick;
} ExplorerApp;

static int explorer_cwd_is_desktop(void) {
    Directory* desk = fs_get_desktop_dir();
    Directory* cwd = fs_get_cwd_dir();
    if (!desk || !cwd) return 0;
    if (desk == cwd) return 1;
    if (desk->fat32 && cwd->fat32 && desk->fat_cluster == cwd->fat_cluster)
        return 1;
    return 0;
}

static void explorer_after_create(int ok, const char* ok_msg, const char* err_msg) {
    if (!ok) {
        vdesk_set_status(err_msg);
        return;
    }
    vdesk_set_status(ok_msg);
    if (explorer_cwd_is_desktop())
        vdesk_refresh_desktop_items(1);
}

static void explorer_create_folder(ExplorerApp* app) {
    char name[32];
    char num[12];
    Directory* dir = fs_get_cwd_dir();
    if (!dir || !app) return;
    strcpy(name, "NewFolder");
    itoa(app->folder_seq++, num, 10);
    strcat(name, num);
    explorer_after_create(fs_dir_create_dir(dir, name) == 0,
                          "Folder created", "Failed to create folder");
}

static void explorer_create_text(ExplorerApp* app) {
    char name[32];
    char num[12];
    Directory* dir = fs_get_cwd_dir();
    if (!dir || !app) return;
    strcpy(name, "NewFile");
    itoa(app->file_seq++, num, 10);
    strcat(name, num);
    strcat(name, ".txt");
    explorer_after_create(fs_dir_create(dir, name) == 0,
                          "Text file created", "Failed to create text file");
}

static void explorer_create_bitmap(ExplorerApp* app) {
    char name[32];
    char num[12];
    uint8_t pixels[32 * 32];
    Directory* dir = fs_get_cwd_dir();
    int i;
    if (!dir || !app) return;
    for (i = 0; i < 32 * 32; i++) pixels[i] = 0;
    strcpy(name, "Artwork");
    itoa(app->bitmap_seq++, num, 10);
    strcat(name, num);
    strcat(name, ".gbm");
    explorer_after_create(fs_dir_write(dir, name, pixels, sizeof(pixels)) == 0,
                          "Bitmap created", "Failed to create bitmap");
}

static void explorer_push_hist(ExplorerApp* app) {
    Directory* cur;
    if (!app) return;
    cur = fs_get_cwd_dir();
    if (!cur) return;
    if (app->hist_count >= EXPLORER_HIST_MAX) {
        int i;
        for (i = 0; i < EXPLORER_HIST_MAX - 1; i++)
            app->hist[i] = app->hist[i + 1];
        app->hist_count = EXPLORER_HIST_MAX - 1;
    }
    app->hist[app->hist_count++] = cur;
}

static void explorer_go_back(ExplorerApp* app) {
    if (!app || app->hist_count <= 0) return;
    app->hist_count--;
    fs_set_current_dir(app->hist[app->hist_count]);
    app->selected = 0;
    app->scroll = 0;
}

static void explorer_go_up(ExplorerApp* app) {
    Directory* cur;
    if (!app) return;
    cur = fs_get_cwd_dir();
    if (!cur || !cur->parent) return;
    explorer_push_hist(app);
    fs_cd_up();
    app->selected = 0;
    app->scroll = 0;
}

static void explorer_goto_dir(ExplorerApp* app, Directory* dir) {
    if (!app || !dir) return;
    if (dir == fs_get_cwd_dir()) return;
    explorer_push_hist(app);
    fs_set_current_dir(dir);
    app->selected = 0;
    app->scroll = 0;
}

static void explorer_goto_root(ExplorerApp* app) {
    if (!app) return;
    explorer_push_hist(app);
    if (fs_change_dir("/") != 0 && app->hist_count > 0)
        app->hist_count--;
    app->selected = 0;
    app->scroll = 0;
}

static void explorer_goto_docs(ExplorerApp* app) {
    Directory* before;
    if (!app) return;
    before = fs_get_cwd_dir();
    explorer_push_hist(app);
    if (fs_change_dir("/") == 0 && fs_change_dir("docs") == 0) {
        app->selected = 0;
        app->scroll = 0;
        return;
    }
    /* Restore and drop failed push. */
    if (before) fs_set_current_dir(before);
    if (app->hist_count > 0) app->hist_count--;
    vdesk_set_status("docs folder not found");
}

static void explorer_open_selected(ExplorerApp* app) {
    const Directory* dir = fs_get_current_dir();
    if (!app || !dir) return;
    if (app->selected < (int)dir->child_count) {
        explorer_push_hist(app);
        fs_change_dir(dir->children[app->selected].name);
        app->selected = 0;
        app->scroll = 0;
    } else {
        int file_idx = app->selected - (int)dir->child_count;
        if (file_idx >= 0 && file_idx < (int)dir->file_count) {
            const char* name = dir->files[file_idx].name;
            Directory* cwd = fs_get_cwd_dir();
            if (has_suffix(name, ".txt")) {
                open_editor_file(name, cwd);
            } else if (has_suffix(name, ".gbm")) {
                open_paint_file(name, cwd);
            } else if (has_suffix(name, ".gc")) {
                open_ide_file(name, cwd);
            } else if (has_suffix(name, ".gob")) {
                Directory* prev = fs_get_cwd_dir();
                if (cwd) fs_set_current_dir(cwd);
                if (gob_exec(name) != 0)
                    vdesk_notify("GooberC", "Failed to run .gob");
                else
                    vdesk_notify("GooberC", "Finished running .gob");
                if (prev) fs_set_current_dir(prev);
            } else {
                vdesk_notify("File Explorer", "No app for this file type");
            }
        }
    }
}

static void explorer_render(VWindow* win, int cx, int cy, int cw, int ch) {
    ExplorerApp* app = (ExplorerApp*)win->user_data;
    const Directory* dir = fs_get_current_dir();
    const VTheme* theme = vdesk_get_theme();
    const int side_w = (cw > EXPLORER_SIDEBAR_W + 160) ? EXPLORER_SIDEBAR_W : 0;
    const int list_x = cx + side_w + (side_w ? 4 : 0);
    const int list_w = cw - (list_x - cx);
    const int toolbar_y = cy;
    const int list_top = cy + EXPLORER_TOOLBAR_H + 4;
    const int list_bottom = cy + ch - EXPLORER_STATUS_H - 4;
    int max_rows = (list_bottom - list_top) / EXPLORER_ROW_H;
    int total = dir ? (int)(dir->child_count + dir->file_count) : 0;
    int row = 0;
    int index = 0;
    int scroll = app ? app->scroll : 0;

    if (max_rows < 1) max_rows = 1;
    if (app && app->selected < scroll) app->scroll = app->selected;
    if (app && app->selected >= scroll + max_rows)
        app->scroll = app->selected - max_rows + 1;
    if (app && app->scroll < 0) app->scroll = 0;
    scroll = app ? app->scroll : 0;

    vdesk_draw_rect(cx, cy, cw, ch, theme->client_bg);

    /* Quick Access sidebar */
    if (side_w > 0) {
        int sy = cy + 4;
        vdesk_draw_rect(cx, cy, side_w, ch - EXPLORER_STATUS_H, theme->taskbar_bg);
        vdesk_draw_text(cx + 6, sy, "Quick access", theme->text_muted, theme->taskbar_bg);
        sy += 20;
        draw_button(cx + 6, sy, side_w - 12, 22, "Home", 0);
        sy += 26;
        draw_button(cx + 6, sy, side_w - 12, 22, "Desktop", 0);
        sy += 26;
        draw_button(cx + 6, sy, side_w - 12, 22, "docs", 0);
    }

    /* Toolbar: Back / Up + path */
    vdesk_draw_rect(list_x, toolbar_y, list_w, EXPLORER_TOOLBAR_H, theme->title_active_bg);
    draw_button(list_x + 4, toolbar_y + 2, 44, 22, "Back", 0);
    draw_button(list_x + 52, toolbar_y + 2, 36, 22, "Up", 0);
    vdesk_draw_text(list_x + 96, toolbar_y + 5, fs_get_cwd(),
                    theme->accent, theme->title_active_bg);

    if (dir) {
        for (int i = 0; i < (int)dir->child_count; i++, index++) {
            int y;
            int sel;
            uint32_t bg, fg;
            if (index < scroll) continue;
            if (row >= max_rows) break;
            y = list_top + row * EXPLORER_ROW_H;
            sel = (app && app->selected == index);
            bg = sel ? theme->accent : theme->client_bg;
            fg = sel ? VCOLOR_WHITE : theme->text;
            if (sel) vdesk_draw_rect(list_x, y, list_w, EXPLORER_ROW_H - 1, bg);
            vdesk_draw_text(list_x + 6, y + 1, "DIR",
                            sel ? VCOLOR_WHITE : theme->accent, bg);
            vdesk_draw_text(list_x + 38, y + 1, dir->children[i].name, fg, bg);
            row++;
        }
        for (int i = 0; i < (int)dir->file_count; i++, index++) {
            int y;
            int sel;
            uint32_t bg, fg;
            if (index < scroll) continue;
            if (row >= max_rows) break;
            y = list_top + row * EXPLORER_ROW_H;
            sel = (app && app->selected == index);
            bg = sel ? theme->accent : theme->client_bg;
            fg = sel ? VCOLOR_WHITE : theme->text;
            if (sel) vdesk_draw_rect(list_x, y, list_w, EXPLORER_ROW_H - 1, bg);
            vdesk_draw_text(list_x + 6, y + 1, "FILE",
                            sel ? VCOLOR_WHITE : theme->text_muted, bg);
            vdesk_draw_text(list_x + 46, y + 1, dir->files[i].name, fg, bg);
            row++;
        }
        (void)total;
    }

    vdesk_draw_rect(cx, cy + ch - EXPLORER_STATUS_H, cw, EXPLORER_STATUS_H, theme->taskbar_bg);
    vdesk_draw_text(cx + 6, cy + ch - EXPLORER_STATUS_H + 2,
                    "Bksp Up  Enter Open  N Folder  T Text  B Bitmap",
                    theme->text_muted, theme->taskbar_bg);
}

static void explorer_key(VWindow* win, char key) {
    ExplorerApp* app = (ExplorerApp*)win->user_data;
    const Directory* dir = fs_get_current_dir();
    if (!app || !dir) return;
    int total = (int)(dir->child_count + dir->file_count);
    if ((unsigned char)key == KEY_UP && app->selected > 0) app->selected--;
    else if ((unsigned char)key == KEY_DOWN && app->selected < total - 1) app->selected++;
    else if ((unsigned char)key == KEY_BACKSPACE) {
        explorer_go_up(app);
    } else if (key == 'n' || key == 'N') {
        explorer_create_folder(app);
    } else if (key == 't' || key == 'T') {
        explorer_create_text(app);
    } else if (key == 'b' || key == 'B') {
        explorer_create_bitmap(app);
    } else if (key == '\r' || key == '\n') {
        explorer_open_selected(app);
    }
}

static void explorer_click(VWindow* win, int client_x, int client_y) {
    ExplorerApp* app = (ExplorerApp*)win->user_data;
    const Directory* dir = fs_get_current_dir();
    const int cw = client_width(win);
    const int ch = win->height - TITLEBAR_HEIGHT - BORDER_SIZE * 2;
    const int side_w = (cw > EXPLORER_SIDEBAR_W + 160) ? EXPLORER_SIDEBAR_W : 0;
    const int list_x = side_w + (side_w ? 4 : 0);
    const int list_top = EXPLORER_TOOLBAR_H + 4;
    const int list_bottom = ch - EXPLORER_STATUS_H - 4;
    int max_rows = (list_bottom - list_top) / EXPLORER_ROW_H;
    int scroll = app ? app->scroll : 0;
    uint32_t now = timer_ticks();

    if (!app) return;
    if (max_rows < 1) max_rows = 1;

    /* Sidebar quick access (client-relative). */
    if (side_w > 0 && client_x < side_w) {
        int sy = 4 + 20;
        if (IN_RECT(client_x, client_y, 6, sy, side_w - 12, 22)) {
            explorer_goto_root(app);
            return;
        }
        sy += 26;
        if (IN_RECT(client_x, client_y, 6, sy, side_w - 12, 22)) {
            explorer_goto_dir(app, fs_get_desktop_dir());
            return;
        }
        sy += 26;
        if (IN_RECT(client_x, client_y, 6, sy, side_w - 12, 22)) {
            explorer_goto_docs(app);
            return;
        }
        return;
    }

    /* Toolbar Back / Up */
    if (client_y < EXPLORER_TOOLBAR_H) {
        int tx = list_x;
        if (IN_RECT(client_x, client_y, tx + 4, 2, 44, 22)) {
            explorer_go_back(app);
            return;
        }
        if (IN_RECT(client_x, client_y, tx + 52, 2, 36, 22)) {
            explorer_go_up(app);
            return;
        }
        return;
    }

    /* File list */
    if (dir && client_x >= list_x && client_y >= list_top && client_y < list_bottom) {
        int row = (client_y - list_top) / EXPLORER_ROW_H;
        int index = scroll + row;
        int total = (int)(dir->child_count + dir->file_count);
        if (row >= 0 && row < max_rows && index >= 0 && index < total) {
            int dbl = (index == app->last_click_index &&
                       (int32_t)(now - app->last_click_tick) <= EXPLORER_DBLCLICK_TICKS);
            app->selected = index;
            app->last_click_index = index;
            app->last_click_tick = now;
            if (dbl) explorer_open_selected(app);
        }
    }
}

/* ---- Text Editor app ---- */
#define EDITOR_MAX_TEXT 2048
typedef struct {
    char filename[32];
    Directory* dir;
    char text[EDITOR_MAX_TEXT];
    int len;
    int cursor;
    int scroll_row;
    int saved;
} EditorApp;

static void editor_render(VWindow* win, int cx, int cy, int cw, int ch) {
    EditorApp* ed = (EditorApp*)win->user_data;
    if (!ed) return;

    int char_w = 8;
    int char_h = 16;
    int cols = cw / char_w;
    int rows = ch / char_h;
    if (cols < 2 || rows < 2) return;

    vdesk_draw_rect(cx, cy, cw, ch, VCOLOR_WHITE);

    int text_row = 0;
    int text_col = 0;
    for (int i = 0; i <= ed->len; i++) {
        int display_row = text_row - ed->scroll_row;
        if (i == ed->cursor && display_row >= 0 && display_row < rows) {
            char buf[2] = { '_', '\0' };
            vdesk_draw_text(cx + 2 + text_col * char_w,
                           cy + display_row * char_h, buf,
                           VCOLOR_BLACK, VCOLOR_CYAN);
        }
        if (i == ed->len) break;

        char ch = ed->text[i];
        if (ch == '\n') {
            text_row++;
            text_col = 0;
            continue;
        }
        if (text_col >= cols) {
            text_row++;
            text_col = 0;
        }
        if (display_row >= 0 && display_row < rows && text_col < cols) {
            char buf[2] = { ch, '\0' };
            vdesk_draw_text(cx + 2 + text_col * char_w,
                           cy + display_row * char_h, buf,
                           VCOLOR_BLACK, VCOLOR_WHITE);
        }
        text_col++;
    }

    char status[32];
    itoa(ed->len, status, 10);
    vdesk_draw_text(cx + 2, cy + ch - char_h, "Chars: ", VCOLOR_BLACK, VCOLOR_GRAY);
    vdesk_draw_text(cx + 56, cy + ch - char_h, status, VCOLOR_BLACK, VCOLOR_GRAY);
    vdesk_draw_text(cx + 112, cy + ch - char_h, "F2=save", VCOLOR_BLACK, VCOLOR_GRAY);
    if (ed->saved)
        vdesk_draw_text(cx + 184, cy + ch - char_h, "saved", VCOLOR_BLACK, VCOLOR_GRAY);
}

static void editor_key(VWindow* win, char key) {
    EditorApp* ed = (EditorApp*)win->user_data;
    if (!ed) return;

    if ((unsigned char)key == KEY_BACKSPACE) {
        if (ed->cursor > 0) {
            for (int i = ed->cursor - 1; i < ed->len; i++)
                ed->text[i] = ed->text[i + 1];
            ed->len--;
            ed->cursor--;
        }
        return;
    }
    if ((unsigned char)key == KEY_LEFT) {
        if (ed->cursor > 0) ed->cursor--;
        return;
    }
    if ((unsigned char)key == KEY_RIGHT) {
        if (ed->cursor < ed->len) ed->cursor++;
        return;
    }
    if ((unsigned char)key == KEY_F2) {
        ed->text[ed->len] = '\0';
        Directory* dir = ed->dir ? ed->dir : fs_get_cwd_dir();
        fs_dir_write(dir, ed->filename[0] ? ed->filename : "Document.txt",
                     (uint8_t*)ed->text, ed->len);
        ed->saved = 1;
        return;
    }
    if (ed->len >= EDITOR_MAX_TEXT - 1) return;
    for (int i = ed->len; i >= ed->cursor; i--)
        ed->text[i + 1] = ed->text[i];
    ed->text[ed->cursor] = (key == '\r') ? '\n' : key;
    ed->len++;
    ed->cursor++;
    ed->saved = 0;
}

static void editor_scroll(VWindow* win, int amount) {
    EditorApp* ed = (EditorApp*)win->user_data;
    if (!ed) return;
    ed->scroll_row -= amount;
    if (ed->scroll_row < 0) ed->scroll_row = 0;
}

static void editor_click(VWindow* win, int lx, int ly) {
    EditorApp* ed = (EditorApp*)win->user_data;
    int cw, ch, cols, rows, status_h;
    int char_w = 8;
    int char_h = 16;
    int cell_col, cell_row;

    if (!ed) return;
    cw = client_width(win);
    ch = win->height - TITLEBAR_HEIGHT - BORDER_SIZE * 2;
    status_h = char_h;
    if (ly >= ch - status_h) return;
    cols = cw / char_w;
    rows = (ch - status_h) / char_h;
    if (cols < 1 || rows < 1) return;
    cell_col = (lx - 2) / char_w;
    cell_row = ly / char_h;
    if (cell_col < 0) cell_col = 0;
    if (cell_row < 0) cell_row = 0;
    if (cell_row >= rows) cell_row = rows - 1;
    ed->cursor = text_pos_at_cell(ed->text, ed->len, cols,
                                  ed->scroll_row, cell_row, cell_col);
}

/* ---- Paint app ---- */
#define PAINT_W 32
#define PAINT_H 32
typedef struct {
    char filename[32];
    Directory* dir;
    uint8_t pixels[PAINT_W * PAINT_H];
    int cursor_x;
    int cursor_y;
    int color;
    int saved;
} PaintApp;

static uint32_t paint_palette(int c) {
    static const uint32_t colors[] = {
        0x000000, 0xFFFFFF, 0xE53935, 0x43A047,
        0x1E88E5, 0xFDD835, 0x8E24AA, 0xFB8C00
    };
    return colors[c & 7];
}

static void paint_render(VWindow* win, int cx, int cy, int cw, int ch) {
    PaintApp* app = (PaintApp*)win->user_data;
    if (!app) return;
    const VTheme* theme = vdesk_get_theme();
    int scale = 6;
    int ox = cx + 8;
    int oy = cy + 8;
    vdesk_draw_rect(cx, cy, cw, ch, theme->client_bg);
    for (int y = 0; y < PAINT_H; y++) {
        for (int x = 0; x < PAINT_W; x++)
            vdesk_draw_rect(ox + x * scale, oy + y * scale, scale, scale,
                            paint_palette(app->pixels[y * PAINT_W + x]));
    }
    vdesk_draw_border(ox + app->cursor_x * scale, oy + app->cursor_y * scale,
                      scale, scale, theme->accent, theme->border_dark);
    vdesk_draw_text(cx + 210, cy + 8, "Goober Paint", theme->text, theme->client_bg);
    vdesk_draw_text(cx + 210, cy + 28, "Arrows move", theme->text_muted, theme->client_bg);
    vdesk_draw_text(cx + 210, cy + 44, "Space draws", theme->text_muted, theme->client_bg);
    vdesk_draw_text(cx + 210, cy + 60, "1-8 color", theme->text_muted, theme->client_bg);
    vdesk_draw_text(cx + 210, cy + 76, "F2 saves", theme->text_muted, theme->client_bg);
    if (app->saved)
        vdesk_draw_text(cx + 210, cy + 100, "saved", theme->accent, theme->client_bg);
}

static void paint_key(VWindow* win, char key) {
    PaintApp* app = (PaintApp*)win->user_data;
    if (!app) return;
    if ((unsigned char)key == KEY_LEFT && app->cursor_x > 0) app->cursor_x--;
    else if ((unsigned char)key == KEY_RIGHT && app->cursor_x < PAINT_W - 1) app->cursor_x++;
    else if ((unsigned char)key == KEY_UP && app->cursor_y > 0) app->cursor_y--;
    else if ((unsigned char)key == KEY_DOWN && app->cursor_y < PAINT_H - 1) app->cursor_y++;
    else if (key >= '1' && key <= '8') app->color = key - '1';
    else if (key == ' ') {
        app->pixels[app->cursor_y * PAINT_W + app->cursor_x] = (uint8_t)app->color;
        app->saved = 0;
    } else if ((unsigned char)key == KEY_F2) {
        Directory* dir = app->dir ? app->dir : fs_get_cwd_dir();
        fs_dir_write(dir, app->filename[0] ? app->filename : "Artwork.gbm",
                     app->pixels, sizeof(app->pixels));
        app->saved = 1;
    }
}

static void open_shell_window(int primary) {
    int top = vdesk_workspace_top();
    int bottom = vdesk_workspace_bottom();
    int w;
    int h;
    int x;
    int y;
    if (primary) {
        w = (int)vesa_get_width();
        h = bottom - top;
        x = 0;
        y = top;
    } else if (vdesk_desktop_experience_visible()) {
        w = 560;
        h = 340;
        x = 72;
        y = top + 36;
        if (w > (int)vesa_get_width() - 24) w = (int)vesa_get_width() - 24;
        if (h > bottom - top - 20) h = bottom - top - 20;
    } else {
        w = (int)vesa_get_width() - 24;
        h = bottom - top - 20;
        x = 12;
        y = top + 8;
    }
    if (w < 320) w = 320;
    if (h < 220) h = 220;
    VWindow* win = vdesk_create_window("GooberShell", x, y, w, h);
    if (win) {
        int pid = create_process("vesa-shell", 4);
        if (pid > 0) win->process_pid = pid;
        ShellApp* sa = create_shell_app();
        win->user_data = sa;
        win->render = shell_render;
        win->key_handler = shell_key;
        win->scroll_handler = shell_scroll;
        win->click_handler = shell_click;
        if (primary) {
            vdesk_set_primary_shell(win);
        }
    }
}

static void open_sysinfo_window(void) {
    VWindow* win = vdesk_create_window("System Info", 220, 40, 390, 310);
    if (win) {
        int pid = create_process("vesa-info", 1);
        if (pid > 0) win->process_pid = pid;
        win->render = sysinfo_render;
    }
}

static void open_explorer_window(void) {
    Directory* cwd = fs_get_cwd_dir();
    if (cwd) fs_dir_refresh(cwd);
    VWindow* win = vdesk_create_window("File Explorer", 70, 80, 480, 300);
    if (win) {
        int pid = create_process("vesa-files", 2);
        if (pid > 0) win->process_pid = pid;
        ExplorerApp* app = (ExplorerApp*)kmalloc(sizeof(ExplorerApp));
        if (app) {
            memset(app, 0, sizeof(ExplorerApp));
            win->user_data = app;
        }
        win->render = explorer_render;
        win->key_handler = explorer_key;
        win->click_handler = explorer_click;
    }
}

static void open_editor_file(const char* filename, Directory* dir) {
    VWindow* win = vdesk_create_window("Text Editor", 420, 160, 420, 240);
    if (win) {
        int pid = create_process("vesa-editor", 6);
        if (pid > 0) win->process_pid = pid;
        EditorApp* ed = (EditorApp*)kmalloc(sizeof(EditorApp));
        if (ed) {
            memset(ed, 0, sizeof(EditorApp));
            ed->dir = dir ? dir : fs_get_cwd_dir();
            strncpy(ed->filename, filename ? filename : "Document.txt", sizeof(ed->filename) - 1);
            ed->filename[sizeof(ed->filename) - 1] = '\0';
            FileHandle* fh = filename ? fs_dir_open(ed->dir, filename) : NULL;
            if (fh) {
                ed->len = (int)fs_read(fh, (uint8_t*)ed->text, EDITOR_MAX_TEXT - 1);
                fs_close(fh);
                ed->text[ed->len] = '\0';
            } else {
                strcpy(ed->text, "Welcome to GooberOS Text Editor!");
                ed->len = (int)strlen(ed->text);
            }
            ed->cursor = ed->len;
            ed->saved = 0;
            win->user_data = ed;
            win->render = editor_render;
            win->key_handler = editor_key;
            win->click_handler = editor_click;
            win->scroll_handler = editor_scroll;
        }
    }
}

static void open_editor_window(void) {
    open_editor_file("Document.txt", fs_get_cwd_dir());
}

static void open_paint_file(const char* filename, Directory* dir) {
    VWindow* win = vdesk_create_window("Bitmap Paint", 150, 110, 470, 260);
    if (win) {
        int pid = create_process("vesa-paint", 4);
        if (pid > 0) win->process_pid = pid;
        PaintApp* app = (PaintApp*)kmalloc(sizeof(PaintApp));
        if (app) {
            memset(app, 0, sizeof(PaintApp));
            app->dir = dir ? dir : fs_get_cwd_dir();
            strncpy(app->filename, filename ? filename : "Artwork.gbm", sizeof(app->filename) - 1);
            app->filename[sizeof(app->filename) - 1] = '\0';
            app->color = 1;
            FileHandle* fh = filename ? fs_dir_open(app->dir, filename) : NULL;
            if (fh) {
                fs_read(fh, app->pixels, sizeof(app->pixels));
                fs_close(fh);
            }
            win->user_data = app;
            win->render = paint_render;
            win->key_handler = paint_key;
        }
    }
}

static void open_paint_window(void) {
    open_paint_file("Artwork.gbm", fs_get_cwd_dir());
}

static void open_taskmgr_window(void) {
    VWindow* win = vdesk_create_window("Task Manager", 250, 80, 360, 300);
    if (win) {
        int pid = create_process("vesa-tasks", 2);
        if (pid > 0) win->process_pid = pid;
        TaskmgrApp* app = (TaskmgrApp*)kmalloc(sizeof(TaskmgrApp));
        if (app) {
            app->selected_pid = -1;
            app->status = 0;
            win->user_data = app;
        }
        win->render = taskmgr_render;
        win->click_handler = taskmgr_click;
    }
}

static void open_display_settings_window(void) {
    VWindow* win = vdesk_create_window("Display Settings", 300, 180, 430, 330);
    if (win) {
        int pid = create_process("vesa-setup", 1);
        if (pid > 0) win->process_pid = pid;
        win->render = display_settings_render;
        win->key_handler = display_settings_key;
        win->click_handler = display_settings_click;
    }
}

static void open_system_settings_window(void) {
    VWindow* win = vdesk_create_window("System Settings", 180, 80, 520, 360);
    if (win) {
        int pid = create_process("vesa-settings", 1);
        if (pid > 0) win->process_pid = pid;
        SystemSettingsApp* app = (SystemSettingsApp*)kmalloc(sizeof(SystemSettingsApp));
        if (app) {
            memset(app, 0, sizeof(*app));
            app->tab = 0;
            app->pointer_speed = g_pointer_speed_pref;
            app->hide_shell = g_hide_shell_pref;
            win->user_data = app;
        }
        win->render = system_settings_render;
        win->key_handler = system_settings_key;
        win->click_handler = system_settings_click;
    }
}

/* ---- GooberC IDE (lightweight VS-like) ---- */
#define IDE_MAX_TEXT   4096
#define IDE_OUT_LINES  6
#define IDE_OUT_LEN    72
#define IDE_TOOLBAR_H  28
#define IDE_TREE_W     140
#define IDE_OUT_H      88
#define IDE_CHAR_W     8
#define IDE_CHAR_H     16

typedef struct {
    Directory* workspace;
    Directory* browse;
    int picker; /* 0 editor, 1 folder picker */
    int browse_sel;
    int browse_scroll;
    char filename[32];
    char text[IDE_MAX_TEXT];
    int len;
    int cursor;
    int scroll_row;
    int tree_sel;
    int tree_scroll;
    char out[IDE_OUT_LINES][IDE_OUT_LEN];
    int out_count;
    int dirty;
    char last_gob[32];
} IdeApp;

static void ide_out(IdeApp* app, const char* msg) {
    int i;
    if (!app || !msg) return;
    if (app->out_count < IDE_OUT_LINES) {
        i = app->out_count++;
    } else {
        for (i = 0; i < IDE_OUT_LINES - 1; i++)
            memcpy(app->out[i], app->out[i + 1], IDE_OUT_LEN);
        i = IDE_OUT_LINES - 1;
    }
    strncpy(app->out[i], msg, IDE_OUT_LEN - 1);
    app->out[i][IDE_OUT_LEN - 1] = '\0';
}

static void ide_load_file(IdeApp* app, const char* name) {
    FileHandle* fh;
    if (!app || !name || !app->workspace) return;
    fh = fs_dir_open(app->workspace, name);
    if (!fh) {
        ide_out(app, "Cannot open file");
        return;
    }
    memset(app->text, 0, sizeof(app->text));
    app->len = (int)fs_read(fh, (uint8_t*)app->text, IDE_MAX_TEXT - 1);
    fs_close(fh);
    if (app->len < 0) app->len = 0;
    app->text[app->len] = '\0';
    app->cursor = app->len;
    app->scroll_row = 0;
    app->dirty = 0;
    strncpy(app->filename, name, sizeof(app->filename) - 1);
    app->filename[sizeof(app->filename) - 1] = '\0';
    ide_out(app, "Opened file");
}

static void ide_save(IdeApp* app) {
    if (!app || !app->workspace) {
        if (app) ide_out(app, "No workspace");
        return;
    }
    if (!app->filename[0]) {
        ide_out(app, "No file name");
        return;
    }
    app->text[app->len] = '\0';
    if (fs_dir_write(app->workspace, app->filename,
                     (uint8_t*)app->text, (size_t)app->len) != 0) {
        ide_out(app, "Save failed");
        return;
    }
    app->dirty = 0;
    ide_out(app, "Saved");
}

static void ide_make_gob_name(const char* gc, char* out, size_t out_sz) {
    size_t n = 0;
    if (!gc || !out || out_sz < 5) return;
    while (gc[n] && gc[n] != '.' && n + 5 < out_sz) {
        out[n] = gc[n];
        n++;
    }
    if (n == 0) {
        strncpy(out, "out.gob", out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    out[n++] = '.';
    out[n++] = 'g';
    out[n++] = 'o';
    out[n++] = 'b';
    out[n] = '\0';
}

static void ide_build(IdeApp* app) {
    Directory* prev;
    char gob[32];
    int rc;
    if (!app || !app->workspace) {
        if (app) ide_out(app, "Open a folder first");
        return;
    }
    if (!app->filename[0] || !has_suffix(app->filename, ".gc")) {
        ide_out(app, "Open a .gc source file");
        return;
    }
    ide_save(app);
    ide_make_gob_name(app->filename, gob, sizeof(gob));
    prev = fs_get_cwd_dir();
    fs_set_current_dir(app->workspace);
    ide_out(app, "Building...");
    rc = gooberc_compile(app->filename, gob);
    if (prev) fs_set_current_dir(prev);
    if (rc != 0) {
        ide_out(app, "Build failed");
        app->last_gob[0] = '\0';
        return;
    }
    strncpy(app->last_gob, gob, sizeof(app->last_gob) - 1);
    app->last_gob[sizeof(app->last_gob) - 1] = '\0';
    ide_out(app, "Build OK");
}

static void ide_run(IdeApp* app) {
    Directory* prev;
    char gob[32];
    if (!app || !app->workspace) {
        if (app) ide_out(app, "Open a folder first");
        return;
    }
    if (app->last_gob[0])
        strncpy(gob, app->last_gob, sizeof(gob) - 1);
    else if (app->filename[0] && has_suffix(app->filename, ".gc"))
        ide_make_gob_name(app->filename, gob, sizeof(gob));
    else {
        ide_out(app, "Build a .gc file first");
        return;
    }
    gob[sizeof(gob) - 1] = '\0';
    prev = fs_get_cwd_dir();
    fs_set_current_dir(app->workspace);
    if (gob_exec(gob) != 0)
        ide_out(app, "Run failed");
    else
        ide_out(app, "Run finished");
    if (prev) fs_set_current_dir(prev);
}

static int ide_tree_entry_count(IdeApp* app) {
    const Directory* d;
    if (!app || !app->workspace) return 0;
    d = app->workspace;
    return (int)(d->child_count + d->file_count);
}

static void ide_open_tree_sel(IdeApp* app) {
    const Directory* d;
    int idx;
    if (!app || !app->workspace) return;
    if (app->workspace) fs_dir_refresh(app->workspace);
    d = app->workspace;
    idx = app->tree_sel;
    if (idx < 0) return;
    if (idx < (int)d->child_count) {
        Directory* child = fs_dir_find_child(app->workspace, d->children[idx].name);
        if (child) {
            app->workspace = child;
            fs_dir_refresh(app->workspace);
            app->tree_sel = 0;
            app->tree_scroll = 0;
            ide_out(app, "Entered folder");
        }
        return;
    }
    idx -= (int)d->child_count;
    if (idx >= 0 && idx < (int)d->file_count) {
        const char* name = d->files[idx].name;
        if (has_suffix(name, ".gc") || has_suffix(name, ".txt"))
            ide_load_file(app, name);
        else
            ide_out(app, "Select a .gc file");
    }
}

static void ide_render(VWindow* win, int cx, int cy, int cw, int ch) {
    IdeApp* app = (IdeApp*)win->user_data;
    const VTheme* theme = vdesk_get_theme();
    const uint32_t ide_bg = 0x1E1E1E;
    const uint32_t ide_panel = 0x252526;
    const uint32_t ide_fg = 0xD4D4D4;
    const uint32_t ide_muted = 0x858585;
    int editor_x, editor_y, editor_w, editor_h;
    int out_y;
    int i, row, index;

    if (!app) return;

    vdesk_draw_rect(cx, cy, cw, ch, ide_bg);

    /* Toolbar */
    vdesk_draw_rect(cx, cy, cw, IDE_TOOLBAR_H, ide_panel);
    draw_button(cx + 4, cy + 3, 88, 22, "Open Folder", 0);
    draw_button(cx + 96, cy + 3, 52, 22, "Save", 0);
    draw_button(cx + 152, cy + 3, 52, 22, "Build", 0);
    draw_button(cx + 208, cy + 3, 52, 22, "Run", 0);
    vdesk_draw_text(cx + 270, cy + 7,
                    app->filename[0] ? app->filename : "(no file)",
                    ide_muted, ide_panel);

    if (app->picker) {
        const Directory* bd = app->browse;
        int list_top = cy + IDE_TOOLBAR_H + 8;
        int max_rows = (ch - IDE_TOOLBAR_H - 40) / IDE_CHAR_H;
        vdesk_draw_text(cx + 8, cy + IDE_TOOLBAR_H + 4, "Select workspace folder",
                        ide_fg, ide_bg);
        draw_button(cx + cw - 200, cy + IDE_TOOLBAR_H + 2, 96, 22, "Select", 1);
        draw_button(cx + cw - 96, cy + IDE_TOOLBAR_H + 2, 88, 22, "Cancel", 0);
        if (bd) {
            index = 0;
            row = 0;
            for (i = 0; i < (int)bd->child_count; i++, index++) {
                int y, sel;
                if (index < app->browse_scroll) continue;
                if (row >= max_rows) break;
                y = list_top + 20 + row * IDE_CHAR_H;
                sel = (app->browse_sel == index);
                if (sel) vdesk_draw_rect(cx + 8, y, cw - 16, IDE_CHAR_H, theme->accent);
                vdesk_draw_text(cx + 12, y, "DIR",
                                sel ? VCOLOR_WHITE : 0x569CD6,
                                sel ? theme->accent : ide_bg);
                vdesk_draw_text(cx + 44, y, bd->children[i].name,
                                sel ? VCOLOR_WHITE : ide_fg,
                                sel ? theme->accent : ide_bg);
                row++;
            }
            for (i = 0; i < (int)bd->file_count; i++, index++) {
                int y, sel;
                if (index < app->browse_scroll) continue;
                if (row >= max_rows) break;
                y = list_top + 20 + row * IDE_CHAR_H;
                sel = (app->browse_sel == index);
                if (sel) vdesk_draw_rect(cx + 8, y, cw - 16, IDE_CHAR_H, theme->accent);
                vdesk_draw_text(cx + 12, y, bd->files[i].name,
                                sel ? VCOLOR_WHITE : ide_muted,
                                sel ? theme->accent : ide_bg);
                row++;
            }
        }
        return;
    }

    /* Tree */
    vdesk_draw_rect(cx, cy + IDE_TOOLBAR_H, IDE_TREE_W, ch - IDE_TOOLBAR_H - IDE_OUT_H,
                    ide_panel);
    vdesk_draw_text(cx + 6, cy + IDE_TOOLBAR_H + 4, "Workspace", ide_muted, ide_panel);
    {
        const Directory* d = app->workspace;
        int list_top = cy + IDE_TOOLBAR_H + 22;
        int max_rows = (ch - IDE_TOOLBAR_H - IDE_OUT_H - 26) / IDE_CHAR_H;
        if (max_rows < 1) max_rows = 1;
        index = 0;
        row = 0;
        if (d) {
            for (i = 0; i < (int)d->child_count; i++, index++) {
                int y, sel;
                if (index < app->tree_scroll) continue;
                if (row >= max_rows) break;
                y = list_top + row * IDE_CHAR_H;
                sel = (app->tree_sel == index);
                if (sel) vdesk_draw_rect(cx + 2, y, IDE_TREE_W - 4, IDE_CHAR_H, theme->accent);
                vdesk_draw_text(cx + 6, y, d->children[i].name,
                                sel ? VCOLOR_WHITE : 0x569CD6,
                                sel ? theme->accent : ide_panel);
                row++;
            }
            for (i = 0; i < (int)d->file_count; i++, index++) {
                int y, sel;
                if (index < app->tree_scroll) continue;
                if (row >= max_rows) break;
                y = list_top + row * IDE_CHAR_H;
                sel = (app->tree_sel == index);
                if (sel) vdesk_draw_rect(cx + 2, y, IDE_TREE_W - 4, IDE_CHAR_H, theme->accent);
                vdesk_draw_text(cx + 6, y, d->files[i].name,
                                sel ? VCOLOR_WHITE : ide_fg,
                                sel ? theme->accent : ide_panel);
                row++;
            }
        } else {
            vdesk_draw_text(cx + 6, list_top, "Open Folder...", ide_muted, ide_panel);
        }
    }

    editor_x = cx + IDE_TREE_W + 4;
    editor_y = cy + IDE_TOOLBAR_H + 2;
    editor_w = cw - IDE_TREE_W - 8;
    editor_h = ch - IDE_TOOLBAR_H - IDE_OUT_H - 4;
    if (editor_w < 40) editor_w = 40;
    if (editor_h < 40) editor_h = 40;
    vdesk_draw_rect(editor_x, editor_y, editor_w, editor_h, ide_bg);

    {
        int cols = editor_w / IDE_CHAR_W;
        int rows = editor_h / IDE_CHAR_H;
        int text_row = 0, text_col = 0;
        if (cols < 2) cols = 2;
        if (rows < 2) rows = 2;
        for (i = 0; i <= app->len; i++) {
            int display_row = text_row - app->scroll_row;
            if (i == app->cursor && display_row >= 0 && display_row < rows) {
                char cur[2] = { '|', '\0' };
                vdesk_draw_text(editor_x + 2 + text_col * IDE_CHAR_W,
                                editor_y + display_row * IDE_CHAR_H, cur,
                                0xAEAFAD, ide_bg);
            }
            if (i == app->len) break;
            if (app->text[i] == '\n') {
                text_row++;
                text_col = 0;
                continue;
            }
            if (text_col >= cols) {
                text_row++;
                text_col = 0;
            }
            if (display_row >= 0 && display_row < rows && text_col < cols) {
                char buf[2] = { app->text[i], '\0' };
                vdesk_draw_text(editor_x + 2 + text_col * IDE_CHAR_W,
                                editor_y + display_row * IDE_CHAR_H, buf,
                                ide_fg, ide_bg);
            }
            text_col++;
        }
    }

    out_y = cy + ch - IDE_OUT_H;
    vdesk_draw_rect(cx, out_y, cw, IDE_OUT_H, ide_panel);
    vdesk_draw_text(cx + 6, out_y + 4, "Output", ide_muted, ide_panel);
    for (i = 0; i < app->out_count; i++) {
        vdesk_draw_text(cx + 6, out_y + 20 + i * 12, app->out[i], ide_fg, ide_panel);
    }
}

static void ide_key(VWindow* win, char key) {
    IdeApp* app = (IdeApp*)win->user_data;
    unsigned char uk = (unsigned char)key;
    if (!app) return;

    if (app->picker) {
        const Directory* bd = app->browse;
        int total = bd ? (int)(bd->child_count + bd->file_count) : 0;
        if (uk == KEY_ESC) {
            app->picker = 0;
            return;
        }
        if (uk == KEY_UP && app->browse_sel > 0) app->browse_sel--;
        else if (uk == KEY_DOWN && app->browse_sel < total - 1) app->browse_sel++;
        else if (uk == KEY_BACKSPACE) {
            if (app->browse && app->browse->parent)
                app->browse = app->browse->parent;
        } else if (key == '\r' || key == '\n') {
            if (bd && app->browse_sel < (int)bd->child_count) {
                Directory* child = fs_dir_find_child(app->browse,
                                                     bd->children[app->browse_sel].name);
                if (child) {
                    app->browse = child;
                    app->browse_sel = 0;
                }
            }
        }
        return;
    }

    if (uk == KEY_F2) {
        ide_save(app);
        return;
    }
    if (uk == KEY_F5) {
        ide_build(app);
        return;
    }
    if (uk == KEY_F6) {
        ide_run(app);
        return;
    }
    if (uk == KEY_BACKSPACE) {
        if (app->cursor > 0) {
            int i;
            for (i = app->cursor - 1; i < app->len; i++)
                app->text[i] = app->text[i + 1];
            app->len--;
            app->cursor--;
            app->dirty = 1;
        }
        return;
    }
    if (uk == KEY_LEFT) {
        if (app->cursor > 0) app->cursor--;
        return;
    }
    if (uk == KEY_RIGHT) {
        if (app->cursor < app->len) app->cursor++;
        return;
    }
    if (uk >= 0x80)
        return;
    if (app->len >= IDE_MAX_TEXT - 1) return;
    {
        int i;
        for (i = app->len; i >= app->cursor; i--)
            app->text[i + 1] = app->text[i];
        app->text[app->cursor] = (key == '\r') ? '\n' : key;
        app->len++;
        app->cursor++;
        app->dirty = 1;
    }
}

static void ide_click(VWindow* win, int lx, int ly) {
    IdeApp* app = (IdeApp*)win->user_data;
    int cw = client_width(win);
    int ch = win->height - TITLEBAR_HEIGHT - BORDER_SIZE * 2;
    if (!app) return;

    if (IN_RECT(lx, ly, 4, 3, 88, 22)) {
        app->picker = 1;
        app->browse = app->workspace ? app->workspace : fs_get_cwd_dir();
        if (!app->browse) app->browse = fs_get_desktop_dir();
        if (app->browse) fs_dir_refresh(app->browse);
        app->browse_sel = 0;
        app->browse_scroll = 0;
        return;
    }

    if (app->picker) {
        const Directory* bd = app->browse;
        int list_top = IDE_TOOLBAR_H + 28;
        int max_rows = (ch - IDE_TOOLBAR_H - 40) / IDE_CHAR_H;
        if (IN_RECT(lx, ly, cw - 200, IDE_TOOLBAR_H + 2, 96, 22)) {
            app->workspace = app->browse;
            if (app->workspace) fs_dir_refresh(app->workspace);
            app->picker = 0;
            app->tree_sel = 0;
            app->tree_scroll = 0;
            ide_out(app, "Workspace set");
            return;
        }
        if (IN_RECT(lx, ly, cw - 96, IDE_TOOLBAR_H + 2, 88, 22)) {
            app->picker = 0;
            return;
        }
        if (bd && ly >= list_top) {
            int row = (ly - list_top) / IDE_CHAR_H;
            int idx = app->browse_scroll + row;
            int total = (int)(bd->child_count + bd->file_count);
            if (row >= 0 && row < max_rows && idx >= 0 && idx < total) {
                if (idx == app->browse_sel && idx < (int)bd->child_count) {
                    Directory* child = fs_dir_find_child(app->browse,
                                                         bd->children[idx].name);
                    if (child) {
                        app->browse = child;
                        app->browse_sel = 0;
                    }
                } else {
                    app->browse_sel = idx;
                }
            }
        }
        return;
    }

    if (IN_RECT(lx, ly, 96, 3, 52, 22)) {
        ide_save(app);
        return;
    }
    if (IN_RECT(lx, ly, 152, 3, 52, 22)) {
        ide_build(app);
        return;
    }
    if (IN_RECT(lx, ly, 208, 3, 52, 22)) {
        ide_run(app);
        return;
    }

    if (lx < IDE_TREE_W && ly > IDE_TOOLBAR_H && ly < ch - IDE_OUT_H) {
        int list_top = IDE_TOOLBAR_H + 22;
        int row = (ly - list_top) / IDE_CHAR_H;
        int idx = app->tree_scroll + row;
        int total = ide_tree_entry_count(app);
        if (row >= 0 && idx >= 0 && idx < total) {
            if (idx == app->tree_sel)
                ide_open_tree_sel(app);
            else
                app->tree_sel = idx;
        }
        return;
    }

    /* Click in editor pane to place the caret. */
    {
        int editor_x = IDE_TREE_W + 4;
        int editor_y = IDE_TOOLBAR_H + 2;
        int editor_w = cw - IDE_TREE_W - 8;
        int editor_h = ch - IDE_TOOLBAR_H - IDE_OUT_H - 4;
        if (editor_w < 40) editor_w = 40;
        if (editor_h < 40) editor_h = 40;
        if (IN_RECT(lx, ly, editor_x, editor_y, editor_w, editor_h)) {
            int cols = editor_w / IDE_CHAR_W;
            int rows = editor_h / IDE_CHAR_H;
            int cell_col = (lx - editor_x - 2) / IDE_CHAR_W;
            int cell_row = (ly - editor_y) / IDE_CHAR_H;
            if (cols < 1) cols = 1;
            if (rows < 1) rows = 1;
            if (cell_col < 0) cell_col = 0;
            if (cell_row < 0) cell_row = 0;
            if (cell_row >= rows) cell_row = rows - 1;
            app->cursor = text_pos_at_cell(app->text, app->len, cols,
                                          app->scroll_row, cell_row, cell_col);
        }
    }
}

static void open_ide_file(const char* filename, Directory* dir) {
    VWindow* win = vdesk_create_window("GooberC IDE", 60, 40, 720, 480);
    if (!win) return;
    {
        int pid = create_process("gooberc-ide", 3);
        if (pid > 0) win->process_pid = pid;
    }
    {
        IdeApp* app = (IdeApp*)kmalloc(sizeof(IdeApp));
        if (!app) return;
        memset(app, 0, sizeof(*app));
        app->workspace = dir ? dir : fs_get_cwd_dir();
        if (app->workspace) fs_dir_refresh(app->workspace);
        win->user_data = app;
        win->render = ide_render;
        win->key_handler = ide_key;
        win->click_handler = ide_click;
        if (filename && filename[0])
            ide_load_file(app, filename);
        else
            ide_out(app, "Open Folder, then pick a .gc file");
    }
}

static void open_ide_window(void) {
    open_ide_file(NULL, fs_get_cwd_dir());
}

/* Open a filesystem-backed desktop item according to its icon kind. Desktop
 * items live in the fixed Desktop directory regardless of where File Explorer
 * is currently browsing, so resolve everything against that handle. */
static void vesa_open_desktop_file(const char* name, int kind) {
    if (!name) return;
    Directory* desk = fs_get_desktop_dir();
    if (kind == VICON_FOLDER) {
        Directory* child = desk ? fs_dir_find_child(desk, name) : NULL;
        if (child) {
            fs_set_current_dir(child);
            open_explorer_window();
        }
    } else if (kind == VICON_TEXT) {
        open_editor_file(name, desk);
    } else if (kind == VICON_BITMAP) {
        open_paint_file(name, desk);
    } else if (kind == VICON_CODE) {
        open_ide_file(name, desk);
    } else if (kind == VICON_GOB) {
        Directory* prev = fs_get_cwd_dir();
        if (desk) fs_set_current_dir(desk);
        if (gob_exec(name) != 0)
            vdesk_notify("GooberC", "Failed to run .gob");
        else
            vdesk_notify("GooberC", "Finished running .gob");
        if (prev) fs_set_current_dir(prev);
    } else {
        vdesk_notify("Desktop", "No app for this file type");
    }
}

static void vesa_launch_app(VDeskAppId app_id) {
    switch (app_id) {
        case VDESK_APP_SHELL:
            open_shell_window(0);
            break;
        case VDESK_APP_EXPLORER:
            open_explorer_window();
            break;
        case VDESK_APP_EDITOR:
            open_editor_window();
            break;
        case VDESK_APP_SYSINFO:
            open_sysinfo_window();
            break;
        case VDESK_APP_TASK_MANAGER:
            open_taskmgr_window();
            break;
        case VDESK_APP_DISPLAY_SETTINGS:
            open_display_settings_window();
            break;
        case VDESK_APP_SYSTEM_SETTINGS:
            open_system_settings_window();
            break;
        case VDESK_APP_PAINT:
            open_paint_window();
            break;
        case VDESK_APP_WELCOME:
            if (fs_is_persistent())
                (void)gob_exec("Apps/Welcome.gob");
            else
                vdesk_set_status("Welcome.gob is install-only");
            break;
        case VDESK_APP_IDE:
            open_ide_window();
            break;
    }
}

/* ---- Main VESA desktop entry ---- */

/*
 * Phase 3e split: the original vesa_desktop_run() inits the desktop and then
 * enters the infinite event-pump loop. That model is fine on x86 where the
 * call site is inside the shell's `gui` command, but the x64 unified-boot
 * orchestrator needs to bound the *init* phase under a generous watchdog
 * (so a wedged framebuffer / dirty-region / icon-enumeration step can't
 * hang the boot) and only THEN hand control to the unbounded event pump
 * after the boot-stage results table has been printed.
 *
 * vesa_desktop_init() performs everything up to (but not including) the
 * event-pump call. It is what the x64 `Shell / desktop` boot stage runs
 * under a 30-second watchdog. vesa_desktop_main_loop() is the unbounded
 * event pump and must be called AFTER the boot stages return.
 *
 * For x86 (where this is invoked from the shell's `gui` command) the
 * original combined behaviour is preserved by vesa_desktop_run() below,
 * which simply chains the two halves together. No x86 call site changes.
 */
/*
 * Phase 4 (display polish, item 2): tear-free repaint requires a back-
 * buffer matching the LFB layout (pitch x height bytes, any bpp). On x64
 * we kmalloc it out of the 8 MiB free-list heap; if that fails (heap too
 * small for a 4 MiB 1366x768x32 panel back-buffer, say) we cleanly fall
 * back to direct-to-LFB rendering -- the OS keeps working, just with the
 * pre-Phase-4 tearing visible. The decision and the allocation size are
 * logged so the boot trail makes it obvious which path armed.
 *
 * On x86 we keep the legacy 4 MiB BSS static back-buffer that kernel.c
 * installs (see VESA_STATIC_BACKBUFFER_BYTES). The new heap-backed path
 * is x64-only so we don't churn the x86 bump-allocator lifecycle.
 */
#ifdef __x86_64__
static void vesa_desktop_alloc_backbuffer_x64(uint32_t w, uint32_t h) {
    uint32_t pitch = vesa_get_pitch();
    uint8_t  bpp   = vesa_get_bpp();
    uint32_t need  = pitch * h;

    char buf[24];
    print("[display] back-buffer alloc request: ");
    itoa((int)need, buf, 10); print(buf); print(" bytes (");
    itoa((int)w, buf, 10); print(buf); print("x");
    itoa((int)h, buf, 10); print(buf); print(" @");
    itoa((int)bpp, buf, 10); print(buf); print(" bpp, pitch=");
    itoa((int)pitch, buf, 10); print(buf); print(")\n");

    print("[display] heap free before alloc: ");
    itoa((int)memory_free_bytes(), buf, 10); print(buf); print(" bytes\n");

    void* bb = kmalloc(need);
    if (!bb) {
        print("[display] back-buffer alloc FAILED (heap too small); "
              "rendering direct-to-LFB. Tearing may be visible.\n");
        vesa_set_backbuffer_bytes(NULL, 0);
        return;
    }

    vesa_set_backbuffer_bytes((uint32_t*)bb, need);

    print("[display] back-buffer allocated: ");
    itoa((int)need, buf, 10); print(buf); print(" bytes (");
    itoa((int)w, buf, 10); print(buf); print("x");
    itoa((int)h, buf, 10); print(buf); print(" @");
    itoa((int)bpp, buf, 10); print(buf); print(" bpp)\n");

    print("[display] heap free after alloc: ");
    itoa((int)memory_free_bytes(), buf, 10); print(buf); print(" bytes\n");

    int fps = kernel_display_target_fps();
    int budget_ms = (fps > 0) ? (1000 / fps) : 16;
    print("[display] frame pacing: target ");
    itoa(fps, buf, 10); print(buf); print(" Hz, budget ");
    itoa(budget_ms, buf, 10); print(buf); print(" ms/frame\n");
}
#endif

/*
 * Phase 4 (item 3): VGA-graphics fallback surface. When the display
 * framework commits to the mode-13h rung (vesa/bochs/intel all rejected),
 * we never call vesa_get_width() / vdesk_init -- there is no VESA LFB.
 * Instead we paint a static 320x200 status screen so the user at least
 * sees SOMETHING beyond a black panel. The minimal kernel REPL/line
 * editor still runs in this mode; the VGA-13h surface is just for
 * visibility ("the OS booted, here's a known palette pattern").
 */
static void desktop_vga13_render(void) {
    if (!vga_graphics_active()) return;

    vga_graphics_clear(VGA13_COLOR_BLUE);

    /* Top status bar. */
    vga_graphics_fill_rect(0, 0, 320, 16, VGA13_COLOR_LIGHT_GREY);
    vga_graphics_draw_string(4, 0, "GooberOS x86_64",
                             VGA13_COLOR_BLACK, VGA13_COLOR_LIGHT_GREY);
    vga_graphics_draw_string(160, 0, "VGA Safe Graphics",
                             VGA13_COLOR_RED, VGA13_COLOR_LIGHT_GREY);

    /* Centered icon column -- placeholder squares so the fallback surface
     * looks like a "minimal desktop" rather than empty wallpaper. */
    int ix = 24, iy = 32;
    static const uint8_t icon_colors[6] = {
        VGA13_COLOR_LIGHT_GREEN, VGA13_COLOR_LIGHT_BROWN,
        VGA13_COLOR_WHITE,       VGA13_COLOR_LIGHT_RED,
        VGA13_COLOR_LIGHT_CYAN,  VGA13_COLOR_LIGHT_MAGENTA
    };
    static const char* icon_labels[6] = {
        "Shell", "Files", "Edit", "Tasks", "Paint", "Help"
    };
    for (int i = 0; i < 6; i++) {
        int y = iy + i * 22;
        vga_graphics_fill_rect(ix, y, 16, 16, icon_colors[i]);
        vga_graphics_draw_string(ix + 22, y + 4, icon_labels[i],
                                 VGA13_COLOR_WHITE, VGA13_COLOR_BLUE);
    }

    /* Faux "shell window" body so the layout matches the LFB desktop's
     * shape (top bar + icons + window). */
    vga_graphics_fill_rect(112, 32, 200, 140, VGA13_COLOR_BLACK);
    vga_graphics_draw_string(116, 36, "Console (mode-13h)",
                             VGA13_COLOR_LIGHT_GREEN, VGA13_COLOR_BLACK);
    vga_graphics_draw_string(116, 56, "VESA framebuffer was not",
                             VGA13_COLOR_LIGHT_GREY, VGA13_COLOR_BLACK);
    vga_graphics_draw_string(116, 72, "available or was rejected",
                             VGA13_COLOR_LIGHT_GREY, VGA13_COLOR_BLACK);
    vga_graphics_draw_string(116, 88, "by the on-panel gate.",
                             VGA13_COLOR_LIGHT_GREY, VGA13_COLOR_BLACK);
    vga_graphics_draw_string(116, 112, "Boot continues on the",
                             VGA13_COLOR_LIGHT_GREY, VGA13_COLOR_BLACK);
    vga_graphics_draw_string(116, 128, "320x200 fallback surface.",
                             VGA13_COLOR_LIGHT_GREY, VGA13_COLOR_BLACK);

    /* Bottom taskbar. */
    vga_graphics_fill_rect(0, 184, 320, 16, VGA13_COLOR_LIGHT_GREY);
    vga_graphics_draw_string(4, 184, "Start",
                             VGA13_COLOR_BLACK, VGA13_COLOR_LIGHT_GREY);
    vga_graphics_draw_string(160, 184, "[GooberOS / VGA-13h]",
                             VGA13_COLOR_BLACK, VGA13_COLOR_LIGHT_GREY);

    vga_graphics_present();
}

void vesa_desktop_init(void) {
    /*
     * x64 VGA-Compatibility path: the display stage committed to the
     * 80x25 text console (textcon) and never brought up a VESA LFB.
     * There is no framebuffer to back a window manager, so the desktop
     * stage is a no-op -- the kernel main loop will dispatch the full
     * interactive text shell after the boot-stage results summary
     * prints. Logging here keeps the per-stage results table honest
     * (the stage reports OK, just with zero work).
     */
    if (kernel_display_is_text_console()) {
        print("[desktop] text-console mode active -- "
              "skipping VESA desktop init.\n");
        return;
    }

    /*
     * Phase 4 (item 3): when the framework committed to the VGA-13h rung
     * we do NOT have a VESA LFB to back the full window manager. Render
     * the minimal fallback surface and skip the rest of init -- the
     * kernel's main loop will sit on the line editor / REPL behind the
     * scenes. vga_graphics_active() will be 1 here because the rung's
     * init() ran successfully.
     */
    if (kernel_display_is_vga_graphics()) {
        print("[desktop] VGA-graphics fallback active -- "
              "painting mode-13h status surface.\n");
        desktop_vga13_render();
        return;
    }

    uint32_t w = vesa_get_width();
    uint32_t h = vesa_get_height();

    print("[desktop] init: ");
    {
        char buf[16];
        itoa((int)w, buf, 10); print(buf); print("x");
        itoa((int)h, buf, 10); print(buf); print("\n");
    }

#ifdef __x86_64__
    /* Allocate the heap-backed back-buffer before we touch any of the
     * draw primitives in vdesk_init -- the bb pointer takes effect via
     * vesa_set_backbuffer_bytes BEFORE the first vesa_fill_rect. */
    vesa_desktop_alloc_backbuffer_x64(w, h);
#endif

    input_set_bounds(w, h);

    vdesk_init(w, h);
    vdesk_set_app_launcher(vesa_launch_app);
    vdesk_set_file_opener(vesa_open_desktop_file);
    int icon_y = vdesk_workspace_top() + 24;
    vdesk_add_icon("Shell", VDESK_APP_SHELL, 24, icon_y);
    vdesk_add_icon("Files", VDESK_APP_EXPLORER, 24, icon_y + 72);
    vdesk_add_icon("Editor", VDESK_APP_EDITOR, 24, icon_y + 144);
    vdesk_add_icon("Tasks", VDESK_APP_TASK_MANAGER, 24, icon_y + 216);
    vdesk_add_icon("Paint", VDESK_APP_PAINT, 24, icon_y + 288);
    vdesk_add_icon("GooberC", VDESK_APP_IDE, 24, icon_y + 360);
    if (fs_is_persistent())
        vdesk_add_icon("Welcome", VDESK_APP_WELCOME, 24, icon_y + 432);

    /* Ensure the dedicated Desktop folder exists (created once if missing). */
    fs_get_desktop_dir();
    if (!fs_is_persistent())
        driver_log_sync_desktop_file();
    vdesk_refresh_desktop_items(1);

    system_settings_load();
    open_shell_window(1);
    if (g_hide_shell_pref)
        vdesk_set_desktop_experience(1);

    print("[desktop] init complete; entering event loop next.\n");
}

void vesa_desktop_main_loop(void) {
    /*
     * Phase 4 (item 3) VGA-graphics fallback: there is no VESA event loop
     * to run -- the panel is in mode-13h and the kernel's REPL is the
     * only interactive surface. We sit in a small refresh loop that
     * periodically repaints the static status surface (so a transient
     * mode-set residue is washed out) and polls USB so HID never gets
     * stuck. The kernel_main_x64 REPL fallback path is what the user
     * actually types into; this is just keeping the panel alive.
     */
    if (kernel_display_is_vga_graphics()) {
        print("[desktop] VGA-graphics fallback: returning to caller for REPL.\n");
        return;
    }

    /* vdesk_run() owns the per-frame usb_poll() call that drains HID input
     * at >= 60 Hz (the event pump runs at the desktop's target frame
     * rate). It never returns. */
    print("[desktop] entering event loop\n");
    vdesk_run();
}

void vesa_desktop_run(void) {
    vesa_desktop_init();
    vesa_desktop_main_loop();
}
