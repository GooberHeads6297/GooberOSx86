#include "window.h"
#include "../drivers/video/vga.h"
#include "../drivers/keyboard/keyboard.h"
#include "../drivers/input/input.h"
#include "../drivers/timer/timer.h"
#include "../fs/filesystem.h"
#include "../lib/memory.h"
#include "../lib/string.h"

typedef struct {
    int tick_count;
} app_sys_state_t;

typedef struct {
    int ball_x;
    int ball_y;
    int dx;
    int dy;
} app_bounce_state_t;

#define TERM_MAX_LINES 80
#define TERM_LINE_LEN 78
typedef struct {
    char lines[TERM_MAX_LINES][TERM_LINE_LEN];
    int line_count;
    int scroll_top;
    char input[TERM_LINE_LEN];
    int input_len;
    int input_cursor;
    char history[16][TERM_LINE_LEN];
    int history_count;
    int history_next;
    int history_nav_offset; // -1 when not browsing history
    char saved_input[TERM_LINE_LEN];
} app_terminal_state_t;

#define NOTE_MAX_TEXT 2048
typedef struct {
    char text[NOTE_MAX_TEXT];
    int len;
    int scroll_row;
    int dirty;
    char filename[32];
    int cursor_pos;
    int preferred_col;
    uint32_t blink_tick;
    int cursor_visible;
} app_notepad_state_t;

typedef struct {
    int body[64][2];
    int length;
    int dir;
    int food_x;
    int food_y;
    int alive;
    int score;
    uint32_t last_step_tick;
} app_snake_state_t;

typedef struct {
    int block_x;
    int block_y;
    int stack[16][10];
    int score;
    uint32_t last_fall_tick;
    int alive;
} app_cubedip_state_t;

typedef struct {
    int selected;
    int scroll_top;
} app_explorer_state_t;

typedef enum {
    LAUNCH_WELCOME = 0,
    LAUNCH_SYSTEM,
    LAUNCH_BOUNCE,
    LAUNCH_SHELL,
    LAUNCH_NOTEPAD,
    LAUNCH_SNAKE,
    LAUNCH_CUBEDIP,
    LAUNCH_EXPLORER,
    LAUNCH_COUNT
} launcher_item_t;

static Window windows[MAX_WINDOWS];
static int z_order[MAX_WINDOWS];
static int z_count = 0;
static int window_count = 0;
static int next_window_id = 1;
static uint16_t backbuffer[SCREEN_WIDTH * SCREEN_HEIGHT];
static int gui_running = 0;
static Window* focused_window = NULL;
static Window* drag_window = NULL;
static int drag_offset_x = 0;
static int drag_offset_y = 0;
static int start_menu_open = 0;

#define TOOLBAR_ROW 0
#define START_BTN_X 1
#define START_BTN_W 7
#define MENU_X 1
#define MENU_Y 1
#define MENU_W 28

static const char* launcher_labels[LAUNCH_COUNT] = {
    "Welcome",
    "System Monitor",
    "Bounce Demo",
    "Shell",
    "Text Editor",
    "snake",
    "cubeDip",
    "File Explorer"
};

static Window* launch_app(launcher_item_t item, const char* arg);
static void bring_to_front(Window* win);
static void set_focused_window(Window* win);

static int min_int(int a, int b) { return (a < b) ? a : b; }
static int max_int(int a, int b) { return (a > b) ? a : b; }

static void append_limited(char* dst, const char* src, int max_len) {
    int d = 0;
    int s = 0;
    if (!dst || !src || max_len <= 0) return;
    while (d < max_len - 1 && dst[d] != '\0') d++;
    while (d < max_len - 1 && src[s] != '\0') dst[d++] = src[s++];
    dst[d] = '\0';
}

static int starts_with(const char* s, const char* pfx) {
    if (!s || !pfx) return 0;
    return strncmp(s, pfx, strlen(pfx)) == 0;
}

static Window* top_active_window(void) {
    for (int z = z_count - 1; z >= 0; z--) {
        Window* win = &windows[z_order[z]];
        if (win->active) return win;
    }
    return NULL;
}

static void focus_cycle_next(void) {
    if (z_count <= 0) return;
    if (!focused_window) {
        set_focused_window(&windows[z_order[z_count - 1]]);
        return;
    }
    for (int z = 0; z < z_count; z++) {
        if (&windows[z_order[z]] == focused_window) {
            int next = (z + 1) % z_count;
            set_focused_window(&windows[z_order[next]]);
            bring_to_front(&windows[z_order[next]]);
            return;
        }
    }
}

static int point_in_window_frame(const Window* win, int x, int y) {
    int left = win->x - 1;
    int right = win->x + win->width;
    int top = win->y - 1;
    int bottom = win->y + win->height;
    return x >= left && x <= right && y >= top && y <= bottom;
}

static int point_in_title_bar(const Window* win, int x, int y) {
    return y == (win->y - 1) && x >= (win->x - 1) && x <= (win->x + win->width);
}

static int title_close_hit(const Window* win, int x, int y) {
    return point_in_title_bar(win, x, y) && x == (win->x + win->width - 1);
}

static int title_maximize_hit(const Window* win, int x, int y) {
    return point_in_title_bar(win, x, y) && x == (win->x + win->width - 3);
}

static void remove_from_z_order(int idx) {
    for (int i = 0; i < z_count; i++) {
        if (z_order[i] == idx) {
            for (int j = i; j < z_count - 1; j++) z_order[j] = z_order[j + 1];
            z_count--;
            return;
        }
    }
}

static void bring_to_front(Window* win) {
    int idx;
    if (!win) return;
    idx = (int)(win - windows);
    if (idx < 0 || idx >= MAX_WINDOWS) return;
    remove_from_z_order(idx);
    if (z_count < MAX_WINDOWS) z_order[z_count++] = idx;
}

static void set_focused_window(Window* win) {
    focused_window = win;
    for (int i = 0; i < MAX_WINDOWS; i++) windows[i].focused = (win == &windows[i]) ? 1 : 0;
}

static Window* top_window_at(int x, int y, int title_only) {
    for (int z = z_count - 1; z >= 0; z--) {
        Window* win = &windows[z_order[z]];
        if (!win->active) continue;
        if (title_only ? point_in_title_bar(win, x, y) : point_in_window_frame(win, x, y)) return win;
    }
    return NULL;
}

static int window_resize_buffer(Window* win, int new_w, int new_h) {
    uint16_t* new_buf;
    int copy_h, copy_w;
    if (!win || new_w <= 0 || new_h <= 0) return 0;
    new_buf = (uint16_t*)kmalloc((size_t)new_w * (size_t)new_h * sizeof(uint16_t));
    if (!new_buf) return 0;

    for (int i = 0; i < new_w * new_h; i++) {
        new_buf[i] = ((VGA_COLOR_WHITE | (VGA_COLOR_BLACK << 4)) << 8) | ' ';
    }

    if (win->buffer) {
        copy_h = min_int(win->height, new_h);
        copy_w = min_int(win->width, new_w);
        for (int r = 0; r < copy_h; r++) {
            for (int c = 0; c < copy_w; c++) {
                new_buf[r * new_w + c] = win->buffer[r * win->width + c];
            }
        }
        kfree(win->buffer);
    }

    win->buffer = new_buf;
    win->width = new_w;
    win->height = new_h;
    win->buffer_cells = new_w * new_h;
    return 1;
}

static void toggle_window_maximize(Window* win) {
    if (!win || !win->active) return;
    if (!win->maximized) {
        win->prev_x = win->x;
        win->prev_y = win->y;
        win->prev_width = win->width;
        win->prev_height = win->height;
        win->x = 1;
        win->y = 2;
        if (window_resize_buffer(win, SCREEN_WIDTH - 2, SCREEN_HEIGHT - 3)) {
            win->maximized = 1;
        }
    } else {
        win->x = win->prev_x;
        win->y = win->prev_y;
        if (window_resize_buffer(win, win->prev_width, win->prev_height)) {
            win->maximized = 0;
        }
    }
}

