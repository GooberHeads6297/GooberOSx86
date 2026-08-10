#ifndef VESA_WINDOW_H
#define VESA_WINDOW_H

#include <stdint.h>

typedef struct Directory Directory;

#define MAX_VWINDOWS 12
#define VWINDOW_TITLE_MAX 48
#define MAX_VDESKTOP_ICONS 32
#define VICON_NAME_MAX 32

/* Standard Windows 98-like colors */
#define VCOLOR_TEAL        0x008080
#define VCOLOR_GRAY        0xC0C0C0
#define VCOLOR_DARK_GRAY   0x808080
#define VCOLOR_WHITE       0xFFFFFF
#define VCOLOR_BLACK       0x000000
#define VCOLOR_TITLE_BLUE  0x000080
#define VCOLOR_TITLE_BG    0x000080
#define VCOLOR_TITLE_TEXT  0xFFFFFF
#define VCOLOR_DESKTOP_BG  0x008080
#define VCOLOR_TASKBAR_BG  0xC0C0C0
#define VCOLOR_START_GREEN 0x008000

/* Additional VESA colors */
#define VCOLOR_CYAN        0x00FFFF
#define VCOLOR_LIGHT_GREEN 0x00FF00
#define VCOLOR_GREEN       0x00AA00
#define VCOLOR_BROWN       0xAA5500
#define VCOLOR_BLUE        0x0000AA
#define VCOLOR_LIGHT_CYAN  0x00AAAA
#define VCOLOR_LIGHT_RED   0xFF0000

/* Button dimensions */
#define TASKBAR_HEIGHT  28
#define TITLEBAR_HEIGHT 20
#define BORDER_SIZE      4

typedef uint32_t color_t;

typedef enum {
    VDESK_APP_SHELL = 0,
    VDESK_APP_EXPLORER,
    VDESK_APP_EDITOR,
    VDESK_APP_SYSINFO,
    VDESK_APP_TASK_MANAGER,
    VDESK_APP_DISPLAY_SETTINGS,
    VDESK_APP_SYSTEM_SETTINGS,
    VDESK_APP_PAINT,
    VDESK_APP_WELCOME,
    VDESK_APP_IDE,
    VDESK_APP_INSTALLER,
    VDESK_APP_DOS,
    VDESK_APP_DOOM,
    VDESK_APP_MINESWEEPER,
    VDESK_APP_CUBEDIP,
    VDESK_APP_SNAKEGAME,
    VDESK_APP_DOOMRAY
} VDeskAppId;

typedef struct {
    color_t desktop_bg;
    color_t taskbar_bg;
    color_t taskbar_top;
    color_t menu_bg;
    color_t menu_fg;
    color_t menu_accent;
    color_t window_bg;
    color_t client_bg;
    color_t title_active_bg;
    color_t title_inactive_bg;
    color_t title_fg;
    color_t text;
    color_t text_muted;
    color_t border_outer;
    color_t border_light;
    color_t border_dark;
    color_t shadow;
    color_t accent;
    color_t button_bg;
    color_t shell_bg;
    color_t shell_output;
    color_t shell_input;
    color_t shell_muted;
} VTheme;

typedef enum {
    VDESK_APPEARANCE_ORIGINAL = 0,
    VDESK_APPEARANCE_MODERN_DARK,
    VDESK_APPEARANCE_LIGHT,
    VDESK_APPEARANCE_COUNT
} VDeskAppearance;

typedef enum {
    VDESK_TASKBAR_TOP = 0,
    VDESK_TASKBAR_BOTTOM
} VDeskTaskbarPosition;

typedef struct {
    uint32_t frame_count;
    uint32_t dirty_frames;
    uint32_t skipped_frames;
    uint32_t input_events;
    uint32_t frame_ticks;
    uint32_t render_ticks;
    uint32_t swap_ticks;
    uint32_t sleep_ticks;
    uint32_t fps;
    uint32_t present_count;
    uint32_t vblank_waits;
    uint32_t vblank_misses;
    uint32_t present_area;
    int present_mode;
    int promoted_frames;
    int last_dirty_x;
    int last_dirty_y;
    int last_dirty_w;
    int last_dirty_h;
    int active_pointer;
    int usb_pointer_active;
    int i2c_touchpad_active;
    int window_count;
    int icon_count;
    int theme_mode;
    int appearance;
} VDeskMetrics;

