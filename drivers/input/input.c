#include "input.h"

#define INPUT_QUEUE_SIZE 128

static volatile uint32_t queue_head = 0;
static volatile uint32_t queue_tail = 0;
static input_event_t queue[INPUT_QUEUE_SIZE];

static int pointer_x = 40;
static int pointer_y = 12;
static uint8_t pointer_buttons = 0;
static int max_width = 80;
static int max_height = 25;
static int usb_pointer_active = 0;
static int i2c_touchpad_active = 0;
static input_device_t active_pointer = INPUT_DEVICE_PS2_MOUSE;

static uint32_t irq_save_disable(void) {
    uint32_t flags;
#ifdef __x86_64__
    /* In long mode the flags register is 64-bit (RFLAGS), but we only ever
     * inspect bit 9 (IF) so keeping the storage at 32 bits is safe and
     * avoids touching every caller. pushfq/popfq are required because the
     * 32-bit pushf/popf forms are illegal in 64-bit mode. */
    uint64_t flags64;
    __asm__ volatile(
        "pushfq\n"
        "popq %0\n"
        "cli\n"
        : "=rm"(flags64)
        :
        : "memory");
    flags = (uint32_t)flags64;
#else
    __asm__ volatile(
        "pushf\n"
        "pop %0\n"
        "cli\n"
        : "=rm"(flags)
        :
        : "memory");
#endif
    return flags;
}

static void irq_restore(uint32_t flags) {
    if (flags & (1U << 9)) {
        __asm__ volatile("sti" : : : "memory");
    }
}

static int queue_push(const input_event_t* event) {
    uint32_t next = (queue_head + 1) % INPUT_QUEUE_SIZE;
    if (next == queue_tail) {
        return 0;
    }
    queue[queue_head] = *event;
    queue_head = next;
    return 1;
}

void input_init(void) {
    uint32_t flags = irq_save_disable();
    queue_head = 0;
    queue_tail = 0;
    pointer_x = 40;
    pointer_y = 12;
    pointer_buttons = 0;
    max_width = 80;
    max_height = 25;
    usb_pointer_active = 0;
    i2c_touchpad_active = 0;
    active_pointer = INPUT_DEVICE_PS2_MOUSE;
    irq_restore(flags);
}

void input_set_bounds(int width, int height) {
    uint32_t flags = irq_save_disable();
    if (width > 0) {
        max_width = width;
    }
    if (height > 0) {
        max_height = height;
    }
    if (pointer_x >= max_width) pointer_x = max_width - 1;
    if (pointer_y >= max_height) pointer_y = max_height - 1;
    if (pointer_x < 0) pointer_x = 0;
    if (pointer_y < 0) pointer_y = 0;
    irq_restore(flags);
}

void input_set_usb_pointer_active(int active) {
    uint32_t flags = irq_save_disable();
    usb_pointer_active = active ? 1 : 0;
    if (usb_pointer_active) {
        active_pointer = INPUT_DEVICE_USB_MOUSE;
    } else if (i2c_touchpad_active) {
        active_pointer = INPUT_DEVICE_I2C_TOUCHPAD;
    } else {
        active_pointer = INPUT_DEVICE_PS2_MOUSE;
    }
    irq_restore(flags);
}

void input_set_i2c_touchpad_active(int active) {
    uint32_t flags = irq_save_disable();
    i2c_touchpad_active = active ? 1 : 0;
    if (usb_pointer_active) {
        active_pointer = INPUT_DEVICE_USB_MOUSE;
    } else if (i2c_touchpad_active) {
        active_pointer = INPUT_DEVICE_I2C_TOUCHPAD;
    } else {
        active_pointer = INPUT_DEVICE_PS2_MOUSE;
    }
    irq_restore(flags);
}

static int is_device_allowed(input_device_t device) {
    if (usb_pointer_active &&
        device != INPUT_DEVICE_USB_MOUSE &&
        device != INPUT_DEVICE_USB_TOUCHPAD &&
        device != INPUT_DEVICE_I2C_TOUCHPAD) {
        return 0;
    }
    if (i2c_touchpad_active && device == INPUT_DEVICE_PS2_MOUSE) {
        return 0;
    }
    return 1;
}

