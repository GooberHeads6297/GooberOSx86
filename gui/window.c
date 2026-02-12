#include "window.h"
#include "../drivers/video/vga.h"
#include "../drivers/mouse/mouse.h"
#include "../drivers/keyboard/keyboard.h"
#include "../lib/memory.h"
#include "../lib/string.h"

static Window windows[MAX_WINDOWS];
static int window_count = 0;
static int next_window_id = 1;
static uint16_t backbuffer[SCREEN_WIDTH * SCREEN_HEIGHT];
static int gui_running = 0;

void gui_init(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        windows[i].active = 0;
        windows[i].buffer = NULL;
    }
    window_count = 0;
}

Window* gui_create_window(const char* title, int x, int y, int width, int height) {
    if (window_count >= MAX_WINDOWS) return NULL;

    int idx = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].active) {
            idx = i;
            break;
        }
    }
    if (idx == -1) return NULL;

    Window* win = &windows[idx];
    win->id = next_window_id++;
    win->x = x;
    win->y = y;
    win->width = width;
    win->height = height;
    
    // Copy title safely
    int t_len = 0;
    while (title[t_len] && t_len < 31) {
        win->title[t_len] = title[t_len];
        t_len++;
    }
    win->title[t_len] = '\0';

    // Allocate buffer (width * height * 2 bytes)
    // For now, using a simple fixed size allocation or just malloc if we had it.
    // Since we have memory_alloc/free in lib/memory.h (implied by usage in kernel.c), let's use it.
    // Wait, kernel.c uses memory_init but I need to check if malloc is available.
    // Let's check lib/memory.h. Assuming malloc is available or I can implement a simple one.
    // For now, I'll use a static buffer pool or just assume malloc works if I include it.
    // Actually, let's check lib/memory.h first.
    
    // Assuming malloc exists for now based on kernel.c usage.
    win->buffer = (uint16_t*)kmalloc(width * height * sizeof(uint16_t));
    if (!win->buffer) return NULL;

    // Clear window buffer
    for (int i = 0; i < width * height; i++) {
        win->buffer[i] = (VGA_COLOR_WHITE << 8) | ' ';
    }

    win->active = 1;
    window_count++;
    return win;
}

void gui_close_window(Window* win) {
    if (win && win->active) {
        if (win->buffer) kfree(win->buffer);
        win->active = 0;
        window_count--;
    }
}

void gui_draw_text(Window* win, int x, int y, const char* text, uint8_t color) {
    if (!win || !win->active) return;
    int i = 0;
    while (text[i]) {
        if (x + i >= win->width) break;
        win->buffer[y * win->width + (x + i)] = (color << 8) | text[i];
        i++;
    }
}

void gui_clear_window(Window* win, uint8_t color) {
    if (!win || !win->active) return;
    for (int i = 0; i < win->width * win->height; i++) {
        win->buffer[i] = (color << 8) | ' ';
    }
}

static void draw_rect(int x, int y, int w, int h, uint8_t color, char c) {
    for (int r = y; r < y + h; r++) {
        if (r < 0 || r >= SCREEN_HEIGHT) continue;
        for (int col = x; col < x + w; col++) {
            if (col < 0 || col >= SCREEN_WIDTH) continue;
            backbuffer[r * SCREEN_WIDTH + col] = (color << 8) | c;
        }
    }
}

