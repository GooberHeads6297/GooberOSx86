#include "../kernel.h"
#include <stddef.h>
#include "../drivers/io/io.h"
#include "../games/snake.h"
#include "../games/cubeDip.h"
#include "../games/pong.h"
#include "../taskmgr/taskmgr.h"
#include "../fs/filesystem.h"
#include "../lib/string.h"

#define PROMPT_COLOR 0x01
#define DEFAULT_COLOR 0x0F
#define SCREEN_COLS 80
#define SCREEN_ROWS 25

extern void clear_screen();
extern void print(const char*);
extern void move_cursor(uint8_t row, uint8_t col);
extern char keyboard_read_char();
extern uint8_t cursor_row;
extern uint8_t cursor_col;
extern uint16_t* const VIDEO_MEMORY;

extern FileHandle* fs_open(const char* filename);
extern size_t fs_read(FileHandle* fh, uint8_t* buffer, size_t bytes);
extern void fs_close(FileHandle* fh);
extern int fs_create(const char* filename);
extern int fs_delete(const char* filename);
extern int fs_delete_dir(const char* dirname);
extern int fs_create_dir(const char* dirname);
extern int fs_write(const char* filename, const uint8_t* data, size_t size);
extern int fs_list(void);
extern int fs_change_dir(const char* path);
extern int fs_cd_up(void);
extern const char* fs_get_cwd();

#define INPUT_BUFFER_SIZE 256
static char input_buffer[INPUT_BUFFER_SIZE];
static size_t input_pos = 0;

static int strcmp_local(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

static int strncmp_local(const char* s1, const char* s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) return (unsigned char)s1[i] - (unsigned char)s2[i];
        if (s1[i] == '\0') return 0;
    }
    return 0;
}

static int prompt_start_row = 0;
static int prompt_start_col = 0;
static int prev_cursor_row = -1;
static int prev_cursor_col = -1;
static uint16_t prev_cell_value = 0;
static int cursor_enabled = 1;

static void put_cell(int r, int c, char ch, uint8_t attr) {
    if (r < 0 || c < 0 || r >= SCREEN_ROWS || c >= SCREEN_COLS) return;
    VIDEO_MEMORY[r * SCREEN_COLS + c] = ((uint16_t)attr << 8) | (uint8_t)ch;
}

static int input_index_to_pos(size_t idx, int* out_r, int* out_c) {
    if (idx > INPUT_BUFFER_SIZE) return -1;
    int linear = prompt_start_col + (int)idx;
    int r = prompt_start_row + linear / SCREEN_COLS;
    int c = linear % SCREEN_COLS;
    if (r < 0 || c < 0 || r >= SCREEN_ROWS || c >= SCREEN_COLS) return -1;
    *out_r = r; *out_c = c;
    return 0;
}

static void ensure_scroll() {
    while (cursor_row >= SCREEN_ROWS) {
        for (int r = 1; r < SCREEN_ROWS; r++) {
            for (int c = 0; c < SCREEN_COLS; c++) {
                VIDEO_MEMORY[(r - 1) * SCREEN_COLS + c] = VIDEO_MEMORY[r * SCREEN_COLS + c];
            }
        }
        for (int c = 0; c < SCREEN_COLS; c++) {
            put_cell(SCREEN_ROWS - 1, c, ' ', DEFAULT_COLOR);
        }
        cursor_row--;
        if (prompt_start_row > 0) prompt_start_row--;
        if (prev_cursor_row > 0) prev_cursor_row--;
    }
}

static void restore_prev_cursor_cell() {
    if (prev_cursor_row == -1) return;
    put_cell(prev_cursor_row, prev_cursor_col,
             (char)(prev_cell_value & 0xFF),
             (uint8_t)(prev_cell_value >> 8));
    prev_cursor_row = -1;
    prev_cursor_col = -1;
    prev_cell_value = 0;
}

