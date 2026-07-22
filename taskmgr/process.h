#ifndef PROCESS_H
#define PROCESS_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int pid;
    char name[16];
    size_t memory_kb;
    uint32_t runtime_ticks;
    uint32_t frame_ticks;
    uint32_t render_ticks;
    uint32_t input_events;
    bool active;
} process_entry_t;

#define MAX_PROCESSES 64

extern process_entry_t process_table[MAX_PROCESSES];
extern int process_count;

int create_process(const char *name, size_t memory_kb);
void kill_process(int pid);

/*
 * Terminate a process by pid without touching the VGA text console (safe to
 * call from the VESA desktop). Refuses to kill the protected kernel process.
 * Returns: 1 on success, 0 if the pid was not found/inactive,
 * PROCESS_KILL_PROTECTED if the pid belongs to the kernel process.
 */
#define PROCESS_KILL_PROTECTED (-2)
int terminate_process(int pid);
int process_is_protected(int pid);
int get_kernel_process_count();
process_entry_t* get_kernel_process_table();
void update_process_runtime_metrics(int pid, uint32_t runtime_ticks, uint32_t frame_ticks,
                                    uint32_t render_ticks, uint32_t input_events);

#endif
