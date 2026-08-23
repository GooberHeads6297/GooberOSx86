#include "memory.h"

/*
 * Phase 3f kernel allocator (both x86 and x86_64).
 *
 * Doubly-linked first-fit free-list with coalescing on free and on
 * growth, real kfree(), and a krealloc() that grows in place when the
 * next neighbour is free.
 *
 * The arena is BSS-backed: each arch passes a dedicated heap array from
 * kernel.c (g_i386_kernel_heap[] / g_x64_kernel_heap[]).
 */

/*
 * Free-list allocator with coalescing.
 *
 * Each block carries a header that holds its size (excluding the
 * header), a magic for hardening, and prev/next pointers when the
 * block is on the free list (the same fields are reused as plain
 * "in-use" markers for allocated blocks; we just unlink them). Blocks
 * are also linked address-sorted via prev_phys/next_phys so we can
 * coalesce neighbours in O(1) on free.
 */

#define HEAP_MAGIC_FREE   0xF12EF12EU
#define HEAP_MAGIC_USED   0xA10CA10CU
#define HEAP_MIN_PAYLOAD  ((size_t)32u)
#define HEAP_ALIGN        ((size_t)16u)

typedef struct heap_block {
    size_t              size;       /* payload size (excludes this header)  */
    uint32_t            magic;      /* HEAP_MAGIC_FREE / HEAP_MAGIC_USED    */
    uint32_t            pad;        /* explicit pad keeps header == 48 B    */
    struct heap_block*  prev_free;
    struct heap_block*  next_free;
    struct heap_block*  prev_phys;  /* address-sorted neighbour links       */
    struct heap_block*  next_phys;
} heap_block_t;

static uint8_t*       g_heap_start = 0;
static size_t         g_heap_capacity = 0;
static heap_block_t*  g_free_head = 0;
static size_t         g_heap_free_bytes = 0;

static inline size_t heap_align_up(size_t v) {
    return (v + (HEAP_ALIGN - 1u)) & ~(HEAP_ALIGN - 1u);
}

static void heap_freelist_unlink(heap_block_t* b) {
    if (b->prev_free) b->prev_free->next_free = b->next_free;
    else              g_free_head = b->next_free;
    if (b->next_free) b->next_free->prev_free = b->prev_free;
    b->prev_free = 0;
    b->next_free = 0;
}

static void heap_freelist_push(heap_block_t* b) {
    b->prev_free = 0;
    b->next_free = g_free_head;
    if (g_free_head) g_free_head->prev_free = b;
    g_free_head = b;
}

void memory_init(void* heap_start, size_t heap_size) {
    /* Align the arena base up to HEAP_ALIGN and trim the size accordingly.
     * The full block header layout is then automatically aligned. */
    uintptr_t raw = (uintptr_t)heap_start;
    uintptr_t aligned = (raw + (HEAP_ALIGN - 1u)) & ~(HEAP_ALIGN - 1u);
    if (heap_size <= (aligned - raw) + sizeof(heap_block_t) + HEAP_MIN_PAYLOAD) {
        g_heap_start    = 0;
        g_heap_capacity = 0;
        g_free_head     = 0;
        g_heap_free_bytes = 0;
        return;
    }
    heap_size -= (size_t)(aligned - raw);
    heap_size &= ~(HEAP_ALIGN - 1u);

    g_heap_start    = (uint8_t*)aligned;
    g_heap_capacity = heap_size;

    heap_block_t* root = (heap_block_t*)g_heap_start;
    root->size      = heap_size - sizeof(heap_block_t);
    root->magic     = HEAP_MAGIC_FREE;
    root->pad       = 0;
    root->prev_free = 0;
    root->next_free = 0;
    root->prev_phys = 0;
    root->next_phys = 0;

    g_free_head        = root;
    g_heap_free_bytes  = root->size;
}

static heap_block_t* heap_find_fit(size_t need) {
    heap_block_t* b = g_free_head;
    while (b) {
        if (b->size >= need) return b;
        b = b->next_free;
    }
    return 0;
}

static void heap_split(heap_block_t* b, size_t need) {
    /* Split b into [need | leftover] when leftover fits another header
     * plus the minimum payload. Otherwise leave the slack inside b. */
    if (b->size < need + sizeof(heap_block_t) + HEAP_MIN_PAYLOAD) return;

    size_t leftover_size = b->size - need - sizeof(heap_block_t);
    heap_block_t* tail = (heap_block_t*)((uint8_t*)b + sizeof(heap_block_t) + need);
    tail->size      = leftover_size;
    tail->magic     = HEAP_MAGIC_FREE;
    tail->pad       = 0;
    tail->prev_free = 0;
    tail->next_free = 0;

    /* Splice tail into the address-sorted phys list between b and b->next_phys. */
    tail->prev_phys = b;
    tail->next_phys = b->next_phys;
    if (b->next_phys) b->next_phys->prev_phys = tail;
    b->next_phys    = tail;

    b->size = need;

    heap_freelist_push(tail);
    g_heap_free_bytes += tail->size;
    /* Net free delta from the split: we gained tail->size but lost
     * sizeof(heap_block_t) of usable bytes that became overhead. */
    g_heap_free_bytes -= sizeof(heap_block_t);
}

