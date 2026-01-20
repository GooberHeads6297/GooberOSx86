#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>
#include <stdint.h>

void memory_init(void* heap_start, size_t heap_size);
void* kmalloc(size_t size);
void kfree(void* ptr);  // We'll implement a no-op for now or a reset function instead
void memory_reset(void);

#endif
