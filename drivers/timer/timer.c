#include "timer.h"
#include "../io/io.h"
#include "../video/vga.h"

#define PIT_CHANNEL0_DATA 0x40
#define PIT_COMMAND       0x43
#define PIT_FREQUENCY     1193180

static volatile uint32_t tick = 0;
static volatile uint32_t timer_hz = 0;

/*
 * Calibrated TSC rate. cycles/ms fits comfortably in 32 bits even for very
 * fast CPUs (10 GHz -> 1e7), so we keep the multiply/compare math 32x32->64
 * and avoid any 64-bit division at runtime (no __udivdi3 in this freestanding
 * build). 0 means "not calibrated yet" -> callers fall back to the PIT tick.
 */
static uint32_t tsc_cycles_per_ms = 0;
static int      tsc_calibrated = 0;

/* Number of PIT ticks (10 ms each at 100 Hz) to average TSC over at init. */
#define TSC_CAL_TICKS 3

/*
 * Absolute spin ceiling for the bounded fixed-delay helper. Sized so it never
 * truncates a legitimate sub-2-second delay even on a fast multi-GHz CPU, yet
 * still guarantees termination (a few seconds) in the impossible case where
 * the TSC itself stops advancing.
 */
#define TIMER_DELAY_CEILING 2000000U

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
    uint32_t spins = 0;
    uint32_t last = start;
    if (timer_hz > 0) {
        ticks_needed = (ms * timer_hz + 999) / 1000;
        if (ticks_needed == 0) ticks_needed = 1;
    }
    /*
     * Prefer HLT (IRQ0 wakes us). CRITICAL: if IF=0, HLT never returns and
     * a post-increment spin guard never runs — that hard-froze the desktop
     * after the first keypress on both VirtualBox and real hardware.
     * Check RFLAGS.IF; if clear, spin with pause and bail out.
     */
    while ((tick - start) < ticks_needed) {
        uintptr_t flags;
#ifdef __x86_64__
        __asm__ volatile("pushfq; pop %0" : "=r"(flags) :: "memory");
#else
        __asm__ volatile("pushf; pop %0" : "=r"(flags) :: "memory");
#endif
        if (flags & (1u << 9))
            __asm__ volatile("hlt");
        else
            __asm__ volatile("pause");
        spins++;
        if (tick != last) {
            last = tick;
            spins = 0;
        } else if (spins > 100000u) {
            return;
        }
    }
}

uint32_t timer_ticks(void) {
    return tick;
}

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

uint64_t timer_tsc(void) {
    return rdtsc();
}

int timer_tsc_calibrated(void) {
    return tsc_calibrated;
}

/*
 * Measure the TSC rate against a fixed number of PIT ticks. Must run while
 * IRQ0 is still being delivered (i.e. before any USB port I/O could trigger
 * an SMM storm). The divisor (TSC_CAL_TICKS * 10) is a compile-time constant
 * so the compiler folds the division to a reciprocal multiply rather than a
 * 64-bit divide libcall.
 */
void timer_calibrate_tsc(void) {
    /*
     * Bound the calibration spins so that if IRQ0 is somehow not advancing
     * the tick, we fall back to the PIT-tick path instead of hanging here.
     * The ceiling is huge relative to the ~30 ms we actually expect to wait.
     */
    uint32_t guard;

    /* Align to a tick edge so the measured window is a whole number of ticks. */
    uint32_t t0 = tick;
    guard = TIMER_DELAY_CEILING;
    while (tick == t0) { if (--guard == 0) { tsc_calibrated = 0; return; } }
    t0 = tick;
    uint64_t c0 = rdtsc();
    guard = TIMER_DELAY_CEILING;
    while ((uint32_t)(tick - t0) < TSC_CAL_TICKS) {
        if (--guard == 0) { tsc_calibrated = 0; return; }
    }
    uint64_t c1 = rdtsc();

    uint64_t elapsed = c1 - c0;
    /* A short PIT-tick window stays well under 2^32 cycles for realistic
     * CPU clocks, so reduce to 32 bits before the constant-divisor divide. */
    uint32_t elapsed32 = (uint32_t)elapsed;
    if ((elapsed >> 32) != 0) {
        /* Implausibly fast clock; clamp to keep the rate non-zero/sane. */
        elapsed32 = 0xFFFFFFFFU;
    }
    tsc_cycles_per_ms = elapsed32 / (TSC_CAL_TICKS * 10);
    if (tsc_cycles_per_ms == 0) {
        /* Calibration failed (TSC not advancing?). Stay on the PIT tick. */
        tsc_calibrated = 0;
        return;
    }
    tsc_calibrated = 1;
}

/* ms -> PIT ticks at 100 Hz (10 ms/tick), rounded up; constant divisor. */
static uint32_t timer_ms_to_ticks(uint32_t ms) {
    uint32_t t = (ms + 9) / 10;
    return t ? t : 1;
}

uint64_t timer_deadline_ms(uint32_t ms) {
    if (tsc_calibrated) {
        uint64_t now = rdtsc();
        /* 32x32 widening multiply: no 64-bit multiply libcall. */
        uint64_t delta = (uint64_t)ms * (uint64_t)tsc_cycles_per_ms;
        return now + delta;
    }
    /* PIT fallback: deadline expressed in ticks (low 32 bits significant). */
    return (uint64_t)(tick + timer_ms_to_ticks(ms));
}

int timer_deadline_expired(uint64_t deadline) {
    if (tsc_calibrated) {
        return (int64_t)(rdtsc() - deadline) >= 0;
    }
    return (int32_t)((uint32_t)tick - (uint32_t)deadline) >= 0;
}

void timer_busy_wait_ms(uint32_t ms) {
    uint64_t deadline = timer_deadline_ms(ms);
    uint32_t guard = TIMER_DELAY_CEILING;
    while (!timer_deadline_expired(deadline)) {
        __asm__ volatile("pause");
        if (--guard == 0) break;  /* belt-and-suspenders: never spin forever */
    }
}
