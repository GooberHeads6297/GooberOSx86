#include "keyboard.h"
#include "../io/io.h"
#include <stddef.h>
#include <stdbool.h>

/*
 * Source arbitration: when a USB HID boot keyboard is bound it delivers its
 * own decoded characters via keyboard_inject_char(). On hosts that expose both
 * an emulated PS/2 keyboard and a USB keyboard (e.g. VirtualBox), the same
 * physical keypress would otherwise be injected twice. Mirror the pointer
 * policy in drivers/input/input.c (USB preferred, PS/2 suppressed) by dropping
 * PS/2-decoded characters whenever a USB keyboard is present.
 */
extern int usb_hid_has_keyboard_device(void);

#define BUFFER_SIZE 128
#define RAW_SIZE 64
#define ISR_TRAIL 24

static char buffer[BUFFER_SIZE];
static volatile int head = 0;
static volatile int tail = 0;

/* Raw scancodes from IRQ1 — decode in thread context, never in the ISR. */
static volatile uint8_t raw_sc[RAW_SIZE];
static volatile int raw_head = 0;
static volatile int raw_tail = 0;

/* Visible to desktop HUD: IRQ1 progress survives a pump freeze. */
static volatile char isr_trail[ISR_TRAIL];
static volatile int isr_trail_len = 0;
static volatile uint8_t isr_last_status = 0;
static volatile uint8_t isr_last_scancode = 0;
static volatile uint32_t isr_count = 0;

static bool key_states[256];
static bool shift_pressed = false;
static bool ctrl_pressed = false;
static bool alt_pressed = false;
static bool caps_lock = false;
static uint8_t extended = 0;