static void gui_shell_push_line(app_terminal_state_t* state, const char* text) {
    int slot;
    if (!state || !text) return;
    if (state->line_count < TERM_MAX_LINES) {
        slot = state->line_count++;
    } else {
        for (int i = 1; i < TERM_MAX_LINES; i++) strcpy(state->lines[i - 1], state->lines[i]);
        slot = TERM_MAX_LINES - 1;
    }
    strncpy(state->lines[slot], text, TERM_LINE_LEN - 1);
    state->lines[slot][TERM_LINE_LEN - 1] = '\0';
}

static void gui_shell_scroll_to_bottom(app_terminal_state_t* state, int view_h) {
    int max_top;
    if (!state) return;
    max_top = max_int(0, state->line_count - view_h);
    state->scroll_top = max_top;
}

static void parse_two_args(const char* in, char* a, int a_len, char* b, int b_len) {
    int i = 0, j = 0, k = 0;
    while (in[i] == ' ') i++;
    while (in[i] && in[i] != ' ' && j < a_len - 1) a[j++] = in[i++];
    a[j] = '\0';
    while (in[i] == ' ') i++;
    while (in[i] && k < b_len - 1) b[k++] = in[i++];
    b[k] = '\0';
}

static void term_insert_char(app_terminal_state_t* state, char ch) {
    if (!state || state->input_len >= TERM_LINE_LEN - 1) return;
    for (int i = state->input_len; i > state->input_cursor; i--) {
        state->input[i] = state->input[i - 1];
    }
    state->input[state->input_cursor] = ch;
    state->input_len++;
    state->input_cursor++;
    state->input[state->input_len] = '\0';
}

static void term_backspace_char(app_terminal_state_t* state) {
    if (!state || state->input_cursor <= 0 || state->input_len <= 0) return;
    for (int i = state->input_cursor - 1; i < state->input_len; i++) {
        state->input[i] = state->input[i + 1];
    }
    state->input_len--;
    state->input_cursor--;
}

static void term_add_history(app_terminal_state_t* state, const char* cmd) {
    if (!state || !cmd || cmd[0] == '\0') return;
    strncpy(state->history[state->history_next], cmd, TERM_LINE_LEN - 1);
    state->history[state->history_next][TERM_LINE_LEN - 1] = '\0';
    state->history_next = (state->history_next + 1) % 16;
    if (state->history_count < 16) state->history_count++;
}

static int term_history_index_from_offset(const app_terminal_state_t* state, int offset) {
    return (state->history_next - 1 - offset + 16) % 16;
}

static void shell_ls_into_lines(app_terminal_state_t* state) {
    const Directory* dir = fs_get_current_dir();
    if (!state || !dir) return;
    for (size_t i = 0; i < dir->child_count; i++) {
        char line[TERM_LINE_LEN];
        line[0] = '\0';
        append_limited(line, "<DIR> ", TERM_LINE_LEN);
        append_limited(line, dir->children[i].name, TERM_LINE_LEN);
        gui_shell_push_line(state, line);
    }
    for (size_t i = 0; i < dir->file_count; i++) {
        gui_shell_push_line(state, dir->files[i].name);
    }
}

static int notepad_line_col_to_cursor(const app_notepad_state_t* note, int target_row, int target_col) {
    int row = 0;
    int col = 0;
    int i = 0;
    if (!note) return 0;
    while (i < note->len) {
        if (row == target_row && col == target_col) return i;
        if (note->text[i] == '\n') {
            row++;
            col = 0;
            if (row > target_row) return i;
            i++;
            continue;
        }
        col++;
        i++;
    }
    return note->len;
}

static void notepad_cursor_to_line_col(const app_notepad_state_t* note, int cursor, int* out_row, int* out_col) {
    int row = 0;
    int col = 0;
    if (!note) { *out_row = 0; *out_col = 0; return; }
    for (int i = 0; i < cursor && i < note->len; i++) {
        if (note->text[i] == '\n') {
            row++;
            col = 0;
        } else {
            col++;
        }
    }
    *out_row = row;
    *out_col = col;
}

static void notepad_load_file(app_notepad_state_t* note, const char* filename) {
    FileHandle* fh;
    uint8_t buf[128];
    size_t n;
    note->len = 0;
    note->text[0] = '\0';
    if (!filename || filename[0] == '\0') return;
    strncpy(note->filename, filename, 31);
    note->filename[31] = '\0';
    fh = fs_open(filename);
    if (!fh) return;
    while ((n = fs_read(fh, buf, sizeof(buf))) > 0 && note->len < NOTE_MAX_TEXT - 1) {
        for (size_t i = 0; i < n && note->len < NOTE_MAX_TEXT - 1; i++) note->text[note->len++] = (char)buf[i];
    }
    note->text[note->len] = '\0';
    fs_close(fh);
}

static void notepad_save_file(app_notepad_state_t* note) {
    if (!note) return;
    if (note->filename[0] == '\0') {
        strcpy(note->filename, "note.txt");
    }
    fs_write(note->filename, (const uint8_t*)note->text, (size_t)note->len);
    note->dirty = 0;
}

static void app_welcome_tick(Window* win, uint32_t ticks) {
    (void)ticks;
    gui_clear_window(win, VGA_COLOR_WHITE | (VGA_COLOR_BLUE << 4));
    gui_draw_text(win, 1, 1, "GooberOS Display Manager", VGA_COLOR_WHITE | (VGA_COLOR_BLUE << 4));
    gui_draw_text(win, 1, 3, "Start menu launches apps and games.", VGA_COLOR_WHITE | (VGA_COLOR_BLUE << 4));
    gui_draw_text(win, 1, 4, "M/R toggles maximize, X closes.", VGA_COLOR_WHITE | (VGA_COLOR_BLUE << 4));
    gui_draw_text(win, 1, 5, "Shell and editor use real FS APIs.", VGA_COLOR_WHITE | (VGA_COLOR_BLUE << 4));
    gui_draw_text(win, 1, 7, "ESC exits GUI mode.", VGA_COLOR_WHITE | (VGA_COLOR_BLUE << 4));
}

static void app_system_tick(Window* win, uint32_t ticks) {
    app_sys_state_t* state = (app_sys_state_t*)win->app_state;
    char buf[16];
    if (!state) return;
    state->tick_count++;
    gui_clear_window(win, VGA_COLOR_BLACK | (VGA_COLOR_LIGHT_GREY << 4));
    gui_draw_text(win, 1, 1, "System", VGA_COLOR_BLACK | (VGA_COLOR_LIGHT_GREY << 4));
    gui_draw_text(win, 1, 2, "Ticks:", VGA_COLOR_BLACK | (VGA_COLOR_LIGHT_GREY << 4));
    itoa((int)ticks, buf, 10);
    gui_draw_text(win, 8, 2, buf, VGA_COLOR_BLACK | (VGA_COLOR_LIGHT_GREY << 4));
    gui_draw_text(win, 1, 3, "Mouse:", VGA_COLOR_BLACK | (VGA_COLOR_LIGHT_GREY << 4));
    itoa(input_get_pointer_x(), buf, 10); gui_draw_text(win, 8, 3, buf, VGA_COLOR_BLACK | (VGA_COLOR_LIGHT_GREY << 4));
    gui_draw_text(win, 12, 3, ",", VGA_COLOR_BLACK | (VGA_COLOR_LIGHT_GREY << 4));
    itoa(input_get_pointer_y(), buf, 10); gui_draw_text(win, 14, 3, buf, VGA_COLOR_BLACK | (VGA_COLOR_LIGHT_GREY << 4));
    gui_draw_text(win, 1, 4, "CWD:", VGA_COLOR_BLACK | (VGA_COLOR_LIGHT_GREY << 4));
    gui_draw_text(win, 6, 4, fs_get_cwd(), VGA_COLOR_BLACK | (VGA_COLOR_LIGHT_GREY << 4));
}

