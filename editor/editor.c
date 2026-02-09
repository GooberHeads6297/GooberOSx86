#include "../drivers/video/vga.h"
#include "../drivers/keyboard/keyboard.h"
#include "../drivers/timer/timer.h"
#include "../fs/filesystem.h"
#include <stddef.h>
#include <stdint.h>

#define EDITOR_MAX_SIZE 4096
#define STATUS_ROW 0
#define TEXT_START_ROW 1
#define TEXT_ROWS 24
#define COLS 80

#define KEY_ESC     0x1B
#define KEY_F2      0x8C
#define KEY_UP      0x80
#define KEY_DOWN    0x81
#define KEY_LEFT    0x82
#define KEY_RIGHT   0x83



static char editor_buf[EDITOR_MAX_SIZE];
static size_t buf_len;
static size_t cursor;
static int view_line;
static int exit_editor;
static const char* edit_filename;

static unsigned char attr_normal;
static unsigned char attr_status;

static size_t get_line_count(void) {
    size_t count = 0;
    for (size_t i = 0; i < buf_len; i++) {
        if (editor_buf[i] == '\n') count++;
    }
    if (buf_len > 0 && (buf_len == 0 || editor_buf[buf_len - 1] != '\n'))
        count++;
    return count > 0 ? count : 1;
}

static size_t get_line_start(size_t line) {
    size_t idx = 0;
    size_t cur_line = 0;
    while (idx < buf_len && cur_line < line) {
        if (editor_buf[idx] == '\n') cur_line++;
        idx++;
    }
    return idx;
}

static size_t get_line_end(size_t line) {
    size_t start = get_line_start(line);
    size_t idx = start;
    while (idx < buf_len && editor_buf[idx] != '\n') idx++;
    return idx;
}

static void cursor_to_line_col(size_t cur, size_t* out_line, size_t* out_col) {
    size_t line = 0;
    size_t col = 0;
    size_t i = 0;
    while (i < buf_len && i < cur) {
        if (editor_buf[i] == '\n') {
            line++;
            col = 0;
        } else {
            col++;
        }
        i++;
    }
    *out_line = line;
    *out_col = col;
}

static size_t line_col_to_cursor(size_t line, size_t col) {
    size_t start = get_line_start(line);
    size_t end = get_line_end(line);
    size_t len = end - start;
    if (col > len) col = len;
    return start + col;
}

static void ensure_cursor_visible(void) {
    size_t cl, cc;
    cursor_to_line_col(cursor, &cl, &cc);
    if ((int)cl < view_line)
        view_line = (int)cl;
    if ((int)cl >= view_line + TEXT_ROWS)
        view_line = (int)cl - TEXT_ROWS + 1;
    if (view_line < 0) view_line = 0;
}

static void insert_char(char c) {
    if (buf_len >= EDITOR_MAX_SIZE - 1) return;
    for (size_t i = buf_len; i > cursor; i--)
        editor_buf[i] = editor_buf[i - 1];
    editor_buf[cursor] = c;
    buf_len++;
    cursor++;
}

static void delete_backward(void) {
    if (cursor == 0) return;
    for (size_t i = cursor; i < buf_len; i++)
        editor_buf[i - 1] = editor_buf[i];
    buf_len--;
    cursor--;
}

static void move_cursor_left(void) {
    if (cursor > 0) cursor--;
}

static void move_cursor_right(void) {
    if (cursor < buf_len) cursor++;
}

static void move_cursor_up(void) {
    size_t cl, cc;
    cursor_to_line_col(cursor, &cl, &cc);
    if (cl == 0) return;
    cursor = line_col_to_cursor(cl - 1, cc);
}

static void move_cursor_down(void) {
    size_t cl, cc;
    cursor_to_line_col(cursor, &cl, &cc);
    size_t line_count = get_line_count();
    if (cl + 1 >= line_count) {
        cursor = buf_len;
        return;
    }
    cursor = line_col_to_cursor(cl + 1, cc);
}