static void draw_cursor() {
    if (!cursor_enabled) return;
    ensure_scroll();
    // DO NOT restore here; we will restore at safe sites before drawing text.
    if (cursor_row < 0 || cursor_col < 0 || cursor_row >= SCREEN_ROWS || cursor_col >= SCREEN_COLS) return;
    prev_cell_value = VIDEO_MEMORY[cursor_row * SCREEN_COLS + cursor_col];
    move_cursor(cursor_row, cursor_col);
    put_cell(cursor_row, cursor_col, '_', PROMPT_COLOR);
    prev_cursor_row = cursor_row;
    prev_cursor_col = cursor_col;
}

static void print_char_shell(char c) {
    restore_prev_cursor_cell();      // <— moved here
    if (c == '\n') {
        cursor_row++;
        cursor_col = 0;
        ensure_scroll();
    } else {
        put_cell(cursor_row, cursor_col, c, DEFAULT_COLOR);
        if (++cursor_col >= SCREEN_COLS) { cursor_col = 0; cursor_row++; ensure_scroll(); }
    }
    draw_cursor();
}

static void prompt() {
    // make sure we don't overwrite our first character later
    restore_prev_cursor_cell();

    const char* cwd = fs_get_cwd();
    const char* left = "GooberOS";
    for (size_t i = 0; left[i] != '\0'; i++) {
        put_cell(cursor_row, cursor_col, left[i], PROMPT_COLOR);
        if (++cursor_col >= SCREEN_COLS) { cursor_col = 0; cursor_row++; ensure_scroll(); }
    }
    if (cwd && cwd[0] != '\0' && strcmp_local(cwd, "/") != 0) {
        put_cell(cursor_row, cursor_col, '[', PROMPT_COLOR);
        if (++cursor_col >= SCREEN_COLS) { cursor_col = 0; cursor_row++; ensure_scroll(); }
        for (size_t i = 0; cwd[i] != '\0'; i++) {
            put_cell(cursor_row, cursor_col, cwd[i], PROMPT_COLOR);
            if (++cursor_col >= SCREEN_COLS) { cursor_col = 0; cursor_row++; ensure_scroll(); }
        }
        put_cell(cursor_row, cursor_col, ']', PROMPT_COLOR);
        if (++cursor_col >= SCREEN_COLS) { cursor_col = 0; cursor_row++; ensure_scroll(); }
    }
    put_cell(cursor_row, cursor_col, '>', PROMPT_COLOR);
    if (++cursor_col >= SCREEN_COLS) { cursor_col = 0; cursor_row++; ensure_scroll(); }
    put_cell(cursor_row, cursor_col, ' ', PROMPT_COLOR);
    if (++cursor_col >= SCREEN_COLS) { cursor_col = 0; cursor_row++; ensure_scroll(); }

    prompt_start_row = cursor_row;
    prompt_start_col = cursor_col;
    input_pos = 0;
    for (size_t i = 0; i < INPUT_BUFFER_SIZE; i++) input_buffer[i] = 0;

    cursor_enabled = 1;
    draw_cursor();
}


static void clear_input() {
    input_pos = 0;
    for (size_t i = 0; i < INPUT_BUFFER_SIZE; i++) input_buffer[i] = 0;
}

static void reboot() {
    __asm__ volatile ("cli");
    struct { uint16_t limit; uint32_t base; } __attribute__((packed)) idt_ptr = {0, 0};
    __asm__ volatile ("lidt %0" : : "m"(idt_ptr));
    __asm__ volatile ("int3\nud2\n");
    while (1) __asm__ volatile ("hlt");
}

static void list_games() {
    print("snakeGame.exe\n");
    print("cubeDip.exe\n");
    print("pong.exe\n");
}

