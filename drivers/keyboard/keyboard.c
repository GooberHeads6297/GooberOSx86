#include "keyboard.h"
#include "../io/io.h"
#include "../pci/pci.h"
#include <stddef.h>
#include <stdbool.h>

/*
 * Source arbitration: when a USB HID boot keyboard is bound it delivers its
 * own decoded characters via keyboard_inject_char(). On hosts that expose both
 * an emulated PS/2 keyboard and a USB keyboard (e.g. VirtualBox), the same
 * physical keypress would otherwise be injected twice.
 */
extern int usb_hid_has_keyboard_device(void);
extern int usb_hid_keyboard_active(void);

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

/*
 * Scancode path (sticky after init / first decisive observation):
 *   '1' = set-1 / AT-translated (real laptop EC)
 *   '2' = software set-2->set-1 (VirtualBox / hypervisor guests)
 * Never oscillates back from '2' to '1' -- that was the "wrong then correct"
 * flipping the user saw when AT was selected first and 0xF0 later upgraded us.
 */
static char g_scancode_mode = '1';
static int g_set2_break = 0; /* next set-2 byte is a break code */
static int g_vm_guest = 0;   /* hypervisor / VBox guest: prefer VM kbd policy */

/*
 * Set-2 make code -> set-1 make code. Unmapped entries stay 0.
 * F7's set-2 code is 0x83 (needs the full 256-entry span).
 */
static const uint8_t set2_to_set1[256] = {
    [0x76] = 0x01, /* Esc */
    [0x16] = 0x02, [0x1E] = 0x03, [0x26] = 0x04, [0x25] = 0x05, [0x2E] = 0x06,
    [0x36] = 0x07, [0x3D] = 0x08, [0x3E] = 0x09, [0x46] = 0x0A, [0x45] = 0x0B,
    [0x4E] = 0x0C, [0x55] = 0x0D, [0x66] = 0x0E, [0x0D] = 0x0F,
    [0x15] = 0x10, [0x1D] = 0x11, [0x24] = 0x12, [0x2D] = 0x13, [0x2C] = 0x14,
    [0x35] = 0x15, [0x3C] = 0x16, [0x43] = 0x17, [0x44] = 0x18, [0x4D] = 0x19,
    [0x54] = 0x1A, [0x5B] = 0x1B, [0x5A] = 0x1C, [0x14] = 0x1D,
    [0x1C] = 0x1E, [0x1B] = 0x1F, [0x23] = 0x20, [0x2B] = 0x21, [0x34] = 0x22,
    [0x33] = 0x23, [0x3B] = 0x24, [0x42] = 0x25, [0x4B] = 0x26, [0x4C] = 0x27,
    [0x52] = 0x28, [0x0E] = 0x29, [0x12] = 0x2A, [0x5D] = 0x2B,
    [0x1A] = 0x2C, [0x22] = 0x2D, [0x21] = 0x2E, [0x2A] = 0x2F, [0x32] = 0x30,
    [0x31] = 0x31, [0x3A] = 0x32, [0x41] = 0x33, [0x49] = 0x34, [0x4A] = 0x35,
    [0x59] = 0x36, [0x29] = 0x39, [0x58] = 0x3A,
    [0x05] = 0x3B, [0x06] = 0x3C, [0x04] = 0x3D, [0x0C] = 0x3E, [0x03] = 0x3F,
    [0x0B] = 0x40, [0x83] = 0x41, [0x0A] = 0x42, [0x01] = 0x43, [0x09] = 0x44,
    [0x78] = 0x57, [0x07] = 0x58,
    /* Extended arrows (after 0xE0): set-2 make matches set-1 make for these */
    [0x75] = 0x48, [0x72] = 0x50, [0x6B] = 0x4B, [0x74] = 0x4D,
};

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

/* Flush the 8042 output buffer (keyboard + AUX). Bounded so a stuck OBF cannot
 * hang boot. */
static void kbd_flush_obf(void) {
    uint32_t t;
    for (t = 0; t < 64u; t++) {
        if ((inb(0x64) & 0x01) == 0) break;
        (void)inb(0x60);
    }
}

