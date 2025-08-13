#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>

extern uint16_t* const VIDEO_MEMORY;
extern uint8_t cursor_row;
extern uint8_t cursor_col;
void update_cursor_visual();
void clear_screen(void);



#endif
