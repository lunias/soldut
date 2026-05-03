#pragma once

#include "arena.h"
#include "world.h"

/*
 * FX particles — blood, sparks, bullet tracers. Distinct from the
 * mech-bone particle pool: no constraints, simpler integrate, drawn as
 * sprites/lines rather than skinned polygons.
 *
 * The cap is 3000 (covers a worst-case 3-mech-explosion blood spew with
 * headroom). At M1 we'll rarely use more than a few hundred at once.
 */

void fx_pool_init(FxPool *pool, Arena *permanent_arena, int capacity);
void fx_clear(FxPool *pool);

/* Spawn helpers. All return the new index, or -1 if the pool is full
 * (we silently discard the oldest if needed by overwriting at index 0;
 * for FX that's fine — drops are imperceptible during heavy combat). */
int fx_spawn_blood(FxPool *pool, Vec2 pos, Vec2 vel, pcg32_t *rng);
int fx_spawn_spark(FxPool *pool, Vec2 pos, Vec2 vel, pcg32_t *rng);
int fx_spawn_tracer(FxPool *pool, Vec2 a, Vec2 b);

/* Step all FX particles. Calls into decal.c for blood that should
 * leave a permanent splat. */
void fx_update(World *w, float dt);

/* Render the live FX particles in their current state.
 * Caller is responsible for being inside BeginMode2D(...). */
void fx_draw(const FxPool *pool);
