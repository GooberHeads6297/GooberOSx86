#include "../drivers/keyboard/keyboard.h"
#include "../drivers/video/vga.h"
#include "../drivers/timer/timer.h"
#include "../kernel.h"
#include "process.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCREEN_WIDTH 80
#define SCREEN_HEIGHT 25
#define TASKMGR_WINDOW_HEIGHT 15

static int scroll_offset = 0;

static void vga_putch(int x, int y, char c) {
    vga_put_char_at(c, x, y, VGA_COLOR_LIGHT_GREY);
}

static void draw_box(int x, int y, int width, int height) {
    vga_putch(x, y, '+');
    vga_putch(x + width - 1, y, '+');
    vga_putch(x, y + height - 1, '+');
    vga_putch(x + width - 1, y + height - 1, '+');
    for (int i = 1; i < width - 1; i++) {
        vga_putch(x + i, y, '-');
        vga_putch(x + i, y + height - 1, '-');
    }
    for (int i = 1; i < height - 1; i++) {
        vga_putch(x, y + i, '|');
        vga_putch(x + width - 1, y + i, '|');
    }
}

static void print_centered(int y, const char* str) {
    int len = 0;
    while (str[len] != '\0') len++;
    int x = (SCREEN_WIDTH - len) / 2;
    for (int i = 0; i < len; i++) {
        vga_putch(x + i, y, str[i]);
    }
}

static void itoa(int value, char* str) {
    char buf[16];
    int pos = 0;
    if (value == 0) {
        str[0] = '0';
        str[1] = '\0';
        return;
    }
    while (value > 0) {
        buf[pos++] = '0' + (value % 10);
        value /= 10;
    }
    for (int i = 0; i < pos; i++) {
        str[i] = buf[pos - i - 1];
    }
    str[pos] = '\0';
}

static void display_processes(int start_y, int x, int width, int height) {
    int visible_lines = height - 2;
    process_entry_t *table = get_kernel_process_table();
    int total = get_kernel_process_count();

    for (int i = 0; i < visible_lines; i++) {
        int idx = i + scroll_offset;
        int line_y = start_y + 1 + i;

        for (int col = x + 1; col < x + width - 1; col++) {
            vga_putch(col, line_y, ' ');
        }

        if (idx < total) {
            process_entry_t *p = &table[idx];
            char pid_str[8];
            char mem_str[16];
            itoa(p->pid, pid_str);
            itoa((int)p->memory_kb, mem_str);

            int col_pos = x + 1;
            for (int j = 0; pid_str[j]; j++) vga_putch(col_pos++, line_y, pid_str[j]);
            vga_putch(col_pos++, line_y, ' ');
            for (int j = 0; p->name[j]; j++) vga_putch(col_pos++, line_y, p->name[j]);
            while (col_pos < x + 1 + 16) vga_putch(col_pos++, line_y, ' ');
            for (int j = 0; mem_str[j]; j++) vga_putch(col_pos++, line_y, mem_str[j]);
            vga_putch(col_pos++, line_y, 'K');
            vga_putch(col_pos++, line_y, 'B');
        }
    }
}

static bool handle_input() {
    bool should_exit = false;
    while (keyboard_has_char()) {
        unsigned char c = (unsigned char)keyboard_read_char();
        switch (c) {
            case 0x48:
                if (scroll_offset > 0) scroll_offset--;
                break;
            case 0x50:
                if (scroll_offset < get_kernel_process_count() - (TASKMGR_WINDOW_HEIGHT - 2))
                    scroll_offset++;
                break;
            case 3:
            case 0x1B:
                should_exit = true;
                clear_screen();
                break;
        }
    }
    return should_exit;
}

void run_task_manager() {
    scroll_offset = 0;

    while (true) {
        clear_screen();
        draw_box(10, 5, 60, TASKMGR_WINDOW_HEIGHT);
        print_centered(5, "GooberOS Task Manager");
        display_processes(5, 10, 60, TASKMGR_WINDOW_HEIGHT);
        if (handle_input()) break;
        timer_sleep(50);
    }
}