static void app_bounce_tick(Window* win, uint32_t ticks) {
    app_bounce_state_t* state = (app_bounce_state_t*)win->app_state;
    (void)ticks;
    if (!state) return;
    state->ball_x += state->dx;
    state->ball_y += state->dy;
    if (state->ball_x < 1 || state->ball_x >= win->width - 1) state->dx = -state->dx;
    if (state->ball_y < 1 || state->ball_y >= win->height - 1) state->dy = -state->dy;
    state->ball_x = max_int(1, min_int(win->width - 2, state->ball_x));
    state->ball_y = max_int(1, min_int(win->height - 2, state->ball_y));
    gui_clear_window(win, VGA_COLOR_WHITE | (VGA_COLOR_BLACK << 4));
    gui_draw_text(win, 1, 0, "Mini game (WASD changes direction)", VGA_COLOR_WHITE | (VGA_COLOR_BLACK << 4));
    win->buffer[state->ball_y * win->width + state->ball_x] = ((VGA_COLOR_LIGHT_RED | (VGA_COLOR_BLACK << 4)) << 8) | 'O';
}

static void app_bounce_key(Window* win, char key) {
    app_bounce_state_t* state = (app_bounce_state_t*)win->app_state;
    if (!state) return;
    if (key == 'w' || key == 'W') state->dy = -1;
    if (key == 's' || key == 'S') state->dy = 1;
    if (key == 'a' || key == 'A') state->dx = -1;
    if (key == 'd' || key == 'D') state->dx = 1;
}

static void snake_place_food(app_snake_state_t* s, int w, int h) {
    int tries = 100;
    while (tries--) {
        int x = (int)(timer_ticks() + tries) % w;
        int y = (int)((timer_ticks() * 3U) + (uint32_t)tries) % h;
        int ok = 1;
        for (int i = 0; i < s->length; i++) {
            if (s->body[i][0] == x && s->body[i][1] == y) { ok = 0; break; }
        }
        if (ok) { s->food_x = x; s->food_y = y; return; }
    }
    s->food_x = 0; s->food_y = 0;
}

static void app_snake_tick(Window* win, uint32_t ticks) {
    app_snake_state_t* s = (app_snake_state_t*)win->app_state;
    int board_w = max_int(8, win->width - 2);
    int board_h = max_int(6, win->height - 2);
    if (!s) return;
    if (!s->alive) {
        gui_clear_window(win, VGA_COLOR_WHITE | (VGA_COLOR_BLACK << 4));
        gui_draw_text(win, 1, 1, "Snake game over. Press R.", VGA_COLOR_WHITE | (VGA_COLOR_BLACK << 4));
        return;
    }
    if (ticks - s->last_step_tick >= 10) {
        int nx = s->body[0][0], ny = s->body[0][1];
        s->last_step_tick = ticks;
        if (s->dir == 0) ny--;
        if (s->dir == 1) nx++;
        if (s->dir == 2) ny++;
        if (s->dir == 3) nx--;
        if (nx < 0 || nx >= board_w || ny < 0 || ny >= board_h) {
            s->alive = 0;
        } else {
            for (int i = 0; i < s->length; i++) {
                if (s->body[i][0] == nx && s->body[i][1] == ny) s->alive = 0;
            }
        }
        if (s->alive) {
            for (int i = s->length; i > 0; i--) {
                if (i < 64) { s->body[i][0] = s->body[i - 1][0]; s->body[i][1] = s->body[i - 1][1]; }
            }
            s->body[0][0] = nx; s->body[0][1] = ny;
            if (nx == s->food_x && ny == s->food_y) {
                if (s->length < 63) s->length++;
                s->score += 10;
                snake_place_food(s, board_w, board_h);
            }
        }
    }
    gui_clear_window(win, VGA_COLOR_WHITE | (VGA_COLOR_BLACK << 4));
    char score[16];
    itoa(s->score, score, 10);
    gui_draw_text(win, 0, 0, "snake", VGA_COLOR_WHITE | (VGA_COLOR_BLACK << 4));
    gui_draw_text(win, 7, 0, score, VGA_COLOR_WHITE | (VGA_COLOR_BLACK << 4));
    for (int i = 0; i < s->length; i++) {
        int x = s->body[i][0];
        int y = s->body[i][1];
        if (x >= 0 && x < board_w && y >= 0 && y < board_h) {
            int idx = (y + 1) * win->width + (x + 1);
            if (idx >= 0 && idx < win->buffer_cells) {
                win->buffer[idx] = ((VGA_COLOR_LIGHT_GREEN | (VGA_COLOR_BLACK << 4)) << 8) | (i == 0 ? '@' : 'o');
            }
        }
    }
    {
        int idx = (s->food_y + 1) * win->width + (s->food_x + 1);
        if (idx >= 0 && idx < win->buffer_cells) {
            win->buffer[idx] = ((VGA_COLOR_LIGHT_RED | (VGA_COLOR_BLACK << 4)) << 8) | '*';
        }
    }
}

static void app_snake_key(Window* win, char key) {
    app_snake_state_t* s = (app_snake_state_t*)win->app_state;
    int board_w = max_int(8, win->width - 2);
    int board_h = max_int(6, win->height - 2);
    if (!s) return;
    if (!s->alive && (key == 'r' || key == 'R')) {
        s->length = 4; s->dir = 1; s->alive = 1; s->score = 0; s->last_step_tick = timer_ticks();
        for (int i = 0; i < s->length; i++) { s->body[i][0] = 3 - i; s->body[i][1] = 3; }
        snake_place_food(s, board_w, board_h);
        return;
    }
    if (key == 'w' || key == 'W') { if (s->dir != 2) s->dir = 0; }
    if (key == 'd' || key == 'D') { if (s->dir != 3) s->dir = 1; }
    if (key == 's' || key == 'S') { if (s->dir != 0) s->dir = 2; }
    if (key == 'a' || key == 'A') { if (s->dir != 1) s->dir = 3; }
}

static void app_cubedip_tick(Window* win, uint32_t ticks) {
    app_cubedip_state_t* s = (app_cubedip_state_t*)win->app_state;
    if (!s) return;
    if (!s->alive) {
        gui_clear_window(win, VGA_COLOR_WHITE | (VGA_COLOR_BLACK << 4));
        gui_draw_text(win, 1, 1, "cubeDip game over. Press R.", VGA_COLOR_WHITE | (VGA_COLOR_BLACK << 4));
        return;
    }
    if (ticks - s->last_fall_tick >= 5) {
        s->last_fall_tick = ticks;
        if (s->block_y + 1 >= 16 || s->stack[s->block_y + 1][s->block_x]) {
            s->stack[s->block_y][s->block_x] = 1;
            s->score += 5;
            s->block_x = (int)(timer_ticks() % 10);
            s->block_y = 0;
            if (s->stack[s->block_y][s->block_x]) s->alive = 0;
        } else {
            s->block_y++;
        }
    }
    gui_clear_window(win, VGA_COLOR_WHITE | (VGA_COLOR_BLACK << 4));
    gui_draw_text(win, 0, 0, "cubeDip", VGA_COLOR_WHITE | (VGA_COLOR_BLACK << 4));
    char sc[16];
    itoa(s->score, sc, 10);
    gui_draw_text(win, 8, 0, sc, VGA_COLOR_WHITE | (VGA_COLOR_BLACK << 4));
    for (int y = 0; y < 16 && y + 1 < win->height; y++) {
        for (int x = 0; x < 10 && x + 1 < win->width; x++) {
            int filled = s->stack[y][x] || (x == s->block_x && y == s->block_y);
            if (filled) {
                int idx = (y + 1) * win->width + (x + 1);
                if (idx >= 0 && idx < win->buffer_cells) {
                    win->buffer[idx] = ((VGA_COLOR_LIGHT_CYAN | (VGA_COLOR_BLACK << 4)) << 8) | '#';
                }
            }
        }
    }
}

