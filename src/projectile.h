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

/* Render — draw each live projectile (caller is inside BeginMode2D).
 * `alpha` is the in-between-ticks fraction in [0,1] used to lerp
 * between the projectile's start-of-tick position and its latest
 * physics result (P03 render-side accumulator). */
void projectile_draw(const ProjectilePool *p, float alpha);

/* Spawn an explosion at `pos`. Walks every alive mech and applies
 * falloff'd damage + impulse to every particle inside `radius`. Casts a
 * ray for line-of-sight from explosion center to mech pelvis; halves
 * damage if blocked. Spawns sparks + smoke + screen shake.
 *
 * Used by AOE projectiles and any other "make a boom here" path. */
void explosion_spawn(World *w, Vec2 pos, float radius, float damage,
                     float impulse, int owner_mech_id, int owner_team,
                     int weapon_id);

/* P06 — Find the live PROJ_GRAPPLE_HEAD owned by `owner_mech_id` in
 * the pool. Returns the slot index or -1. Used by render.c to draw the
 * rope from the firer's hand to the in-flight head. */
int projectile_find_grapple_head(const ProjectilePool *p, int owner_mech_id);

/* wan-fixes-21 — push an entry into the world's explosion-record
 * ring. `source` is EXPL_SRC_PREDICTED (client's local detonate)
 * or EXPL_SRC_SERVER (NET_MSG_EXPLOSION handler). The opposite-
 * source lookup is used to suppress double-flash. */
void explosion_record_push(World *w, Vec2 pos, int owner_mech,
                           int weapon_id, int source);

/* wan-fixes-21 — find the OLDEST valid record matching
 * (owner_mech, weapon_id, source) within `max_age_ticks` of
 * `w->tick`. Expires stale entries on the way (sets valid=false).
 * Returns NULL if no match. Returned pointer is mutable so the
 * caller can mark the matched record consumed (valid=false) to
 * keep subsequent server events from re-matching the same one. */
ExplosionRecord *explosion_record_find_consume(World *w,
                                                int owner_mech,
                                                int weapon_id,
                                                int source,
                                                uint32_t max_age_ticks);