/* Desktop icon kind: app launcher vs. a filesystem item rendered on the desktop. */
typedef enum {
    VICON_APP = 0,
    VICON_FOLDER,
    VICON_TEXT,
    VICON_BITMAP,
    VICON_CODE,
    VICON_GOB,
    VICON_DOS
} VIconKind;

#define VDESK_TOAST_MAX 3
#define VDESK_TOAST_TITLE_MAX 28
#define VDESK_TOAST_BODY_MAX 48

typedef struct {
    int used;
    char title[VDESK_TOAST_TITLE_MAX];
    char body[VDESK_TOAST_BODY_MAX];
    uint32_t expire_tick;
} VDeskToast;

typedef struct {
    int id;
    int x, y;
    char label[32];
    VDeskAppId app_id;
    int selected;
    int drag_active;
    int drag_off_x, drag_off_y;
    VIconKind kind;
    char filename[VICON_NAME_MAX];
} VDesktopIcon;

typedef struct VWindow {
    int id;
    int x, y;
    int width, height;
    char title[VWINDOW_TITLE_MAX];

    int visible;
    int minimized;
    int focused;
    int has_close;
    int has_maximize;
    int has_minimize;

    int maximized;
    int saved_x, saved_y, saved_w, saved_h;

    int drag_active;
    int drag_off_x, drag_off_y;

    /* Interactive border/corner resize (VESA). Edges: N=1 S=2 E=4 W=8. */
    int resize_active;
    int resize_edges;
    int resize_start_mx, resize_start_my;
    int resize_orig_x, resize_orig_y, resize_orig_w, resize_orig_h;

    color_t title_bg;
    color_t title_fg;

    void (*render)(struct VWindow* win, int client_x, int client_y, int client_w, int client_h);
    void (*key_handler)(struct VWindow* win, char key);
    void (*scroll_handler)(struct VWindow* win, int amount);
    void (*tick_handler)(struct VWindow* win);
    /* Pointer click inside the client area; coords are client-relative. */
    void (*click_handler)(struct VWindow* win, int client_x, int client_y);
    /* Right-click / Shift+left in client area (optional). */
    void (*rclick_handler)(struct VWindow* win, int client_x, int client_y);
    void* user_data;
    /* Ring-0 app framework: process table PID for taskmgr End Task. */
    int process_pid;
} VWindow;

typedef struct {
    int screen_w;
    int screen_h;
    VWindow windows[MAX_VWINDOWS];
    int window_count;
    int next_id;
    int z_order[MAX_VWINDOWS];
    int z_count;

    int start_open;
    int context_open;
    int context_x, context_y;
    int context_kind;
    int context_target_icon;
    int context_target_window_id;
    int rename_open;
    int rename_target_kind;
    Directory* rename_dir;
    char rename_old_name[VICON_NAME_MAX];
    char rename_input[VICON_NAME_MAX];
    int rename_len;
    int rename_status;
    /* Cut/copy clipboard for desktop + explorer */
    int clip_mode; /* 0=none 1=copy 2=cut */
    Directory* clip_dir;
    char clip_name[VICON_NAME_MAX];
    int clip_is_dir;
    /* FS item context (desktop file icon or explorer row) */
    Directory* context_fs_dir;
    char context_fs_name[VICON_NAME_MAX];
    int context_fs_is_dir;
    int theme_mode;
    int appearance;
    int primary_shell_id;
    int shell_first_mode;
    int desktop_experience_visible;
    int tile_wm; /* 1 = auto-tile apps opened from shell (default) */
    int shift_click_rmb; /* 1 = Shift+left-click opens context menu (default) */
    int taskbar_position;
    int target_frame_ms;
    int adaptive_pacing;
    int dirty;
    int dirty_x1, dirty_y1, dirty_x2, dirty_y2;
    int last_mouse_x, last_mouse_y;
    int icon_press_index;
    int icon_press_x, icon_press_y;
    int icon_drag_moved;
    int icon_count;
    int app_icon_count;
    uint32_t fs_signature;
    uint32_t last_scan_tick;
    VDesktopIcon icons[MAX_VDESKTOP_ICONS];
    VDeskMetrics metrics;
    void (*launch_app)(VDeskAppId app_id);
    void (*open_file)(const char* name, int kind);
    void (*open_fs_item)(Directory* dir, const char* name, int is_dir);
    int mouse_x, mouse_y;
    int mouse_buttons;
    int running;
    char status_msg[64];
    uint32_t status_until_tick;
    VDeskToast toasts[VDESK_TOAST_MAX];
    int clock_tray_stamp;
} VDesktop;

