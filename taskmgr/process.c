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

const char* process_state_name(process_state_t s) {
    switch (s) {
    case PROC_STATE_READY: return "Ready";
    case PROC_STATE_RUNNING: return "Running";
    case PROC_STATE_BLOCKED: return "Blocked";
    case PROC_STATE_ZOMBIE: return "Zombie";
    default: return "?";
    }
}

const char* process_kind_name(process_kind_t k) {
    if (k == PROC_KIND_GOB) return "GooberApp";
    if (k == PROC_KIND_DOS) return "GooberDOS";
    return "Kernel";
}

int create_process_ex(const char *name, size_t memory_kb, process_kind_t kind) {
    if (process_count >= MAX_PROCESSES) return -1;
    process_entry_t *p = &process_table[process_count];
    p->pid = next_pid++;
    str_copy(p->name, name, sizeof(p->name));
    p->memory_kb = memory_kb;
    p->runtime_ticks = 0;
    p->frame_ticks = 0;
    p->render_ticks = 0;
    p->input_events = 0;
    p->active = true;
    p->kind = kind;
    p->state = PROC_STATE_RUNNING;
    process_count++;
    return p->pid;
}

int create_process(const char *name, size_t memory_kb) {
    return create_process_ex(name, memory_kb, PROC_KIND_KERNEL);
}

process_entry_t* process_get(int pid) {
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].pid == pid && process_table[i].active)
            return &process_table[i];
    }
    return 0;
}

void process_set_state(int pid, process_state_t state) {
    process_entry_t* p = process_get(pid);
    if (p) p->state = state;
}

void kill_process(int pid) {
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].pid == pid && process_table[i].active) {
            process_table[i].active = false;
            process_table[i].state = PROC_STATE_ZOMBIE;
            for (int j = i; j < process_count - 1; j++)
                process_table[j] = process_table[j + 1];
            process_count--;
            return;
        }
    }
}

static int names_equal(const char *a, const char *b) {
    size_t i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

int process_is_protected(int pid) {
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].pid == pid && process_table[i].active)
            return names_equal(process_table[i].name, "kernel.bin");
    }
    return 0;
}

int terminate_process(int pid) {
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].pid == pid && process_table[i].active) {
            if (names_equal(process_table[i].name, "kernel.bin"))
                return PROCESS_KILL_PROTECTED;
            process_table[i].active = false;
            process_table[i].state = PROC_STATE_ZOMBIE;
            for (int j = i; j < process_count - 1; j++)
                process_table[j] = process_table[j + 1];
            process_count--;
            return 1;
        }
    }
    return 0;
}

int get_kernel_process_count() {
    return process_count;
}

process_entry_t* get_kernel_process_table() {
    return process_table;
}

void update_process_runtime_metrics(int pid, uint32_t runtime_ticks, uint32_t frame_ticks,
                                    uint32_t render_ticks, uint32_t input_events) {
    process_entry_t *p = process_get(pid);
    if (!p) return;
    p->runtime_ticks = runtime_ticks;
    p->frame_ticks = frame_ticks;
    p->render_ticks = render_ticks;
    p->input_events = input_events;
}