static char scancode_to_ascii[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8',
    '9', '0', '-', '=', '\b', '\t', 'q', 'w', 'e', 'r',
    't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    '\'', '`', 0, '\\', 'z', 'x', 'c', 'v', 'b', 'n',
    'm', ',', '.', '/', 0, '*', 0, ' ', 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

static char scancode_to_ascii_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*',
    '(', ')', '_', '+', '\b', '\t', 'Q', 'W', 'E', 'R',
    'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    '"', '~', 0, '|', 'Z', 'X', 'C', 'V', 'B', 'N',
    'M', '<', '>', '?', 0, '*', 0, ' ', 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

static void isr_mark(char c) {
    int n = isr_trail_len;
    if (n < ISR_TRAIL) {
        isr_trail[n] = c;
        isr_trail_len = n + 1;
    } else {
        int i;
        for (i = 1; i < ISR_TRAIL; i++)
            isr_trail[i - 1] = isr_trail[i];
        isr_trail[ISR_TRAIL - 1] = c;
    }
}

static int kbd_wait_ibf_clear(void) {
    uint32_t t;
    for (t = 0; t < 100000u; t++)
        if ((inb(0x64) & 0x02) == 0) return 1;
    return 0;
}

static int kbd_wait_obf_set(void) {
    uint32_t t;
    for (t = 0; t < 100000u; t++)
        if (inb(0x64) & 0x01) return 1;
    return 0;
}

/* Send one byte to the keyboard DEVICE (port 0x60) and return its response
 * (0xFA = ACK). Used to program the scancode set directly. */
static uint8_t kbd_dev_command(uint8_t b) {
    kbd_wait_ibf_clear();
    outb(0x60, b);
    if (!kbd_wait_obf_set()) return 0x00;
    return inb(0x60);
}

static void keyboard_enable_irq(void) {
    uint32_t t;
    uint8_t cfg;
    uint8_t mask;

    kbd_wait_ibf_clear();
    outb(0x64, 0x20);
    if (kbd_wait_obf_set()) cfg = inb(0x60); else cfg = 0;
    cfg |= 0x01;               /* bit0: enable first-port (keyboard) IRQ1 */
    cfg &= (uint8_t)~0x10;     /* bit4=0: enable first-port clock (keyboard on) */
    /*
     * bit6 = controller scancode TRANSLATION (set 2 -> set 1). Real laptop
     * firmware leaves it on, so hardware handed us set-1 codes. VirtualBox does
     * NOT actually translate even when this bit is set (verified: "d" still
     * arrived as set-2 0x23 and decoded to set-1 "h"). So instead of relying on
     * the controller, we command the KEYBOARD to emit set 1 directly (below) and
     * keep translation OFF to avoid any double conversion on hosts that DO honor
     * it. That gives one consistent scancode set (1) everywhere.
     */
    cfg &= (uint8_t)~0x40;

    kbd_wait_ibf_clear();
    outb(0x64, 0x60);
    kbd_wait_ibf_clear();
    outb(0x60, cfg);

    /* Force the keyboard into scancode set 1 regardless of host/firmware default.
     * 0xF0 = "set scancode set" command, then 0x01 selects set 1. Each byte is
     * ACKed with 0xFA. VirtualBox defaults the keyboard to set 2, which is what
     * made every key decode wrong until now. */
    (void)kbd_dev_command(0xF0);
    (void)kbd_dev_command(0x01);

    /* Flush any bytes (command ACKs, firmware/VBox leftovers) from the output
     * buffer BEFORE unmasking, so we start with OBF clear and the IRQ1 line low. */
    for (t = 0; t < 64u; t++) {
        if ((inb(0x64) & 0x01) == 0) break;
        (void)inb(0x60);
    }

    mask = inb(0x21);
    outb(0x21, (uint8_t)(mask & (uint8_t)~(1u << 1)));
}

void keyboard_init(void) {
    head = 0;
    tail = 0;
    raw_head = 0;
    raw_tail = 0;
    isr_trail_len = 0;
    isr_count = 0;
    shift_pressed = false;
    ctrl_pressed = false;
    alt_pressed = false;
    extended = 0;
    for (int i = 0; i < 256; i++) key_states[i] = false;
    keyboard_enable_irq();
}

void keyboard_inject_char(char c) {
    if (!c) return;
    int next = (head + 1) % BUFFER_SIZE;
    if (next != tail) {
        buffer[head] = c;
        head = next;
    }
}

/*
 * IRQ1 body: drain 8042 only. No decode, no print, no 0xE9.
 * The asm wrapper issues EOI AFTER this returns (this drain drops the IRQ1
 * line first). This bounded loop must never stall so the wrapper can ack.
 *
 * The 8042 output buffer must be drained COMPLETELY, not one byte per IRQ.
 * VirtualBox queues several scancodes behind a single IRQ1 edge -- e.g. a
 * NumLock make+break (0x45,0xC5) at focus-in, or a make+break pair for a fast
 * keypress. The PS/2 IRQ is edge-triggered: if we read only one byte, OBF stays
 * set, the IRQ1 line never falls, no new edge is generated, and EVERY later
 * keypress is silently dropped -- the keyboard appears hard-frozen while the
 * rest of the system keeps running. Looping while OBF (status bit0) is set
 * guarantees OBF ends clear so the line drops and the next keypress re-arms the
 * edge. The guard bounds the loop so a runaway OBF (broken emulation) cannot
 * spin the ISR forever.
 */
void keyboard_interrupt_handler(void) {
    uint8_t status;
    uint8_t scancode;
    int next;
    int guard;

    isr_mark('1');
    isr_count++;

    for (guard = 0; guard < 64; guard++) {
        status = inb(0x64);
        isr_last_status = status;
        if (!(status & 0x01)) {           /* OBF clear: buffer fully drained */
            isr_mark('s');
            break;
        }
        scancode = inb(0x60);
        isr_last_scancode = scancode;
        isr_mark('r');
        /* Drop AUX/mouse bytes but keep draining so OBF still clears. */
        if (status & 0x20) {
            isr_mark('a');
            continue;
        }
        next = (raw_head + 1) % RAW_SIZE;
        if (next != raw_tail) {
            raw_sc[raw_head] = scancode;
            raw_head = next;
            isr_mark('q');
        } else {
            isr_mark('Q'); /* raw ring full */
        }
    }
    isr_mark('!');
}

static void keyboard_handle_scancode(uint8_t scancode) {
    if (scancode == 0xE0) {
        extended = 1;
        return;
    }

    bool released = (scancode & 0x80) != 0;
    uint8_t code = scancode & 0x7F;

    if (code < 128) {
        key_states[code] = !released;
    }

    if (code == 0x2A || code == 0x36) shift_pressed = !released;
    if (code == 0x1D) ctrl_pressed = !released;
    if (code == 0x38) alt_pressed = !released;
    if (code == 0x3A && !released) caps_lock = !caps_lock;

    if (!released) {
        char c = 0;

        if (extended) {
            switch (code) {
                case 0x48: c = KEY_UP; break;
                case 0x50: c = KEY_DOWN; break;
                case 0x4B: c = KEY_LEFT; break;
                case 0x4D: c = KEY_RIGHT; break;
            }
        } else if (code < 128) {
            if (code >= 0x3B && code <= 0x44) c = KEY_F1 + (code - 0x3B);
            else if (code == 0x57) c = KEY_F1 + 10;
            else if (code == 0x58) c = KEY_F1 + 11;
            else {
                if (caps_lock && scancode_to_ascii[code] >= 'a' &&
                    scancode_to_ascii[code] <= 'z') {
                    c = scancode_to_ascii_shift[code];
                    if (shift_pressed) c = scancode_to_ascii[code];
                } else {
                    c = shift_pressed ? scancode_to_ascii_shift[code]
                                      : scancode_to_ascii[code];
                }
            }
        }

        /* PS/2 is the low-priority source: suppress it while a USB keyboard is
         * bound so keystrokes are not double-injected. Modifier/key_states
         * tracking above still runs (harmless) to stay consistent if the USB
         * keyboard is later removed. */
        if (c && !usb_hid_has_keyboard_device()) keyboard_inject_char(c);
    }

    extended = 0;
}

/* Save IF and disable interrupts; restore exactly. Portable across the 32-bit
 * and 64-bit builds that share this driver. */
static inline unsigned long kbd_irq_save(void) {
    unsigned long f;
#ifdef __x86_64__
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
#else
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
#endif
    return f;
}
static inline void kbd_irq_restore(unsigned long f) {
#ifdef __x86_64__
    __asm__ volatile("pushq %0; popfq" :: "r"(f) : "memory", "cc");
#else
    __asm__ volatile("push %0; popf" :: "r"(f) : "memory", "cc");
#endif
}

/*
 * Poll the 8042 output buffer directly and push any waiting scancodes into the
 * same raw ring the ISR feeds. This is the fallback that makes the keyboard work
 * on VirtualBox, whose 8042 places a byte in the output buffer (OBF set) but
 * does NOT pulse the IRQ1 line for anything after the first byte -- verified by
 * serial: OBF=1 while the PIC IRR and ISR are both 0 and IRQ1 is unmasked, so no
 * interrupt ever arrives. On real hardware/QEMU the ISR normally drains the byte
 * first and this finds an empty buffer, so it is a harmless no-op there.
 *
 * Runs with interrupts disabled so it cannot race the ISR reading port 0x60
 * (whichever path reads a byte clears OBF; the other simply sees it empty).
 */
static void keyboard_drain_hw(void) {
    unsigned long flags = kbd_irq_save();
    int guard;
    for (guard = 0; guard < 64; guard++) {
        uint8_t status = inb(0x64);
        uint8_t sc;
        int next;
        if (!(status & 0x01)) break;      /* OBF clear: nothing waiting */
        sc = inb(0x60);
        isr_last_status = status;
        isr_last_scancode = sc;
        if (status & 0x20) continue;       /* AUX/mouse byte: drop, keep draining */
        next = (raw_head + 1) % RAW_SIZE;
        if (next != raw_tail) {
            raw_sc[raw_head] = sc;
            raw_head = next;
        }
    }
    kbd_irq_restore(flags);
}

void keyboard_poll(void) {
    keyboard_drain_hw();
    while (raw_tail != raw_head) {
        uint8_t sc = raw_sc[raw_tail];
        raw_tail = (raw_tail + 1) % RAW_SIZE;
        keyboard_handle_scancode(sc);
    }
}

int keyboard_has_char(void) {
    keyboard_poll();
    return head != tail;
}

char keyboard_read_char(void) {
    keyboard_poll();
    if (head == tail) return 0;
    char c = buffer[tail];
    tail = (tail + 1) % BUFFER_SIZE;
    return c;
}

bool keyboard_is_pressed(uint8_t scancode) {
    return key_states[scancode & 0x7F];
}

bool keyboard_is_shift_active(void) { return shift_pressed; }
bool keyboard_is_ctrl_active(void) { return ctrl_pressed; }
bool keyboard_is_alt_active(void) { return alt_pressed; }

uint8_t keyboard_live_status(void) {
    return inb(0x64);
}

uint8_t keyboard_pic_mask(void) {
    return inb(0x21);
}

/* Master 8259 In-Service Register (OCW3 read-ISR). Bit1 set = IRQ1 is stuck
 * in-service, which blocks all further IRQ1 delivery until EOI'd. */
uint8_t keyboard_pic_isr(void) {
    outb(0x20, 0x0B);
    return inb(0x20);
}

/* Master 8259 Interrupt Request Register (OCW3 read-IRR). Bit1 set = a keyboard
 * IRQ is pending/latched but not yet serviced. */
uint8_t keyboard_pic_irr(void) {
    outb(0x20, 0x0A);
    return inb(0x20);
}

void keyboard_debug_snapshot(char* out, int out_max,
                             uint8_t* last_status, uint8_t* last_sc,
                             uint32_t* irq_count) {
    int n = isr_trail_len;
    int i;
    if (last_status) *last_status = isr_last_status;
    if (last_sc) *last_sc = isr_last_scancode;
    if (irq_count) *irq_count = isr_count;
    if (!out || out_max <= 0) return;
    if (n > out_max - 1) n = out_max - 1;
    for (i = 0; i < n; i++) out[i] = isr_trail[i];
    out[n] = '\0';
}
