#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>
#include <stddef.h>

#define MAX_WINDOWS 10
#define SCREEN_WIDTH 80
#define SCREEN_HEIGHT 25

struct Window;
typedef void (*gui_window_tick_fn)(struct Window* win, uint32_t ticks);
typedef void (*gui_window_key_fn)(struct Window* win, char key);

typedef enum {
    GUI_APP_NONE = 0,
    GUI_APP_WELCOME,
    GUI_APP_SYSTEM,
    GUI_APP_BOUNCE,
    GUI_APP_SHELL,
    GUI_APP_NOTEPAD,
    GUI_APP_SNAKE,
    GUI_APP_CUBEDIP,
    GUI_APP_EXPLORER
} gui_app_type_t;

typedef struct Window {
    int id;
    int x, y;
    int width, height;
    char title[32];
    uint16_t* buffer; // Content buffer (char + attr)
    int active;
    int focused;
    int maximized;
    int prev_x;
    int prev_y;
    int prev_width;
    int prev_height;
    int buffer_cells;
    gui_app_type_t app_type;
    void* app_state;
    gui_window_tick_fn on_tick;
    gui_window_key_fn on_key;
} Window;

void gui_init(void);
Window* gui_create_window(const char* title, int x, int y, int width, int height);
void gui_close_window(Window* win);
void gui_draw_text(Window* win, int x, int y, const char* text, uint8_t color);
void gui_clear_window(Window* win, uint8_t color);
void gui_update(void);
void gui_run(void); // Main loop for GUI mode

#endif
