#include "game.h"
#include "log.h"
#include "particle.h"

#include <string.h>

/* Allocate the SoA particle pool out of the permanent arena. We size
 * it once and never grow. */
static bool init_particle_pool(Game *g) {
    ParticlePool *p = &g->world.particles;
    int cap = PARTICLES_CAPACITY;
    p->capacity = cap;
    p->count = 0;
    p->pos_x   = (float    *)arena_alloc(&g->permanent, sizeof(float)   * cap);
    p->pos_y   = (float    *)arena_alloc(&g->permanent, sizeof(float)   * cap);
    p->prev_x  = (float    *)arena_alloc(&g->permanent, sizeof(float)   * cap);
    p->prev_y  = (float    *)arena_alloc(&g->permanent, sizeof(float)   * cap);
    p->inv_mass= (float    *)arena_alloc(&g->permanent, sizeof(float)   * cap);
    p->flags   = (uint8_t  *)arena_alloc(&g->permanent, sizeof(uint8_t) * cap);
    if (!p->pos_x || !p->pos_y || !p->prev_x || !p->prev_y ||
        !p->inv_mass || !p->flags) return false;
    memset(p->flags, 0, (size_t)cap);
    return true;
}

static bool init_constraint_pool(Game *g) {
    ConstraintPool *c = &g->world.constraints;
    c->capacity = CONSTRAINTS_CAPACITY;
    c->count = 0;
    c->items = (Constraint *)arena_alloc_aligned(
        &g->permanent, sizeof(Constraint) * (size_t)c->capacity, _Alignof(Constraint));
    if (!c->items) return false;
    memset(c->items, 0, sizeof(Constraint) * (size_t)c->capacity);
    return true;
}

bool game_init(Game *g) {
    memset(g, 0, sizeof(*g));

    /* Budgets per [10-performance-budget.md]. */
    arena_init(&g->permanent,  32u * 1024u * 1024u, "permanent");
    arena_init(&g->level_arena, 24u * 1024u * 1024u, "level");
    arena_init(&g->frame_arena,  4u * 1024u * 1024u, "frame");
    if (!g->permanent.base || !g->level_arena.base || !g->frame_arena.base) {
        LOG_E("game_init: arena allocation failed");
        return false;
    }

    pcg32_seed(&g->rng, 0xA17C0DEull, 0xDEADBEEFull);

    if (!init_particle_pool(g))   { LOG_E("particle pool init failed");   return false; }
    if (!init_constraint_pool(g)) { LOG_E("constraint pool init failed"); return false; }

    fx_pool_init(&g->world.fx, &g->permanent, MAX_BLOOD);

    g->world.rng = &g->rng;
    g->world.local_mech_id = -1;
    g->world.dummy_mech_id = -1;
    g->world.camera_zoom = 1.0f;

    g->mode = MODE_LOBBY;
    g->tick = 0;
    g->time_seconds = 0.0;
    LOG_I("game_init: ok (perm=%zu level=%zu frame=%zu, particles=%d, cstr=%d, fx=%d)",
          g->permanent.size, g->level_arena.size, g->frame_arena.size,
          g->world.particles.capacity, g->world.constraints.capacity,
          g->world.fx.capacity);
    return true;
}

void game_shutdown(Game *g) {
    LOG_I("game_shutdown: peak perm=%zu level=%zu frame=%zu",
          g->permanent.peak, g->level_arena.peak, g->frame_arena.peak);
    arena_destroy(&g->permanent);
    arena_destroy(&g->level_arena);
    arena_destroy(&g->frame_arena);
    memset(g, 0, sizeof(*g));
}

void game_step(Game *g, double dt) {
    arena_reset(&g->frame_arena);
    g->tick += 1;
    g->time_seconds += dt;
}