static void app_cubedip_key(Window* win, char key) {
    app_cubedip_state_t* s = (app_cubedip_state_t*)win->app_state;
    (void)win;
    if (!s) return;
    if (!s->alive && (key == 'r' || key == 'R')) {
        memset(s->stack, 0, sizeof(s->stack));
        s->block_x = 5; s->block_y = 0; s->score = 0; s->alive = 1;
        return;
    }
    if (key == 'a' || key == 'A') s->block_x = max_int(0, s->block_x - 1);
    if (key == 'd' || key == 'D') s->block_x = min_int(9, s->block_x + 1);
    if (key == 's' || key == 'S') s->last_fall_tick = 0;
}

static int explorer_total_entries(const Directory* dir) {
    if (!dir) return 0;
    return (int)dir->child_count + (int)dir->file_count;
}

static int explorer_is_dir_entry(const Directory* dir, int idx) {
    return dir && idx >= 0 && idx < (int)dir->child_count;
}

static const char* explorer_entry_name(const Directory* dir, int idx) {
    if (!dir || idx < 0) return "";
    if (idx < (int)dir->child_count) return dir->children[idx].name;
    idx -= (int)dir->child_count;
    if (idx < (int)dir->file_count) return dir->files[idx].name;
    return "";
}

static void app_explorer_tick(Window* win, uint32_t ticks) {
    const Directory* dir = fs_get_current_dir();
    app_explorer_state_t* state = (app_explorer_state_t*)win->app_state;
    int total;
    int row = 1;
    int visible_rows = max_int(1, win->height - 2);
    (void)ticks;
    if (!state || !dir) return;

    total = explorer_total_entries(dir);
    if (state->selected < 0) state->selected = 0;
    if (state->selected >= total && total > 0) state->selected = total - 1;
    if (state->scroll_top < 0) state->scroll_top = 0;
    if (state->selected < state->scroll_top) state->scroll_top = state->selected;
    if (state->selected >= state->scroll_top + visible_rows) {
        state->scroll_top = state->selected - visible_rows + 1;
    }
    if (state->scroll_top > max_int(0, total - visible_rows)) {
        state->scroll_top = max_int(0, total - visible_rows);
    }

    // TempleOS-inspired compact contrast: dark blue background + bright text.
    gui_clear_window(win, VGA_COLOR_WHITE | (VGA_COLOR_BLUE << 4));
    gui_draw_text(win, 0, 0, "DIR ", VGA_COLOR_LIGHT_CYAN | (VGA_COLOR_BLUE << 4));
    gui_draw_text(win, 4, 0, fs_get_cwd(), VGA_COLOR_WHITE | (VGA_COLOR_BLUE << 4));

    for (int i = state->scroll_top; i < total && row < win->height - 1; i++, row++) {
        char line[64];
        const char* name = explorer_entry_name(dir, i);
        int is_dir = explorer_is_dir_entry(dir, i);
        line[0] = '\0';
        append_limited(line, is_dir ? "[D] " : "[F] ", sizeof(line));
        append_limited(line, name, sizeof(line));
        gui_draw_text(
            win,
            0,
            row,
            line,
            (i == state->selected)
                ? (VGA_COLOR_BLACK | (VGA_COLOR_LIGHT_GREY << 4))
                : (is_dir ? (VGA_COLOR_LIGHT_GREEN | (VGA_COLOR_BLUE << 4))
                          : (VGA_COLOR_WHITE | (VGA_COLOR_BLUE << 4))));
    }

    if (row < win->height) {
        gui_draw_text(win, 0, row, "Enter=open  Backspace=up  F2=edit", VGA_COLOR_LIGHT_CYAN | (VGA_COLOR_BLUE << 4));
    }
}

static void app_explorer_key(Window* win, char key) {
    const Directory* dir = fs_get_current_dir();
    app_explorer_state_t* state = (app_explorer_state_t*)win->app_state;
    int total;
    if (!state || !dir) return;
    total = explorer_total_entries(dir);
    if (total <= 0) return;

    if ((unsigned char)key == KEY_UP) {
        if (state->selected > 0) state->selected--;
        return;
    }
    if ((unsigned char)key == KEY_DOWN) {
        if (state->selected < total - 1) state->selected++;
        return;
    }
    if ((unsigned char)key == KEY_BACKSPACE) {
        if (fs_cd_up() == 0) state->selected = 0;
        return;
    }
    if (key == '\r' || key == '\n' || (unsigned char)key == KEY_F2) {
        const char* name = explorer_entry_name(dir, state->selected);
        if (explorer_is_dir_entry(dir, state->selected)) {
            if (fs_change_dir(name) == 0) state->selected = 0;
        } else {
            launch_app(LAUNCH_NOTEPAD, name);
        }
        return;
    }
}

static void app_shell_execute(Window* shell_win, app_terminal_state_t* state) {
    char cmd[TERM_LINE_LEN], a[64], b[128], line[TERM_LINE_LEN];
    int view_h;
    if (!shell_win || !state) return;
    view_h = max_int(1, shell_win->height - 1);
    strncpy(cmd, state->input, TERM_LINE_LEN - 1);
    cmd[TERM_LINE_LEN - 1] = '\0';
    if (strlen(cmd) > 0) {
        term_add_history(state, cmd);
        strcpy(line, "> ");
        append_limited(line, cmd, TERM_LINE_LEN);
        gui_shell_push_line(state, line);
    }
    if (strcmp(cmd, "help") == 0) {
        gui_shell_push_line(state, "help,pwd,cd,mkdir,rmdir,new,del,read,write,edit");
        gui_shell_push_line(state, "snake,cubeDip,explorer");
    } else if (strcmp(cmd, "clear") == 0) {
        state->line_count = 0;
        state->scroll_top = 0;
    } else if (strcmp(cmd, "pwd") == 0) {
        gui_shell_push_line(state, fs_get_cwd());
    } else if (starts_with(cmd, "echo ")) {
        gui_shell_push_line(state, cmd + 5);
    } else if (strcmp(cmd, "cd ..") == 0) {
        if (fs_cd_up() == 0) gui_shell_push_line(state, "ok");
        else gui_shell_push_line(state, "cd failed");
    } else if (starts_with(cmd, "cd ")) {
        if (fs_change_dir(cmd + 3) == 0) gui_shell_push_line(state, "ok");
        else gui_shell_push_line(state, "cd failed");
    } else if (starts_with(cmd, "mkdir ")) {
        if (fs_create_dir(cmd + 6) == 0) gui_shell_push_line(state, "dir created");
        else gui_shell_push_line(state, "mkdir failed");
    } else if (starts_with(cmd, "rmdir ")) {
        if (fs_delete_dir(cmd + 6) == 0) gui_shell_push_line(state, "dir removed");
        else gui_shell_push_line(state, "rmdir failed");
    } else if (starts_with(cmd, "new ")) {
        if (fs_create(cmd + 4) == 0) gui_shell_push_line(state, "file created");
        else gui_shell_push_line(state, "new failed");
    } else if (starts_with(cmd, "del ")) {
        if (fs_delete(cmd + 4) == 0) gui_shell_push_line(state, "file deleted");
        else gui_shell_push_line(state, "del failed");
    } else if (starts_with(cmd, "write ")) {
        parse_two_args(cmd + 6, a, sizeof(a), b, sizeof(b));
        if (a[0] == '\0') gui_shell_push_line(state, "write <file> <text>");
        else if (fs_write(a, (const uint8_t*)b, strlen(b)) == 0) gui_shell_push_line(state, "written");
        else gui_shell_push_line(state, "write failed");
    } else if (starts_with(cmd, "read ")) {
        FileHandle* fh = fs_open(cmd + 5);
        if (!fh) {
            gui_shell_push_line(state, "read failed");
        } else {
            uint8_t buf[64];
            size_t n;
            while ((n = fs_read(fh, buf, sizeof(buf) - 1)) > 0) {
                buf[n] = '\0';
                gui_shell_push_line(state, (const char*)buf);
            }
            fs_close(fh);
        }
    } else if (starts_with(cmd, "edit ")) {
        if (!launch_app(LAUNCH_NOTEPAD, cmd + 5)) gui_shell_push_line(state, "failed to launch editor");
    } else if (strcmp(cmd, "snake") == 0) {
        launch_app(LAUNCH_SNAKE, NULL);
    } else if (strcmp(cmd, "cubeDip") == 0) {
        launch_app(LAUNCH_CUBEDIP, NULL);
    } else if (strcmp(cmd, "explorer") == 0) {
        launch_app(LAUNCH_EXPLORER, NULL);
    } else if (strcmp(cmd, "ls") == 0) {
        shell_ls_into_lines(state);
    } else if (strlen(cmd) > 0) {
        gui_shell_push_line(state, "Unknown command");
    }
    state->input_len = 0;
    state->input_cursor = 0;
    state->input[0] = '\0';
    state->history_nav_offset = -1;
    gui_shell_scroll_to_bottom(state, view_h);
}

