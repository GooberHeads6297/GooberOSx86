#include "softclock.h"
#include "timer.h"

static softclock_t g_clock;
static uint32_t g_last_tick;
static int g_ready;

static int days_in_month(int year, int month) {
    static const int dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int d;
    if (month < 1 || month > 12) return 30;
    d = dim[month - 1];
    if (month == 2) {
        int leap = ((year % 4) == 0 && ((year % 100) != 0 || (year % 400) == 0));
        if (leap) d = 29;
    }
    return d;
}

static void clamp_fields(softclock_t* c) {
    int dim;
    if (!c) return;
    if (c->year < 1970) c->year = 1970;
    if (c->year > 2099) c->year = 2099;
    if (c->month < 1) c->month = 1;
    if (c->month > 12) c->month = 12;
    dim = days_in_month(c->year, c->month);
    if (c->day < 1) c->day = 1;
    if (c->day > dim) c->day = dim;
    if (c->hour < 0) c->hour = 0;
    if (c->hour > 23) c->hour = 23;
    if (c->minute < 0) c->minute = 0;
    if (c->minute > 59) c->minute = 59;
    if (c->second < 0) c->second = 0;
    if (c->second > 59) c->second = 59;
}

static void advance_one_second(void) {
    int dim;
    g_clock.second++;
    if (g_clock.second < 60) return;
    g_clock.second = 0;
    g_clock.minute++;
    if (g_clock.minute < 60) return;
    g_clock.minute = 0;
    g_clock.hour++;
    if (g_clock.hour < 24) return;
    g_clock.hour = 0;
    g_clock.day++;
    dim = days_in_month(g_clock.year, g_clock.month);
    if (g_clock.day <= dim) return;
    g_clock.day = 1;
    g_clock.month++;
    if (g_clock.month <= 12) return;
    g_clock.month = 1;
    g_clock.year++;
}

void softclock_init(void) {
    g_clock.year = 2026;
    g_clock.month = 1;
    g_clock.day = 1;
    g_clock.hour = 12;
    g_clock.minute = 0;
    g_clock.second = 0;
    g_last_tick = timer_ticks();
    g_ready = 1;
}

void softclock_tick(void) {
    uint32_t now;
    uint32_t elapsed;
    if (!g_ready) softclock_init();
    now = timer_ticks();
    if ((int32_t)(now - g_last_tick) < 0) {
        g_last_tick = now;
        return;
    }
    elapsed = now - g_last_tick;
    /* 100 Hz → 1 second every 100 ticks */
    while (elapsed >= 100u) {
        advance_one_second();
        g_last_tick += 100u;
        elapsed -= 100u;
    }
}

void softclock_get(softclock_t* out) {
    if (!out) return;
    if (!g_ready) softclock_init();
    softclock_tick();
    *out = g_clock;
}

void softclock_set(const softclock_t* in) {
    if (!in) return;
    g_clock = *in;
    clamp_fields(&g_clock);
    g_last_tick = timer_ticks();
    g_ready = 1;
}

static void put2(char* p, int v) {
    if (v < 0) v = 0;
    if (v > 99) v = 99;
    p[0] = (char)('0' + (v / 10));
    p[1] = (char)('0' + (v % 10));
}

char* softclock_format(char* out, int out_sz) {
    softclock_t c;
    if (!out || out_sz < 17) {
        if (out && out_sz > 0) out[0] = '\0';
        return out;
    }
    softclock_get(&c);
    /* YYYY-MM-DD HH:MM */
    out[0] = (char)('0' + ((c.year / 1000) % 10));
    out[1] = (char)('0' + ((c.year / 100) % 10));
    out[2] = (char)('0' + ((c.year / 10) % 10));
    out[3] = (char)('0' + (c.year % 10));
    out[4] = '-';
    put2(out + 5, c.month);
    out[7] = '-';
    put2(out + 8, c.day);
    out[10] = ' ';
    put2(out + 11, c.hour);
    out[13] = ':';
    put2(out + 14, c.minute);
    out[16] = '\0';
    return out;
}

char* softclock_format_short(char* out, int out_sz) {
    softclock_t c;
    if (!out || out_sz < 12) {
        if (out && out_sz > 0) out[0] = '\0';
        return out;
    }
    softclock_get(&c);
    put2(out + 0, c.month);
    out[2] = '/';
    put2(out + 3, c.day);
    out[5] = ' ';
    put2(out + 6, c.hour);
    out[8] = ':';
    put2(out + 9, c.minute);
    out[11] = '\0';
    return out;
}

int softclock_minute_stamp(void) {
    softclock_t c;
    softclock_get(&c);
    return c.year * 527040 + c.month * 44640 + c.day * 1440 + c.hour * 60 + c.minute;
}
