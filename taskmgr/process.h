#ifndef PROCESS_H
#define PROCESS_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PROC_KIND_KERNEL = 0,
    PROC_KIND_GOB = 1,
    PROC_KIND_DOS = 2
} process_kind_t;

typedef enum {
    PROC_STATE_READY = 0,
    PROC_STATE_RUNNING = 1,
    PROC_STATE_BLOCKED = 2,
    PROC_STATE_ZOMBIE = 3
} process_state_t;

typedef struct {
    int pid;
    char name[16];
    size_t memory_kb;
    uint32_t runtime_ticks;
    uint32_t frame_ticks;
    uint32_t render_ticks;
    uint32_t input_events;
    bool active;
    process_kind_t kind;
    process_state_t state;
} process_entry_t;

#define MAX_PROCESSES 64

extern process_entry_t process_table[MAX_PROCESSES];
extern int process_count;

int create_process(const char *name, size_t memory_kb);
int create_process_ex(const char *name, size_t memory_kb, process_kind_t kind);
void kill_process(int pid);

#define PROCESS_KILL_PROTECTED (-2)
int terminate_process(int pid);
int process_is_protected(int pid);
int get_kernel_process_count();
process_entry_t* get_kernel_process_table();
process_entry_t* process_get(int pid);
void update_process_runtime_metrics(int pid, uint32_t runtime_ticks, uint32_t frame_ticks,
                                    uint32_t render_ticks, uint32_t input_events);
void process_set_state(int pid, process_state_t state);
const char* process_state_name(process_state_t s);
const char* process_kind_name(process_kind_t k);

#endif
