#ifndef PROCESS_H
#define PROCESS_H

#include <stddef.h>
#include <stdbool.h>

typedef struct {
    int pid;
    char name[16];
    size_t memory_kb;
    bool active;
} process_entry_t;

#define MAX_PROCESSES 64

extern process_entry_t process_table[MAX_PROCESSES];
extern int process_count;

int create_process(const char *name, size_t memory_kb);
void kill_process(int pid);
int get_kernel_process_count();
process_entry_t* get_kernel_process_table();

#endif
