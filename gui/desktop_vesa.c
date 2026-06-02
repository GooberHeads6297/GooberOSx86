#include "vesa_window.h"
#include "vga_passthrough.h"
#include "../drivers/video/vesa.h"
#include "../drivers/video/vga.h"
#include "../drivers/keyboard/keyboard.h"
#include "../drivers/mouse/mouse.h"
#include "../drivers/input/input.h"
#include "../drivers/timer/timer.h"
#include "../lib/string.h"
#include "../lib/memory.h"
#include "../shell/shell.h"
#include "../fs/filesystem.h"
#include "../taskmgr/process.h"
#include "../kernel.h"
#include "../drivers/usb/host/host.h"

static int has_suffix(const char* name, const char* suffix) {
    int nl = (int)strlen(name);
    int sl = (int)strlen(suffix);
    if (sl > nl) return 0;
    return strcmp(name + nl - sl, suffix) == 0;
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
#define SHELL_LINE_LEN  80

typedef struct {
    char lines[SHELL_MAX_LINES][SHELL_LINE_LEN];
    int line_count;
    int scroll;
    char input[SHELL_LINE_LEN];
    int input_len;
    int input_pos;
} ShellApp;

static void shell_write_line(ShellApp* sa, const char* text) {
    if (!sa || !text) return;
    if (sa->line_count < SHELL_MAX_LINES) {
        strncpy(sa->lines[sa->line_count], text, SHELL_LINE_LEN - 1);
        sa->lines[sa->line_count][SHELL_LINE_LEN - 1] = '\0';
        sa->line_count++;
    } else {
        for (int i = 1; i < SHELL_MAX_LINES; i++)
            strcpy(sa->lines[i - 1], sa->lines[i]);
        strncpy(sa->lines[SHELL_MAX_LINES - 1], text, SHELL_LINE_LEN - 1);
        sa->lines[SHELL_MAX_LINES - 1][SHELL_LINE_LEN - 1] = '\0';
    }
    if (sa->scroll < sa->line_count - 1)
        sa->scroll = sa->line_count - 1;
}

static void shell_capture_write(const char* text, void* ctx) {
    ShellApp* sa = (ShellApp*)ctx;
    if (!sa || !text) return;
    char line[SHELL_LINE_LEN];
    int li = 0;
    for (int i = 0; text[i] && li < SHELL_LINE_LEN - 1; i++) {
        if (text[i] == '\n') {
            line[li] = '\0';
            shell_write_line(sa, line);
            li = 0;
        } else if (text[i] != '\r') {
            line[li++] = text[i];
        }
    }
    if (li > 0) {
        line[li] = '\0';
        shell_write_line(sa, line);
    }
}

static void shell_do_exec(ShellApp* sa) {
    char cmd[SHELL_LINE_LEN];
    if (sa->input_len <= 0) return;

    strncpy(cmd, sa->input, SHELL_LINE_LEN - 1);
    cmd[SHELL_LINE_LEN - 1] = '\0';

    {
        char prompt[SHELL_LINE_LEN];
        prompt[0] = '>';
        prompt[1] = ' ';
        int pi = 2;
        for (int i = 0; cmd[i] && pi < SHELL_LINE_LEN - 1; i++)
            prompt[pi++] = cmd[i];
        prompt[pi] = '\0';
        shell_write_line(sa, prompt);
    }

    if (strcmp(cmd, "clear") == 0 || strcmp(cmd, "cls") == 0) {
        sa->line_count = 0;
        sa->scroll = 0;
    } else if (strcmp(cmd, "snakeGame.exe") == 0 ||
               strcmp(cmd, "cubeDip.exe") == 0 ||
               strcmp(cmd, "pong.exe") == 0 ||
               strcmp(cmd, "doom.exe") == 0 ||
               strcmp(cmd, "taskview") == 0 ||
               strncmp(cmd, "edit ", 5) == 0 ||
               strcmp(cmd, "gui") == 0) {
        shell_write_line(sa, "VESA shell: blocking VGA apps are disabled here.");
        shell_write_line(sa, "Use desktop icons, Start, or right-click menu instead.");
    } else {
        shell_set_redirect(shell_capture_write, NULL, sa);
        execute_command(cmd);
        shell_clear_redirect();
    }

    sa->input_len = 0;
    sa->input_pos = 0;
    sa->input[0] = '\0';
}

static void shell_render(VWindow* win, int cx, int cy, int cw, int ch) {
    ShellApp* sa = (ShellApp*)win->user_data;
    if (!sa) return;

    int char_w = 8;
    int char_h = 16;
    int cols = cw / char_w;
    int rows = ch / char_h;
    if (cols < 2 || rows < 2) return;

    vdesk_draw_rect(cx, cy, cw, ch, VCOLOR_BLACK);

    int draw_lines = rows - 1;
    if (sa->scroll > sa->line_count - draw_lines)
        sa->scroll = sa->line_count - draw_lines;
    if (sa->scroll < 0) sa->scroll = 0;

    for (int r = 0; r < draw_lines; r++) {
        int src = sa->scroll + r;
        if (src >= 0 && src < sa->line_count) {
            vdesk_draw_text(cx + 2, cy + r * char_h, sa->lines[src],
                           VCOLOR_GREEN, VCOLOR_BLACK);
        }
    }

    {
        char prompt[128];
        prompt[0] = '>';
        prompt[1] = ' ';
        int pi = 2;
        int max_c = (cols > 4) ? cols - 4 : 0;
        for (int i = 0; i < sa->input_len && pi < max_c + 2 && pi < 126; i++)
            prompt[pi++] = sa->input[i];

        int cursor_display = sa->input_pos + 2;
        if (cursor_display < pi) {
            char old = prompt[cursor_display];
            prompt[cursor_display] = '_';
            vdesk_draw_text(cx + 2, cy + (rows - 1) * char_h, prompt,
                           VCOLOR_WHITE, VCOLOR_BLACK);
            prompt[cursor_display] = old;
        } else {
            prompt[pi] = '\0';
            vdesk_draw_text(cx + 2, cy + (rows - 1) * char_h, prompt,
                           VCOLOR_WHITE, VCOLOR_BLACK);
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

static ShellApp* create_shell_app(void) {
    ShellApp* sa = (ShellApp*)kmalloc(sizeof(ShellApp));
    if (!sa) return NULL;
    memset(sa, 0, sizeof(ShellApp));
    shell_write_line(sa, "GooberOS VESA Shell");
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
    vdesk_draw_text(cx + 4, row, "Pointer:", theme->text, theme->client_bg);
    if (metrics->usb_pointer_active)
        vdesk_draw_text(cx + 84, row, "USB", theme->accent, theme->client_bg);
    else
        vdesk_draw_text(cx + 84, row, "PS/2", theme->accent, theme->client_bg);
    row += 16;
    vdesk_draw_text(cx + 4, row, "Theme:", theme->text, theme->client_bg);
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

#define TASKMGR_ROW_TOP 38
#define TASKMGR_ROW_H 16

static void taskmgr_button_rect(int cw, int* bx, int* by, int* bw, int* bh) {
    *bw = 90;
    *bh = 18;
    *bx = cw - *bw - 4;
    *by = 2;
}

static void taskmgr_render(VWindow* win, int cx, int cy, int cw, int ch) {
    TaskmgrApp* app = (TaskmgrApp*)win->user_data;
    const VTheme* theme = vdesk_get_theme();
    const VDeskMetrics* metrics = vdesk_get_metrics();
    process_entry_t* table = get_kernel_process_table();
    int total = get_kernel_process_count();
    int row = cy + 4;
    int max_rows = (ch - 96) / 16;

    vdesk_draw_rect(cx, cy, cw, ch, theme->client_bg);
    vdesk_draw_text(cx + 4, row, "Task Manager", theme->text, theme->client_bg);

    {
        int bx, by, bw, bh;
        taskmgr_button_rect(cw, &bx, &by, &bw, &bh);
        draw_button(cx + bx, cy + by, bw, bh, "End Task", 1);
    }

    row += 18;
    vdesk_draw_text(cx + 4, row, "PID", theme->text_muted, theme->client_bg);
    vdesk_draw_text(cx + 48, row, "Name", theme->text_muted, theme->client_bg);
    vdesk_draw_text(cx + 176, row, "Mem", theme->text_muted, theme->client_bg);
    row += 14;
    vdesk_draw_rect(cx + 4, row, cw - 8, 1, theme->border_dark);
    row += 6;

    for (int i = 0; i < total && row < cy + TASKMGR_ROW_TOP + max_rows * 16; i++) {
        char pid[12];
        char mem[16];
        if (!table[i].active) continue;
        int sel = (app && table[i].pid == app->selected_pid);
        uint32_t bg = sel ? theme->accent : theme->client_bg;
        uint32_t fg = sel ? VCOLOR_WHITE : theme->text;
        if (sel) vdesk_draw_rect(cx + 2, row - 1, cw - 4, 16, bg);
        itoa(table[i].pid, pid, 10);
        itoa((int)table[i].memory_kb, mem, 10);
        vdesk_draw_text(cx + 4, row, pid, fg, bg);
        vdesk_draw_text(cx + 48, row, table[i].name, fg, bg);
        vdesk_draw_text(cx + 176, row, mem, sel ? VCOLOR_WHITE : theme->accent, bg);
        vdesk_draw_text(cx + 216, row, "KB", sel ? VCOLOR_WHITE : theme->text_muted, bg);
        row += 16;
    }

    if (app && app->status) {
        const char* msg = "";
        if (app->status == 1) msg = "Process terminated.";
        else if (app->status == 2) msg = "Cannot kill kernel process.";
        else if (app->status == 3) msg = "Process not found.";
        vdesk_draw_text(cx + 4, cy + ch - 72, msg,
                        app->status == 2 ? theme->accent : theme->text_muted,
                        theme->client_bg);
    }

    row = cy + ch - 56;
    vdesk_draw_rect(cx + 4, row - 4, cw - 8, 1, theme->border_dark);
    {
        char buf[16];
        itoa((int)metrics->window_count, buf, 10);
        vdesk_draw_text(cx + 4, row, "Windows:", theme->text, theme->client_bg);
        vdesk_draw_text(cx + 84, row, buf, theme->accent, theme->client_bg);
    }
    row += 16;
    {
        char buf[16];
        itoa((int)metrics->render_ticks, buf, 10);
        vdesk_draw_text(cx + 4, row, "Render ticks:", theme->text, theme->client_bg);
        vdesk_draw_text(cx + 116, row, buf, theme->accent, theme->client_bg);
    }
    row += 16;
    {
        char buf[16];
        itoa((int)metrics->swap_ticks, buf, 10);
        vdesk_draw_text(cx + 4, row, "Swap ticks:", theme->text, theme->client_bg);
        vdesk_draw_text(cx + 100, row, buf, theme->accent, theme->client_bg);
    }
    row += 16;
    {
        char buf[16];
        itoa((int)metrics->input_events, buf, 10);
        vdesk_draw_text(cx + 4, row, "Input events:", theme->text, theme->client_bg);
        vdesk_draw_text(cx + 116, row, buf, theme->accent, theme->client_bg);
    }
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
                app->status = 1;
                app->selected_pid = -1;
            } else {
                app->status = 3;
            }
        }
        return;
    }

    if (ly >= TASKMGR_ROW_TOP) {
        int disp = (ly - TASKMGR_ROW_TOP) / TASKMGR_ROW_H;
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

static void display_settings_render(VWindow* win, int cx, int cy, int cw, int ch) {
    (void)win;
    const VTheme* theme = vdesk_get_theme();
    const VDeskMetrics* metrics = vdesk_get_metrics();
    vdesk_draw_rect(cx, cy, cw, ch, theme->client_bg);
    vdesk_draw_text(cx + 4, cy + 4, "Display Settings", theme->text, theme->client_bg);
    vdesk_draw_text(cx + 4, cy + 24, "Theme:", theme->text, theme->client_bg);
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
                "Toggle Theme", 0);
    vdesk_draw_text(cx + 4, cy + 150, "Click button or press F2 to toggle.",
                    theme->text_muted, theme->client_bg);
    vdesk_draw_text(cx + 4, cy + 166, "Use VGA Safe Mode if VESA freezes.",
                    theme->text_muted, theme->client_bg);
}

static void display_settings_key(VWindow* win, char key) {
    (void)win;
    if ((unsigned char)key == KEY_F2) vdesk_toggle_theme();
}

static void display_settings_click(VWindow* win, int lx, int ly) {
    (void)win;
    if (IN_RECT(lx, ly, SETTINGS_BTN_X, 118, SETTINGS_BTN_W, SETTINGS_BTN_H))
        vdesk_toggle_theme();
}

static void system_settings_render(VWindow* win, int cx, int cy, int cw, int ch) {
    (void)win;
    const VTheme* theme = vdesk_get_theme();
    const VDeskMetrics* metrics = vdesk_get_metrics();
    vdesk_draw_rect(cx, cy, cw, ch, theme->client_bg);
    vdesk_draw_text(cx + 4, cy + 4, "System Settings", theme->text, theme->client_bg);
    vdesk_draw_text(cx + 4, cy + 28, "Pointer:", theme->text, theme->client_bg);
    vdesk_draw_text(cx + 92, cy + 28, metrics->usb_pointer_active ? "USB HID" : "PS/2 or none",
                    theme->accent, theme->client_bg);
    vdesk_draw_text(cx + 4, cy + 44, "Controller:", theme->text, theme->client_bg);
    vdesk_draw_text(cx + 92, cy + 44, usb_host_controller_name(), theme->accent, theme->client_bg);
    vdesk_draw_text(cx + 4, cy + 68, "Theme:", theme->text, theme->client_bg);
    vdesk_draw_text(cx + 92, cy + 68, vdesk_get_theme_name(), theme->accent, theme->client_bg);
    draw_button(cx + SETTINGS_BTN_X, cy + 92, SETTINGS_BTN_W, SETTINGS_BTN_H,
                "Toggle Theme", 0);
    vdesk_draw_text(cx + 4, cy + 124, "Click button or press F2 to toggle.",
                    theme->text_muted, theme->client_bg);
    vdesk_draw_text(cx + 4, cy + 140, "Apps are lazy-launched; they must not",
                    theme->text_muted, theme->client_bg);
    vdesk_draw_text(cx + 4, cy + 156, "take over the graphics main loop.",
                    theme->text_muted, theme->client_bg);
}

static void system_settings_key(VWindow* win, char key) {
    (void)win;
    if ((unsigned char)key == KEY_F2) vdesk_toggle_theme();
}

static void system_settings_click(VWindow* win, int lx, int ly) {
    (void)win;
    if (IN_RECT(lx, ly, SETTINGS_BTN_X, 92, SETTINGS_BTN_W, SETTINGS_BTN_H))
        vdesk_toggle_theme();
}

/* ---- File Explorer app ---- */
static void open_editor_file(const char* filename, Directory* dir);
static void open_paint_file(const char* filename, Directory* dir);
static void open_vga_console_window(void);

typedef struct {
    int selected;
} ExplorerApp;

static void explorer_render(VWindow* win, int cx, int cy, int cw, int ch) {
    (void)cw; (void)ch;
    ExplorerApp* app = (ExplorerApp*)win->user_data;
    const Directory* dir = fs_get_current_dir();
    vdesk_draw_rect(cx, cy, cw, ch, VCOLOR_BLUE);

    vdesk_draw_text(cx + 2, cy + 2, "CWD: ", VCOLOR_CYAN, VCOLOR_BLUE);
    vdesk_draw_text(cx + 40, cy + 2, fs_get_cwd(), VCOLOR_WHITE, VCOLOR_BLUE);

    int row = 2;
    int index = 0;
    if (dir) {
        for (int i = 0; i < (int)dir->child_count && row < ch / 16; i++, row++) {
            uint32_t bg = (app && app->selected == index) ? VCOLOR_DARK_GRAY : VCOLOR_BLUE;
            vdesk_draw_rect(cx, cy + row * 16, cw, 16, bg);
            vdesk_draw_text(cx + 2, cy + row * 16, "[D] ", VCOLOR_LIGHT_GREEN, bg);
            vdesk_draw_text(cx + 30, cy + row * 16, dir->children[i].name,
                           VCOLOR_LIGHT_GREEN, bg);
            index++;
        }
        for (int i = 0; i < (int)dir->file_count && row < ch / 16; i++, row++) {
            uint32_t bg = (app && app->selected == index) ? VCOLOR_DARK_GRAY : VCOLOR_BLUE;
            vdesk_draw_rect(cx, cy + row * 16, cw, 16, bg);
            vdesk_draw_text(cx + 2, cy + row * 16, "[F] ", VCOLOR_BROWN, bg);
            vdesk_draw_text(cx + 30, cy + row * 16, dir->files[i].name,
                           VCOLOR_BROWN, bg);
            index++;
        }
    }

    vdesk_draw_text(cx + 2, cy + ch - 16, "Backspace=up  Enter=open",
                   VCOLOR_CYAN, VCOLOR_BLUE);
}

static void explorer_key(VWindow* win, char key) {
    ExplorerApp* app = (ExplorerApp*)win->user_data;
    const Directory* dir = fs_get_current_dir();
    if (!app || !dir) return;
    int total = (int)(dir->child_count + dir->file_count);
    if ((unsigned char)key == KEY_UP && app->selected > 0) app->selected--;
    else if ((unsigned char)key == KEY_DOWN && app->selected < total - 1) app->selected++;
    else if ((unsigned char)key == KEY_BACKSPACE) {
        fs_cd_up();
        app->selected = 0;
    } else if (key == '\r' || key == '\n') {
        if (app->selected < (int)dir->child_count) {
            fs_change_dir(dir->children[app->selected].name);
            app->selected = 0;
        } else {
            int file_idx = app->selected - (int)dir->child_count;
            if (file_idx >= 0 && file_idx < (int)dir->file_count) {
                const char* name = dir->files[file_idx].name;
                if (has_suffix(name, ".txt")) open_editor_file(name, fs_get_cwd_dir());
                else if (has_suffix(name, ".gbm")) open_paint_file(name, fs_get_cwd_dir());
            }
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

static void open_shell_window(void) {
    VWindow* win = vdesk_create_window("Shell", 60, 40, 500, 320);
    if (win) {
        create_process("vesa-shell", 4);
        ShellApp* sa = create_shell_app();
        win->user_data = sa;
        win->render = shell_render;
        win->key_handler = shell_key;
        win->scroll_handler = shell_scroll;
    }
}

static void open_sysinfo_window(void) {
    VWindow* win = vdesk_create_window("System Info", 220, 40, 390, 310);
    if (win) {
        create_process("vesa-info", 1);
        win->render = sysinfo_render;
    }
}

static void open_explorer_window(void) {
    VWindow* win = vdesk_create_window("File Explorer", 90, 150, 330, 230);
    if (win) {
        create_process("vesa-files", 2);
        ExplorerApp* app = (ExplorerApp*)kmalloc(sizeof(ExplorerApp));
        if (app) {
            app->selected = 0;
            win->user_data = app;
        }
        win->render = explorer_render;
        win->key_handler = explorer_key;
    }
}

static void open_editor_file(const char* filename, Directory* dir) {
    VWindow* win = vdesk_create_window("Text Editor", 420, 160, 420, 240);
    if (win) {
        create_process("vesa-editor", 6);
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
        create_process("vesa-paint", 4);
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
        create_process("vesa-tasks", 2);
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
    VWindow* win = vdesk_create_window("Display Settings", 300, 180, 380, 230);
    if (win) {
        create_process("vesa-setup", 1);
        win->render = display_settings_render;
        win->key_handler = display_settings_key;
        win->click_handler = display_settings_click;
    }
}

static void open_system_settings_window(void) {
    VWindow* win = vdesk_create_window("System Settings", 260, 120, 360, 230);
    if (win) {
        create_process("vesa-settings", 1);
        win->render = system_settings_render;
        win->key_handler = system_settings_key;
        win->click_handler = system_settings_click;
    }
}

/*
 * ---- Phase 4 (item 5): VGA-passthrough window class ----
 *
 * A minimal VESA window whose body is the 80x25 virtual cell grid owned by
 * gui/vga_passthrough.c. Designed for legacy text-mode apps (editor,
 * games) that historically wrote directly to 0xB8000: those writes are
 * intercepted by the shim and end up in the cell grid, then this
 * window's render callback composites them through the 8x16 font onto
 * the panel pixel grid.
 *
 * Per-app integration (running the actual game/editor inside this
 * window) is intentionally out of scope here; that requires async input
 * pumping and a per-app render state machine. What this window class
 * provides today is the SURFACE -- a guaranteed-correct conduit from
 * the shim's cell grid to the panel pixels, exercised by booting an
 * app inside the shim and watching the cells light up.
 */
static void vga_console_render(VWindow* win, int cx, int cy, int cw, int ch) {
    (void)win;
    /* Background -- classic VGA black so unset cells look right. */
    vdesk_draw_rect(cx, cy, cw, ch, VCOLOR_BLACK);

    /* Idempotent: arm the shim on first paint so writes coming from a
     * legacy app inside the process body land in the cell grid. */
    if (!vga_passthrough_active()) {
        vga_passthrough_arm();
        vga_passthrough_clear();
        /* Seed a banner so the user sees the conduit is alive even
         * before any app writes into it. */
        const char* banner = "GooberOS VGA passthrough -- "
                             "legacy text-mode apps render here.";
        int x = 0;
        for (const char* p = banner; *p && x < VGA_PT_COLS; p++, x++) {
            vga_passthrough_writechar(x, 0, *p, 0x1F); /* white on blue */
        }
        const char* hint = "Launch editor or games from the shell to "
                           "see them paint into this window.";
        x = 0;
        for (const char* p = hint; *p && x < VGA_PT_COLS; p++, x++) {
            vga_passthrough_writechar(x, 2, *p, 0x07); /* light grey on black */
        }
    }

    vga_passthrough_present_into_window(cx, cy, cw, ch);
}

static void open_vga_console_window(void) {
    /* 8x16 font -> 80 cols * 8 = 640 px, 25 rows * 16 = 400 px is the
     * "natural" passthrough surface size, but we cap to a friendlier
     * 480x320 client area on small panels (60x20 visible cells). */
    VWindow* win = vdesk_create_window("VGA Console", 220, 130, 488, 360);
    if (!win) return;
    create_process("vga-console", 4);
    win->render = vga_console_render;
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
    }
}

static void vesa_launch_app(VDeskAppId app_id) {
    switch (app_id) {
        case VDESK_APP_SHELL:
            open_shell_window();
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
        case VDESK_APP_VGA_CONSOLE:
            open_vga_console_window();
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
    /* Pre-zero so the first frame has well-defined contents under the
     * desktop background (avoid flashing uninitialised heap memory if
     * the first paint somehow misses any region). */
    memset(bb, 0, need);
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
    vdesk_add_icon("Shell", VDESK_APP_SHELL, 24, 24);
    vdesk_add_icon("Files", VDESK_APP_EXPLORER, 24, 96);
    vdesk_add_icon("Editor", VDESK_APP_EDITOR, 24, 168);
    vdesk_add_icon("Tasks", VDESK_APP_TASK_MANAGER, 24, 240);
    vdesk_add_icon("Paint", VDESK_APP_PAINT, 24, 312);
    /* Phase 4 (item 5): launcher for the VGA-passthrough window class.
     * Editor / games launched standalone (via shell) into the shim end up
     * painted here through the 8x16 font path. */
    vdesk_add_icon("VGA App", VDESK_APP_VGA_CONSOLE, 24, 384);

    /* Ensure the dedicated Desktop folder exists (created once if missing) so
     * the desktop always reflects a fixed directory rather than the cwd. */
    fs_get_desktop_dir();
    vdesk_refresh_desktop_items(1);

    open_shell_window();

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