void input_report_pointer_delta(input_device_t device, int dx, int dy, uint8_t buttons, int8_t wheel) {
    input_event_t event;
    uint8_t old_buttons;
    uint32_t flags = irq_save_disable();

    if (!is_device_allowed(device)) {
        irq_restore(flags);
        return;
    }

    if (device == INPUT_DEVICE_USB_MOUSE || device == INPUT_DEVICE_USB_TOUCHPAD) {
        usb_pointer_active = 1;
        active_pointer = device;
    } else if (device == INPUT_DEVICE_I2C_TOUCHPAD) {
        i2c_touchpad_active = 1;
        if (!usb_pointer_active) active_pointer = device;
    } else {
        active_pointer = INPUT_DEVICE_PS2_MOUSE;
    }

    old_buttons = pointer_buttons;
    pointer_x += dx;
    pointer_y += dy;

    if (pointer_x < 0) pointer_x = 0;
    if (pointer_y < 0) pointer_y = 0;
    if (pointer_x >= max_width) pointer_x = max_width - 1;
    if (pointer_y >= max_height) pointer_y = max_height - 1;

    pointer_buttons = buttons & 0x07;

    if (dx != 0 || dy != 0) {
        event.type = INPUT_EVENT_POINTER_MOVE;
        event.device = device;
        event.x = (int16_t)pointer_x;
        event.y = (int16_t)pointer_y;
        event.dx = (int16_t)dx;
        event.dy = (int16_t)dy;
        event.wheel = 0;
        event.button = 0;
        event.buttons = pointer_buttons;
        queue_push(&event);
    }

    for (uint8_t bit = 0; bit < 3; bit++) {
        uint8_t mask = (uint8_t)(1U << bit);
        uint8_t old_down = old_buttons & mask;
        uint8_t new_down = pointer_buttons & mask;
        if (old_down == new_down) continue;

        event.type = new_down ? INPUT_EVENT_BUTTON_DOWN : INPUT_EVENT_BUTTON_UP;
        event.device = device;
        event.x = (int16_t)pointer_x;
        event.y = (int16_t)pointer_y;
        event.dx = 0;
        event.dy = 0;
        event.wheel = 0;
        event.button = bit;
        event.buttons = pointer_buttons;
        queue_push(&event);
    }

    if (wheel != 0) {
        event.type = INPUT_EVENT_SCROLL;
        event.device = device;
        event.x = (int16_t)pointer_x;
        event.y = (int16_t)pointer_y;
        event.dx = 0;
        event.dy = 0;
        event.wheel = wheel;
        event.button = 0;
        event.buttons = pointer_buttons;
        queue_push(&event);
    }

    irq_restore(flags);
}

void input_report_pointer_absolute(input_device_t device, int x, int y, uint8_t buttons, int8_t wheel) {
    input_event_t event;
    uint8_t old_buttons;
    uint32_t flags = irq_save_disable();

    if (!is_device_allowed(device)) {
        irq_restore(flags);
        return;
    }

    if (device == INPUT_DEVICE_USB_MOUSE || device == INPUT_DEVICE_USB_TOUCHPAD) {
        usb_pointer_active = 1;
        active_pointer = device;
    } else if (device == INPUT_DEVICE_I2C_TOUCHPAD) {
        i2c_touchpad_active = 1;
        if (!usb_pointer_active) active_pointer = device;
    } else {
        active_pointer = INPUT_DEVICE_PS2_MOUSE;
    }

    old_buttons = pointer_buttons;

    int old_x = pointer_x;
    int old_y = pointer_y;
    pointer_x = x;
    pointer_y = y;

    if (pointer_x < 0) pointer_x = 0;
    if (pointer_y < 0) pointer_y = 0;
    if (pointer_x >= max_width) pointer_x = max_width - 1;
    if (pointer_y >= max_height) pointer_y = max_height - 1;

    pointer_buttons = buttons & 0x07;

    int dx = pointer_x - old_x;
    int dy = pointer_y - old_y;
    if (dx != 0 || dy != 0) {
        event.type = INPUT_EVENT_POINTER_MOVE;
        event.device = device;
        event.x = (int16_t)pointer_x;
        event.y = (int16_t)pointer_y;
        event.dx = (int16_t)dx;
        event.dy = (int16_t)dy;
        event.wheel = 0;
        event.button = 0;
        event.buttons = pointer_buttons;
        queue_push(&event);
    }

    for (uint8_t bit = 0; bit < 3; bit++) {
        uint8_t mask = (uint8_t)(1U << bit);
        uint8_t old_down = old_buttons & mask;
        uint8_t new_down = pointer_buttons & mask;
        if (old_down == new_down) continue;

        event.type = new_down ? INPUT_EVENT_BUTTON_DOWN : INPUT_EVENT_BUTTON_UP;
        event.device = device;
        event.x = (int16_t)pointer_x;
        event.y = (int16_t)pointer_y;
        event.dx = 0;
        event.dy = 0;
        event.wheel = 0;
        event.button = bit;
        event.buttons = pointer_buttons;
        queue_push(&event);
    }

    if (wheel != 0) {
        event.type = INPUT_EVENT_SCROLL;
        event.device = device;
        event.x = (int16_t)pointer_x;
        event.y = (int16_t)pointer_y;
        event.dx = 0;
        event.dy = 0;
        event.wheel = wheel;
        event.button = 0;
        event.buttons = pointer_buttons;
        queue_push(&event);
    }

    irq_restore(flags);
}

