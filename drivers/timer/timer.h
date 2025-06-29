#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

void timer_init(uint32_t frequency);
void timer_phase(uint32_t hz);
void timer_interrupt_handler(void);

#endif