static void execute_command(const char* cmd) {
    if (!cmd) return;

    restore_prev_cursor_cell();
    move_cursor(cursor_row, cursor_col);
    cursor_enabled = 0;

    if (cmd[0] == '\0') {
        cursor_enabled = 1;
        draw_cursor();
        return;
    }

    if (!strcmp_local(cmd, "help")) {
        print("Available commands:\nhelp\ncls\necho\nls\ncd\nexit\ngames\ntaskview\nnew\nwrite\nmkdir\ndel\nrmdir\nread\nsnakeGame.exe\ncubeDip.exe\npong.exe\n");
    } else if (!strcmp_local(cmd, "cls")) {
        clear_screen();
        cursor_row = 0;
        cursor_col = 0;
    } else if (!strncmp_local(cmd, "echo ", 5)) {
        print(cmd + 5);
        print("\n");
    } else if (!strcmp_local(cmd, "ls")) {
        fs_list();
    } else if (!strncmp_local(cmd, "cd ", 3)) {
        const char* path = cmd + 3;
        if (!strcmp_local(path, "..")) {
            if (fs_cd_up() == 0) print("Moved up one directory\n");
            else print("Already at root directory\n");
        } else if (path[0] == '\0') {
            print("cd: Directory required\n");
        } else {
            if (fs_change_dir(path) == 0) {
                print("Changed directory to ");
                print(path);
                print("\n");
            } else {
                print("cd: Directory not found: ");
                print(path);
                print("\n");
            }
        }
    } else if (!strncmp_local(cmd, "new ", 4)) {
        const char* filename = cmd + 4;
        if (filename[0] == '\0') {
            print("new: Filename required\n");
        } else {
            if (fs_create(filename) == 0) {
                print("Created ");
                print(filename);
                print("\n");
            } else {
                print("new: Failed to create ");
                print(filename);
                print("\n");
            }
        }
    } else if (!strncmp_local(cmd, "write ", 6)) {
        const char* rest = cmd + 6;
        size_t i = 0;
        while (rest[i] && rest[i] != ' ') i++;
        char filename[INPUT_BUFFER_SIZE] = {0};
        size_t fn_len = i < INPUT_BUFFER_SIZE - 1 ? i : INPUT_BUFFER_SIZE - 1;
        for (size_t j = 0; j < fn_len; j++) filename[j] = rest[j];
        filename[fn_len] = '\0';
        const char* content = rest + i;
        while (*content == ' ') content++;
        if (filename[0] == '\0') {
            print("write: Filename required\n");
        } else {
            if (fs_write(filename, (const uint8_t*)content, strlen(content)) == 0) {
                print("Wrote ");
                print(filename);
                print("\n");
            } else {
                print("write: Failed to write ");
                print(filename);
                print("\n");
            }
        }
    } else if (!strncmp_local(cmd, "mkdir ", 6)) {
        const char* arg = cmd + 6;
        size_t len = 0;
        while (arg[len] != '\0') len++;
        int trailing = (len > 0 && arg[len - 1] == '/');
        char dirname[INPUT_BUFFER_SIZE] = {0};
        size_t copy_len = trailing ? len - 1 : len;
        if (copy_len >= INPUT_BUFFER_SIZE) copy_len = INPUT_BUFFER_SIZE - 1;
        for (size_t j = 0; j < copy_len; j++) dirname[j] = arg[j];
        dirname[copy_len] = '\0';
        if (dirname[0] == '\0') {
            print("mkdir: Directory name required\n");
        } else {
            if (fs_create_dir(dirname) == 0) {
                print("Created ");
                print(dirname);
                print(" directory\n");
            } else {
                print("mkdir: Failed to create ");
                print(dirname);
                print("\n");
            }
        }
    } else if (!strncmp_local(cmd, "del ", 4)) {
        const char* target = cmd + 4;
        size_t len = 0;
        while (target[len] != '\0') len++;
        if (len > 1 && target[len - 1] == '/') {
            char dirname[INPUT_BUFFER_SIZE];
            size_t copy_len = len - 1 < INPUT_BUFFER_SIZE - 1 ? len - 1 : INPUT_BUFFER_SIZE - 1;
            for (size_t i = 0; i < copy_len; i++) dirname[i] = target[i];
            dirname[copy_len] = '\0';
            if (fs_delete_dir(dirname) == 0) {
                print("(deleted ");
                print(dirname);
                print(" directory)\n");
            } else {
                print("del: Directory not found or failed: ");
                print(dirname);
                print("\n");
            }
        } else if (target[0] == '\0') {
            print("del: Filename or directory required\n");
        } else {
            if (fs_delete(target) == 0) {
                print("Erased ");
                print(target);
                print("\n");
            } else {
                print("del: File not found or failed: ");
                print(target);
                print("\n");
            }
        }
    } else if (!strncmp_local(cmd, "rmdir ", 6)) {
        const char* dirname = cmd + 6;
        if (dirname[0] == '\0') {
            print("rmdir: Directory name required\n");
        } else {
            if (fs_delete_dir(dirname) == 0) {
                print("Removed directory ");
                print(dirname);
                print("\n");
            } else {
                print("rmdir: failed to remove ");
                print(dirname);
                print("\n");
            }
        }
    } else if (!strncmp_local(cmd, "read ", 5)) {
        const char* filename = cmd + 5;
        if (filename[0] == '\0') {
            print("read: Filename required\n");
        } else {
            FileHandle* fh = fs_open(filename);
            if (!fh) {
                print("read: File not found: ");
                print(filename);
                print("\n");
            } else {
                uint8_t buffer[128];
                size_t n;
                while ((n = fs_read(fh, buffer, sizeof(buffer) - 1)) > 0) {
                    buffer[n] = 0;
                    print((const char*)buffer);
                }
                fs_close(fh);
                print("\n");
            }
        }
    } else if (!strcmp_local(cmd, "exit")) {
        print("Rebooting...\n");
        reboot();
    } else if (!strcmp_local(cmd, "games")) {
        list_games();
    } else if (!strcmp_local(cmd, "snakeGame.exe")) {
        print("Launching snakeGame.exe... Press ESC to quit.\n");
        run_snake_game();
        print("Exited snakeGame.exe\n");
    } else if (!strcmp_local(cmd, "cubeDip.exe")) {
        print("Launching cubeDip.exe... Press ESC to quit.\n");
        run_cubeDip_game();
        print("Exited cubeDip.exe\n");
    } else if (!strcmp_local(cmd, "pong.exe")) {
        print("Launching Pong... Press ESC to quit.\n");
        run_pong_game();
        print("Exited Pong\n");
    } else if (!strcmp_local(cmd, "taskview")) {
        run_task_manager();
        print("Exited task manager\n");
    } else {
        print("Unknown command: ");
        print(cmd);
        print("\n");
    }


    cursor_enabled = 1;
    draw_cursor();
}