static void draw_status(const char* extra) {
    for (int x = 0; x < COLS; x++)
        vga_put_char_at(' ', x, STATUS_ROW, attr_status);
    int x = 0;
    const char* p = "Editing: ";
    while (*p && x < COLS) { vga_put_char_at(*p++, x++, STATUS_ROW, attr_status); }
    if (edit_filename) {
        const char* fn = edit_filename;
        while (*fn && x < COLS) {
            vga_put_char_at(*fn++, x++, STATUS_ROW, attr_status);
        }
    }
    p = " | F2 save, ESC exit";
    while (*p && x < COLS) { vga_put_char_at(*p++, x++, STATUS_ROW, attr_status); }
    if (extra) {
        while (*extra && x < COLS) {
            vga_put_char_at(*extra++, x++, STATUS_ROW, attr_status);
        }
    }
}

static void render(void) {
    size_t line_count = get_line_count();
    for (int row = 0; row < TEXT_ROWS; row++) {
        int line_idx = view_line + row;
        int y = TEXT_START_ROW + row;
        for (int x = 0; x < COLS; x++)
            vga_put_char_at(' ', x, y, attr_normal);
        if (line_idx >= 0 && (size_t)line_idx < line_count) {
            size_t start = get_line_start((size_t)line_idx);
            size_t end = get_line_end((size_t)line_idx);
            int col = 0;
            for (size_t i = start; i < end && col < COLS; i++, col++)
                vga_put_char_at(editor_buf[i] == '\n' ? ' ' : editor_buf[i], col, y, attr_normal);
        }
    }
    size_t cur_line, cur_col;
    cursor_to_line_col(cursor, &cur_line, &cur_col);
    int disp_line = (int)cur_line - view_line;
    if (disp_line >= 0 && disp_line < TEXT_ROWS) {
        int y = TEXT_START_ROW + disp_line;
        int x = cur_col < (size_t)COLS ? (int)cur_col : COLS - 1;
        vga_set_cursor(y, x);
        vga_put_char_at('_', x, y, VGA_COLOR_WHITE | (VGA_COLOR_BLACK << 4));
    }
}

static void load_file(void) {
    buf_len = 0;
    cursor = 0;
    view_line = 0;
    FileHandle* fh = fs_open(edit_filename);
    if (!fh) return;
    size_t n;
    uint8_t tmp[128];
    while ((n = fs_read(fh, tmp, sizeof(tmp))) > 0) {
        for (size_t i = 0; i < n && buf_len < EDITOR_MAX_SIZE - 1; i++) {
            editor_buf[buf_len++] = (char)tmp[i];
        }
        if (buf_len >= EDITOR_MAX_SIZE - 1) break;
    }
    fs_close(fh);
}

static void save_file(void) {
    if (fs_write(edit_filename, (const uint8_t*)editor_buf, buf_len) == 0)
        draw_status(" | Saved!   ");
    else
        draw_status(" | Save failed.");
}

static void handle_input(void) {
    while (keyboard_has_char()) {
        unsigned char c = (unsigned char)keyboard_read_char();
        if (c == KEY_ESC) {
            exit_editor = 1;
            return;
        }
        if (c == KEY_F2) {
            save_file();
            continue;
        }
        if (c == KEY_LEFT) {
            move_cursor_left();
            continue;
        }
        if (c == KEY_RIGHT) {
            move_cursor_right();
            continue;
        }
        if (c == KEY_UP) {
            move_cursor_up();
            continue;
        }
        if (c == KEY_DOWN) {
            move_cursor_down();
            continue;
        }
        if (c == 0x08 || c == 127) {
            delete_backward();
            continue;
        }
        if (c == '\n' || c == '\r') {
            insert_char('\n');
            continue;
        }
        if (c >= 32 && c < 127) {
            insert_char((char)c);
            continue;
        }
    }
}

void run_editor(const char* filename) {
    edit_filename = filename;
    buf_len = 0;
    cursor = 0;
    view_line = 0;
    exit_editor = 0;
    for (size_t i = 0; i < EDITOR_MAX_SIZE; i++) editor_buf[i] = 0;
    while (keyboard_has_char()) (void)keyboard_read_char();
    load_file();
    attr_normal = VGA_COLOR_LIGHT_GREY | (VGA_COLOR_BLACK << 4);
    attr_status = VGA_COLOR_BLACK | (VGA_COLOR_LIGHT_CYAN << 4);
    vga_set_text_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    for (int y = 0; y < 25; y++)
        for (int x = 0; x < 80; x++)
            vga_put_char_at(' ', x, y, attr_normal);
    while (!exit_editor) {
        handle_input();
        ensure_cursor_visible();
        draw_status(NULL);
        render();
        timer_sleep(20);
    }
    vga_set_text_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK); // Reset text color to light green
}
