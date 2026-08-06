#ifndef GOOBER_SOFTCLOCK_H
#define GOOBER_SOFTCLOCK_H

#include <stdint.h>

typedef struct {
    int year;   /* full year, e.g. 2026 */
    int month;  /* 1..12 */
    int day;    /* 1..31 */
    int hour;   /* 0..23 */
    int minute; /* 0..59 */
    int second; /* 0..59 */
} softclock_t;

void softclock_init(void);
void softclock_tick(void); /* advance from timer_ticks(); call each frame */
void softclock_get(softclock_t* out);
void softclock_set(const softclock_t* in);
/* "YYYY-MM-DD HH:MM" into out (needs >= 17 bytes). Returns out. */
char* softclock_format(char* out, int out_sz);
/* Compact "MM/DD HH:MM" (needs >= 12 bytes). */
char* softclock_format_short(char* out, int out_sz);
int softclock_minute_stamp(void); /* year*525600-ish unique per displayed minute */

#endif
