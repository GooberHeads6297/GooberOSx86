#include "mouse.h"
#include "../io/io.h"
#include "../video/vga.h"

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

            mouse_buttons = mouse_byte[0] & 0x07;
            
            // Update position
            mouse_x += mouse_byte[1];
            mouse_y -= mouse_byte[2]; // Y is inverted in PS/2

            // Clamp to screen
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_x >= 80) mouse_x = 79;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_y >= 25) mouse_y = 24;
            
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
