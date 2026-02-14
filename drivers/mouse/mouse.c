#include "mouse.h"
#include "../io/io.h"
#include "../video/vga.h"
#include "../input/input.h"

#define MOUSE_PORT_DATA 0x60
#define MOUSE_PORT_CMD  0x64
#define MOUSE_PORT_STATUS 0x64

static uint8_t mouse_cycle = 0;
static int8_t mouse_byte[3];
static int mouse_x = 40;
static int mouse_y = 12;
static uint8_t mouse_buttons = 0;

// Wait until the keyboard controller is ready to send data
static void mouse_wait(uint8_t type) {
    uint32_t time_out = 100000;
    if (type == 0) {
        while (time_out--) {
            if ((inb(MOUSE_PORT_STATUS) & 1) == 1) {
                return;
            }
        }
        return;
    } else {
        while (time_out--) {
            if ((inb(MOUSE_PORT_STATUS) & 2) == 0) {
                return;
            }
        }
        return;
    }
}

static void mouse_write(uint8_t write) {
    mouse_wait(1);
    outb(MOUSE_PORT_CMD, 0xD4);
    mouse_wait(1);
    outb(MOUSE_PORT_DATA, write);
}

static uint8_t mouse_read() {
    mouse_wait(0);
    return inb(MOUSE_PORT_DATA);
}

void mouse_init(void) {
    uint8_t status;

    // Enable auxiliary device
    mouse_wait(1);
    outb(MOUSE_PORT_CMD, 0xA8);

    // Enable interrupts
    mouse_wait(1);
    outb(MOUSE_PORT_CMD, 0x20);
    mouse_wait(0);
    status = (inb(MOUSE_PORT_DATA) | 2);
    mouse_wait(1);
    outb(MOUSE_PORT_CMD, 0x60);
    mouse_wait(1);
    outb(MOUSE_PORT_DATA, status);

    // Set defaults
    mouse_write(0xF6);
    mouse_read();  // Acknowledge

    // Enable streaming
    mouse_write(0xF4);
    mouse_read();  // Acknowledge

    input_set_bounds(80, 25);
}

void mouse_handler_main(void) {
    uint8_t status = inb(MOUSE_PORT_STATUS);
    if (!(status & 0x20)) {
        return; // Not mouse data
    }

    uint8_t data = inb(MOUSE_PORT_DATA);

    switch (mouse_cycle) {
        case 0:
            if ((data & 0x08) == 0) break; // Alignment check
            mouse_byte[0] = data;
            mouse_cycle++;
            break;
        case 1:
            mouse_byte[1] = data;
            mouse_cycle++;
            break;
        case 2:
            mouse_byte[2] = data;
            mouse_cycle = 0;
            mouse_buttons = (uint8_t)(mouse_byte[0] & 0x07);
            input_report_pointer_delta(
                INPUT_DEVICE_PS2_MOUSE,
                mouse_byte[1],
                (int)(-mouse_byte[2]),
                mouse_buttons,
                0);
            mouse_x = input_get_pointer_x();
            mouse_y = input_get_pointer_y();
            mouse_buttons = input_get_pointer_buttons();
            break;
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
