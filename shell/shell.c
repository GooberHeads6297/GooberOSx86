#include "../kernel.h"
#include <stddef.h>
#include "../drivers/io/io.h"
#include "../games/snake.h"
#include "../games/cubeDip.h"
#include "../games/pong.h"
#include "../games/doom.h"
#include "../editor/editor.h"
#include "../taskmgr/taskmgr.h"
#include "../fs/filesystem.h"
#include "../lib/string.h"
#include "../drivers/video/vga.h"
#include "../drivers/keyboard/keyboard.h"
#include "../drivers/timer/timer.h"
#include "../gui/window.h"
#include "../drivers/storage/bios_disk.h"

#define PROMPT_COLOR VGA_COLOR_BLUE
static uint8_t current_color = VGA_COLOR_LIGHT_GREEN;
#define SCREEN_COLS 80
#define SCREEN_ROWS 25

extern void print(const char*);
extern char keyboard_read_char();
extern uint8_t cursor_row;
extern uint8_t cursor_col;

extern unsigned char _binary_GooberOSx86_iso_start;
extern unsigned char _binary_GooberOSx86_iso_end;

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
#define HISTORY_SIZE 32
static char input_buffer[INPUT_BUFFER_SIZE];
static size_t input_pos = 0;
static char history[HISTORY_SIZE][INPUT_BUFFER_SIZE];
static int history_next = 0;
static int history_count = 0;
static int history_nav_offset = -1; // -1 = not browsing history
static char saved_input[INPUT_BUFFER_SIZE];

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
static int cursor_drawn = 0;
static uint32_t last_blink_tick = 0;

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
            put_cell(SCREEN_ROWS - 1, c, ' ', current_color);
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
    cursor_drawn = 0;
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
    cursor_drawn = 1;
}

static void blink_cursor() {
    if (!cursor_enabled) return;
    uint32_t now = timer_ticks();
    if (last_blink_tick == 0) last_blink_tick = now;
    if ((now - last_blink_tick) >= 20) { // ~200ms at 100Hz
        last_blink_tick = now;
        if (cursor_drawn) restore_prev_cursor_cell();
        else draw_cursor();
    }
}

