#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

void timer_init(uint32_t frequency);
void timer_phase(uint32_t hz);
void timer_interrupt_handler(void);
void timer_sleep(uint32_t ms);
uint32_t timer_ticks(void);
/* Monotonic milliseconds since boot (TSC when calibrated, else PIT×10). */
uint32_t timer_millis(void);

/*
 * ---- IRQ-independent monotonic time source (rdtsc-based) ----
 *
 * timer_ticks() is driven by IRQ0. On real hardware a USB legacy-emulation
 * SMM storm can stop IRQ0 (freezing tick) while the CPU keeps executing,
 * which turns every `while (timer_ticks() < deadline)` busy-wait into an
 * infinite loop until the platform watchdog hard-resets the machine.
 *
 * The helpers below provide a deadline clock backed by rdtsc, which keeps
 * counting regardless of interrupt delivery (and across SMM on the CPUs we
 * target). Calibrate once at init while IRQ0 is still live, then drive every
 * USB busy-wait off timer_deadline_ms()/timer_deadline_expired() instead of
 * the raw tick. When the TSC has not been calibrated the API transparently
 * falls back to the PIT tick (10 ms resolution).
 *
 * Assumes a 100 Hz PIT (1 tick = 10 ms), matching timer_init(100).
 */
void     timer_calibrate_tsc(void);        /* call once at init; IRQ0 must be live */
int      timer_tsc_calibrated(void);
uint64_t timer_tsc(void);                  /* raw rdtsc reading */
uint64_t timer_deadline_ms(uint32_t ms);   /* opaque deadline `ms` in the future */
int      timer_deadline_expired(uint64_t deadline);
void     timer_busy_wait_ms(uint32_t ms);  /* bounded fixed delay (clock + ceiling) */

#endif
