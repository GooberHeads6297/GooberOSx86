#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>

void run_shell();
void execute_command(const char* cmd);
void print(const char* str);
void print_colored(const char* str, uint8_t fg, uint8_t bg);
void clear_screen(void);
void draw_cursor(void);
void restore_prev_cursor_cell(void);
void shell_init();
void shell_run();

#endif