static void print_char_shell(char c) {
    restore_prev_cursor_cell();      // <— moved here
    if (c == '\n') {
        cursor_row++;
        cursor_col = 0;
        ensure_scroll();
    } else {
        put_cell(cursor_row, cursor_col, c, current_color);
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

static size_t input_len(void) {
    size_t i = 0;
    while (i < INPUT_BUFFER_SIZE && input_buffer[i] != '\0') i++;
    return i;
}

static int history_index_from_offset(int offset) {
    // offset 0 = newest entry, offset 1 = previous...
    return (history_next - 1 - offset + HISTORY_SIZE) % HISTORY_SIZE;
}

static void set_input_line(const char* text) {
    size_t len = 0;
    while (text[len] && len < INPUT_BUFFER_SIZE - 1) len++;
    for (size_t i = 0; i < INPUT_BUFFER_SIZE; i++)
        input_buffer[i] = (i < len) ? text[i] : '\0';
    input_pos = len;
    int r = prompt_start_row, c = prompt_start_col;
    for (size_t i = 0; i < INPUT_BUFFER_SIZE; i++) {
        put_cell(r, c, (i < len) ? text[i] : ' ', current_color);
        c++;
        if (c >= SCREEN_COLS) { c = 0; r++; }
    }
    if (input_index_to_pos(input_pos, &r, &c) == 0) {
        cursor_row = r;
        cursor_col = c;
    }
    draw_cursor();
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
    print("doom.exe\n");
}

static int parse_hex(const char* s, uint32_t* out) {
    uint32_t val = 0;
    if (!s || *s == '\0') return -1;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (*s == '\0') return -1;
    while (*s) {
        char c = *s++;
        uint8_t d;
        if (c >= '0' && c <= '9') d = (uint8_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint8_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint8_t)(c - 'A' + 10);
        else return -1;
        val = (val << 4) | d;
    }
    *out = val;
    return 0;
}

static void list_devices() {
    bios_disk_scan();
    int count = bios_disk_count();
    if (count <= 0) {
        print("No BIOS drives detected.\n");
        return;
    }
    print("BIOS drives:\n");
    for (int i = 0; i < count; i++) {
        const bios_drive_info_t* d = bios_disk_get(i);
        if (!d || !d->present) continue;
        print("  Drive 0x");
        char buf[16];
        itoa(d->drive, buf, 16);
        print(buf);
        print(" (sector size ");
        itoa((int)d->sector_size, buf, 10);
        print(buf);
        print(")\n");
    }
}

static void install_iso_to_drive(uint8_t drive) {
    unsigned char* iso_start = &_binary_GooberOSx86_iso_start;
    unsigned char* iso_end = &_binary_GooberOSx86_iso_end;
    size_t iso_size = (size_t)(iso_end - iso_start);
    size_t total_sectors = (iso_size + 511) / 512;

    print("Writing ISO to drive 0x");
    char buf[16];
    itoa(drive, buf, 16);
    print(buf);
    print("... ");

    for (size_t i = 0; i < total_sectors; i++) {
        const void* src = iso_start + (i * 512);
        if (bios_write_lba(drive, (uint64_t)i, 1, src) != 0) {
            print("\ninstall: write failed at LBA ");
            itoa((int)i, buf, 10);
            print(buf);
            print("\n");
            return;
        }
    }
    print("done\n");
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
    {
        size_t len = 0;
        while (cmd[len] && len < INPUT_BUFFER_SIZE) len++;
        if (len > 0) {
            for (size_t i = 0; i < len && i < INPUT_BUFFER_SIZE - 1; i++)
                history[history_next][i] = cmd[i];
            history[history_next][(len < INPUT_BUFFER_SIZE - 1 ? len : INPUT_BUFFER_SIZE - 1)] = '\0';
            history_next = (history_next + 1) % HISTORY_SIZE;
            if (history_count < HISTORY_SIZE) history_count++;
        }
        history_nav_offset = -1;
    }

    if (!strcmp_local(cmd, "help")) {
        print("Available commands:\nhelp\ncls\necho\nls\ncd\nexit\ngames\ntaskview\ndevices\ninstall\nedit\nnew\nwrite\nmkdir\ndel\nrmdir\nread\ngui\ncolor\n");
    } else if (!strcmp_local(cmd, "gui")) {
        gui_run();
        prompt();
    } else if (!strncmp_local(cmd, "color ", 6)) {
        const char* args = cmd + 6;
        uint32_t val;
        if (parse_hex(args, &val) == 0) {
            current_color = (uint8_t)val;
            print("Color changed.\n");
        } else {
            print("Usage: color <hex>\nExample: color 0A (Green on Black)\n");
        }
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
    } else if (!strcmp_local(cmd, "devices")) {
        list_devices();
    } else if (!strncmp_local(cmd, "install ", 8)) {
        const char* args = cmd + 8;
        while (*args == ' ') args++;
        if (*args == '\0') {
            print("install: drive required (e.g. install 0x80 YES)\n");
        } else {
            const char* drive_str = args;
            while (*args && *args != ' ') args++;
            char tmp[16] = {0};
            size_t len = (size_t)(args - drive_str);
            if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
            for (size_t i = 0; i < len; i++) tmp[i] = drive_str[i];
            tmp[len] = '\0';

            uint32_t drive_val = 0;
            int parse_ok = 0;
            if (tmp[0] == '0' && (tmp[1] == 'x' || tmp[1] == 'X')) {
                parse_ok = (parse_hex(tmp, &drive_val) == 0);
            } else {
                drive_val = (uint32_t)atoi(tmp);
                parse_ok = 1;
            }

            while (*args == ' ') args++;
            if (!parse_ok) {
                print("install: invalid drive\n");
            } else if (!(args[0] == 'Y' && args[1] == 'E' && args[2] == 'S' && args[3] == '\0')) {
                print("install: add YES to confirm (destructive)\n");
            } else {
                install_iso_to_drive((uint8_t)drive_val);
            }
        }
    } else if (!strncmp_local(cmd, "edit ", 5)) {
        const char* filename = cmd + 5;
        while (*filename == ' ') filename++;
        if (*filename == '\0') {
            print("edit: Filename required\n");
        } else {
            run_editor(filename);
            clear_screen();
            vga_set_text_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            cursor_row = 0;
            cursor_col = 0;
            print("Exited editor\n");
            vga_set_text_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
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
    } else if (!strcmp_local(cmd, "doom.exe")) {
        print("Launching Doom prototype... Press ESC to quit.\n");
        run_doom_game();
        print("Exited Doom\n");
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
    if (!c) {
        blink_cursor();
        return;
    }

    if (c == '\r' || c == '\n') {
        restore_prev_cursor_cell();        // <— restore first
        print_char_shell('\n');            // moves to new line cleanly
        input_buffer[input_pos] = '\0';
        execute_command(input_buffer);     // printing is safe (cursor disabled inside)
        clear_input();
        prompt();                          // prompt restores before drawing itself
    } else if (c == '\b' || c == 127) {
        if (input_pos > 0) {
            size_t len = input_len();
            for (size_t i = input_pos - 1; i < len && i + 1 < INPUT_BUFFER_SIZE; i++)
                input_buffer[i] = input_buffer[i + 1];
            input_buffer[len > 0 ? len - 1 : 0] = '\0';
            input_pos--;
            restore_prev_cursor_cell();
            set_input_line(input_buffer);
        }
    } else if ((unsigned char)c == KEY_F1) {
        restore_prev_cursor_cell();       // safety
        run_task_manager();
        print("Exited task manager\n");
        prompt();
    } else if ((unsigned char)c == KEY_UP) {
        // Up arrow: older command
        if (history_count > 0) {
            if (history_nav_offset < 0) {
                strcpy(saved_input, input_buffer);
                history_nav_offset = 0;
            } else if (history_nav_offset < history_count - 1) {
                history_nav_offset++;
            }

            int idx = history_index_from_offset(history_nav_offset);
            strcpy(input_buffer, history[idx]);
            input_pos = strlen(input_buffer);
            restore_prev_cursor_cell();
            set_input_line(input_buffer);
        }
    } else if ((unsigned char)c == KEY_DOWN) {
        // Down arrow: newer command
        if (history_count > 0 && history_nav_offset >= 0) {
            history_nav_offset--;
            if (history_nav_offset < 0) {
                strcpy(input_buffer, saved_input);
            } else {
                int idx = history_index_from_offset(history_nav_offset);
                strcpy(input_buffer, history[idx]);
            }
            input_pos = strlen(input_buffer);
            restore_prev_cursor_cell();
            set_input_line(input_buffer);
        }
    } else if ((unsigned char)c == KEY_LEFT) {
        // Left arrow: move cursor left
        if (input_pos > 0) {
            input_pos--;
            int r, col;
            input_index_to_pos(input_pos, &r, &col);
            restore_prev_cursor_cell();
            move_cursor(r, col);
            draw_cursor();
        }
    } else if ((unsigned char)c == KEY_RIGHT) {
        // Right arrow: move cursor right
        size_t len = input_len();
        if (input_pos < len) {
            input_pos++;
            int r, col;
            input_index_to_pos(input_pos, &r, &col);
            restore_prev_cursor_cell();
            move_cursor(r, col);
            draw_cursor();
        }
    } else if (input_pos < INPUT_BUFFER_SIZE - 1) {
        size_t len = input_len();
        if (input_pos < len && len < INPUT_BUFFER_SIZE - 1) {
            for (size_t i = len + 1; i > input_pos; i--) input_buffer[i] = input_buffer[i - 1];
            input_buffer[input_pos] = c;
            input_pos++;
            restore_prev_cursor_cell();
            set_input_line(input_buffer);
        } else if (len < INPUT_BUFFER_SIZE - 1) {
            int r = cursor_row, col = cursor_col;
            restore_prev_cursor_cell();
            input_buffer[input_pos++] = c;
            put_cell(r, col, c, current_color);
            cursor_row = r;
            cursor_col = col;
            if (++cursor_col >= SCREEN_COLS) { cursor_col = 0; cursor_row++; ensure_scroll(); }
            draw_cursor();
        }
    }
}