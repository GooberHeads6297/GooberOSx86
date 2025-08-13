#include "process.h"
#include "../drivers/video/vga.h"

process_entry_t process_table[MAX_PROCESSES];
int process_count = 0;
static int next_pid = 1;

static void str_copy(char *dest, const char *src, size_t max_len) {
    size_t i = 0;
    while (src[i] != '\0' && i < max_len - 1) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

static void print_process_table_debug() {
    vga_set_text_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    int row = 0;
    for (int i = 0; i < process_count; i++) {
        process_entry_t *p = &process_table[i];
        if (!p->active) continue;
        char line[64];
        int pos = 0;
        line[pos++] = 'P'; line[pos++] = 'I'; line[pos++] = 'D'; line[pos++] = ':';
        line[pos++] = ' ';
        int pid = p->pid;
        char pid_buf[6] = {0};
        int pid_len = 0;
        if (pid == 0) {
            pid_buf[pid_len++] = '0';
        } else {
            int temp = pid;
            while (temp > 0) {
                pid_buf[pid_len++] = '0' + (temp % 10);
                temp /= 10;
            }
            for (int j = 0; j < pid_len/2; j++) {
                char t = pid_buf[j];
                pid_buf[j] = pid_buf[pid_len - 1 - j];
                pid_buf[pid_len - 1 - j] = t;
            }
        }
        for (int j = 0; j < pid_len; j++) {
            line[pos++] = pid_buf[j];
        }
        line[pos++] = ' ';
        line[pos++] = 'N'; line[pos++] = 'a'; line[pos++] = 'm'; line[pos++] = 'e'; line[pos++] = ':';
        line[pos++] = ' ';
        for (int j = 0; j < 15 && p->name[j] != '\0'; j++) {
            line[pos++] = p->name[j];
        }
        line[pos++] = ' ';
        line[pos++] = 'M'; line[pos++] = 'e'; line[pos++] = 'm'; line[pos++] = ':';
        line[pos++] = ' ';
        size_t mem = p->memory_kb;
        char mem_buf[10] = {0};
        int mem_len = 0;
        if (mem == 0) {
            mem_buf[mem_len++] = '0';
        } else {
            size_t temp = mem;
            while (temp > 0) {
                mem_buf[mem_len++] = '0' + (temp % 10);
                temp /= 10;
            }
            for (int j = 0; j < mem_len/2; j++) {
                char t = mem_buf[j];
                mem_buf[j] = mem_buf[mem_len - 1 - j];
                mem_buf[mem_len - 1 - j] = t;
            }
        }
        for (int j = 0; j < mem_len; j++) {
            line[pos++] = mem_buf[j];
        }
        line[pos++] = 'K';
        line[pos++] = 'B';
        line[pos++] = '\0';

        for (int c = 0; c < pos; c++) {
            vga_put_char_at(line[c], c, row, VGA_COLOR_LIGHT_GREEN | (VGA_COLOR_BLACK << 4));
        }
        row++;
    }
}

int create_process(const char *name, size_t memory_kb) {
    if (process_count >= MAX_PROCESSES) return -1;

    process_entry_t *p = &process_table[process_count];
    p->pid = next_pid++;
    str_copy(p->name, name, sizeof(p->name));
    p->memory_kb = memory_kb;
    p->active = true;

    process_count++;

    

    return p->pid;
}

void kill_process(int pid) {
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].pid == pid && process_table[i].active) {
            process_table[i].active = false;
            // Shift remaining entries left to fill gap
            for (int j = i; j < process_count - 1; j++) {
                process_table[j] = process_table[j + 1];
            }
            process_count--;

            print_process_table_debug();

            return;
        }
    }
}

int get_kernel_process_count() {
    return process_count;
}

process_entry_t* get_kernel_process_table() {
    return process_table;
}
