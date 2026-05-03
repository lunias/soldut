#pragma once

#include "world.h"

/*
 * Projectiles — the live ballistic things flying around the world.
 *
 * Hitscan weapons (Pulse Rifle, Rail Cannon, Sidearm) don't go through
 * here; they ray-test on the fire tick and apply damage immediately.
 * Everything else (grenades, rockets, plasma orbs, Riot Cannon pellets,
 * etc.) lives in this pool until it hits something, expires, or
 * detonates.
 *
 * The pool is SoA per [09-codebase-architecture.md] — the integrate /
 * collide loops walk the same fields for every kind of projectile, and
 * we branch on `kind` only at hit time (where AOE / non-AOE behavior
 * diverges).
 */

void projectile_pool_init(ProjectilePool *pool);
void projectile_pool_clear(ProjectilePool *pool);

/* Spawn parameters for a new projectile. Caller fills the kind, owner,
 * and physical properties; we look up the rest from the projectile
 * stat table. Returns the slot index, or -1 if the pool is full. */
typedef struct {
    int   kind;             /* ProjectileKind */
    int   weapon_id;        /* For kill-feed attribution */
    int   owner_mech_id;
    int   owner_team;
    Vec2  origin;
    Vec2  velocity;         /* px/sec */
    float damage;           /* direct-hit damage */
    float aoe_radius;       /* 0 → no AOE */
    float aoe_damage;
    float aoe_impulse;      /* impulse applied to particles in AOE */
    float life;             /* seconds until expiry / fuse */
    float gravity_scale;    /* 0 = no gravity (rockets), 1 = full (knives), 0.4 (grenades) */
    float drag;             /* per-second velocity damping */
    bool  bouncy;           /* tile hit → bounce instead of detonate */
} ProjectileSpawn;

int  projectile_spawn(World *w, ProjectileSpawn s);

/* Per-tick step: integrate every live projectile, do tile + bone
 * collision, decrement fuses, detonate AOE on hit / fuse expiry. */
void projectile_step(World *w, float dt);

/* Render — draw each live projectile (caller is inside BeginMode2D). */
void projectile_draw(const ProjectilePool *p);

/* Spawn an explosion at `pos`. Walks every alive mech and applies
 * falloff'd damage + impulse to every particle inside `radius`. Casts a
 * ray for line-of-sight from explosion center to mech pelvis; halves
 * damage if blocked. Spawns sparks + smoke + screen shake.
 *
 * Used by AOE projectiles and any other "make a boom here" path. */
void explosion_spawn(World *w, Vec2 pos, float radius, float damage,
                     float impulse, int owner_mech_id, int owner_team,
                     int weapon_id);