static int heap_block_in_arena(const heap_block_t* b) {
    uintptr_t p = (uintptr_t)b;
    uintptr_t start = (uintptr_t)g_heap_start;
    uintptr_t end = start + g_heap_capacity;
    if (p < start || p >= end) return 0;
    if (p + sizeof(heap_block_t) > end) return 0;
    if (p + sizeof(heap_block_t) + b->size > end) return 0;
    return 1;
}

void* kmalloc(size_t size) {
    if (size == 0)   return 0;
    if (!g_heap_start) return 0;

    size_t need = heap_align_up(size < HEAP_MIN_PAYLOAD ? HEAP_MIN_PAYLOAD : size);

    heap_block_t* b = heap_find_fit(need);
    if (!b) return 0;
    if (!heap_block_in_arena(b) || b->magic != HEAP_MAGIC_FREE) return 0;

    heap_freelist_unlink(b);
    g_heap_free_bytes -= b->size;
    heap_split(b, need);
    b->magic = HEAP_MAGIC_USED;

    return (void*)((uint8_t*)b + sizeof(heap_block_t));
}

static void heap_coalesce(heap_block_t* b) {
    /* Merge b with its right neighbour if free. */
    heap_block_t* next = b->next_phys;
    if (next && heap_block_in_arena(next) && next->magic == HEAP_MAGIC_FREE) {
        heap_freelist_unlink(next);
        b->size      += sizeof(heap_block_t) + next->size;
        b->next_phys  = next->next_phys;
        if (next->next_phys) next->next_phys->prev_phys = b;
        g_heap_free_bytes += sizeof(heap_block_t);  /* reclaimed header */
        next->magic = 0;
    }
    /* Merge b with its left neighbour if free. */
    heap_block_t* prev = b->prev_phys;
    if (prev && heap_block_in_arena(prev) && prev->magic == HEAP_MAGIC_FREE) {
        heap_freelist_unlink(b);
        prev->size      += sizeof(heap_block_t) + b->size;
        prev->next_phys  = b->next_phys;
        if (b->next_phys) b->next_phys->prev_phys = prev;
        g_heap_free_bytes += sizeof(heap_block_t);
        b->magic = 0;
    }
}

void kfree(void* ptr) {
    if (!ptr) return;
    if (!g_heap_start) return;

    heap_block_t* b = (heap_block_t*)((uint8_t*)ptr - sizeof(heap_block_t));
    if (!heap_block_in_arena(b) || b->magic != HEAP_MAGIC_USED) {
        /* Double-free, wild pointer, or arena smash: drop without walking. */
        return;
    }
    b->magic = HEAP_MAGIC_FREE;
    heap_freelist_push(b);
    g_heap_free_bytes += b->size;
    heap_coalesce(b);
}

void* krealloc(void* ptr, size_t new_size) {
    if (!ptr)             return kmalloc(new_size);
    if (new_size == 0)    { kfree(ptr); return 0; }
    if (!g_heap_start)    return 0;

    heap_block_t* b = (heap_block_t*)((uint8_t*)ptr - sizeof(heap_block_t));
    if (b->magic != HEAP_MAGIC_USED) return 0;

    size_t need = heap_align_up(new_size < HEAP_MIN_PAYLOAD ? HEAP_MIN_PAYLOAD : new_size);

    if (b->size >= need) {
        /* Shrinking in place: optionally split off the tail. */
        heap_split(b, need);
        return ptr;
    }

    /* Try to extend in place by absorbing the next phys block if it's free. */
    heap_block_t* next = b->next_phys;
    if (next && next->magic == HEAP_MAGIC_FREE &&
        b->size + sizeof(heap_block_t) + next->size >= need) {
        heap_freelist_unlink(next);
        g_heap_free_bytes -= next->size;
        b->size      += sizeof(heap_block_t) + next->size;
        b->next_phys  = next->next_phys;
        if (next->next_phys) next->next_phys->prev_phys = b;
        next->magic   = 0;
        g_heap_free_bytes += sizeof(heap_block_t);  /* header reclaimed */
        heap_split(b, need);
        return ptr;
    }

    /* Fall back to alloc + copy + free. */
    void* nptr = kmalloc(new_size);
    if (!nptr) return 0;
    size_t copy = b->size < new_size ? b->size : new_size;
    uint8_t* dst = (uint8_t*)nptr;
    const uint8_t* src = (const uint8_t*)ptr;
    for (size_t i = 0; i < copy; i++) dst[i] = src[i];
    kfree(ptr);
    return nptr;
}

void memory_reset(void) {
    /* Best-effort full reset: re-seed the arena from g_heap_start. */
    if (g_heap_start && g_heap_capacity)
        memory_init(g_heap_start, g_heap_capacity);
}

size_t memory_total_bytes(void) { return g_heap_capacity; }
size_t memory_free_bytes(void)  { return g_heap_free_bytes; }
