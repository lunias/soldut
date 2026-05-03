#include "hash.h"

uint64_t fnv1a_64(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

uint64_t fnv1a_64_str(const char *s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*s) {
        h ^= (uint8_t)(*s++);
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* PCG XSH-RR 64/32. Reference: https://www.pcg-random.org/ */

void pcg32_seed(pcg32_t *r, uint64_t seed, uint64_t stream) {
    r->state = 0;
    r->inc   = (stream << 1u) | 1u;
    pcg32_next(r);
    r->state += seed;
    pcg32_next(r);
}

uint32_t pcg32_next(pcg32_t *r) {
    uint64_t old = r->state;
    r->state = old * 6364136223846793005ULL + r->inc;
    uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = (uint32_t)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((-(int)rot) & 31));
}

float pcg32_float01(pcg32_t *r) {
    /* 24 random bits → [0, 1). */
    return (pcg32_next(r) >> 8) * (1.0f / 16777216.0f);
}

int32_t pcg32_range(pcg32_t *r, int32_t lo, int32_t hi) {
    if (hi <= lo) return lo;
    uint32_t span = (uint32_t)(hi - lo);
    return lo + (int32_t)(pcg32_next(r) % span);
}