void shell_init() {
    clear_input();
    prompt();
}

void shell_run() {
    char c = keyboard_read_char();
    if (!c) return;

    if (c == '\r' || c == '\n') {
        restore_prev_cursor_cell();        // <— restore first
        print_char_shell('\n');            // moves to new line cleanly
        input_buffer[input_pos] = '\0';
        execute_command(input_buffer);     // printing is safe (cursor disabled inside)
        clear_input();
        prompt();                          // prompt restores before drawing itself
    } else if (c == '\b' || c == 127) {
        if (input_pos > 0) {
            input_pos--;
            int r, col;
            if (input_index_to_pos(input_pos, &r, &col) == 0) {
                restore_prev_cursor_cell();  // <— restore first
                cursor_row = r;
                cursor_col = col;
                put_cell(cursor_row, cursor_col, ' ', DEFAULT_COLOR);
                draw_cursor();
            }
        }
    } else if ((unsigned char)c == 0x8B) {
        restore_prev_cursor_cell();       // <— safety
        run_task_manager();
        print("Exited task manager\n");
        prompt();
    } else if (input_pos < INPUT_BUFFER_SIZE - 1) {
        int r = cursor_row, col = cursor_col;
        restore_prev_cursor_cell();       // <— restore first
        input_buffer[input_pos++] = c;
        put_cell(r, col, c, DEFAULT_COLOR);
        cursor_row = r;
        cursor_col = col;
        if (++cursor_col >= SCREEN_COLS) { cursor_col = 0; cursor_row++; ensure_scroll(); }
        draw_cursor();
    }
}