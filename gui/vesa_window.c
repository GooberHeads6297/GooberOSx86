#include "vesa_window.h"
#include "../drivers/video/vesa.h"
#include "../drivers/keyboard/keyboard.h"
#include "../drivers/mouse/mouse.h"
#include "../drivers/input/input.h"
#include "../drivers/input/touchpad.h"
#include "../drivers/timer/timer.h"
#include "../drivers/usb/usb.h"
#include "../fs/filesystem.h"
#include "../lib/string.h"
#include "../kernel.h"

/* Parse gooberos.theme=<original|dark|light>. Original is the shell-first default. */
static int vdesk_initial_theme_from_cmdline(void) {
    const char* cmdline = kernel_boot_cmdline();
    if (!cmdline) return VDESK_APPEARANCE_ORIGINAL;
    const char* p = cmdline;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        const char* needle = "gooberos.theme=";
        int i = 0;
        while (needle[i] && p[i] && needle[i] == p[i]) i++;
        if (!needle[i]) {
            const char* v = p + i;
            if (v[0] == 'l' && v[1] == 'i') return VDESK_APPEARANCE_LIGHT;
            if (v[0] == 'd' && v[1] == 'a') return VDESK_APPEARANCE_MODERN_DARK;
            return VDESK_APPEARANCE_ORIGINAL;
        }
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return VDESK_APPEARANCE_ORIGINAL;
}

static VDesktop desktop;
static int new_file_count = 1;
static int new_bitmap_count = 1;
static int new_folder_count = 1;
static int shell_output_color_index = 0;
static int shell_input_color_index = 0;

static void clear_icon_selection(void);

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

static void mark_mouse_dirty(int x, int y) {
    vdesk_mark_dirty(x - 2, y - 2, 10, 10);
}