static void app_shell_tick(Window* win, uint32_t ticks) {
    app_terminal_state_t* state = (app_terminal_state_t*)win->app_state;
    int view_h, text_w, max_top;
    (void)ticks;
    if (!state) return;
    view_h = max_int(1, win->height - 1);
    text_w = max_int(1, win->width - 1);
    max_top = max_int(0, state->line_count - view_h);
    state->scroll_top = max_int(0, min_int(max_top, state->scroll_top));
    gui_clear_window(win, VGA_COLOR_LIGHT_GREY | (VGA_COLOR_BLACK << 4));

    for (int row = 0; row < view_h; row++) {
        int src = state->scroll_top + row;
        if (src < state->line_count) {
            char line[TERM_LINE_LEN];
            strncpy(line, state->lines[src], min_int(text_w, TERM_LINE_LEN - 1));
            line[min_int(text_w, TERM_LINE_LEN - 1)] = '\0';
            gui_draw_text(win, 0, row, line, VGA_COLOR_LIGHT_GREEN | (VGA_COLOR_BLACK << 4));
        }
    }

    {
        char prompt[TERM_LINE_LEN];
        int cursor_col;
        int input_max = TERM_LINE_LEN - 4;
        strcpy(prompt, "> ");
        append_limited(prompt, state->input, TERM_LINE_LEN);
        prompt[min_int(text_w, TERM_LINE_LEN - 1)] = '\0';

        if (state->input_cursor < 0) state->input_cursor = 0;
        if (state->input_cursor > state->input_len) state->input_cursor = state->input_len;
        cursor_col = 2 + state->input_cursor;
        if (cursor_col >= input_max) cursor_col = input_max;
        if ((int)strlen(prompt) >= input_max) prompt[input_max] = '\0';
        if ((int)strlen(prompt) <= cursor_col) {
            int plen = (int)strlen(prompt);
            while (plen <= cursor_col && plen < TERM_LINE_LEN - 1) prompt[plen++] = ' ';
            prompt[min_int(plen, TERM_LINE_LEN - 1)] = '\0';
        }
        prompt[cursor_col] = '_';
        gui_draw_text(win, 0, win->height - 1, prompt, VGA_COLOR_WHITE | (VGA_COLOR_BLACK << 4));
    }

    for (int y = 0; y < win->height; y++) {
        int idx = y * win->width + (win->width - 1);
        win->buffer[idx] = ((VGA_COLOR_DARK_GREY | (VGA_COLOR_BLACK << 4)) << 8) | 176;
    }
    if (state->line_count > view_h) {
        int thumb_h = max_int(1, (view_h * view_h) / state->line_count);
        int thumb_y = (state->scroll_top * max_int(1, view_h - thumb_h)) / max_top;
        for (int y = thumb_y; y < thumb_y + thumb_h && y < view_h; y++) {
            int idx = y * win->width + (win->width - 1);
            win->buffer[idx] = ((VGA_COLOR_LIGHT_GREY | (VGA_COLOR_BLUE << 4)) << 8) | 219;
        }
    } else {
        for (int y = 0; y < view_h; y++) {
            int idx = y * win->width + (win->width - 1);
            win->buffer[idx] = ((VGA_COLOR_LIGHT_GREY | (VGA_COLOR_BLUE << 4)) << 8) | 219;
        }
    }
}

static void app_shell_key(Window* win, char key) {
    app_terminal_state_t* state = (app_terminal_state_t*)win->app_state;
    if (!state) return;
    if ((unsigned char)key == KEY_BACKSPACE || (unsigned char)key == 127) {
        term_backspace_char(state);
        return;
    }
    if ((unsigned char)key == KEY_LEFT) {
        if (state->input_cursor > 0) state->input_cursor--;
        return;
    }
    if ((unsigned char)key == KEY_RIGHT) {
        if (state->input_cursor < state->input_len) state->input_cursor++;
        return;
    }
    if ((unsigned char)key == KEY_UP) {
        if (state->history_count > 0) {
            if (state->history_nav_offset < 0) {
                strcpy(state->saved_input, state->input);
                state->history_nav_offset = 0;
            } else if (state->history_nav_offset < state->history_count - 1) {
                state->history_nav_offset++;
            }
            int idx = term_history_index_from_offset(state, state->history_nav_offset);
            strcpy(state->input, state->history[idx]);
            state->input_len = (int)strlen(state->input);
            state->input_cursor = state->input_len;
        }
        return;
    }
    if ((unsigned char)key == KEY_DOWN) {
        if (state->history_count > 0 && state->history_nav_offset >= 0) {
            state->history_nav_offset--;
            if (state->history_nav_offset < 0) {
                strcpy(state->input, state->saved_input);
            } else {
                int idx = term_history_index_from_offset(state, state->history_nav_offset);
                strcpy(state->input, state->history[idx]);
            }
            state->input_len = (int)strlen(state->input);
            state->input_cursor = state->input_len;
        }
        return;
    }
    if (key == '\r' || key == '\n') { app_shell_execute(win, state); return; }
    if ((unsigned char)key < 32 || (unsigned char)key > 126) return;
    term_insert_char(state, key);
}

static void app_notepad_tick(Window* win, uint32_t ticks) {
    app_notepad_state_t* note = (app_notepad_state_t*)win->app_state;
    int row = 0, col = 0, visual_row = 0;
    int cursor_row = 0, cursor_col = 0;
    int status_row = win->height - 1;
    if (!note) return;

    if (ticks - note->blink_tick >= 20) {
        note->blink_tick = ticks;
        note->cursor_visible = !note->cursor_visible;
    }

    notepad_cursor_to_line_col(note, note->cursor_pos, &cursor_row, &cursor_col);
    if (cursor_row < note->scroll_row) note->scroll_row = cursor_row;
    if (cursor_row >= note->scroll_row + status_row) {
        note->scroll_row = cursor_row - status_row + 1;
    }
    if (note->scroll_row < 0) note->scroll_row = 0;

    gui_clear_window(win, VGA_COLOR_WHITE | (VGA_COLOR_BLACK << 4));
    for (int i = 0; i < note->len && visual_row < win->height - 1; i++) {
        char ch = note->text[i];
        if (row < note->scroll_row) {
            if (ch == '\n') { row++; col = 0; }
            else {
                col++;
                if (col >= win->width) { row++; col = 0; }
            }
            continue;
        }
        if (ch == '\n' || col >= win->width) {
            visual_row++; col = 0;
            if (ch == '\n') continue;
        }
        if (visual_row < win->height - 1 && col < win->width) {
            int idx = visual_row * win->width + col;
            if (idx >= 0 && idx < win->buffer_cells) {
                win->buffer[idx] = ((VGA_COLOR_WHITE | (VGA_COLOR_BLACK << 4)) << 8) | ch;
            }
            col++;
        }
    }

    if (note->cursor_visible) {
        int draw_row = cursor_row - note->scroll_row;
        int draw_col = cursor_col;
        if (draw_row >= 0 && draw_row < status_row && draw_col >= 0 && draw_col < win->width) {
            int idx = draw_row * win->width + draw_col;
            if (idx >= 0 && idx < win->buffer_cells) {
                win->buffer[idx] = ((VGA_COLOR_BLACK | (VGA_COLOR_LIGHT_GREY << 4)) << 8) | '_';
            }
        }
    }

    {
        char status[TERM_LINE_LEN];
        strcpy(status, "F2 save | ");
        if (note->filename[0]) append_limited(status, note->filename, TERM_LINE_LEN);
        else append_limited(status, "(untitled)", TERM_LINE_LEN);
        if (note->dirty) append_limited(status, " *", TERM_LINE_LEN);
        gui_draw_text(win, 0, win->height - 1, status, VGA_COLOR_BLACK | (VGA_COLOR_LIGHT_GREY << 4));
    }
}

