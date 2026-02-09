#include "timer.h"
#include "../io/io.h"
#include "../video/vga.h"

#define PIT_CHANNEL0_DATA 0x40
#define PIT_COMMAND       0x43
#define PIT_FREQUENCY     1193180

static volatile uint32_t tick = 0;
static volatile uint32_t timer_hz = 0;

void timer_phase(uint32_t hz) {
    uint16_t divisor = (uint16_t)(PIT_FREQUENCY / hz);
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0_DATA, (uint8_t)((divisor >> 8) & 0xFF));
}

void timer_init(uint32_t frequency) {
    timer_hz = frequency;
    timer_phase(frequency);
}

void timer_interrupt_handler() {
    tick++;

    outb(0x20, 0x20);
}

void timer_sleep(uint32_t ms) {
    uint32_t start = tick;
    uint32_t ticks_needed = ms;
    if (timer_hz > 0) {
        ticks_needed = (ms * timer_hz + 999) / 1000;
        if (ticks_needed == 0) ticks_needed = 1;
    }
    while ((tick - start) < ticks_needed) {
        __asm__ volatile ("hlt");
    }
}

uint32_t timer_ticks(void) {
    return tick;
}