static void update_metrics_pointer(void) {
    input_device_t active = input_get_active_pointer();
    desktop.metrics.active_pointer = (int)active;
    desktop.metrics.usb_pointer_active =
        (active == INPUT_DEVICE_USB_MOUSE || active == INPUT_DEVICE_USB_TOUCHPAD);
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

void vdesk_init(int screen_w, int screen_h) {
    desktop.screen_w = screen_w;
    desktop.screen_h = screen_h;
    desktop.window_count = 0;
    desktop.next_id = 1;
    desktop.z_count = 0;
    desktop.start_open = 0;
    desktop.context_open = 0;
    desktop.context_x = 0;
    desktop.context_y = 0;
    desktop.context_kind = VCTX_DESKTOP;
    desktop.context_target_icon = -1;
    desktop.context_target_window_id = 0;
    desktop.rename_open = 0;
    desktop.rename_target_kind = VICON_FOLDER;
    desktop.rename_old_name[0] = '\0';
    desktop.rename_input[0] = '\0';
    desktop.rename_len = 0;
    desktop.rename_status = 0;
    desktop.appearance = vdesk_initial_theme_from_cmdline();
    desktop.theme_mode = (desktop.appearance == VDESK_APPEARANCE_LIGHT);
    desktop.primary_shell_id = 0;
    desktop.shell_first_mode = 1;
    desktop.desktop_experience_visible = 1;
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
    memset(&desktop.metrics, 0, sizeof(desktop.metrics));
    desktop.metrics.theme_mode = desktop.theme_mode;
    desktop.metrics.appearance = desktop.appearance;
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
    shell->minimized = 0;
    vdesk_bring_to_front(shell);
    vdesk_mark_full_dirty();
}

static int should_auto_focus_primary_shell(void) {
    return desktop.shell_first_mode && !desktop.desktop_experience_visible;
}

void vdesk_tile_window(VWindow* win) {
    (void)win;
    int ids[MAX_VWINDOWS];
    int count = 0;
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
    win->title_bg = theme()->title_active_bg;
    win->title_fg = theme()->title_fg;
    win->render = NULL;
    win->key_handler = NULL;
    win->scroll_handler = NULL;
    win->tick_handler = NULL;
    win->click_handler = NULL;
    win->user_data = NULL;

    strncpy(win->title, title, VWINDOW_TITLE_MAX - 1);
    win->title[VWINDOW_TITLE_MAX - 1] = '\0';

    desktop.window_count++;
    vdesk_bring_to_front(win);
    mark_window_dirty(win);
    return win;
}

void vdesk_close_window(VWindow* win) {
    if (!win || !win->visible) return;
    if (win->id == desktop.primary_shell_id) {
        vdesk_focus_primary_shell();
        return;
    }
    win->visible = 0;
    win->minimized = 0;
    mark_window_dirty(win);
    remove_z(win->id);
    desktop.window_count--;
    if (should_auto_focus_primary_shell() && !vdesk_has_active_app_focus())
        vdesk_focus_primary_shell();
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

static int desktop_name_kind(const char* name, int is_dir) {
    if (is_dir) return VICON_FOLDER;
    int n = (int)strlen(name);
    if (n >= 4 && strcmp(name + n - 4, ".gbm") == 0) return VICON_BITMAP;
    if (n >= 4 && strcmp(name + n - 4, ".txt") == 0) return VICON_TEXT;
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
    /* Grid laid out to the right of the app-launcher column. */
    int col_w = 80;
    int row_h = 72;
    int start_x = 110;
    int start_y = vdesk_workspace_top() + 24;
    int usable_h = vdesk_workspace_bottom() - vdesk_workspace_top() - 64;
    int rows = (usable_h - start_y) / row_h;
    if (rows < 1) rows = 1;
    int col = slot / rows;
    int row = slot % rows;
    icon->x = start_x + col * col_w;
    icon->y = start_y + row * row_h;
}

/*
 * Re-scan the current filesystem directory and rebuild the filesystem-backed
 * desktop icons (folders, .txt, .gbm). Existing icon positions/selection are
 * preserved across rescans when the file set is unchanged; the whole desktop is
 * marked dirty only when the set actually changes (so we never thrash the
 * render loop). Pass force=1 to rebuild unconditionally.
 */
void vdesk_refresh_desktop_items(int force) {
    const Directory* dir = fs_get_desktop_dir();
    if (!dir) return;

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
        if (prev >= 0) { icon->x = old[prev].x; icon->y = old[prev].y; }
        else place_file_icon(icon, slot);
        slot++;
        desktop.icon_count++;
    }

    for (size_t i = 0; i < dir->file_count && desktop.icon_count < MAX_VDESKTOP_ICONS; i++) {
        int kind = desktop_name_kind(dir->files[i].name, 0);
        if (kind < 0) continue;
        VDesktopIcon* icon = &desktop.icons[desktop.icon_count];
        icon->id = desktop.icon_count + 1;
        icon->kind = (VIconKind)kind;
        icon->app_id = (kind == VICON_TEXT) ? VDESK_APP_EDITOR : VDESK_APP_PAINT;
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
        if (prev >= 0) { icon->x = old[prev].x; icon->y = old[prev].y; }
        else place_file_icon(icon, slot);
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
}

void vdesk_set_appearance(int appearance) {
    if (appearance < 0 || appearance >= VDESK_APPEARANCE_COUNT)
        appearance = VDESK_APPEARANCE_ORIGINAL;
    desktop.appearance = appearance;
    desktop.theme_mode = (appearance == VDESK_APPEARANCE_LIGHT);
    desktop.metrics.theme_mode = desktop.theme_mode;
    desktop.metrics.appearance = desktop.appearance;
    vdesk_mark_full_dirty();
}

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

void vdesk_toggle_desktop_experience(void) {
    VWindow* shell = get_window(desktop.primary_shell_id);
    desktop.desktop_experience_visible = desktop.desktop_experience_visible ? 0 : 1;
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

static int taskbar_button_rect(int slot, int* bx, int* bw) {
    int x = TASK_BTN_START + slot * (TASK_BTN_W + TASK_BTN_GAP);
    if (x + TASK_BTN_W > desktop.screen_w - 2) return 0;
    *bx = x;
    *bw = TASK_BTN_W;
    return 1;
}

static void render_taskbar(void) {
    int ty = (desktop.taskbar_position == VDESK_TASKBAR_TOP) ?
             0 : desktop.screen_h - TASKBAR_HEIGHT;
    const VTheme* t = theme();

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

static void render_start_menu(void) {
    if (!desktop.start_open) return;
    const VTheme* t = theme();

    int menu_w = 160;
    int menu_h = 224;
    int mx = 2;
    int my = (desktop.taskbar_position == VDESK_TASKBAR_TOP) ?
             TASKBAR_HEIGHT : desktop.screen_h - TASKBAR_HEIGHT - menu_h;

    vdesk_draw_rect(mx, my, menu_w, menu_h, t->menu_bg);
    vdesk_draw_border(mx, my, menu_w, menu_h, t->border_light, t->border_outer);
    vdesk_draw_rect(mx + 2, my + 2, menu_w - 4, 18, t->menu_accent);
    vdesk_draw_text(mx + 4, my + 4, "GooberOS", VCOLOR_WHITE, t->menu_accent);

    const char* items[] = {"Shell", "File Explorer", "Text Editor", "System Info",
                           "Task Manager", "System Settings", "Display Settings", "Paint",
                           desktop.desktop_experience_visible ? "Hide Desktop" : "Show Desktop"};
    int n_items = 9;
    for (int i = 0; i < n_items; i++) {
        int iy = my + 24 + i * 22;
        vdesk_draw_rect(mx + 2, iy, menu_w - 4, 20, t->menu_bg);
        vdesk_draw_text(mx + 8, iy + 4, items[i], t->menu_fg, t->menu_bg);
    }
}

static int start_menu_hit(int x, int y) {
    int ty = (desktop.taskbar_position == VDESK_TASKBAR_TOP) ?
             0 : desktop.screen_h - TASKBAR_HEIGHT;
    return (y >= ty && y < ty + TASKBAR_HEIGHT && x >= 2 && x < 62);
}

static int start_menu_item_at(int x, int y) {
    if (!desktop.start_open) return -1;
    int menu_w = 160;
    int menu_h = 224;
    int mx = 2;
    int my = (desktop.taskbar_position == VDESK_TASKBAR_TOP) ?
             TASKBAR_HEIGHT : desktop.screen_h - TASKBAR_HEIGHT - menu_h;

    if (x < mx || x >= mx + menu_w || y < my || y >= my + menu_h)
        return -1;

    int n_items = 9;
    for (int i = 0; i < n_items; i++) {
        int iy = my + 24 + i * 22;
        if (y >= iy && y < iy + 20) return i;
    }
    return -1;
}

static VDeskAppId start_item_to_app(int item) {
    switch (item) {
        case 0: return VDESK_APP_SHELL;
        case 1: return VDESK_APP_EXPLORER;
        case 2: return VDESK_APP_EDITOR;
        case 3: return VDESK_APP_SYSINFO;
        case 4: return VDESK_APP_TASK_MANAGER;
        case 5: return VDESK_APP_SYSTEM_SETTINGS;
        case 6: return VDESK_APP_DISPLAY_SETTINGS;
        case 7: return VDESK_APP_PAINT;
        case 8: return VDESK_APP_SHELL;
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
    if (icon->kind == VICON_BITMAP) {
        vdesk_draw_rect(x + 2, y + 2, 26, 24, 0xFFFFFF);
        vdesk_draw_border(x + 2, y + 2, 26, 24, t->border_light, t->border_dark);
        vdesk_draw_rect(x + 5, y + 16, 6, 8, 0xE53935);
        vdesk_draw_rect(x + 12, y + 11, 6, 13, 0x43A047);
        vdesk_draw_rect(x + 19, y + 6, 6, 18, 0x1E88E5);
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
    } else {
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

static int context_item_at(int x, int y) {
    if (!desktop.context_open) return -1;
    int menu_w = 156;
    int item_h = 20;
    int count = 10;
    if (desktop.context_kind == VCTX_ICON) count = 2;
    else if (desktop.context_kind == VCTX_TASKBAR) count = 3;
    if (x < desktop.context_x || x >= desktop.context_x + menu_w ||
        y < desktop.context_y || y >= desktop.context_y + count * item_h)
        return -1;
    return (y - desktop.context_y) / item_h;
}

static void render_context_menu(void) {
    if (!desktop.context_open) return;
    const VTheme* t = theme();
    int count = 10;
    int menu_w = 156;
    int item_h = 20;
    int x = desktop.context_x;
    int y = desktop.context_y;
    const char* desktop_items[] = {"New Folder", "New Text File", "New Bitmap", "Open Shell",
                                   "Open Editor", "File Explorer", "System Info", "Task Manager",
                                   "Display Settings", "System Settings"};
    const char* icon_items[] = {"Open", "Rename"};
    const char* task_items[3];
    const char** items = desktop_items;

    if (desktop.context_kind == VCTX_ICON) {
        items = icon_items;
        count = 2;
    } else if (desktop.context_kind == VCTX_TASKBAR) {
        VWindow* win = get_window(desktop.context_target_window_id);
        task_items[0] = (win && win->minimized) ? "Restore" : "Minimize";
        task_items[1] = (win && win->maximized) ? "Restore Size" : "Maximize";
        task_items[2] = "Close";
        items = task_items;
        count = 3;
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
    if (!desktop.rename_open || desktop.rename_input[0] == '\0') {
        desktop.rename_status = 1;
        return;
    }
    if (fs_dir_rename(fs_get_desktop_dir(), desktop.rename_old_name,
                      desktop.rename_input) == 0) {
        desktop.rename_open = 0;
        desktop.rename_status = 0;
        vdesk_refresh_desktop_items(1);
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

static void open_rename_modal_for_icon(int icon_idx) {
    if (icon_idx < 0 || icon_idx >= desktop.icon_count) return;
    VDesktopIcon* icon = &desktop.icons[icon_idx];
    if (icon->kind == VICON_APP) return;
    desktop.rename_open = 1;
    desktop.rename_target_kind = icon->kind;
    strncpy(desktop.rename_old_name, icon->filename, VICON_NAME_MAX - 1);
    desktop.rename_old_name[VICON_NAME_MAX - 1] = '\0';
    strncpy(desktop.rename_input, icon->filename, VICON_NAME_MAX - 1);
    desktop.rename_input[VICON_NAME_MAX - 1] = '\0';
    desktop.rename_len = (int)strlen(desktop.rename_input);
    desktop.rename_status = 0;
    vdesk_mark_full_dirty();
}

static void context_run_icon_item(int item) {
    int idx = desktop.context_target_icon;
    if (idx < 0 || idx >= desktop.icon_count) return;
    if (item == 0) activate_icon(idx);
    else if (item == 1) open_rename_modal_for_icon(idx);
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

    if (desktop.context_kind == VCTX_ICON) {
        context_run_icon_item(item);
        return;
    }
    if (desktop.context_kind == VCTX_TASKBAR) {
        context_run_taskbar_item(item);
        return;
    }

    /* New items are placed in the fixed Desktop folder (not whatever directory
     * File Explorer happens to be browsing) so they appear on the desktop. */
    Directory* desk = fs_get_desktop_dir();

    if (item == 0) {
        strcpy(name, "NewFolder");
        itoa(new_folder_count++, num, 10);
        strcat(name, num);
        fs_dir_create_dir(desk, name);
        vdesk_refresh_desktop_items(1);
        vdesk_mark_full_dirty();
        return;
    }

    if (item == 1) {
        strcpy(name, "NewFile");
        itoa(new_file_count++, num, 10);
        strcat(name, num);
        strcat(name, ".txt");
        fs_dir_create(desk, name);
        vdesk_refresh_desktop_items(1);
        vdesk_mark_full_dirty();
        return;
    }

    if (item == 2) {
        uint8_t pixels[32 * 32];
        for (int i = 0; i < 32 * 32; i++) pixels[i] = 0;
        strcpy(name, "Artwork");
        itoa(new_bitmap_count++, num, 10);
        strcat(name, num);
        strcat(name, ".gbm");
        fs_dir_write(desk, name, pixels, sizeof(pixels));
        vdesk_refresh_desktop_items(1);
        vdesk_mark_full_dirty();
        return;
    }

    launch_app(context_item_to_app(item));
}

static void render_mouse(void) {
    int mx = desktop.mouse_x;
    int my = desktop.mouse_y;
    if (mx < 0 || mx >= desktop.screen_w || my < 0 || my >= desktop.screen_h)
        return;

    vesa_fill_rect(mx, my, 6, 2, VCOLOR_WHITE);
    vesa_fill_rect(mx, my, 2, 8, VCOLOR_WHITE);
    vesa_fill_rect(mx + 2, my + 2, 2, 2, theme()->border_outer);
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
    if (!keyboard_has_char()) return;

    while (keyboard_has_char()) {
        char c = keyboard_read_char();

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
        if ((unsigned char)c == KEY_F4 && !vdesk_has_active_app_focus()) {
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
            return;
        }

        if ((unsigned char)c == KEY_F9) {
            vdesk_toggle_theme();
            continue;
        }

        for (int zi = desktop.z_count - 1; zi >= 0; zi--) {
            VWindow* win = get_window(desktop.z_order[zi]);
            if (win && win->visible && !win->minimized && win->focused && win->key_handler) {
                win->key_handler(win, c);
                mark_window_dirty(win);
                break;
            }
        }
        if (should_auto_focus_primary_shell() && !vdesk_has_active_app_focus())
            vdesk_focus_primary_shell();
    }
}

static void handle_events(void) {
    input_event_t ev;
    while (input_poll_event(&ev)) {
        desktop.metrics.input_events++;
        if (ev.type == INPUT_EVENT_POINTER_MOVE) {
            mark_mouse_dirty(desktop.mouse_x, desktop.mouse_y);
            desktop.mouse_x = ev.x;
            desktop.mouse_y = ev.y;
            desktop.mouse_x = CLAMP(desktop.mouse_x, 0, desktop.screen_w - 1);
            desktop.mouse_y = CLAMP(desktop.mouse_y, 0, desktop.screen_h - 1);
            mark_mouse_dirty(desktop.mouse_x, desktop.mouse_y);
        }

        if (ev.type == INPUT_EVENT_BUTTON_DOWN && ev.button == INPUT_BUTTON_LEFT) {
            int mx = desktop.mouse_x;
            int my = desktop.mouse_y;

            if (rename_modal_click(mx, my)) {
                desktop.context_open = 0;
                desktop.start_open = 0;
                continue;
            }

            if (start_menu_hit(mx, my)) {
                desktop.start_open = !desktop.start_open;
                desktop.context_open = 0;
                vdesk_mark_dirty(0, 0, 180, 248 + TASKBAR_HEIGHT);
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
                int item = start_menu_item_at(mx, my);
                if (item >= 0) {
                    desktop.start_open = 0;
                    vdesk_mark_full_dirty();
                    if (item == 8) {
                        vdesk_toggle_desktop_experience();
                        continue;
                    }
                    launch_app(start_item_to_app(item));
                    continue;
                }
                desktop.start_open = 0;
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
            for (int i = 0; i < MAX_VWINDOWS; i++) {
                if (desktop.windows[i].visible)
                    desktop.windows[i].drag_active = 0;
            }
            for (int i = 0; i < desktop.icon_count; i++)
                desktop.icons[i].drag_active = 0;
            if (released_icon >= 0 && released_icon < desktop.icon_count &&
                !desktop.icon_drag_moved) {
                activate_icon(released_icon);
            }
            desktop.icon_press_index = -1;
        }

        if (ev.type == INPUT_EVENT_BUTTON_DOWN && ev.button == INPUT_BUTTON_RIGHT) {
            int mx = desktop.mouse_x;
            int my = desktop.mouse_y;
            if (desktop.rename_open) continue;
            VWindow* task_win = taskbar_window_at(mx, my);
            if (task_win && task_win->id != desktop.primary_shell_id) {
                desktop.start_open = 0;
                desktop.context_open = 1;
                desktop.context_kind = VCTX_TASKBAR;
                desktop.context_target_window_id = task_win->id;
                desktop.context_target_icon = -1;
                desktop.context_x = CLAMP(mx, 0, desktop.screen_w - 160);
                desktop.context_y = CLAMP(my, vdesk_workspace_top(),
                                          vdesk_workspace_bottom() - 62);
                vdesk_mark_full_dirty();
                continue;
            } else if (task_win) {
                continue;
            }
            if (!vdesk_window_at(mx, my)) {
                int idx = icon_at(mx, my);
                desktop.start_open = 0;
                desktop.context_open = 1;
                desktop.context_kind = (idx >= desktop.app_icon_count) ? VCTX_ICON : VCTX_DESKTOP;
                desktop.context_target_icon = idx;
                desktop.context_target_window_id = 0;
                desktop.context_x = CLAMP(mx, 0, desktop.screen_w - 160);
                desktop.context_y = CLAMP(my, vdesk_workspace_top(),
                                          vdesk_workspace_bottom() - 202);
                clear_icon_selection();
                if (idx >= 0 && idx < desktop.icon_count)
                    desktop.icons[idx].selected = 1;
                vdesk_mark_full_dirty();
                continue;
            }
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
    while (desktop.running) {
        uint32_t frame_start = timer_ticks();
        uint32_t render_start;
        uint32_t swap_start;
        int had_dirty;

        usb_poll();
        touchpad_poll();
        update_metrics_pointer();
        handle_keyboard();
        handle_events();

        /*
         * Re-scan the filesystem for desktop items a couple of times per
         * second so files/folders created via the context menu or by apps
         * appear without a reboot. The scan only marks the desktop dirty when
         * the set actually changed, so it does not force redraws.
         */
        if ((uint32_t)(frame_start - desktop.last_scan_tick) >= 25) {
            desktop.last_scan_tick = frame_start;
            vdesk_refresh_desktop_items(0);
        }

        /*
         * Periodically repaint visible windows so live content (clocks, task
         * manager, FPS counters, blinking cursors) refreshes.
         */
        if ((desktop.metrics.frame_count % 25) == 0) {
            for (int i = 0; i < MAX_VWINDOWS; i++) {
                if (desktop.windows[i].visible && !desktop.windows[i].minimized)
                    mark_window_dirty(&desktop.windows[i]);
            }
        }

        for (int zi = 0; zi < desktop.z_count; zi++) {
            VWindow* win = get_window(desktop.z_order[zi]);
            if (win && win->visible && !win->minimized && win->tick_handler)
                win->tick_handler(win);
        }

        had_dirty = desktop.dirty;
        if (had_dirty) {
            int dirty_x = desktop.dirty_x1;
            int dirty_y = desktop.dirty_y1;
            int dirty_w = desktop.dirty_x2 - desktop.dirty_x1;
            int dirty_h = desktop.dirty_y2 - desktop.dirty_y1;
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
            render_start_menu();
            render_context_menu();
            render_rename_modal();
            /* Cursor sprite is the LAST step of the composite so it
             * always lands on top of every other surface. */
            render_mouse();
            vesa_clear_clip();
            desktop.metrics.render_ticks = timer_ticks() - render_start;

            swap_start = timer_ticks();
            /* Publish only the invalidated rectangle when a back-buffer exists. */
            vesa_swap_rect(dirty_x, dirty_y, dirty_w, dirty_h);
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
                timer_sleep((uint32_t)(budget - (int)elapsed_ms));
            }
            desktop.metrics.sleep_ticks = (timer_ticks() - frame_start) - elapsed_ticks;
            desktop.metrics.frame_ticks = timer_ticks() - frame_start;
        }
        desktop.metrics.frame_count++;
        desktop.metrics.window_count = desktop.window_count;
        desktop.metrics.icon_count = desktop.icon_count;
        desktop.metrics.theme_mode = desktop.theme_mode;
        if (desktop.metrics.frame_ticks > 0) {
            desktop.metrics.fps = 100 / desktop.metrics.frame_ticks;
        }
    }

    vesa_boot_splash("Desktop stopped. Reboot or choose VGA safe mode.");
}
