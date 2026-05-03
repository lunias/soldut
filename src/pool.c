#include "pool.h"
#include "log.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

void pool_init(Pool *p, void *storage, int stride, int capacity) {
    assert(stride >= (int)sizeof(int));
    p->items = storage;
    p->stride = stride;
    p->capacity = capacity;
    p->count = 0;
    p->free_head = -1;
    /* Storage starts unbroken; we lazily extend `count` as we allocate. */
}

int pool_alloc(Pool *p) {
    int idx;
    if (p->free_head >= 0) {
        idx = p->free_head;
        int *slot = (int *)((uint8_t *)p->items + (size_t)idx * (size_t)p->stride);
        p->free_head = *slot;
    } else if (p->count < p->capacity) {
        idx = p->count++;
    } else {
        LOG_W("pool exhausted (capacity %d)", p->capacity);
        return -1;
    }
    /* Caller is responsible for initializing the slot. We do *not* zero — the
     * old struct contents may be useful for diagnostics, and zeroing is a
     * surprise hidden-cost. Code that needs zeroed memory uses memset itself. */
    return idx;
}

void pool_free(Pool *p, int index) {
    assert(index >= 0 && index < p->capacity);
    int *slot = (int *)((uint8_t *)p->items + (size_t)index * (size_t)p->stride);
    *slot = p->free_head;
    p->free_head = index;
}

void pool_reset(Pool *p) {
    p->count = 0;
    p->free_head = -1;
}

void *pool_at(const Pool *p, int index) {
    assert(index >= 0 && index < p->capacity);
    return (uint8_t *)p->items + (size_t)index * (size_t)p->stride;
}

int pool_capacity(const Pool *p) { return p->capacity; }

int pool_live_count(const Pool *p) {
    /* Walk the free list. Cheap-ish (free list is short relative to count). */
    int free_count = 0;
    for (int i = p->free_head; i != -1; ) {
        ++free_count;
        int *slot = (int *)((uint8_t *)p->items + (size_t)i * (size_t)p->stride);
        i = *slot;
    }
    return p->count - free_count;
}
