#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>
#include <stddef.h>

#define MAX_WINDOWS 10
#define SCREEN_WIDTH 80
#define SCREEN_HEIGHT 25

typedef struct {
    int id;
    int x, y;
    int width, height;
    char title[32];
    uint16_t* buffer; // Content buffer (char + attr)
    int active;
} Window;

void gui_init(void);
Window* gui_create_window(const char* title, int x, int y, int width, int height);
void gui_close_window(Window* win);
void gui_draw_text(Window* win, int x, int y, const char* text, uint8_t color);
void gui_clear_window(Window* win, uint8_t color);
void gui_update(void);
void gui_run(void); // Main loop for GUI mode

#endif
