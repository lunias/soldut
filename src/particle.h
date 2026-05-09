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
/* P12 — Smoke puff. Mirrors fx_spawn_blood with darker color, longer
 * life, additive-darker render. Spawned by the per-limb damage threshold
 * check in simulate_step when a limb's HP frac drops below 0.30. */
int fx_spawn_smoke(FxPool *pool, Vec2 pos, Vec2 vel, pcg32_t *rng);
/* P12 — Pinned dismemberment emitter. Lives in the FX pool as an FX_STUMP
 * particle that doesn't itself draw or integrate; each tick of fx_update
 * looks up the parent particle (`mech_id` + parent of `limb`) and spawns
 * 1–2 blood drops at that world position. Self-deactivates on duration
 * expiry or invalid pin. */
int fx_spawn_stump_emitter(FxPool *pool, int mech_id, int limb, float duration_s);

/* Step all FX particles. Calls into decal.c for blood that should
 * leave a permanent splat. */
void fx_update(World *w, float dt);

/* Render the live FX particles in their current state.
 * Caller is responsible for being inside BeginMode2D(...).
 * `alpha` is the in-between-ticks fraction in [0,1] (P03). */
void fx_draw(const FxPool *pool, float alpha);
