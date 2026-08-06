#include "mouse.h"
#include "../io/io.h"
#include "../video/vga.h"
#include "../input/input.h"

#define MOUSE_PORT_DATA   0x60
#define MOUSE_PORT_CMD    0x64
#define MOUSE_PORT_STATUS 0x64

/* 8042 status register bits */
#define MOUSE_STATUS_OUTPUT_FULL 0x01  /* output buffer full (data to read) */
#define MOUSE_STATUS_INPUT_FULL  0x02  /* input buffer full (busy, can't write) */
#define MOUSE_STATUS_AUX_DATA    0x20  /* pending byte came from the aux device */

/* Aux-device responses */
#define MOUSE_ACK    0xFA
#define MOUSE_RESEND 0xFE

/* PIC mask ports */
#define PIC1_DATA 0x21
#define PIC2_DATA 0xA1

static uint8_t mouse_cycle = 0;
static uint8_t mouse_byte[3];
static int mouse_x = 40;
static int mouse_y = 12;
static uint8_t mouse_buttons = 0;

/*
 * Wait until the controller input buffer is empty so it is safe to write a
 * command/data byte. Bounded so a missing/locked-up controller never hangs.
 */
static int mouse_wait_input(void) {
    uint32_t timeout = 100000;
    while (timeout--) {
        if ((inb(MOUSE_PORT_STATUS) & MOUSE_STATUS_INPUT_FULL) == 0) {
            return 0;
        }
    }
    return -1;
}

/* Wait until the output buffer is full (a byte is available to read). Bounded. */
static int mouse_wait_output(void) {
    uint32_t timeout = 100000;
    while (timeout--) {
        if (inb(MOUSE_PORT_STATUS) & MOUSE_STATUS_OUTPUT_FULL) {
            return 0;
        }
    }
    return -1;
}

/* Read one byte from the controller data port, or -1 on timeout. */
static int mouse_read_data(void) {
    if (mouse_wait_output() != 0) {
        return -1;
    }
    return (int)inb(MOUSE_PORT_DATA);
}

/* Drain any stale bytes left in the controller output buffer. */
static void mouse_flush(void) {
    uint32_t guard = 64;
    while (guard-- && (inb(MOUSE_PORT_STATUS) & MOUSE_STATUS_OUTPUT_FULL)) {
        (void)inb(MOUSE_PORT_DATA);
    }
}

/*
 * Send a command byte to the aux (mouse) device and wait for an ACK (0xFA),
 * honoring resend (0xFE) requests. Returns 0 on ACK, -1 on timeout/failure.
 */
static int mouse_command(uint8_t cmd) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (mouse_wait_input() != 0) return -1;
        outb(MOUSE_PORT_CMD, 0xD4);            /* address next byte to the aux device */
        if (mouse_wait_input() != 0) return -1;
        outb(MOUSE_PORT_DATA, cmd);

        int resp = mouse_read_data();
        if (resp == MOUSE_ACK) return 0;
        if (resp == MOUSE_RESEND) continue;    /* device asked us to retry */
        if (resp < 0) return -1;               /* timeout: no mouse present */
        mouse_flush();                         /* unexpected byte; resync and retry */
    }
    return -1;
}

/*
 * Unmask the PS/2 mouse interrupt at the PIC.
 *
 * pic_remap() in the kernel restores the BIOS-provided masks, which on real
 * hardware frequently leave IRQ12 (and sometimes the slave cascade IRQ2)
 * masked. The controller-level "enable IRQ" bit in the 8042 config byte is not
 * enough on its own -- without clearing these PIC mask bits the mouse IRQ never
 * reaches the CPU. We only touch the cascade and IRQ12 bits, preserving every
 * other BIOS-configured mask so we don't accidentally expose unhandled lines.
 */
static void mouse_enable_irq_line(void) {
    uint8_t master = inb(PIC1_DATA);
    master = (uint8_t)(master & ~(1u << 2));   /* IRQ2: cascade to slave PIC */
    outb(PIC1_DATA, master);

    uint8_t slave = inb(PIC2_DATA);
    slave = (uint8_t)(slave & ~(1u << 4));      /* IRQ12: PS/2 mouse */
    outb(PIC2_DATA, slave);
}

