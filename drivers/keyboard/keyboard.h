#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

void keyboard_init(void);
void keyboard_interrupt_handler(void);
/* Drain raw IRQ1 scancodes into the char buffer (call from desktop pump). */
void keyboard_poll(void);
char keyboard_read_char(void);
int keyboard_has_char(void);
void keyboard_inject_char(char c);
bool keyboard_is_shift_active(void);
bool keyboard_is_ctrl_active(void);
bool keyboard_is_alt_active(void);
/* Copy ISR breadcrumb trail for on-screen HUD (survives pump freeze). */
void keyboard_debug_snapshot(char* out, int out_max,
                             uint8_t* last_status, uint8_t* last_sc,
                             uint32_t* irq_count);
/* Live 8042 status port (0x64) read WITHOUT consuming the data byte. Used by the
 * freeze oracle to tell "output buffer stuck full" from "no keypress arriving".
 * Also returns the current PIC master IRQ mask (0x21) so we can see if IRQ1 got
 * masked. */
uint8_t keyboard_live_status(void);
uint8_t keyboard_pic_mask(void);
/* Master 8259 In-Service (bit1=IRQ1 stuck) and Request (bit1=IRQ1 pending). */
uint8_t keyboard_pic_isr(void);
uint8_t keyboard_pic_irr(void);

/* Key codes produced by the keyboard driver */
#define KEY_ESC     0x1B
#define KEY_BACKSPACE 0x08

/* Function keys (non-extended) mapped by keyboard.c */
#define KEY_F1      0x8B
#define KEY_F2      0x8C
#define KEY_F3      0x8D
#define KEY_F4      0x8E
#define KEY_F5      0x8F
#define KEY_F6      0x90
#define KEY_F7      0x91
#define KEY_F8      0x92
#define KEY_F9      0x93
#define KEY_F10     0x94
#define KEY_F11     0x95
#define KEY_F12     0x96

/* Arrow keys (extended scancodes mapped) */
#define KEY_UP      0x80
#define KEY_DOWN    0x81
#define KEY_LEFT    0x82
#define KEY_RIGHT   0x83

#endif
