#include "memory.h"

static uint8_t* heap_base = 0;
static size_t heap_capacity = 0;
static size_t heap_offset = 0;

void memory_init(void* heap_start, size_t heap_size) {
    heap_base = (uint8_t*)heap_start;
    heap_capacity = heap_size;
    heap_offset = 0;
}

void* kmalloc(size_t size) {
    if (heap_offset + size > heap_capacity) {
        return 0; // out of memory
    }

    void* ptr = heap_base + heap_offset;
    heap_offset += size;

    return ptr;
}

void kfree(void* ptr) {
    // no-op for now; no freeing individual blocks in bump allocator
    (void)ptr;
}

void memory_reset(void) {
    heap_offset = 0;
}
