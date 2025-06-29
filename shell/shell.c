#include "../kernel.h"
#include <stddef.h>
#include "../drivers/io/io.h"


extern void clear_screen();
extern void print(const char*);
extern void move_cursor(uint8_t row, uint8_t col);
extern char keyboard_read_char();
extern unsigned char inb(unsigned short port);
extern void outb(unsigned short port, unsigned char val);
extern uint8_t cursor_row;
extern uint8_t cursor_col;
extern uint16_t* const VIDEO_MEMORY;

// filesystem commands declarations
extern int fs_list();
extern int fs_change_dir(const char* path);

#define INPUT_BUFFER_SIZE 128
static char input_buffer[INPUT_BUFFER_SIZE];
static size_t input_pos = 0;

static int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++; s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

static int strncmp(const char* s1, const char* s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) return (unsigned char)s1[i] - (unsigned char)s2[i];
        if (s1[i] == '\0') return 0;
    }
    return 0;
}

static void print_char_shell(char c) {
    if (c == '\n') {
        cursor_row++;
        cursor_col = 0;
    } else {
        VIDEO_MEMORY[cursor_row * 80 + cursor_col] = ((uint16_t)0x0F << 8) | c;
        cursor_col++;
        if (cursor_col >= 80) {
            cursor_col = 0;
            cursor_row++;
        }
    }
    move_cursor(cursor_row, cursor_col);
    VIDEO_MEMORY[cursor_row * 80 + cursor_col] = ((uint16_t)0x0F << 8) | '_';
}

static void prompt() {
    print("GooberOS> ");
    cursor_col = 10;
    move_cursor(cursor_row, cursor_col);
    VIDEO_MEMORY[cursor_row * 80 + cursor_col] = ((uint16_t)0x0F << 8) | '_';
}

static void clear_input() {
    input_pos = 0;
    for (size_t i = 0; i < INPUT_BUFFER_SIZE; i++)
        input_buffer[i] = 0;
}

static void reboot() {
    while ((inb(0x64) & 0x02) != 0);
    outb(0x64, 0xFE);
    while (1) __asm__ volatile ("hlt");
}

static void execute_command(const char* cmd) {
    if (cmd[0] == '\0') return;

    if (!strcmp(cmd, "help")) {
        print("Available commands:\nhelp\ncls\necho\nls\ncd\nexit\n");
    } else if (!strcmp(cmd, "cls")) {
        clear_screen();
        cursor_row = 0;
        cursor_col = 0;
    } else if (!strncmp(cmd, "echo ", 5)) {
        print(cmd + 5);
        print("\n");
    } else if (!strcmp(cmd, "ls")) {
        fs_list();
    } else if (!strncmp(cmd, "cd ", 3)) {
        const char* path = cmd + 3;
        if (fs_change_dir(path) == 0) {
            print("Changed directory to ");
            print(path);
            print("\n");
        } else {
            print("cd: Directory not found: ");
            print(path);
            print("\n");
        }
    } else if (!strcmp(cmd, "exit")) {
        print("Rebooting...\n");
        reboot();
    } else {
        print("Unknown command: ");
        print(cmd);
        print("\n");
    }

    move_cursor(cursor_row, cursor_col);
    VIDEO_MEMORY[cursor_row * 80 + cursor_col] = ((uint16_t)0x0F << 8) | '_';
}

void shell_init() {
    clear_input();
    prompt();
}

void shell_run() {
    char c = keyboard_read_char();
    if (!c) return;

    if (c == '\r' || c == '\n') {
        VIDEO_MEMORY[cursor_row * 80 + cursor_col] = ((uint16_t)0x0F << 8) | ' ';

        print_char_shell('\n');
        input_buffer[input_pos] = '\0';
        execute_command(input_buffer);
        clear_input();
        prompt();

    } else if (c == '\b' || c == 127) {
        if (input_pos > 0) {
            VIDEO_MEMORY[cursor_row * 80 + cursor_col] = ((uint16_t)0x0F << 8) | ' ';

            input_pos--;
            if (cursor_col == 0 && cursor_row > 0) {
                cursor_row--;
                cursor_col = 79;
            } else {
                cursor_col--;
            }

            VIDEO_MEMORY[cursor_row * 80 + cursor_col] = ((uint16_t)0x0F << 8) | ' ';

            move_cursor(cursor_row, cursor_col);

            VIDEO_MEMORY[cursor_row * 80 + cursor_col] = ((uint16_t)0x0F << 8) | '_';
        }

    } else if (input_pos < INPUT_BUFFER_SIZE - 1) {
        VIDEO_MEMORY[cursor_row * 80 + cursor_col] = ((uint16_t)0x0F << 8) | ' ';

        input_buffer[input_pos++] = c;
        print_char_shell(c);

        VIDEO_MEMORY[cursor_row * 80 + cursor_col] = ((uint16_t)0x0F << 8) | '_';
    }
}
