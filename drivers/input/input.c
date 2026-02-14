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
static input_device_t active_pointer = INPUT_DEVICE_PS2_MOUSE;

static uint32_t irq_save_disable(void) {
    uint32_t flags;
    __asm__ volatile(
        "pushf\n"
        "pop %0\n"
        "cli\n"
        : "=rm"(flags)
        :
        : "memory");
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
    active_pointer = usb_pointer_active ? INPUT_DEVICE_USB_MOUSE : INPUT_DEVICE_PS2_MOUSE;
    irq_restore(flags);
}

static int is_device_allowed(input_device_t device) {
    if (device == INPUT_DEVICE_PS2_MOUSE && usb_pointer_active) {
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