static void app_notepad_key(Window* win, char key) {
    app_notepad_state_t* note = (app_notepad_state_t*)win->app_state;
    int row, col, max_row;
    if (!note) return;
    if ((unsigned char)key == KEY_F2) { notepad_save_file(note); return; }
    if ((unsigned char)key == KEY_LEFT) {
        if (note->cursor_pos > 0) note->cursor_pos--;
        note->preferred_col = -1;
        note->cursor_visible = 1;
        return;
    }
    if ((unsigned char)key == KEY_RIGHT) {
        if (note->cursor_pos < note->len) note->cursor_pos++;
        note->preferred_col = -1;
        note->cursor_visible = 1;
        return;
    }
    if ((unsigned char)key == KEY_UP) {
        notepad_cursor_to_line_col(note, note->cursor_pos, &row, &col);
        if (note->preferred_col < 0) note->preferred_col = col;
        if (row > 0) note->cursor_pos = notepad_line_col_to_cursor(note, row - 1, note->preferred_col);
        note->cursor_visible = 1;
        return;
    }
    if ((unsigned char)key == KEY_DOWN) {
        notepad_cursor_to_line_col(note, note->cursor_pos, &row, &col);
        if (note->preferred_col < 0) note->preferred_col = col;
        max_row = row;
        for (int i = note->cursor_pos; i < note->len; i++) {
            if (note->text[i] == '\n') { max_row++; break; }
        }
        note->cursor_pos = notepad_line_col_to_cursor(note, max_row, note->preferred_col);
        note->cursor_visible = 1;
        return;
    }
    if ((unsigned char)key == KEY_BACKSPACE || (unsigned char)key == 127) {
        if (note->cursor_pos > 0 && note->len > 0) {
            for (int i = note->cursor_pos - 1; i < note->len; i++) {
                note->text[i] = note->text[i + 1];
            }
            note->len--;
            note->cursor_pos--;
            note->dirty = 1;
        }
        note->preferred_col = -1;
        note->cursor_visible = 1;
        return;
    }
    if (key == '\r') key = '\n';
    if ((unsigned char)key < 32 && key != '\n') return;
    if (note->len < NOTE_MAX_TEXT - 1) {
        for (int i = note->len; i > note->cursor_pos; i--) {
            note->text[i] = note->text[i - 1];
        }
        note->text[note->cursor_pos++] = key;
        note->len++;
        note->text[note->len] = '\0';
        note->dirty = 1;
        note->preferred_col = -1;
        note->cursor_visible = 1;
    }
}

void gui_init(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        windows[i].active = 0;
        windows[i].focused = 0;
        windows[i].maximized = 0;
        windows[i].prev_x = 0;
        windows[i].prev_y = 0;
        windows[i].prev_width = 0;
        windows[i].prev_height = 0;
        windows[i].buffer_cells = 0;
        windows[i].app_type = GUI_APP_NONE;
        windows[i].buffer = NULL;
        windows[i].app_state = NULL;
        windows[i].on_tick = NULL;
        windows[i].on_key = NULL;
    }
    z_count = 0;
    window_count = 0;
    focused_window = NULL;
    drag_window = NULL;
    drag_offset_x = 0;
    drag_offset_y = 0;
    start_menu_open = 0;
    input_set_bounds(SCREEN_WIDTH, SCREEN_HEIGHT);
}

Window* gui_create_window(const char* title, int x, int y, int width, int height) {
    int idx = -1;
    Window* win;
    if (window_count >= MAX_WINDOWS || width <= 0 || height <= 0) return NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].active) { idx = i; break; }
    }
    if (idx == -1) return NULL;
    win = &windows[idx];
    win->id = next_window_id++;
    win->x = x;
    win->y = y;
    win->width = width;
    win->height = height;
    win->focused = 0;
    win->maximized = 0;
    win->prev_x = x; win->prev_y = y; win->prev_width = width; win->prev_height = height;
    win->app_type = GUI_APP_NONE;
    win->app_state = NULL;
    win->on_tick = NULL;
    win->on_key = NULL;
    strncpy(win->title, title, 31);
    win->title[31] = '\0';
    win->buffer = NULL;
    if (!window_resize_buffer(win, width, height)) return NULL;
    win->active = 1;
    window_count++;
    z_order[z_count++] = idx;
    return win;
}

void gui_close_window(Window* win) {
    int idx;
    if (!win || !win->active) return;
    idx = (int)(win - windows);
    if (drag_window == win) drag_window = NULL;
    if (focused_window == win) set_focused_window(NULL);
    if (win->app_state) { kfree(win->app_state); win->app_state = NULL; }
    if (win->buffer) { kfree(win->buffer); win->buffer = NULL; }
    win->active = 0;
    win->buffer_cells = 0;
    win->app_type = GUI_APP_NONE;
    win->on_tick = NULL;
    win->on_key = NULL;
    remove_from_z_order(idx);
    window_count--;
}

void gui_draw_text(Window* win, int x, int y, const char* text, uint8_t color) {
    int i = 0;
    if (!win || !win->active || !text || y < 0 || y >= win->height) return;
    while (text[i]) {
        int tx = x + i;
        if (tx >= 0 && tx < win->width) {
            int idx = y * win->width + tx;
            if (idx >= 0 && idx < win->buffer_cells) win->buffer[idx] = (color << 8) | text[i];
        }
        i++;
    }
}

void gui_clear_window(Window* win, uint8_t color) {
    if (!win || !win->active) return;
    for (int i = 0; i < win->buffer_cells; i++) win->buffer[i] = (color << 8) | ' ';
}

