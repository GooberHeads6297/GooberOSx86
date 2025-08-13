#include "timer.h"
#include "../io/io.h"
#include "../video/vga.h"

#define PIT_CHANNEL0_DATA 0x40
#define PIT_COMMAND       0x43
#define PIT_FREQUENCY     1193180

static volatile uint32_t tick = 0;

void timer_phase(uint32_t hz) {
    uint16_t divisor = (uint16_t)(PIT_FREQUENCY / hz);
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0_DATA, (uint8_t)((divisor >> 8) & 0xFF));
}

void timer_init(uint32_t frequency) {
    timer_phase(frequency);
}

void timer_interrupt_handler() {
    tick++;

    if (tick % 50 == 0) {
        vga_toggle_cursor();
    }

    outb(0x20, 0x20);
}

void timer_sleep(uint32_t ms) {
    uint32_t start = tick;
    while ((tick - start) < ms) {
        __asm__ volatile ("hlt");
    }
}
