#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Bump-allocator. One malloc at init, then arena_alloc hands out chunks
 * with no per-allocation bookkeeping. arena_reset rewinds `used` to zero.
 *
 * Three of these live in Game: permanent, level_arena, frame_arena.
 * Nothing in the inner loop talks to the C heap.
 */

typedef struct Arena {
    uint8_t *base;
    size_t   size;
    size_t   used;
    size_t   peak;          /* high-water mark, useful for budgeting */
    const char *name;       /* for diagnostics; not freed */
} Arena;

void  arena_init(Arena *a, size_t bytes, const char *name);
void  arena_destroy(Arena *a);

/* Returns NULL only when the arena is exhausted. Logs an error. */
void *arena_alloc(Arena *a, size_t bytes);
void *arena_alloc_aligned(Arena *a, size_t bytes, size_t alignment);

void  arena_reset(Arena *a);
size_t arena_used(const Arena *a);
size_t arena_peak(const Arena *a);

#define ARENA_NEW(arena, T)        ((T *)arena_alloc_aligned((arena), sizeof(T), _Alignof(T)))
#define ARENA_NEW_N(arena, T, n)   ((T *)arena_alloc_aligned((arena), sizeof(T) * (n), _Alignof(T)))
