#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>
#include <stdint.h>

/*
 * Phase 3f extends the heap API with krealloc() so storage drivers, the
 * install path, the editor, taskmgr, games, and GooberDOS can resize
 * buffers cleanly. Both x86 and x86_64 use the free-list allocator in
 * lib/memory.c backed by a BSS arena from kernel.c.
 *
 * Semantics:
 *   - kmalloc(0)   -> NULL
 *   - kfree(NULL)  -> no-op
 *   - krealloc(p, 0)        is equivalent to kfree(p) and returns NULL
 *   - krealloc(NULL, n)     is equivalent to kmalloc(n)
 *
 * memory_init(start, size) seeds the allocator with the given BSS-backed
 * region. Both arches require this to be called before the first kmalloc.
 */

void memory_init(void* heap_start, size_t heap_size);
void* kmalloc(size_t size);
void  kfree(void* ptr);
void* krealloc(void* ptr, size_t new_size);
void memory_reset(void);

/*
 * Phase 3f diagnostics: total budget vs. remaining free bytes (free-list
 * sums "free regions" minus header overhead on x64; on x86 these report
 * the bump allocator's capacity / unused-tail bytes). Used by the heap
 * boot stage to print a `[heap] init` line.
 */
size_t memory_total_bytes(void);
size_t memory_free_bytes(void);

#endif