/* Desktop functions */
void vdesk_init(int screen_w, int screen_h);
/* Update desktop metrics after a runtime modeset (keeps windows). */
void vdesk_set_screen_size(int screen_w, int screen_h);
void vdesk_run(void);
VWindow* vdesk_create_window(const char* title, int x, int y, int w, int h);
void vdesk_close_window(VWindow* win);
/* Close every visible window registered with the given process PID. */
void vdesk_close_windows_by_pid(int pid);
void vdesk_bring_to_front(VWindow* win);
VWindow* vdesk_window_at(int x, int y);
void vdesk_set_app_launcher(void (*launcher)(VDeskAppId app_id));
void vdesk_set_file_opener(void (*opener)(const char* name, int kind));
/* Open a file/folder from an arbitrary directory (explorer context Open). */
void vdesk_set_fs_item_opener(void (*opener)(Directory* dir, const char* name,
                                             int is_dir));
void vdesk_add_icon(const char* label, VDeskAppId app_id, int x, int y);
void vdesk_refresh_desktop_items(int force);
void vdesk_set_status(const char* msg);
void vdesk_notify(const char* title, const char* body);
void vdesk_mark_dirty(int x, int y, int w, int h);
void vdesk_mark_full_dirty(void);
/* One paint+present while a long sync command (e.g. install) holds the event loop. */
void vdesk_pump_one_frame(void);
void vdesk_toggle_theme(void);
void vdesk_set_appearance(int appearance);
int vdesk_get_appearance(void);
void vdesk_set_shift_click_rmb(int enabled);
int vdesk_shift_click_rmb_enabled(void);
/* Optional strong hook from desktop_vesa: persist prefs after theme/F9 changes. */
void vdesk_prefs_persist(void);
void vdesk_set_primary_shell(VWindow* win);
void vdesk_focus_primary_shell(void);
int vdesk_has_active_app_focus(void);
void vdesk_toggle_desktop_experience(void);
/* show_desktop=1 minimizes primary shell (desktop icons visible). */
void vdesk_set_desktop_experience(int show_desktop);
int vdesk_desktop_experience_visible(void);
void vdesk_tile_window(VWindow* win);
void vdesk_set_tile_wm(int enabled);
int vdesk_tile_wm_enabled(void);
color_t vdesk_shell_bg_color(void);
color_t vdesk_shell_output_color(void);
color_t vdesk_shell_input_color(void);
color_t vdesk_shell_muted_color(void);
void vdesk_cycle_shell_output_color(void);
void vdesk_cycle_shell_input_color(void);
int vdesk_workspace_top(void);
int vdesk_workspace_bottom(void);
const VTheme* vdesk_get_theme(void);
const VDeskMetrics* vdesk_get_metrics(void);
const char* vdesk_get_theme_name(void);

/* Drawing helpers */
void vdesk_draw_rect(int x, int y, int w, int h, color_t color);
void vdesk_draw_border(int x, int y, int w, int h, color_t light, color_t dark);
void vdesk_draw_text(int x, int y, const char* str, color_t fg, color_t bg);

/* In-OS file drag/drop (Explorer → /Dos or folder icons). */
void vdesk_get_pointer(int* x, int* y, int* buttons);
void vdesk_file_drag_begin(Directory* src_dir, const char* name);
void vdesk_file_drag_begin_ex(Directory* src_dir, const char* name, int is_dir);
int vdesk_file_drag_active(void);
void vdesk_file_drag_cancel(void);
/* Returns 1 if a drop was handled. Call on left button release. */
int vdesk_file_drag_drop(void);
/* Shared Open/Rename/Delete/Cut/Copy context for a filesystem item. */
void vdesk_open_fs_context(Directory* dir, const char* name, int is_dir,
                           int mx, int my);
void vdesk_clipboard_paste_into(Directory* dst);

#endif