/* Send one byte to the keyboard DEVICE (port 0x60) and return its response
 * (0xFA = ACK). Used to program the scancode set / re-enable scanning. */
static uint8_t kbd_dev_command(uint8_t b) {
    kbd_wait_ibf_clear();
    outb(0x60, b);
    if (!kbd_wait_obf_set()) return 0x00;
    return inb(0x60);
}

static int kbd_dev_command_ack(uint8_t b) {
    return kbd_dev_command(b) == 0xFA;
}

static void kbd_write_controller_cfg(uint8_t cfg) {
    kbd_wait_ibf_clear();
    outb(0x64, 0x60);
    kbd_wait_ibf_clear();
    outb(0x60, cfg);
}

static void kbd_cpuid(uint32_t leaf, uint32_t subleaf,
                      uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    uint32_t eax, ebx, ecx, edx;
    /* Matching constraints: leaf in EAX, subleaf in ECX; all four outs. */
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "0"(leaf), "2"(subleaf));
    if (a) *a = eax;
    if (b) *b = ebx;
    if (c) *c = ecx;
    if (d) *d = edx;
}

/* True when CPUID.1.ECX[31] (Hypervisor Present) is set. */
static int kbd_hypervisor_present(void) {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    kbd_cpuid(1u, 0u, &eax, &ebx, &ecx, &edx);
    return (ecx & (1u << 31)) != 0;
}

/*
 * VirtualBox vendor string at leaf 0x40000000 ("VBoxVBoxVBox"). Checked
 * WITHOUT requiring the hypervisor-present bit -- with Paravirtualization
 * Interface = None that bit is often clear while this leaf may still identify
 * VBox (PCI 0x80EE below covers the empty-leaf case).
 */
static int kbd_cpuid_is_vbox(void) {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    kbd_cpuid(0x40000000u, 0u, &eax, &ebx, &ecx, &edx);
    return (ebx == 0x786F4256u) &&
           (ecx == 0x786F4256u) &&
           (edx == 0x786F4256u);
}

/* InnoTek/Oracle VirtualBox virtual PCI devices use vendor 0x80EE. */
static int kbd_pci_is_vbox(void) {
    uint8_t slot, func;
    for (slot = 0; slot < 32; slot++) {
        for (func = 0; func < 8; func++) {
            uint32_t id = pci_read_config_dword(0, slot, func, 0);
            uint16_t vendor = (uint16_t)(id & 0xFFFFu);
            if (vendor == 0xFFFFu) {
                if (func == 0) break;
                continue;
            }
            if (vendor == 0x80EEu) return 1;
            if (func == 0 && (pci_read_config_byte(0, slot, 0, 0x0E) & 0x80) == 0)
                break;
        }
    }
    return 0;
}

static int kbd_detect_vm_guest(void) {
    if (kbd_hypervisor_present()) return 1;
    if (kbd_cpuid_is_vbox()) return 1;
    if (kbd_pci_is_vbox()) return 1;
    return 0;
}

/* gooberos.kbd=set1|set2 -> VM path; gooberos.kbd=at -> laptop path */
static int kbd_cmdline_force_vm(void) {
    extern const char* kernel_boot_cmdline(void);
    const char* s = kernel_boot_cmdline();
    if (!s) return 0;
    for (; *s; s++) {
        if (s[0]=='g'&&s[1]=='o'&&s[2]=='o'&&s[3]=='b'&&s[4]=='e'&&
            s[5]=='r'&&s[6]=='o'&&s[7]=='s'&&s[8]=='.'&&s[9]=='k'&&
            s[10]=='b'&&s[11]=='d'&&s[12]=='=') {
            if (s[13]=='s'&&s[14]=='e'&&s[15]=='t'&&(s[16]=='1'||s[16]=='2'))
                return 1;
        }
    }
    return 0;
}