void input_report_pointer_absolute_scaled(input_device_t device, int x, int y,
                                          int raw_max_x, int raw_max_y,
                                          uint8_t buttons, int8_t wheel) {
    if (raw_max_x <= 0) raw_max_x = 2047;
    if (raw_max_y <= 0) raw_max_y = 2047;

    int scaled_x = (x * (max_width > 1 ? max_width - 1 : 1)) / raw_max_x;
    int scaled_y = (y * (max_height > 1 ? max_height - 1 : 1)) / raw_max_y;
    input_report_pointer_absolute(device, scaled_x, scaled_y, buttons, wheel);
}

int input_poll_event(input_event_t* event) {
    int has_event = 0;
    uint32_t flags = irq_save_disable();
    if (queue_head != queue_tail) {
        if (event) {
            *event = queue[queue_tail];
        }
        queue_tail = (queue_tail + 1) % INPUT_QUEUE_SIZE;
        has_event = 1;
    }
    irq_restore(flags);
    return has_event;
}

int input_get_pointer_x(void) {
    return pointer_x;
}

int input_get_pointer_y(void) {
    return pointer_y;
}

uint8_t input_get_pointer_buttons(void) {
    return pointer_buttons;
}

input_device_t input_get_active_pointer(void) {
    return active_pointer;
}

void input_remove_device(input_device_t device) {
    uint32_t flags = irq_save_disable();

    /*
     * Compact the queue in place: copy the events we keep to a fresh tail,
     * dropping any whose .device matches. This runs with IRQs disabled but
     * the queue is bounded at 128 entries, so the linear scan is cheap.
     */
    uint32_t read = queue_tail;
    uint32_t write = queue_tail;
    while (read != queue_head) {
        input_event_t ev = queue[read];
        read = (read + 1) % INPUT_QUEUE_SIZE;
        if (ev.device == device) continue;
        queue[write] = ev;
        write = (write + 1) % INPUT_QUEUE_SIZE;
    }
    queue_head = write;

    /* If the removed device was the active pointer, revert to PS/2 +
     * disarm USB-pointer-active so PS/2 events flow again. */
    if (device == INPUT_DEVICE_USB_MOUSE || device == INPUT_DEVICE_USB_TOUCHPAD) {
        usb_pointer_active = 0;
    }
    if (device == INPUT_DEVICE_I2C_TOUCHPAD) {
        i2c_touchpad_active = 0;
    }
    if (active_pointer == device) {
        if (usb_pointer_active) active_pointer = INPUT_DEVICE_USB_MOUSE;
        else if (i2c_touchpad_active) active_pointer = INPUT_DEVICE_I2C_TOUCHPAD;
        else active_pointer = INPUT_DEVICE_PS2_MOUSE;
        pointer_buttons = 0;
    }

    irq_restore(flags);
}
