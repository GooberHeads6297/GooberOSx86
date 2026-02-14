#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

typedef enum {
    INPUT_EVENT_POINTER_MOVE = 0,
    INPUT_EVENT_BUTTON_DOWN,
    INPUT_EVENT_BUTTON_UP,
    INPUT_EVENT_SCROLL
} input_event_type_t;

typedef enum {
    INPUT_DEVICE_NONE = 0,
    INPUT_DEVICE_PS2_MOUSE,
    INPUT_DEVICE_USB_MOUSE,
    INPUT_DEVICE_USB_TOUCHPAD
} input_device_t;

typedef enum {
    INPUT_BUTTON_LEFT = 0,
    INPUT_BUTTON_RIGHT,
    INPUT_BUTTON_MIDDLE
} input_button_t;

typedef struct {
    input_event_type_t type;
    input_device_t device;
    int16_t x;
    int16_t y;
    int16_t dx;
    int16_t dy;
    int8_t wheel;
    uint8_t button;
    uint8_t buttons;
} input_event_t;

void input_init(void);
void input_set_bounds(int width, int height);
int input_poll_event(input_event_t* event);
void input_set_usb_pointer_active(int active);
void input_report_pointer_delta(input_device_t device, int dx, int dy, uint8_t buttons, int8_t wheel);

int input_get_pointer_x(void);
int input_get_pointer_y(void);
uint8_t input_get_pointer_buttons(void);
input_device_t input_get_active_pointer(void);

#endif