static void render_window(Window* win) {
    int bx = win->x - 1, by = win->y - 1, bw = win->width + 2, bh = win->height + 2;
    uint8_t border = win->focused ? (VGA_COLOR_LIGHT_BROWN | (VGA_COLOR_BLUE << 4))
                                  : (VGA_COLOR_LIGHT_GREY | (VGA_COLOR_BLUE << 4));
    for (int i = 0; i < bw; i++) {
        int c = bx + i;
        if (c >= 0 && c < SCREEN_WIDTH) {
            if (by >= 0 && by < SCREEN_HEIGHT) backbuffer[by * SCREEN_WIDTH + c] = (border << 8) | 205;
            if (by + bh - 1 >= 0 && by + bh - 1 < SCREEN_HEIGHT) backbuffer[(by + bh - 1) * SCREEN_WIDTH + c] = (border << 8) | 205;
        }
    }
    for (int i = 0; i < bh; i++) {
        int r = by + i;
        if (r >= 0 && r < SCREEN_HEIGHT) {
            if (bx >= 0 && bx < SCREEN_WIDTH) backbuffer[r * SCREEN_WIDTH + bx] = (border << 8) | 186;
            if (bx + bw - 1 >= 0 && bx + bw - 1 < SCREEN_WIDTH) backbuffer[r * SCREEN_WIDTH + bx + bw - 1] = (border << 8) | 186;
        }
    }
    if (bx >= 0 && bx < SCREEN_WIDTH && by >= 0 && by < SCREEN_HEIGHT) backbuffer[by * SCREEN_WIDTH + bx] = (border << 8) | 201;
    if (bx + bw - 1 >= 0 && bx + bw - 1 < SCREEN_WIDTH && by >= 0 && by < SCREEN_HEIGHT) backbuffer[by * SCREEN_WIDTH + bx + bw - 1] = (border << 8) | 187;
    if (bx >= 0 && bx < SCREEN_WIDTH && by + bh - 1 >= 0 && by + bh - 1 < SCREEN_HEIGHT) backbuffer[(by + bh - 1) * SCREEN_WIDTH + bx] = (border << 8) | 200;
    if (bx + bw - 1 >= 0 && bx + bw - 1 < SCREEN_WIDTH && by + bh - 1 >= 0 && by + bh - 1 < SCREEN_HEIGHT) backbuffer[(by + bh - 1) * SCREEN_WIDTH + bx + bw - 1] = (border << 8) | 188;

    for (int i = 0; win->title[i] && i < win->width; i++) {
        int c = win->x + i;
        if (c >= 0 && c < SCREEN_WIDTH && by >= 0 && by < SCREEN_HEIGHT) backbuffer[by * SCREEN_WIDTH + c] = (border << 8) | win->title[i];
    }
    if (win->width > 4 && by >= 0 && by < SCREEN_HEIGHT) {
        int max_x = win->x + win->width - 3;
        int close_x = win->x + win->width - 1;
        if (max_x >= 0 && max_x < SCREEN_WIDTH) backbuffer[by * SCREEN_WIDTH + max_x] = (border << 8) | (win->maximized ? 'R' : 'M');
        if (close_x >= 0 && close_x < SCREEN_WIDTH) backbuffer[by * SCREEN_WIDTH + close_x] = (border << 8) | 'X';
    }

    for (int r = 0; r < win->height; r++) {
        int screen_r = win->y + r;
        if (screen_r < 0 || screen_r >= SCREEN_HEIGHT) continue;
        for (int c = 0; c < win->width; c++) {
            int screen_c = win->x + c;
            int src = r * win->width + c;
            if (screen_c < 0 || screen_c >= SCREEN_WIDTH) continue;
            if (src >= 0 && src < win->buffer_cells) backbuffer[screen_r * SCREEN_WIDTH + screen_c] = win->buffer[src];
        }
    }
}

static void render_toolbar(void) {
    uint8_t bar_attr = VGA_COLOR_WHITE | (VGA_COLOR_DARK_GREY << 4);
    const char* start_label = start_menu_open ? "[Start*]" : "[Start]";
    const char* mode = "GooberOS DM";
    for (int x = 0; x < SCREEN_WIDTH; x++) backbuffer[TOOLBAR_ROW * SCREEN_WIDTH + x] = (bar_attr << 8) | ' ';
    for (int i = 0; start_label[i] && START_BTN_X + i < SCREEN_WIDTH; i++) {
        backbuffer[TOOLBAR_ROW * SCREEN_WIDTH + START_BTN_X + i] = (bar_attr << 8) | start_label[i];
    }
    for (int i = 0; mode[i] && SCREEN_WIDTH - 12 + i < SCREEN_WIDTH; i++) {
        backbuffer[TOOLBAR_ROW * SCREEN_WIDTH + SCREEN_WIDTH - 12 + i] = (bar_attr << 8) | mode[i];
    }
}

static void draw_menu_item(int row, const char* text, int selected) {
    uint8_t attr = selected ? (VGA_COLOR_BLACK | (VGA_COLOR_LIGHT_GREY << 4))
                            : (VGA_COLOR_WHITE | (VGA_COLOR_BLUE << 4));
    int y = MENU_Y + 1 + row;
    for (int x = MENU_X + 1; x < MENU_X + MENU_W - 1; x++) {
        if (y >= 0 && y < SCREEN_HEIGHT && x >= 0 && x < SCREEN_WIDTH) backbuffer[y * SCREEN_WIDTH + x] = (attr << 8) | ' ';
    }
    for (int i = 0; text[i] && MENU_X + 2 + i < MENU_X + MENU_W - 1; i++) backbuffer[y * SCREEN_WIDTH + MENU_X + 2 + i] = (attr << 8) | text[i];
}

static void render_start_menu(void) {
    int h = LAUNCH_COUNT + 2;
    if (!start_menu_open) return;
    for (int y = MENU_Y; y < MENU_Y + h; y++) {
        for (int x = MENU_X; x < MENU_X + MENU_W; x++) {
            if (y < 0 || y >= SCREEN_HEIGHT || x < 0 || x >= SCREEN_WIDTH) continue;
            char ch = ' ';
            uint8_t border = VGA_COLOR_WHITE | (VGA_COLOR_BLUE << 4);
            if (y == MENU_Y || y == MENU_Y + h - 1) ch = '-';
            if (x == MENU_X || x == MENU_X + MENU_W - 1) ch = '|';
            if ((x == MENU_X || x == MENU_X + MENU_W - 1) && (y == MENU_Y || y == MENU_Y + h - 1)) ch = '+';
            backbuffer[y * SCREEN_WIDTH + x] = (border << 8) | ch;
        }
    }
    for (int i = 0; i < LAUNCH_COUNT; i++) draw_menu_item(i, launcher_labels[i], 0);
}

static int point_on_start_button(int x, int y) {
    return y == TOOLBAR_ROW && x >= START_BTN_X && x <= START_BTN_X + START_BTN_W;
}

static int point_in_start_menu(int x, int y) {
    int h = LAUNCH_COUNT + 2;
    return start_menu_open && x >= MENU_X && x < MENU_X + MENU_W && y >= MENU_Y && y < MENU_Y + h;
}

static int launcher_item_from_point(int x, int y) {
    int row;
    if (!point_in_start_menu(x, y)) return -1;
    row = y - (MENU_Y + 1);
    return (row >= 0 && row < LAUNCH_COUNT) ? row : -1;
}

static void app_shell_pointer_scroll(Window* win, app_terminal_state_t* state, const input_event_t* event) {
    int view_h, max_top, local_y, thumb_h;
    if (!win || !state || !event) return;
    view_h = max_int(1, win->height - 1);
    max_top = max_int(0, state->line_count - view_h);
    if (event->type == INPUT_EVENT_SCROLL && win == focused_window) {
        state->scroll_top = max_int(0, min_int(max_top, state->scroll_top - event->wheel));
    }
    if (event->type == INPUT_EVENT_BUTTON_DOWN && event->button == INPUT_BUTTON_LEFT) {
        if (event->x == win->x + win->width - 1 && event->y >= win->y && event->y < win->y + view_h) {
            local_y = event->y - win->y;
            thumb_h = max_int(1, (view_h * view_h) / max_int(1, state->line_count));
            state->scroll_top = (local_y * max_int(1, max_top)) / max_int(1, view_h - thumb_h);
        }
    }
}

void gui_update(void) {
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) backbuffer[i] = (VGA_COLOR_CYAN << 12) | (VGA_COLOR_BLUE << 8) | 176;
    for (int z = 0; z < z_count; z++) if (windows[z_order[z]].active) render_window(&windows[z_order[z]]);
    render_toolbar();
    render_start_menu();
    {
        int mx = input_get_pointer_x(), my = input_get_pointer_y();
        if (mx >= 0 && mx < SCREEN_WIDTH && my >= 0 && my < SCREEN_HEIGHT) backbuffer[my * SCREEN_WIDTH + mx] = ((VGA_COLOR_WHITE | (VGA_COLOR_RED << 4)) << 8) | 219;
    }
    memcpy(VIDEO_MEMORY, backbuffer, SCREEN_WIDTH * SCREEN_HEIGHT * 2);
}