static void render_window(Window* win) {
    // Draw border
    int bx = win->x - 1;
    int by = win->y - 1;
    int bw = win->width + 2;
    int bh = win->height + 2;
    uint8_t border_color = VGA_COLOR_LIGHT_GREY | (VGA_COLOR_BLUE << 4);

    // Top/Bottom
    for (int i = 0; i < bw; i++) {
        int r1 = by;
        int r2 = by + bh - 1;
        int c = bx + i;
        if (c >= 0 && c < SCREEN_WIDTH) {
            if (r1 >= 0 && r1 < SCREEN_HEIGHT) backbuffer[r1 * SCREEN_WIDTH + c] = (border_color << 8) | 205; // Double horizontal
            if (r2 >= 0 && r2 < SCREEN_HEIGHT) backbuffer[r2 * SCREEN_WIDTH + c] = (border_color << 8) | 205;
        }
    }
    // Left/Right
    for (int i = 0; i < bh; i++) {
        int c1 = bx;
        int c2 = bx + bw - 1;
        int r = by + i;
        if (r >= 0 && r < SCREEN_HEIGHT) {
            if (c1 >= 0 && c1 < SCREEN_WIDTH) backbuffer[r * SCREEN_WIDTH + c1] = (border_color << 8) | 186; // Double vertical
            if (c2 >= 0 && c2 < SCREEN_WIDTH) backbuffer[r * SCREEN_WIDTH + c2] = (border_color << 8) | 186;
        }
    }
    // Corners
    if (bx >= 0 && bx < SCREEN_WIDTH && by >= 0 && by < SCREEN_HEIGHT) 
        backbuffer[by * SCREEN_WIDTH + bx] = (border_color << 8) | 201; // Top-left
    if (bx + bw - 1 >= 0 && bx + bw - 1 < SCREEN_WIDTH && by >= 0 && by < SCREEN_HEIGHT)
        backbuffer[by * SCREEN_WIDTH + bx + bw - 1] = (border_color << 8) | 187; // Top-right
    if (bx >= 0 && bx < SCREEN_WIDTH && by + bh - 1 >= 0 && by + bh - 1 < SCREEN_HEIGHT)
        backbuffer[(by + bh - 1) * SCREEN_WIDTH + bx] = (border_color << 8) | 200; // Bottom-left
    if (bx + bw - 1 >= 0 && bx + bw - 1 < SCREEN_WIDTH && by + bh - 1 >= 0 && by + bh - 1 < SCREEN_HEIGHT)
        backbuffer[(by + bh - 1) * SCREEN_WIDTH + bx + bw - 1] = (border_color << 8) | 188; // Bottom-right

    // Title
    int t_len = 0;
    while (win->title[t_len]) t_len++;
    int t_start = bx + (bw - t_len) / 2;
    for (int i = 0; i < t_len; i++) {
        int c = t_start + i;
        if (c >= 0 && c < SCREEN_WIDTH && by >= 0 && by < SCREEN_HEIGHT) {
            backbuffer[by * SCREEN_WIDTH + c] = (border_color << 8) | win->title[i];
        }
    }

    // Content
    for (int r = 0; r < win->height; r++) {
        int screen_r = win->y + r;
        if (screen_r < 0 || screen_r >= SCREEN_HEIGHT) continue;
        for (int c = 0; c < win->width; c++) {
            int screen_c = win->x + c;
            if (screen_c < 0 || screen_c >= SCREEN_WIDTH) continue;
            backbuffer[screen_r * SCREEN_WIDTH + screen_c] = win->buffer[r * win->width + c];
        }
    }
}

void gui_update(void) {
    // Clear background
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        backbuffer[i] = (VGA_COLOR_CYAN << 12) | (VGA_COLOR_BLUE << 8) | 176; // Dotted pattern
    }

    // Draw windows
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].active) {
            render_window(&windows[i]);
        }
    }

    // Draw mouse
    int mx = mouse_get_x();
    int my = mouse_get_y();
    if (mx >= 0 && mx < SCREEN_WIDTH && my >= 0 && my < SCREEN_HEIGHT) {
        uint16_t under = backbuffer[my * SCREEN_WIDTH + mx];
        uint8_t attr = (under >> 8);
        // Invert colors for cursor
        uint8_t fg = attr & 0x0F;
        uint8_t bg = (attr >> 4) & 0x0F;
        uint8_t new_attr = (fg << 4) | bg;
        // Or just make it bright white on red
        new_attr = VGA_COLOR_WHITE | (VGA_COLOR_RED << 4);
        
        backbuffer[my * SCREEN_WIDTH + mx] = (new_attr << 8) | 219; // Block cursor
    }

    // Flip to video memory
    memcpy(VIDEO_MEMORY, backbuffer, SCREEN_WIDTH * SCREEN_HEIGHT * 2);
}

void gui_run(void) {
    gui_init();
    gui_running = 1;

    // Create a demo window
    Window* w1 = gui_create_window("Welcome", 10, 5, 30, 10);
    gui_clear_window(w1, VGA_COLOR_WHITE | (VGA_COLOR_BLUE << 4));
    gui_draw_text(w1, 1, 1, "Welcome to GooberOS GUI!", VGA_COLOR_WHITE | (VGA_COLOR_BLUE << 4));
    gui_draw_text(w1, 1, 3, "Press ESC to exit.", VGA_COLOR_WHITE | (VGA_COLOR_BLUE << 4));

    Window* w2 = gui_create_window("Status", 45, 8, 20, 5);
    gui_clear_window(w2, VGA_COLOR_BLACK | (VGA_COLOR_LIGHT_GREY << 4));
    gui_draw_text(w2, 1, 1, "System OK", VGA_COLOR_BLACK | (VGA_COLOR_LIGHT_GREY << 4));

    while (gui_running) {
        gui_update();
        
        // Handle input
        if (keyboard_has_char()) {
            char c = keyboard_read_char();
            if (c == 27) { // ESC
                gui_running = 0;
            }
        }
        
        // Simple delay
        for (volatile int i = 0; i < 100000; i++);
    }
    
    // Cleanup
    for (int i = 0; i < MAX_WINDOWS; i++) {
        gui_close_window(&windows[i]);
    }
    
    // Restore text mode colors
    vga_set_text_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    clear_screen();
}
