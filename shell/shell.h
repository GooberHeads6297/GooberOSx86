#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>
#include "../kernel.h"

typedef void (*shell_clear_sink_t)(void* ctx);

void run_shell();
void execute_command(const char* cmd);
void print(const char* str);
void print_colored(const char* str, uint8_t fg, uint8_t bg);
void clear_screen(void);
void shell_set_redirect(kernel_print_sink_t write_sink, shell_clear_sink_t clear_sink, void* ctx);
void shell_clear_redirect(void);
void shell_init();
void shell_run();

#endif
