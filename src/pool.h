#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Fixed-capacity object pool with stable indices.
 *
 * Storage is allocated *once* (typically out of an Arena) at startup.
 * pool_alloc returns an index in [0, capacity); pool_free flips an item
 * back to the free list. Indices are stable across alloc/free cycles —
 * callers can hold them as identifiers (MechId, ProjectileId, etc.).
 *
 * The pool itself doesn't know its element type. The caller knows.
 * pool_at(p, i) returns a typed pointer the caller casts.
 *
 * For SoA pools (particles, projectiles) we don't use this — those keep
 * parallel arrays directly. Pool is for AoS when stable handles matter.
 */

typedef struct Pool {
    void *items;
    int   stride;       /* bytes per item */
    int   capacity;
    int   count;        /* live + freed; freed items live on the free list */
    int   free_head;    /* index of first free slot, or -1 */
} Pool;

/*
 * Backing storage must be at least capacity * stride bytes, and must
 * outlive the Pool. The pool stores indices into the storage; freed
 * slots reuse the first sizeof(int) bytes for a free-list link.
 *
 * Therefore stride must be >= sizeof(int).
 */
void pool_init(Pool *p, void *storage, int stride, int capacity);

/* Returns -1 if the pool is full. */
int  pool_alloc(Pool *p);
void pool_free(Pool *p, int index);
void pool_reset(Pool *p);

void *pool_at(const Pool *p, int index);

int  pool_capacity(const Pool *p);
int  pool_live_count(const Pool *p);   /* approx; counts allocations minus frees */
