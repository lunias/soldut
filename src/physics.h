#pragma once

#include "world.h"

/*
 * Physics: Verlet integrate, constraint relaxation, map collision.
 *
 * The order each tick:
 *   1. physics_apply_gravity   — adds level->gravity to every active particle.
 *   2. physics_integrate       — Verlet step (advances pos, stashes prev).
 *   3. physics_solve_constraints — relaxes the constraint pool N times.
 *   4. physics_collide_map     — pushes particles out of solid tiles.
 *
 * (Pose drive happens *before* (1) and is in mech.c.)
 *
 * Dead/sleeping mechs skip all of this. We checkpoint that with the
 * mech.sleeping flag; physics functions look at the per-particle
 * PARTICLE_FLAG_ACTIVE bit to opt out of integrate/collide.
 */

/* Tunables — exposed so tests and dev builds can change them. */
#define PHYSICS_VELOCITY_DAMP 0.99f      /* Soldat's RKV; lower = more drag */
#define PHYSICS_CONSTRAINT_ITERATIONS 12 /* relaxation passes per tick */
#define PHYSICS_PARTICLE_RADIUS 4.0f     /* px; for tile push-out */

void physics_apply_gravity(World *w, float dt);

/* Verlet integrate. Each particle remembers its previous position; this
 * step both moves it forward and updates the prev-slot. dt is the
 * physics sub-step (see [10-performance-budget.md] — 120 Hz internal). */
void physics_integrate(World *w, float dt);

/* The combined relaxation step: each iteration runs *all* distance
 * constraints once, then runs a collision-push pass against the tile
 * grid. Interleaving ensures that when a foot is held by collision,
 * the chain above it gets pulled up over the iterations rather than
 * sagging. Velocity correction (zeroing the normal component, applying
 * tangential friction) only happens on the final iteration so we don't
 * double-friction a foot that's still settling. */
void physics_constrain_and_collide(World *w);

/* Single-particle helpers exposed for animation pose-drive and weapon
 * recoil to nudge a specific particle. */
void physics_apply_impulse(ParticlePool *p, int particle_idx, Vec2 impulse);

/* Move a particle without changing its velocity. (Pose drive,
 * pre-collision projection, anything that's a "kinematic snap" rather
 * than a force.) Updates both pos and prev so the next integrate step
 * doesn't read the displacement as velocity. */
static inline void physics_translate_kinematic(ParticlePool *p, int i, float dx, float dy) {
    p->pos_x [i] += dx; p->pos_y [i] += dy;
    p->prev_x[i] += dx; p->prev_y[i] += dy;
}

/* Same as physics_translate_kinematic but clamps the move so the
 * particle doesn't pass through a solid tile. Pose drive can pull a
 * head 30+ px upward in one tick (strength 0.7 across a 50-px gap),
 * which tunnels through a 1-tile-thick platform if applied raw — the
 * subsequent collision pass sees only the post-teleport position and
 * has no opportunity to push the particle back. */
void physics_translate_kinematic_swept(ParticlePool *p, const Level *L,
                                       int i, float dx, float dy);

/* M6 — Post-pose terrain push-out for one mech's bones.
 *
 * After `pose_compute` + `pose_write_to_particles` finishes a tick,
 * the deterministic bone offsets may land inside solid tiles or
 * polygons (slopes). For LOCAL/AUTH mechs the pre-pose collision
 * pass handles this — but it gates on `inv_mass > 0`, so REMOTE
 * mechs on the client (which run kinematically) skip it. Result:
 * a remote mech's bones can dangle inside a slope while the same
 * mech's local view shows the bones clipped to the surface.
 *
 * This function does a single pass: for each of the mech's
 * PART_COUNT skeleton particles, check tile + poly overlaps and
 * push the particle out along the contact normal, kinematically
 * (pos AND prev shifted by the same delta — velocity preserved).
 * Ignores `inv_mass`, so it works for remote-on-client mechs.
 *
 * Pelvis is included; the constraint solver (grapple) and physics
 * already operate on pelvis, but a redundant push-out is cheap and
 * idempotent. Dismembered particles (per `m->dismember_mask`) are
 * skipped — those are free-flying Verlet bodies and the regular
 * collision passes handle them. */
void physics_push_mech_out_of_terrain(World *w, int mech_id);

/* Set the per-tick velocity directly via prev. Useful for input-driven
 * movement: pressing right should *be* a horizontal velocity, not an
 * accumulating force. (Vertical velocity is preserved by setting only
 * the relevant axis.) */
static inline void physics_set_velocity_x(ParticlePool *p, int i, float vx_per_tick) {
    p->prev_x[i] = p->pos_x[i] - vx_per_tick;
}
static inline void physics_set_velocity_y(ParticlePool *p, int i, float vy_per_tick) {
    p->prev_y[i] = p->pos_y[i] - vy_per_tick;
}
