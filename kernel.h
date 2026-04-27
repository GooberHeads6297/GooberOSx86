#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>

typedef void (*kernel_print_sink_t)(const char* str, void* ctx);

extern uint16_t* const VIDEO_MEMORY;
extern uint8_t cursor_row;
extern uint8_t cursor_col;
void update_cursor_visual();
void print(const char* str);
void kernel_set_print_sink(kernel_print_sink_t sink, void* ctx);
void kernel_clear_print_sink(void);
void clear_screen(void);



#endif
