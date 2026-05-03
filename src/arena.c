#include "arena.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

static size_t align_up(size_t x, size_t a) {
    return (x + (a - 1)) & ~(a - 1);
}

void arena_init(Arena *a, size_t bytes, const char *name) {
    a->base = (uint8_t *)malloc(bytes);
    a->size = a->base ? bytes : 0;
    a->used = 0;
    a->peak = 0;
    a->name = name ? name : "arena";
    if (!a->base) {
        LOG_E("arena_init: failed to allocate %zu bytes for '%s'", bytes, a->name);
    }
}

void arena_destroy(Arena *a) {
    free(a->base);
    a->base = NULL;
    a->size = a->used = a->peak = 0;
}

void *arena_alloc(Arena *a, size_t bytes) {
    return arena_alloc_aligned(a, bytes, 16);
}

void *arena_alloc_aligned(Arena *a, size_t bytes, size_t alignment) {
    if (alignment < 1) alignment = 1;
    size_t start = align_up(a->used, alignment);
    size_t end = start + bytes;
    if (end > a->size) {
        LOG_E("arena '%s' exhausted: requested %zu, used %zu, capacity %zu",
              a->name, bytes, a->used, a->size);
        return NULL;
    }
    a->used = end;
    if (a->used > a->peak) a->peak = a->used;
    return a->base + start;
}

void arena_reset(Arena *a) {
    a->used = 0;
}

size_t arena_used(const Arena *a) { return a->used; }
size_t arena_peak(const Arena *a) { return a->peak; }