static int kbd_cmdline_force_at(void) {
    extern const char* kernel_boot_cmdline(void);
    const char* s = kernel_boot_cmdline();
    if (!s) return 0;
    for (; *s; s++) {
        if (s[0]=='g'&&s[1]=='o'&&s[2]=='o'&&s[3]=='b'&&s[4]=='e'&&
            s[5]=='r'&&s[6]=='o'&&s[7]=='s'&&s[8]=='.'&&s[9]=='k'&&
            s[10]=='b'&&s[11]=='d'&&s[12]=='='&&
            s[13]=='a'&&s[14]=='t')
            return 1;
    }
    return 0;
}

static void kbd_apply_at_translated(uint8_t* cfg) {
    *cfg |= 0x40; /* translation ON */
    kbd_write_controller_cfg(*cfg);
    kbd_flush_obf();
    (void)kbd_dev_command_ack(0xF4);
    kbd_flush_obf();
}

static void keyboard_enable_irq(void) {
    uint8_t cfg;
    uint8_t mask;
    int use_vm_set2;

    kbd_wait_ibf_clear();
    outb(0x64, 0x20);
    if (kbd_wait_obf_set()) cfg = inb(0x60); else cfg = 0;
    cfg |= 0x01;               /* bit0: enable first-port (keyboard) IRQ1 */
    cfg &= (uint8_t)~0x10;     /* bit4=0: enable first-port clock (keyboard on) */

    /*
     * Sticky decision for the whole boot (no wrong<->correct oscillation):
     *   gooberos.kbd=at          -> classic AT (laptop)
     *   gooberos.kbd=set1|set2   -> VM software set-2
     *   VBox PCI 0x80EE / CPUID VBox / hypervisor bit -> VM software set-2
     *   else                     -> classic AT (Acer EC, etc.)
     */
    if (kbd_cmdline_force_at())
        use_vm_set2 = 0;
    else if (kbd_cmdline_force_vm() || kbd_detect_vm_guest())
        use_vm_set2 = 1;
    else
        use_vm_set2 = 0;

    g_vm_guest = use_vm_set2;

    if (use_vm_set2) {
        cfg &= (uint8_t)~0x40; /* translation OFF -- raw set-2 into software */
        kbd_write_controller_cfg(cfg);
        kbd_flush_obf();
        (void)kbd_dev_command_ack(0xF4);
        kbd_flush_obf();
        g_scancode_mode = '2';
        g_set2_break = 0;
    } else {
        kbd_apply_at_translated(&cfg);
        g_scancode_mode = '1';
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
    g_set2_break = 0;
    /* g_scancode_mode is set inside keyboard_enable_irq(). */
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
    /*
     * Set-2 break prefix. One-way upgrade only: if we wrongly started in AT
     * mode on a VM, the first 0xF0 locks us into software set-2 for the rest
     * of the boot. Never switch back to '1' (that oscillation was the bug).
     */
    if (scancode == 0xF0) {
        g_scancode_mode = '2';
        g_set2_break = 1;
        return;
    }

    if (g_scancode_mode == '2') {
        uint8_t s1;
        if (scancode == 0xE0) {
            extended = 1;
            return;
        }
        s1 = set2_to_set1[scancode];
        if (!s1) {
            g_set2_break = 0;
            extended = 0;
            return;
        }
        if (g_set2_break) {
            scancode = (uint8_t)(s1 | 0x80u);
            g_set2_break = 0;
        } else {
            scancode = s1;
        }
    }

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

        /*
         * PS/2 suppress policy:
         *   VM guest: if a USB boot keyboard is enumerated, drop PS/2 entirely
         *     (avoids correct-USB + wrong-PS/2 interleaving that looks like
         *     mapping "switching").
         *   Real hardware: only suppress once USB has delivered a report, so a
         *     silent USB HID bind cannot mute the laptop EC keyboard.
         */
        if (c) {
            int suppress = 0;
            if (g_vm_guest && usb_hid_has_keyboard_device())
                suppress = 1;
            else if (!g_vm_guest && usb_hid_keyboard_active())
                suppress = 1;
            if (!suppress) keyboard_inject_char(c);
        }
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

char keyboard_scancode_mode(void) {
    return g_scancode_mode;
}

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
