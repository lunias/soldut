#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Two hashes and one PRNG.
 *
 *   - fnv1a_64       : data hashing, deterministic, asset IDs.
 *   - pcg32_state_t  : seedable PRNG used in simulation paths.
 *
 * No global RNG state: every random consumer carries its own PCG. The
 * world's RNG is on `World` so simulate() is a pure function of inputs.
 */

uint64_t fnv1a_64(const void *data, size_t len);
uint64_t fnv1a_64_str(const char *s);

typedef struct {
    uint64_t state;
    uint64_t inc;       /* must be odd */
} pcg32_t;

void     pcg32_seed(pcg32_t *r, uint64_t seed, uint64_t stream);
uint32_t pcg32_next(pcg32_t *r);

/* Helpers built on pcg32_next. */
float    pcg32_float01(pcg32_t *r);          /* [0, 1) */
int32_t  pcg32_range(pcg32_t *r, int32_t lo, int32_t hi);  /* [lo, hi) */