void mouse_init(void) {
    int status;

    mouse_flush();

    /* Enable the auxiliary (mouse) device. */
    if (mouse_wait_input() == 0) {
        outb(MOUSE_PORT_CMD, 0xA8);
    }

    /*
     * Read the controller "compaq" config byte, enable IRQ12 (bit 1) and make
     * sure the mouse clock is enabled (bit 5 clear), then write it back.
     */
    if (mouse_wait_input() == 0) {
        outb(MOUSE_PORT_CMD, 0x20);
    }
    status = mouse_read_data();
    if (status < 0) status = 0;
    status |= 0x02;        /* enable mouse interrupt (IRQ12) */
    status &= ~0x20;       /* clear "disable mouse clock" */
    if (mouse_wait_input() == 0) {
        outb(MOUSE_PORT_CMD, 0x60);
    }
    if (mouse_wait_input() == 0) {
        outb(MOUSE_PORT_DATA, (uint8_t)status);
    }

    /* Restore sane defaults (100 samples/s, 4 cnt/mm, 1:1 scaling). */
    if (mouse_command(0xF6) != 0) {
        /* No AUX device (I2C-only Advanced mode) — leave IRQ12 masked. */
        return;
    }
    /* Enable data reporting (streaming mode). */
    if (mouse_command(0xF4) != 0) {
        return;
    }

    /* Discard any packets queued before IRQ12 is live so we start aligned. */
    mouse_flush();
    mouse_cycle = 0;

    /* Make sure the mouse IRQ can actually reach the CPU. */
    mouse_enable_irq_line();

    /*
     * A reasonable text-mode default; the VESA desktop overrides this with the
     * real framebuffer resolution via input_set_bounds() once it starts.
     */
    input_set_bounds(80, 25);
}

void mouse_handler_main(void) {
    uint8_t status = inb(MOUSE_PORT_STATUS);

    /* Only consume bytes the controller tagged as coming from the aux device. */
    if (!(status & MOUSE_STATUS_OUTPUT_FULL) || !(status & MOUSE_STATUS_AUX_DATA)) {
        return;
    }

    uint8_t data = inb(MOUSE_PORT_DATA);

    switch (mouse_cycle) {
        case 0:
            /*
             * Bit 3 of the first packet byte is always 1. If it is clear we are
             * out of sync (a byte was dropped); stay on byte 0 to resynchronize.
             */
            if ((data & 0x08) == 0) {
                mouse_cycle = 0;
                break;
            }
            mouse_byte[0] = data;
            mouse_cycle = 1;
            break;
        case 1:
            mouse_byte[1] = data;
            mouse_cycle = 2;
            break;
        case 2: {
            mouse_byte[2] = data;
            mouse_cycle = 0;

            uint8_t b0 = mouse_byte[0];
            mouse_buttons = (uint8_t)(b0 & 0x07);

            int dx = 0;
            int dy = 0;
            /*
             * If either overflow bit is set the movement bytes are meaningless,
             * so drop the motion but still report button state so clicks are
             * never lost. Otherwise sign-extend the 8-bit deltas; PS/2 reports
             * +Y as up, while screen coordinates grow downward, so invert Y.
             */
            if ((b0 & 0xC0) == 0) {
                dx = (int)(int8_t)mouse_byte[1];
                dy = -(int)(int8_t)mouse_byte[2];
            }

            input_report_pointer_delta(
                INPUT_DEVICE_PS2_MOUSE,
                dx,
                dy,
                mouse_buttons,
                0);

            mouse_x = input_get_pointer_x();
            mouse_y = input_get_pointer_y();
            mouse_buttons = input_get_pointer_buttons();
            break;
        }
    }
}

int mouse_get_x(void) {
    return mouse_x;
}

int mouse_get_y(void) {
    return mouse_y;
}

uint8_t mouse_get_buttons(void) {
    return mouse_buttons;
}