static Window* launch_app(launcher_item_t item, const char* arg) {
    Window* win = NULL;
    if (item == LAUNCH_WELCOME) {
        win = gui_create_window("Welcome", 3, 3, 38, 10);
        if (win) { win->app_type = GUI_APP_WELCOME; win->on_tick = app_welcome_tick; }
    } else if (item == LAUNCH_SYSTEM) {
        win = gui_create_window("System Monitor", 44, 2, 33, 8);
        if (win) {
            app_sys_state_t* s = (app_sys_state_t*)kmalloc(sizeof(app_sys_state_t));
            if (s) {
                memset(s, 0, sizeof(*s));
                win->app_state = s;
                win->app_type = GUI_APP_SYSTEM;
                win->on_tick = app_system_tick;
            }
        }
    } else if (item == LAUNCH_BOUNCE) {
        win = gui_create_window("Bounce Demo", 24, 14, 30, 9);
        if (win) {
            app_bounce_state_t* s = (app_bounce_state_t*)kmalloc(sizeof(app_bounce_state_t));
            if (s) {
                s->ball_x = 5; s->ball_y = 3; s->dx = 1; s->dy = 1;
                win->app_state = s;
                win->app_type = GUI_APP_BOUNCE;
                win->on_tick = app_bounce_tick;
                win->on_key = app_bounce_key;
            }
        }
    } else if (item == LAUNCH_SHELL) {
        win = gui_create_window("Shell", 6, 5, 54, 13);
        if (win) {
            app_terminal_state_t* s = (app_terminal_state_t*)kmalloc(sizeof(app_terminal_state_t));
            if (s) {
                memset(s, 0, sizeof(*s));
                s->history_nav_offset = -1;
                gui_shell_push_line(s, "GooberOS GUI shell (FS-backed)");
                gui_shell_push_line(s, "Type 'help' for commands.");
                win->app_state = s;
                win->app_type = GUI_APP_SHELL;
                win->on_tick = app_shell_tick;
                win->on_key = app_shell_key;
            }
        }
    } else if (item == LAUNCH_NOTEPAD) {
        win = gui_create_window("Text Editor", 16, 4, 50, 15);
        if (win) {
            app_notepad_state_t* s = (app_notepad_state_t*)kmalloc(sizeof(app_notepad_state_t));
            if (s) {
                memset(s, 0, sizeof(*s));
                s->preferred_col = -1;
                s->cursor_visible = 1;
                if (arg && arg[0]) {
                    notepad_load_file(s, arg);
                } else {
                    strcpy(s->text, "Window editor ready...");
                    s->len = (int)strlen(s->text);
                }
                s->cursor_pos = s->len;
                win->app_state = s;
                win->app_type = GUI_APP_NOTEPAD;
                win->on_tick = app_notepad_tick;
                win->on_key = app_notepad_key;
            }
        }
    } else if (item == LAUNCH_SNAKE) {
        win = gui_create_window("snake", 10, 6, 32, 14);
        if (win) {
            app_snake_state_t* s = (app_snake_state_t*)kmalloc(sizeof(app_snake_state_t));
            if (s) {
                memset(s, 0, sizeof(*s));
                s->length = 4;
                s->dir = 1;
                s->alive = 1;
                s->last_step_tick = timer_ticks();
                for (int i = 0; i < s->length; i++) { s->body[i][0] = 3 - i; s->body[i][1] = 3; }
                snake_place_food(s, max_int(8, win->width - 2), max_int(6, win->height - 2));
                win->app_state = s;
                win->app_type = GUI_APP_SNAKE;
                win->on_tick = app_snake_tick;
                win->on_key = app_snake_key;
            }
        }
    } else if (item == LAUNCH_CUBEDIP) {
        win = gui_create_window("cubeDip", 45, 6, 24, 20);
        if (win) {
            app_cubedip_state_t* s = (app_cubedip_state_t*)kmalloc(sizeof(app_cubedip_state_t));
            if (s) {
                memset(s, 0, sizeof(*s));
                s->block_x = 5; s->block_y = 0; s->alive = 1; s->last_fall_tick = timer_ticks();
                win->app_state = s;
                win->app_type = GUI_APP_CUBEDIP;
                win->on_tick = app_cubedip_tick;
                win->on_key = app_cubedip_key;
            }
        }
    } else if (item == LAUNCH_EXPLORER) {
        win = gui_create_window("File Explorer", 42, 5, 34, 16);
        if (win) {
            app_explorer_state_t* s = (app_explorer_state_t*)kmalloc(sizeof(app_explorer_state_t));
            if (s) {
                s->selected = 0;
                win->app_state = s;
                win->app_type = GUI_APP_EXPLORER;
                win->on_tick = app_explorer_tick;
                win->on_key = app_explorer_key;
            }
        }
    }
    if (win) { bring_to_front(win); set_focused_window(win); }
    return win;
}

static void setup_window_apps(void) {
    launch_app(LAUNCH_WELCOME, NULL);
    launch_app(LAUNCH_SYSTEM, NULL);
    launch_app(LAUNCH_SHELL, NULL);
}

void gui_run(void) {
    gui_init();
    gui_running = 1;
    setup_window_apps();
    if (z_count > 0) set_focused_window(&windows[z_order[z_count - 1]]);

    while (gui_running) {
        uint32_t ticks = timer_ticks();
        if (keyboard_has_char()) {
            char c = keyboard_read_char();
            int ctrl = keyboard_is_ctrl_active();
            if (c == KEY_ESC) {
                gui_running = 0;
            } else if (ctrl && (c == 'q' || c == 'Q')) {
                gui_running = 0;
            } else if (ctrl && (c == 'w' || c == 'W')) {
                if (focused_window) {
                    gui_close_window(focused_window);
                    set_focused_window(top_active_window());
                }
            } else if (ctrl && (c == 'm' || c == 'M')) {
                if (focused_window) toggle_window_maximize(focused_window);
            } else if (ctrl && (c == 'e' || c == 'E')) {
                start_menu_open = !start_menu_open;
            } else if (c == '\t') {
                focus_cycle_next();
            } else if (focused_window && focused_window->on_key) {
                focused_window->on_key(focused_window, c);
            }
        }

        input_event_t event;
        while (input_poll_event(&event)) {
            if (focused_window && focused_window->app_type == GUI_APP_SHELL) {
                app_shell_pointer_scroll(focused_window, (app_terminal_state_t*)focused_window->app_state, &event);
            }
            if (event.type == INPUT_EVENT_BUTTON_DOWN && event.button == INPUT_BUTTON_LEFT) {
                if (point_on_start_button(event.x, event.y)) {
                    start_menu_open = !start_menu_open;
                    continue;
                }
                if (start_menu_open) {
                    int item = launcher_item_from_point(event.x, event.y);
                    if (item >= 0) launch_app((launcher_item_t)item, NULL);
                    start_menu_open = 0;
                    if (item >= 0) continue;
                }
                Window* hit = top_window_at(event.x, event.y, 0);
                if (!hit) { set_focused_window(NULL); continue; }
                if (title_close_hit(hit, event.x, event.y)) {
                    gui_close_window(hit);
                    set_focused_window(top_active_window());
                    continue;
                }
                if (title_maximize_hit(hit, event.x, event.y)) {
                    toggle_window_maximize(hit);
                    continue;
                }
                bring_to_front(hit);
                set_focused_window(hit);
                if (point_in_title_bar(hit, event.x, event.y) && !hit->maximized) {
                    drag_window = hit;
                    drag_offset_x = event.x - hit->x;
                    drag_offset_y = event.y - hit->y;
                }
            } else if (event.type == INPUT_EVENT_POINTER_MOVE) {
                if (drag_window && (event.buttons & 0x01)) {
                    int nx = event.x - drag_offset_x;
                    int ny = event.y - drag_offset_y;
                    nx = max_int(1, min_int(SCREEN_WIDTH - drag_window->width - 2, nx));
                    ny = max_int(2, min_int(SCREEN_HEIGHT - drag_window->height - 2, ny));
                    drag_window->x = nx;
                    drag_window->y = ny;
                }
            } else if (event.type == INPUT_EVENT_BUTTON_UP && event.button == INPUT_BUTTON_LEFT) {
                drag_window = NULL;
            }
        }

        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (windows[i].active && windows[i].on_tick) windows[i].on_tick(&windows[i], ticks);
        }

        gui_update();
        timer_sleep(16);
    }

    for (int i = 0; i < MAX_WINDOWS; i++) if (windows[i].active) gui_close_window(&windows[i]);
    vga_set_text_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    clear_screen();
}
