#include "shell.h"
#include <stddef.h>
#include <stdint.h>

extern void clear_screen();  // declared here since no kernel.h

#define INPUT_BUFFER_SIZE 128

extern char keyboard_read_char();
extern void print(const char*);

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

static void prompt() {
    print("GooberOS> ");
}

static void clear_input() {
    input_pos = 0;
    for (size_t i = 0; i < INPUT_BUFFER_SIZE; i++)
        input_buffer[i] = 0;
}

static void execute_command(const char* cmd) {
    if (cmd[0] == '\0') return;

    if (!strcmp(cmd, "help")) {
        print("Available commands:\nhelp\nclear\necho\n");
    } else if (!strcmp(cmd, "clear")) {
        clear_screen();
    } else if (!strncmp(cmd, "echo ", 5)) {
        print(cmd + 5);
        print("\n");
    } else {
        print("Unknown command: ");
        print(cmd);
        print("\n");
    }
}

void shell_init() {
    clear_input();
    prompt();
}

void shell_run() {
    char c = keyboard_read_char();
    if (!c) return;

    if (c == '\r' || c == '\n') {
        print("\n");
        input_buffer[input_pos] = '\0';
        execute_command(input_buffer);
        clear_input();
        prompt();
    } else if (c == '\b' || c == 127) {  // Backspace
        if (input_pos > 0) {
            input_pos--;
            print("\b \b");
        }
    } else if (input_pos < INPUT_BUFFER_SIZE - 1) {
        input_buffer[input_pos++] = c;
        char str[2] = {c, '\0'};
        print(str);
    }
}
